use std::ffi::OsStr;
use std::fs::File;
use std::io::prelude::*;
use std::process::{Command, Stdio};

use stable_eyre::eyre::{ensure, Result};

use crate::object::*;

// TODO doesn't work
#[macro_export]
macro_rules! command {
    ($e:expr) => (command!($e,));
    ($e:expr, $($es:expr),*) => {{
        let mut cmd = {
            let mut cmd = ::std::process::Command::new($e);
            command!(expand cmd; $($es),*);
            cmd
        };
        cmd.status()?
    }};
    (expand $c:expr;,) => ($c);
    (expand $c:expr; $e:expr, $($es:expr),*) => {
        command!(expand $c.arg($e); $($es),*,)
    }
}

pub fn check_plan(plan_path: impl AsRef<OsStr>, terminal: impl AsRef<OsStr>) -> Result<()> {
    let check = Command::new("./check-plan")
        .arg(plan_path)
        .arg(terminal)
        .status()?;
    ensure!(check.success(), "invalid plan");
    Ok(())
}

pub fn unit_to_plan(unit: impl AsRef<OsStr>, root_pos: impl AsRef<OsStr>) -> Result<Plan> {
    let output = Command::new("./unit-to-plan")
        .arg(root_pos)
        .arg(unit)
        .stderr(Stdio::inherit())
        .output()?;
    ensure!(output.status.success(), "unit-to-plan failed");
    let plan = Plan::from_reader(&mut output.stdout.as_slice())?;
    Ok(plan)
}

pub fn param_to_plan(params: &Vec<String>) -> Result<Plan> {
    let unit = "gen/param.unit";
    {
        let mut file = File::create(unit)?;
        file.write_all(b"process=identity\n")?;
        for param in params.iter() {
            file.write_all(param.as_bytes())?;
            file.write_all(b"\n")?;
        }
    }
    unit_to_plan(unit, "_param")
}

pub fn write_job_cache(job_id: Id, production_id: Id) -> Result<()> {
    let status = Command::new("./kgit")
        .arg("update-ref")
        .arg(format!("refs/job/{}/lastproduction", job_id))
        .arg(production_id.to_string())
        .status()?;
    ensure!(status.success(), "update-ref failed");
    Ok(())
}

pub fn read_job_cache(job_id: Id) -> Result<Option<Id>> {
    // TODO more nuanced caching policy
    let output = Command::new("./kgit")
        .arg("rev-parse")
        .arg("--verify")
        .arg("-q")
        .arg(format!("refs/job/{}/lastproduction", job_id))
        .output()?;
    if output.status.success() {
        Ok(Some(
            std::str::from_utf8(&output.stdout)?.trim_end().parse()?,
        ))
    } else {
        Ok(None)
    }
}
