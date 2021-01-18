use std::collections::HashMap;
use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};

use chrono::prelude::*;
use stable_eyre::eyre::{bail, ensure, Result};
use tempfile::TempDir;
use walkdir::WalkDir;

use crate::cas;
use crate::compat::attributes::{self, Attributes};
use crate::compat::shell;
use crate::object::*;

#[cfg(unix)]
fn exit_status_to_code(exit_status: ExitStatus) -> i32 {
    use std::os::unix::process::ExitStatusExt;
    if let Some(code) = exit_status.code() {
        code
    } else if let Some(signal) = exit_status.signal() {
        -signal
    } else {
        unreachable!("child process must be exited or signaled")
    }
}

#[cfg(not(unix))]
fn exit_status_to_code(exit_status: ExitStatus) -> i32 {
    if let Some(code) = exit_status.code() {
        code
    } else {
        unreachable!("child process must be exited")
    }
}

// https://github.com/chronotope/chrono/issues/104
fn local_now_with_offset() -> DateTime<FixedOffset> {
    let now = Local::now();
    now.with_timezone(now.offset())
}

struct WorkDir {
    dir: TempDir,
}

impl WorkDir {
    fn new() -> Result<Self> {
        let dir = tempfile::Builder::new().prefix("job-").tempdir_in("gen")?;
        Ok(WorkDir { dir })
    }

    fn path(&self) -> &Path {
        self.dir.path()
    }

    fn create(&self, relpath: impl AsRef<Path>) -> Result<File> {
        ensure!(
            relpath.as_ref().is_relative(),
            "path not relative to workdir"
        );
        if let Some(parent) = relpath.as_ref().parent() {
            fs::create_dir_all(self.dir.path().join(parent))?;
        }
        Ok(File::create(self.dir.path().join(relpath))?)
    }

    fn create_dir(&self, relpath: impl AsRef<Path>) -> Result<()> {
        ensure!(
            relpath.as_ref().is_relative(),
            "path not relative to workdir"
        );
        fs::create_dir(self.dir.path().join(relpath))?;
        Ok(())
    }

    fn is_dir(&self, relpath: impl AsRef<Path>) -> bool {
        relpath.as_ref().is_relative() && self.dir.path().join(relpath).is_dir()
    }

    fn retain(self) -> PathBuf {
        self.dir.into_path()
    }

    fn scan_files(
        &self,
        relpath: impl AsRef<Path>,
    ) -> Result<impl IntoIterator<Item = (String, PathBuf)>> {
        assert!(relpath.as_ref().is_relative());
        let mut files = Vec::new();
        let root = self.dir.path().join(relpath);
        for entry in WalkDir::new(&root) {
            let entry = entry?;
            if !entry.file_type().is_file() {
                continue;
            }
            let stripped = entry.path().strip_prefix(&root)?;
            if let Some(key) = stripped.to_str() {
                files.push((key.to_owned(), entry.path().to_owned()));
            }
        }
        Ok(files)
    }

    pub fn run_with_envs<S, E, K, V>(&self, command: S, envs: E) -> Result<(i32, Vec<u8>)>
    where
        S: AsRef<OsStr>,
        E: IntoIterator<Item = (K, V)>,
        K: AsRef<OsStr>,
        V: AsRef<OsStr>,
    {
        let output = Command::new("/bin/bash")
            .arg("-c")
            .arg(command)
            .current_dir(self.dir.path())
            .envs(envs)
            .stderr(Stdio::inherit())
            .output()?;
        let exit_code = exit_status_to_code(output.status);
        Ok((exit_code, output.stdout))
    }
}

struct JobRunner<'a> {
    store: &'a mut dyn cas::Store,
    workdir: WorkDir,
    envs: Attributes,
    start_ts: DateTime<FixedOffset>,
}

impl<'a> JobRunner<'a> {
    fn from_job(store: &'a mut dyn cas::Store, job: &Job) -> Result<Self> {
        let workdir = WorkDir::new()?;
        let mut job_buf = Vec::new();
        attributes::to_writer(&mut job_buf, job)?;
        io::copy(&mut job_buf.as_slice(), &mut workdir.create("job")?)?;

        for (input_path, &input_id) in job.inputs.iter() {
            let mut input_file = workdir.create(input_path)?;
            if input_path.starts_with("in/") {
                let mut input_read = store.read("input", input_id)?;
                io::copy(&mut input_read, &mut input_file)?;
            } else if input_path.starts_with("inref/") {
                writeln!(input_file, "{}", input_id)?;
            } else {
                bail!("invalid input path");
            }
        }

        let envs = Attributes::from_reader(&mut job_buf.as_slice())?;
        let start_ts = local_now_with_offset();
        Ok(JobRunner {
            store,
            workdir,
            envs,
            start_ts,
        })
    }

    fn produce(mut self, job_id: Id, process: &Process) -> Result<Production> {
        let result = match process {
            Process::Identity => unreachable!(), // early return from run_job
            Process::Command(command) => self.run_command(command),
            Process::Nested(command) => self.run_nested(command),
        };

        match result {
            Ok((exit_code, outputs, log, invocation)) => {
                Ok(Production {
                    job: job_id,
                    exit_code,
                    outputs,
                    log,
                    invocation,
                    start_ts: Some(self.start_ts),
                    end_ts: Some(local_now_with_offset()),
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

    fn save_log(&mut self, log: &[u8]) -> Result<Option<Id>> {
        if log.is_empty() {
            Ok(None)
        } else {
            // Consider how to save logs with storage system.
            // Write stdout to file first?
            Ok(Some(self.store.write("resource", log)?))
        }
    }

    fn run_command(
        &mut self,
        command: impl AsRef<OsStr>,
    ) -> Result<(i32, HashMap<String, Id>, Option<Id>, Option<Id>)> {
        if self.workdir.is_dir("inref") {
            eprintln!("warning: inref used in command process");
        }

        self.workdir.create_dir("out")?;
        let (exit_code, output) = self.workdir.run_with_envs(command, self.envs.clone())?;
        let log = self.save_log(&output)?;

        let mut outputs = HashMap::new();
        for (filename, path) in self.workdir.scan_files("out")? {
            let key = format!("out/{}", filename);
            let output_id = self.store.write("resource", &fs::read(path)?)?;
            outputs.insert(key, output_id);
        }
        Ok((exit_code, outputs, log, None))
    }

    fn run_nested(
        &mut self,
        command: impl AsRef<OsStr>,
    ) -> Result<(i32, HashMap<String, Id>, Option<Id>, Option<Id>)> {
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
        plan.to_writer(&mut self.workdir.create("plan")?)?;
        // all is hardcoded as the nested flow terminal pos.
        // TODO failed check should still return Ok
        shell::check_plan(self.workdir.path().join("plan"), "all")?;

        let invocation = run_plan(self.store, plan)?;
        let exit_code = match invocation.status {
            InvocationStatus::Ok => 0,
            // This is ambiguous with a failure in the nested process.
            InvocationStatus::Fail => 1,
        };

        let mut invocation_buf = Vec::new();
        attributes::to_writer(&mut invocation_buf, &invocation)?;
        let invocation_id = self.store.write("invocation", &invocation_buf)?;

        let outputs = match invocation.production {
            Some(id) => {
                let mut reader = self.store.read("production", id)?;
                let production = Production::from_reader(&mut reader)?;
                production.outputs
            }
            None => HashMap::new(),
        };

        Ok((exit_code, outputs, log, Some(invocation_id)))
    }

    fn plan_nested_steps(&mut self) -> Result<Plan> {
        let mut plan = Plan {
            steps: HashMap::new(),
        };
        for (filename, path) in self.workdir.scan_files("steps")? {
            let mut step = Step::from_reader(&mut File::open(path)?)?;
            step.source = Some(format!("nested:_pos:{}", &filename));
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
                    // "resolve-input"
                    let mut file_buf = Vec::new();
                    File::open(file)?.read_to_end(&mut file_buf)?;
                    let id = self.store.write("resource", &file_buf)?;
                    *input = Input::Id(id);
                }
            }

            plan.steps.insert(filename, step);
        }
        Ok(plan)
    }
}

pub fn run_job(store: &mut dyn cas::Store, job_id: Id) -> Result<Production> {
    let mut job_buf = Vec::new();
    store.read("job", job_id)?.read_to_end(&mut job_buf)?;
    let job = Job::from_reader(&mut job_buf.as_slice())?;

    if let Process::Identity = job.process {
        let mut outputs = HashMap::new();
        for (key, value) in job.inputs {
            ensure!(key.starts_with("in/"), "expected in/");
            let key = "out/".to_owned() + &key[3..];
            outputs.insert(key, value);
        }
        return Ok(Production {
            job: job_id,
            exit_code: 0,
            outputs,
            // defaults
            dependencies: Default::default(),
            log: None,
            invocation: None,
            source: None,
            start_ts: None,
            end_ts: None,
        });
    }

    let runtime = JobRunner::from_job(store, &job)?;
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

    fn complete_step(&mut self, completed_pos: &str, production: Production, production_id: Id) {
        let step = self.plan.steps.get_mut(completed_pos).unwrap();
        step.exit_code = Some(production.exit_code);
        step.production = Some(production_id);
        if production.exit_code != 0 {
            return;
        }

        for step in self.plan.steps.values_mut() {
            let mut mapped_inputs = HashMap::new();
            for (inpath, input) in step.inputs.iter() {
                if let Input::Pos(pos, ref outpath) = input {
                    if pos == completed_pos {
                        step.dependencies
                            .insert(format!("_dep:{}", inpath), production_id);
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

    /// Get the production roots of an execution plan. These are the roots of all
    /// executed productions. They are also the steps closest to the plan frontier. For
    /// a successfully completed plan it should be a single terminal.
    // TODO why doesn't this exhibit the caching issue? are we not caching properly?
    fn reduce_productions(self) -> (Plan, Vec<Id>) {
        let mut roots: Vec<Id> = self
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

struct StepRunner<'a> {
    store: &'a mut dyn cas::Store,
    step: &'a Step,
}

impl<'a> StepRunner<'a> {
    fn to_job(&mut self) -> Result<Id> {
        let mut inputs = HashMap::new();
        for (path, input) in self.step.inputs.iter() {
            if let Input::Id(id) = input {
                inputs.insert(path.clone(), id.clone());
            } else {
                panic!("unresolved input");
            }
        }

        let job = Job {
            process: self.step.process.clone(),
            inputs,
        };

        let mut job_buf = Vec::new();
        attributes::to_writer(&mut job_buf, &job)?;
        let job_id = self.store.write("job", &job_buf)?;

        Ok(job_id)
    }

    fn run_job(&mut self, job_id: Id) -> Result<(Id, Production)> {
        eprintln!(
            "Running job:{} {}",
            &job_id,
            self.step.source.as_deref().unwrap_or("")
        );

        let mut production = run_job(self.store, job_id)?;
        production.source = self.step.source.clone();
        for (path, dependency) in self.step.dependencies.iter() {
            production
                .dependencies
                .insert(path[1..].to_string(), *dependency); // remove leading _
        }

        let mut production_buf = Vec::new();
        attributes::to_writer(&mut production_buf, &production)?;
        let production_id = self.store.write("production", &production_buf)?;
        shell::write_job_cache(job_id, production_id)?;

        Ok((production_id, production))
    }
}

// TODO allow manual jobs
pub fn run_plan(store: &mut dyn cas::Store, plan: Plan) -> Result<Invocation> {
    let mut plan_buf = Vec::new();
    plan.to_writer(&mut plan_buf)?;
    let initial_plan = store.write("plan", &plan_buf)?;

    let mut scheduler = Scheduler { plan };
    while let Some((pos, step)) = scheduler.schedule_step() {
        let mut runner = StepRunner { store, step };
        let job_id = runner.to_job()?;

        let (production_id, production) = match shell::read_job_cache(job_id)? {
            Some(production_id) => {
                let mut production_reader = store.read("production", production_id)?;
                let production = Production::from_reader(&mut production_reader)?;
                (production_id, production)
            }
            None => runner.run_job(job_id)?,
        };

        if production.exit_code != 0 {
            eprintln!(
                "Failed job:{} {} with exit code {}",
                job_id,
                step.source.as_deref().unwrap_or(""),
                production.exit_code
            );
        }

        scheduler.complete_step(&pos, production, production_id);
    }

    let (plan, productions) = scheduler.reduce_productions();
    let mut plan_buf = Vec::new();
    plan.to_writer(&mut plan_buf)?;
    let annotated_plan = store.write("plan", &plan_buf)?;

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
