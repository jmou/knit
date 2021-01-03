use std::io::prelude::*;
use std::process::{Command, Stdio};
use std::str;

use stable_eyre::eyre::{anyhow, Result};

use crate::object::Id;

// TODO not a great interface
pub trait Store {
    fn write(&mut self, objtype: &str, data: &[u8]) -> Result<Id>;
    fn read(&self, objtype: &str, oid: Id) -> Result<Box<dyn Read>>;
}

pub struct GitStore {}

impl Store for GitStore {
    fn write(&mut self, _objtype: &str, data: &[u8]) -> Result<Id> {
        let mut child = Command::new("./kgit")
            .arg("hash-object")
            .arg("-w")
            .arg("--stdin")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()?;
        child.stdin.as_mut().expect("piped stdin").write_all(data)?;
        let stdout = child.wait_with_output()?.stdout;
        if let Some(b'\n') = stdout.last() {
            Ok(str::from_utf8(&stdout[0..stdout.len() - 1])?.parse()?)
        } else {
            Err(anyhow!("missing newline"))
        }
    }

    fn read(&self, _objtype: &str, oid: Id) -> Result<Box<dyn Read>> {
        let child = Command::new("./kgit")
            .arg("cat-file")
            .arg("blob")
            .arg(oid.hex())
            .stdout(Stdio::piped())
            .spawn()?;
        // It seems that dropping child doesn't kill the process.
        Ok(Box::new(child.stdout.expect("piped stdout")))
    }
}
