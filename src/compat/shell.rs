use std::process::Command;

use stable_eyre::eyre::{ensure, Result};

use crate::cas::{Id, UntypedId};
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

pub fn write_job_cache(job_id: &Id<Job>, production_id: &Id<Production>) -> Result<()> {
    let status = Command::new("./kgit")
        .arg("update-ref")
        .arg(format!("refs/job/{}/lastproduction", job_id))
        .arg(production_id.to_string())
        .status()?;
    ensure!(status.success(), "update-ref failed");
    Ok(())
}

pub fn read_job_cache(job_id: &Id<Job>) -> Result<Option<Id<Production>>> {
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
