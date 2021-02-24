//! Temporary refactoring of execution context into trait.

use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::prelude::*;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};

use chrono::prelude::*;
use stable_eyre::eyre::{ensure, Result};
use tempfile::TempDir;
use walkdir::WalkDir;

use crate::cas::{self, UntypedId};
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

// Maybe simplifies cas::Store and makes trait objects unnecessary?
pub(crate) trait Context<'a> {
    type WorkDir: WorkDir;
    // Violates Demeter's Law
    fn store(&self) -> &'a cas::Store;
    fn local_now_with_offset(&self) -> DateTime<FixedOffset>;
    fn write_job_cache(&self, job_id: &Id<Job>, production_id: &Id<Production>) -> Result<()>;
    fn read_job_cache(&self, job_id: &Id<Job>) -> Result<Option<Id<Production>>>;
    fn resolve_input(&self, path: impl AsRef<Path>) -> Result<Id<Resource>>;
    fn read_nested_step(&self, path: impl AsRef<Path>) -> Result<Step>;
    fn write_output(&self, path: impl AsRef<Path>) -> Result<Id<Resource>>;
    fn new_workdir(&self) -> Result<Self::WorkDir>;
}

pub(crate) trait WorkDir {
    fn path(&self) -> &Path;
    fn create(&self, relpath: impl AsRef<Path>) -> Result<File>;
    fn create_dir(&self, relpath: impl AsRef<Path>) -> Result<()>;
    fn is_dir(&self, relpath: impl AsRef<Path>) -> bool;
    fn retain(self) -> PathBuf;
    fn scan_files(&self, relpath: impl AsRef<Path>) -> Result<Vec<(String, PathBuf)>>;
    fn run_with_envs<S, E, K, V>(&self, command: S, envs: E) -> Result<(i32, Vec<u8>)>
    where
        S: AsRef<OsStr>,
        E: IntoIterator<Item = (K, V)>,
        K: AsRef<OsStr>,
        V: AsRef<OsStr>;
}

pub(crate) struct RealEnvironment<'a> {
    store: &'a cas::Store,
}

impl<'a> RealEnvironment<'a> {
    pub fn new(store: &'a cas::Store) -> Self {
        Self { store }
    }
}

impl<'a> Context<'a> for RealEnvironment<'a> {
    type WorkDir = RealWorkDir;

    fn store(&self) -> &'a cas::Store {
        self.store
    }

    // https://github.com/chronotope/chrono/issues/104
    fn local_now_with_offset(&self) -> DateTime<FixedOffset> {
        let now = Local::now();
        now.with_timezone(now.offset())
    }

    fn write_job_cache(&self, job_id: &Id<Job>, production_id: &Id<Production>) -> Result<()> {
        let status = Command::new("./kgit")
            .arg("update-ref")
            .arg(format!("refs/job/{}/lastproduction", job_id))
            .arg(production_id.to_string())
            .status()?;
        ensure!(status.success(), "update-ref failed");
        Ok(())
    }

    fn read_job_cache(&self, job_id: &Id<Job>) -> Result<Option<Id<Production>>> {
        // TODO more nuanced caching policy
        let output = Command::new("./kgit")
            .arg("rev-parse")
            .arg("--verify")
            .arg("-q")
            .arg(format!("refs/job/{}/lastproduction", job_id))
            .output()?;
        if output.status.success() {
            Ok(Some(
                std::str::from_utf8(&output.stdout)?
                    .trim_end()
                    .parse::<UntypedId>()
                    .map(Id::<Production>::new)?,
            ))
        } else {
            Ok(None)
        }
    }

    fn resolve_input(&self, path: impl AsRef<Path>) -> Result<Id<Resource>> {
        let mut file_buf = Vec::new();
        File::open(path)?.read_to_end(&mut file_buf)?;
        self.store.write_resource(&file_buf)
    }

    fn read_nested_step(&self, path: impl AsRef<Path>) -> Result<Step> {
        Step::from_reader(&mut File::open(path)?)
    }

    fn write_output(&self, path: impl AsRef<Path>) -> Result<Id<Resource>> {
        self.store.write_resource(&fs::read(path)?)
    }

    fn new_workdir(&self) -> Result<Self::WorkDir> {
        let dir = tempfile::Builder::new().prefix("job-").tempdir_in("gen")?;
        Ok(RealWorkDir { dir })
    }
}

pub(crate) struct RealWorkDir {
    dir: TempDir,
}

impl RealWorkDir {}

impl WorkDir for RealWorkDir {
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

    fn scan_files(&self, relpath: impl AsRef<Path>) -> Result<Vec<(String, PathBuf)>> {
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

    fn run_with_envs<S, E, K, V>(&self, command: S, envs: E) -> Result<(i32, Vec<u8>)>
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
