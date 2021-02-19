use std::collections::HashMap;
use std::ffi::OsStr;
use std::hash::Hash;
use std::io::{self, prelude::*};

use chrono::prelude::*;
use stable_eyre::eyre::{anyhow, bail, ensure, Result};

use crate::compat::attributes::{self, Attributes};
use crate::compat::context::{Context, WorkDir};
use crate::object::*;

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

    fn produce(mut self, job_id: &Id<Job>, process: &Process) -> Result<Production> {
        let result = match process {
            Process::Identity => unreachable!(), // early return from run_job
            Process::Command(command) => self.run_command(command),
            Process::Nested(command) => self.run_nested(command),
            Process::Dynamic => unreachable!(), // early return from run_job
        };

        match result {
            Ok((exit_code, outputs, log, invocation)) => {
                Ok(Production {
                    job: *job_id,
                    exit_code,
                    outputs,
                    log,
                    invocation,
                    cache: None,
                    start_ts: Some(self.start_ts),
                    end_ts: Some(self.context.local_now_with_offset()),
                    // populated by run-plan
                    dependencies: HashMap::new(),
                    source: None,
                })
            }
            Err(e) => {
                eprintln!(
                    "warn: job dir retained: {}",
                    self.workdir.retain().display()
                );
                Err(e)
            }
        }
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

    fn run_command(
        &mut self,
        command: impl AsRef<OsStr>,
    ) -> Result<(
        i32,
        HashMap<String, Id<Resource>>,
        Option<Id<Resource>>,
        Option<Id<Invocation>>,
    )> {
        self.workdir.create_dir("out")?;
        let (exit_code, output) = self.workdir.run_with_envs(command, self.envs.clone())?;
        let log = self.save_log(&output)?;

        let mut outputs = HashMap::new();
        for (filename, path) in self.workdir.scan_files("out")? {
            let key = format!("out/{}", filename);
            outputs.insert(key, self.context.write_output(path)?);
        }
        Ok((exit_code, outputs, log, None))
    }

    // TODO replace with dynamic
    fn run_nested(
        &mut self,
        command: impl AsRef<OsStr>,
    ) -> Result<(
        i32,
        HashMap<String, Id<Resource>>,
        Option<Id<Resource>>,
        Option<Id<Invocation>>,
    )> {
        // Nested flows don't really need parameterization because the
        // command can wire inputs as parameters. But it's natural to
        // consider if params should be exposed in some consistent way here.
        self.workdir.create_dir("steps")?;
        let (exit_code, output) = self.workdir.run_with_envs(command, self.envs.clone())?;
        let log = self.save_log(&output)?;
        if exit_code != 0 {
            return Ok((exit_code, HashMap::new(), log, None));
        }

        let plan = self.plan_nested_steps()?;
        // all is hardcoded as the nested flow terminal pos.
        // TODO failed check should still return Ok
        plan.check_terminal("all")?;

        let invocation = run_plan(self.context, plan)?;
        let exit_code = match invocation.status {
            InvocationStatus::Ok => 0,
            // This is ambiguous with a failure in the nested process.
            InvocationStatus::Fail => 1,
        };

        let invocation_id = self.context.store().write(&invocation)?;

        let outputs = match invocation.production {
            Some(id) => self.context.store().read(&id)?.outputs,
            None => HashMap::new(),
        };

        Ok((exit_code, outputs, log, Some(invocation_id)))
    }

    fn plan_nested_steps(&mut self) -> Result<Plan> {
        let mut plan = Plan {
            steps: HashMap::new(),
        };
        for (filename, path) in self.workdir.scan_files("steps")? {
            let mut step = self.context.read_nested_step(path)?;
            let nested_source = format!("nested:_pos:{}", &filename);
            step.source = match step.source {
                Some(orig_source) => Some(format!("{}@{}", orig_source, nested_source)),
                None => Some(nested_source),
            };
            step.pos = Some(filename.clone());

            // Kludge for relative file: resources for nested flows.
            for input in step.inputs.values_mut() {
                if let Input::File(file) = input {
                    if let Some(suffix) = file.strip_prefix("./") {
                        // Doesn't properly handle edge cases.
                        *file = self
                            .workdir
                            .path()
                            .join(suffix)
                            .to_string_lossy()
                            .to_string();
                    }
                    *input = Input::Id(self.context.resolve_input(file)?);
                }
            }

            plan.steps.insert(filename, step);
        }
        Ok(plan)
    }
}

fn run_dynamic<'a, Ctx: Context<'a>>(
    context: &'a Ctx,
    job_id: &Id<Job>,
    job: &Job,
) -> Result<Production> {
    let start_ts = Some(context.local_now_with_offset());

    let mut steps = HashMap::new();
    for (path, resource_id) in job.inputs.iter() {
        if let Some(filename) = path.strip_prefix("in/steps/") {
            let mut resource = context.store().read_resource(resource_id)?;
            let mut step = Step::from_reader(&mut resource)?;
            let nested_source = format!("nested:_pos:{}", filename);
            step.source = match step.source {
                Some(orig_source) => Some(format!("{}@{}", orig_source, nested_source)),
                None => Some(nested_source),
            };
            step.pos = Some(filename.to_string());
            for (_, input) in step.inputs.iter_mut() {
                // Map file to corresponding input (output of previous step).
                if let Input::File(path) = input {
                    let suffix = path
                        .strip_prefix("out/")
                        .ok_or_else(|| anyhow!("file must be output"))?;
                    let resource = job
                        .inputs
                        .get(&format!("in/{}", suffix))
                        .ok_or_else(|| anyhow!("missing input"))?;
                    *input = Input::Id(*resource);
                }
            }
            steps.insert(filename.to_string(), step);
        }
    }

    let plan = Plan { steps };

    // all is hardcoded as the nested flow terminal pos.
    // TODO failed check should still return Ok
    plan.check_terminal("all")?;

    let invocation = run_plan(context, plan)?;
    let exit_code = match invocation.status {
        InvocationStatus::Ok => 0,
        InvocationStatus::Fail => 1,
    };

    let invocation_id = context.store().write(&invocation)?;

    let outputs = match invocation.production {
        Some(id) => context.store().read(&id)?.outputs,
        None => HashMap::new(),
    };

    let production = Production {
        job: *job_id,
        exit_code,
        outputs,
        log: None,
        invocation: Some(invocation_id),
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

    if let Process::Identity = job.process {
        let mut outputs = HashMap::new();
        for (key, value) in job.inputs {
            ensure!(key.starts_with("in/"), "expected in/");
            let key = "out/".to_owned() + &key[3..];
            outputs.insert(key, value);
        }
        return Ok(Production {
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
        });
    }

    if let Process::Dynamic = job.process {
        return run_dynamic(context, job_id, &job);
    }

    let runtime = JobRunner::from_job(context, &job)?;
    let production = runtime.produce(job_id, &job.process)?;

    Ok(production)
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
    ) {
        let step = self.plan.steps.get_mut(completed_pos).unwrap();
        step.exit_code = Some(production.exit_code);
        step.production = Some(*production_id);
        if production.exit_code != 0 {
            return;
        }

        for step in self.plan.steps.values_mut() {
            let mut mapped_inputs = HashMap::new();
            for (inpath, input) in step.inputs.iter() {
                if let Input::Pos(pos, ref outpath) = input {
                    if pos == completed_pos {
                        step.dependencies
                            .insert(format!("_dep:{}", inpath), *production_id);
                        // TODO handle missing outputs
                        // TODO would make more sense to evaluate (and fail) dependencies from depender
                        if inpath.ends_with('/') && outpath.ends_with('/') {
                            for (outfull, &output) in production.outputs.iter() {
                                if let Some(suffix) = outfull.strip_prefix(outpath) {
                                    let infull = inpath.to_string() + suffix;
                                    mapped_inputs.insert(infull, Input::Id(output));
                                }
                            }
                        } else {
                            mapped_inputs
                                .insert(inpath.clone(), Input::Id(production.outputs[outpath]));
                        }
                        continue;
                    }
                }
                mapped_inputs.insert(inpath.clone(), input.clone());
            }
            step.inputs = mapped_inputs;
        }
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
                Input::Value(value) => {
                    // TODO when this is changed, cache still hits
                    let terminated = format!("{}\n", value);
                    let id = self.context.store().write_resource(terminated.as_bytes())?;
                    inputs.insert(path.clone(), id);
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
pub(crate) fn run_plan<'a, Ctx: Context<'a>>(context: &'a Ctx, plan: Plan) -> Result<Invocation> {
    let initial_plan = context.store().write(&plan)?;

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

        scheduler.complete_step(&pos, production, &production_id);
    }

    let (plan, productions) = scheduler.reduce_productions();
    let annotated_plan = context.store().write(&plan)?;

    if productions.len() == 1 {
        for step in plan.steps.values() {
            if step.production == Some(productions[0]) && step.exit_code == Some(0) {
                return Ok(Invocation {
                    production: Some(productions[0]),
                    partial_productions: HashMap::new(),
                    status: InvocationStatus::Ok,
                    plan: initial_plan,
                    annotated_plan,
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
        plan: initial_plan,
        annotated_plan,
    })
}

pub(crate) fn run_unit<'a, Ctx: Context<'a>>(
    context: &'a Ctx,
    unit: &str,
    params: &[String],
) -> Result<Invocation> {
    // TODO fold in with run-portable
    let mut plan = Plan::from_unit_file(context.store(), unit, "main")?;

    // Avoid populating an orphan param step.
    let has_param_dependency = plan
        .steps
        .values()
        .flat_map(|step| step.inputs.values())
        .any(|input| matches!(input, Input::Pos(pos, _) if pos == "_param"));
    if has_param_dependency {
        plan.steps.extend(context.params_to_steps(params)?);
    } else if !params.is_empty() {
        eprintln!("warn: parameters specified for non-parameterized flow");
    }

    run_plan(context, plan)
}
