//! Temporary refactoring of execution context into trait.

use std::ffi::OsStr;
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};

use chrono::prelude::*;
use stable_eyre::eyre::{ensure, Context as _, Result};
use tempfile::TempDir;
use walkdir::WalkDir;

use crate::cas::{self, UntypedId};
use crate::object::*;
use crate::plan::ResourceAccessor;

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
    fn write_output(&self, path: impl AsRef<Path>) -> Result<Id<Resource>>;
    fn new_workdir(&self) -> Result<Self::WorkDir>;
}

pub(crate) trait WorkDir {
    fn create(&self, relpath: impl AsRef<Path>) -> Result<File>;
    fn create_dir(&self, relpath: impl AsRef<Path>) -> Result<()>;
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

    fn write_output(&self, path: impl AsRef<Path>) -> Result<Id<Resource>> {
        self.store.write_resource(&fs::read(path)?)
    }

    fn new_workdir(&self) -> Result<Self::WorkDir> {
        let dir = tempfile::Builder::new().prefix("job-").tempdir_in("gen")?;
        Ok(RealWorkDir { dir })
    }
}

pub struct DirectoryResourceAccessor<'a> {
    dir: PathBuf,
    store: &'a cas::Store,
}

impl<'a> DirectoryResourceAccessor<'a> {
    pub fn new(dir: impl Into<PathBuf>, store: &'a cas::Store) -> Self {
        Self {
            dir: dir.into(),
            store,
        }
    }
}

impl<'a> ResourceAccessor for DirectoryResourceAccessor<'a> {
    fn read(&self, path: &str) -> Result<Id<Resource>> {
        let path = self.dir.join(path);
        let data = fs::read(&path).with_context(|| format!("could not read {}", path.display()))?;
        let id = self.store.write_resource(&data)?;
        Ok(id)
    }

    fn for_each_file_suffix<F>(&self, root: &str, mut f: F) -> Result<()>
    where
        F: FnMut(&str, &Id<Resource>) -> Result<()>,
    {
        let fullroot = self.dir.join(root);
        for entry in WalkDir::new(&fullroot).follow_links(true) {
            let entry = entry?;
            if !entry.file_type().is_file() {
                continue;
            }
            let stripped = entry.path().strip_prefix(&fullroot)?;
            let suffix = stripped.to_str().expect("non-UTF-8 path");
            let resource_id = self.read(&format!("{}{}", root, suffix))?;
            f(suffix, &resource_id)?;
        }
        Ok(())
    }
}

pub(crate) struct RealWorkDir {
    dir: TempDir,
}

impl RealWorkDir {}

impl WorkDir for RealWorkDir {
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

    fn retain(self) -> PathBuf {
        self.dir.into_path()
    }

    fn scan_files(&self, relpath: impl AsRef<Path>) -> Result<Vec<(String, PathBuf)>> {
        assert!(relpath.as_ref().is_relative());
        let mut files = Vec::new();
        let root = self.dir.path().join(relpath);
        for entry in WalkDir::new(&root).follow_links(true) {
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
