use std::collections::HashMap;
use std::hash::Hash;
use std::io::{self, prelude::*};

use chrono::prelude::*;
use stable_eyre::eyre::{anyhow, bail, ensure, Result};

use crate::compat::attributes::{self, Attributes};
use crate::compat::context::{Context, WorkDir};
use crate::object::*;
use crate::plan::{ResourceAccessor, TextPlan};

struct JobRunner<'a, Ctx: Context<'a>> {
    context: &'a Ctx,
    workdir: Ctx::WorkDir,
    envs: Attributes,
    start_ts: DateTime<FixedOffset>,
}

impl<'a, Ctx: Context<'a>> JobRunner<'a, Ctx> {
    fn from_job(context: &'a Ctx, job: &Job) -> Result<Self> {
        let workdir = context.new_workdir()?;
        let mut job_buf = Vec::new();
        attributes::to_writer(&mut job_buf, job)?;
        io::copy(&mut job_buf.as_slice(), &mut workdir.create("job")?)?;

        for (input_path, input_id) in job.inputs.iter() {
            let mut input_file = workdir.create(input_path)?;
            if input_path.starts_with("in/") {
                let mut reader = context.store().read_resource(input_id)?;
                io::copy(&mut reader, &mut input_file)?;
            } else if input_path.starts_with("inref/") {
                writeln!(input_file, "{}", input_id)?;
            } else {
                bail!("invalid input path");
            }
        }

        let envs = Attributes::from_reader(&mut job_buf.as_slice())?;
        let start_ts = context.local_now_with_offset();
        Ok(JobRunner {
            context,
            workdir,
            envs,
            start_ts,
        })
    }

    fn run_command(mut self, job_id: &Id<Job>, command: &str) -> Result<Production> {
        self.workdir.create_dir("out")?;
        // TODO failure to run should be reflected in exit code?
        let (exit_code, output) = self.workdir.run_with_envs(command, self.envs.clone())?;
        let log = self.save_log(&output)?;

        let mut outputs = HashMap::new();
        for (filename, path) in self.workdir.scan_files("out")? {
            let key = format!("out/{}", filename);
            outputs.insert(key, self.context.write_output(path)?);
        }

        if exit_code != 0 {
            eprintln!(
                "warn: job dir retained: {}",
                self.workdir.retain().display()
            );
        }

        Ok(Production {
            job: *job_id,
            exit_code,
            outputs,
            log,
            invocation: None,
            cache: None,
            start_ts: Some(self.start_ts),
            end_ts: Some(self.context.local_now_with_offset()),
            // populated by run-plan
            dependencies: HashMap::new(),
            source: None,
        })
    }

    fn save_log(&mut self, log: &[u8]) -> Result<Option<Id<Resource>>> {
        if log.is_empty() {
            Ok(None)
        } else {
            // Consider how to save logs with storage system.
            // Write stdout to file first?
            Ok(Some(self.context.store().write_resource(log)?))
        }
    }
}

impl ResourceAccessor for Job {
    fn read(&self, path: &str) -> Result<Id<Resource>> {
        self.inputs
            .get(&format!("in/files/{}", path))
            .cloned()
            .ok_or_else(|| anyhow!(format!("missing input {}", path)))
    }

    fn for_each_file_suffix<F>(&self, root: &str, mut f: F) -> Result<()>
    where
        F: FnMut(&str, &Id<Resource>) -> Result<()>,
    {
        for (path, resource_id) in self.inputs.iter() {
            if let Some(suffix) = path.strip_prefix(&format!("in/files/{}", root)) {
                f(suffix, resource_id)?;
            }
        }
        Ok(())
    }
}

fn try_run_dynamic<'a, Ctx: Context<'a>>(context: &'a Ctx, job: &Job) -> Result<Invocation> {
    let plan_id = job
        .inputs
        .get("in/plan")
        .ok_or_else(|| anyhow!("missing plan"))?;
    let resource = context.store().read_resource(&plan_id)?;
    let mut text_plan = TextPlan::from_reader(resource)?;
    for step in text_plan.steps.iter_mut() {
        if step.source.is_none() {
            step.source = Some(format!("nested:_pos:{}", step.pos));
        }
    }

    let plan = text_plan.encode(job, context.store())?;

    // main is hardcoded as the nested flow terminal pos.
    // TODO failed check should still return Ok
    plan.check_terminal("main")?;

    run_plan(context, plan, plan_id)
}

fn run_dynamic<'a, Ctx: Context<'a>>(
    context: &'a Ctx,
    job_id: &Id<Job>,
    job: &Job,
) -> Result<Production> {
    let start_ts = Some(context.local_now_with_offset());

    // Messy error handling. Internal errors should not fail the job.
    let (exit_code, invocation_id, outputs) = match try_run_dynamic(context, job) {
        Ok(invocation) => {
            let exit_code = match invocation.status {
                InvocationStatus::Ok => 0,
                InvocationStatus::Fail => 1,
            };

            let invocation_id = context.store().write(&invocation)?;

            let outputs = match invocation.production {
                Some(id) => context.store().read(&id)?.outputs,
                None => HashMap::new(),
            };

            (exit_code, Some(invocation_id), outputs)
        }
        // TODO propagate error better
        Err(e) => {
            eprintln!("{}", e);
            (1, None, HashMap::new())
        }
    };

    let production = Production {
        job: *job_id,
        exit_code,
        outputs,
        log: None,
        invocation: invocation_id,
        cache: None,
        start_ts,
        end_ts: Some(context.local_now_with_offset()),
        // populated by run-plan
        dependencies: HashMap::new(),
        source: None,
    };
    Ok(production)
}

pub(crate) fn run_job<'a, Ctx: Context<'a>>(
    context: &'a Ctx,
    job_id: &Id<Job>,
) -> Result<Production> {
    let job = context.store().read(job_id)?;

    match &job.process {
        Process::Identity => {
            let mut outputs = HashMap::new();
            for (key, value) in job.inputs {
                ensure!(key.starts_with("in/"), "expected in/");
                let key = "out/".to_owned() + &key[3..];
                outputs.insert(key, value);
            }
            Ok(Production {
                job: *job_id,
                exit_code: 0,
                outputs,
                // defaults
                dependencies: Default::default(),
                log: None,
                invocation: None,
                cache: None,
                source: None,
                start_ts: None,
                end_ts: None,
            })
        }
        Process::Command(command) => {
            let runtime = JobRunner::from_job(context, &job)?;
            let production = runtime.run_command(job_id, command)?;
            Ok(production)
        }
        Process::Nested(_) => unreachable!(), // translated into dynamic in run_plan
        Process::Dynamic => run_dynamic(context, job_id, &job),
    }
}

struct Scheduler {
    plan: Plan,
}

impl Scheduler {
    /// Find next steps to run in the execution plan.
    fn schedule_step(&mut self) -> Option<(String, &Step)> {
        let mut schedulable = Vec::new();
        for (pos, step) in self.plan.steps.iter() {
            if step.production.is_some() {
                continue;
            }
            let mut unresolved = false;
            for input in step.inputs.values() {
                if let Input::Pos(_, _) = input {
                    unresolved = true;
                    break;
                }
            }
            if !unresolved {
                schedulable.push(pos);
            }
        }

        match schedulable.first() {
            Some(&pos) => Some((pos.clone(), &self.plan.steps[pos])),
            None => None,
        }
    }

    fn complete_step(
        &mut self,
        completed_pos: &str,
        production: Production,
        production_id: &Id<Production>,
    ) -> Result<()> {
        let step = self.plan.steps.get_mut(completed_pos).unwrap();
        step.exit_code = Some(production.exit_code);
        step.production = Some(*production_id);
        if production.exit_code != 0 {
            return Ok(());
        }

        for step in self.plan.steps.values_mut() {
            let mut mapped_inputs = HashMap::new();
            for (inpath, input) in step.inputs.iter() {
                if let Input::Pos(pos, ref outpath) = input {
                    if pos == completed_pos {
                        step.dependencies
                            .insert(format!("_dep:{}", inpath), *production_id);
                        if inpath.ends_with('/') && outpath.ends_with('/') {
                            // TODO handle missing outputs
                            for (outfull, &output) in production.outputs.iter() {
                                if let Some(suffix) = outfull.strip_prefix(outpath) {
                                    let infull = inpath.to_string() + suffix;
                                    mapped_inputs.insert(infull, Input::Id(output));
                                }
                            }
                        } else {
                            match production.outputs.get(outpath) {
                                Some(output) => {
                                    mapped_inputs.insert(inpath.clone(), Input::Id(*output));
                                }
                                None => bail!(
                                    "step {} expects missing output {} from step {}",
                                    step.source
                                        .as_ref()
                                        .or_else(|| step.pos.as_ref())
                                        .map_or("<unknown>", String::as_str),
                                    outpath,
                                    pos,
                                ),
                            }
                        }
                        continue;
                    }
                }
                mapped_inputs.insert(inpath.clone(), input.clone());
            }
            step.inputs = mapped_inputs;
        }
        Ok(())
    }

    /// Get the production roots of an execution plan. These are the roots of
    /// all executed productions. They are also the steps closest to the plan
    /// frontier. For a successfully completed plan it should be a single
    /// terminal. Note unlike the shell implementation, this uses the plan graph
    /// not the production graph (should be equivalent except for source).
    fn reduce_productions(self) -> (Plan, Vec<Id<Production>>) {
        let mut roots: Vec<_> = self
            .plan
            .steps
            .iter()
            .flat_map(|(_, step)| step.production)
            .collect();
        for (_, step) in self.plan.steps.iter() {
            for (_, id) in step.dependencies.iter() {
                if let Some(pos) = roots.iter().position(|x| x == id) {
                    roots.remove(pos);
                }
            }
        }
        (self.plan, roots)
    }
}

struct StepRunner<'a, 'b, Ctx: Context<'a>> {
    context: &'a Ctx,
    step: &'b Step,
}

impl<'a, 'b, Ctx: Context<'a>> StepRunner<'a, 'b, Ctx> {
    fn to_job(&self) -> Result<Id<Job>> {
        let mut inputs = HashMap::new();
        for (path, input) in self.step.inputs.iter() {
            match input {
                Input::Id(id) => {
                    inputs.insert(path.clone(), *id);
                }
                _ => panic!("unresolved input"),
            }
        }

        let job = Job {
            process: self.step.process.clone(),
            inputs,
        };

        self.context.store().write(&job)
    }

    fn run_job(&mut self, job_id: &Id<Job>) -> Result<(Id<Production>, Production)> {
        eprintln!(
            "Running job:{} {}",
            &job_id,
            self.step.source.as_deref().unwrap_or("")
        );

        let mut production = run_job(self.context, job_id)?;
        production.source = self.step.source.clone();
        for (path, &dependency) in self.step.dependencies.iter() {
            production
                .dependencies
                .insert(path[1..].to_string(), dependency); // remove leading _
        }

        let production_id = self.context.store().write(&production)?;
        self.context.write_job_cache(job_id, &production_id)?;

        Ok((production_id, production))
    }
}

fn hashmap_equals<K: Eq + Hash, V: PartialEq>(a: &HashMap<K, V>, b: &HashMap<K, V>) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for (key, value) in a.iter() {
        if b.get(key) != Some(value) {
            return false;
        }
    }
    true
}

// TODO allow manual jobs
pub(crate) fn run_plan<'a, Ctx: Context<'a>>(
    context: &'a Ctx,
    mut plan: Plan,
    // confusingly, this is the TextPlan id
    plan_id: &Id<Resource>,
) -> Result<Invocation> {
    plan.steps = plan
        .steps
        .into_iter()
        .flat_map(|(pos, mut step)| {
            if let Process::Nested(command) = step.process {
                let command_pos = format!("{}@plan", pos);
                let dynamic_pos = step.pos.replace(pos.clone());
                let dynamic_source = step.source;
                step.source = dynamic_source
                    .as_ref()
                    .map(|s| format!("{}@plan", s))
                    .or_else(|| Some("plan".into()));
                step.process = Process::Command(command);
                let mut dynamic_inputs = HashMap::new();
                dynamic_inputs.insert("in/".into(), Input::Pos(command_pos.clone(), "out/".into()));
                let dynamic_step = Step {
                    pos: dynamic_pos,
                    process: Process::Dynamic,
                    exit_code: None,
                    production: None,
                    source: dynamic_source,
                    inputs: dynamic_inputs,
                    dependencies: HashMap::new(),
                };
                // Rust doesn't allow us to return array literals here. Maybe it
                // wouldn't work anyway because the arrays are different sizes?
                // https://github.com/rust-lang/rust/pull/65819
                // https://stackoverflow.com/q/59115305/13773246
                vec![(command_pos, step), (pos, dynamic_step)]
            } else {
                vec![(pos, step)]
            }
        })
        .collect();

    let mut scheduler = Scheduler { plan };
    while let Some((pos, step)) = scheduler.schedule_step() {
        let mut runner = StepRunner { context, step };
        let job_id = runner.to_job()?;

        let (production_id, production) = match context.read_job_cache(&job_id)? {
            Some(mut production_id) => {
                // Cache hit handling is quite messy.
                let mut production = context.store().read(&production_id)?;
                // Compute expected production dependencies.
                let mut dependencies = HashMap::new();
                for (path, &dependency) in step.dependencies.iter() {
                    dependencies.insert(path[1..].to_string(), dependency);
                    // remove leading _
                }
                // If dependencies differ from cached production, synthesize a new one.
                if !hashmap_equals(&production.dependencies, &dependencies) {
                    production.dependencies = dependencies;
                    production.source = step.source.clone();
                    production.cache = Some(production_id);
                    production_id = context.store().write(&production)?;
                }
                (production_id, production)
            }
            None => runner.run_job(&job_id)?,
        };

        if production.exit_code != 0 {
            eprintln!(
                "Failed job:{} {} with exit code {}",
                job_id,
                step.source.as_deref().unwrap_or(""),
                production.exit_code
            );
        }

        // Failing to complete a step leaves unresolved steps that will fail the
        // overall invocation.
        if let Err(error) = scheduler.complete_step(&pos, production, &production_id) {
            eprintln!("warn: {}", error);
        }
    }

    let (plan, productions) = scheduler.reduce_productions();

    if productions.len() == 1 {
        for step in plan.steps.values() {
            if step.production == Some(productions[0]) && step.exit_code == Some(0) {
                return Ok(Invocation {
                    production: Some(productions[0]),
                    partial_productions: HashMap::new(),
                    status: InvocationStatus::Ok,
                    plan: *plan_id,
                });
            }
        }
    }
    Ok(Invocation {
        production: None,
        partial_productions: productions
            .into_iter()
            .enumerate()
            .map(|(i, id)| (format!("partial_production:{}", i), id))
            .collect(),
        status: InvocationStatus::Fail,
        plan: *plan_id,
    })
}
