//! Git-based content address store.

use std::io::prelude::*;
use std::process::{Command, Stdio};
use std::str;

use stable_eyre::eyre::{anyhow, Result};

use crate::cas::{UntypedId, UntypedStore, Writable};

pub struct GitStore {}

impl UntypedStore for GitStore {
    fn write(&self, _objtype: &[u8], w: &dyn Writable) -> Result<UntypedId> {
        let mut child = Command::new("./kgit")
            .arg("hash-object")
            .arg("-w")
            .arg("--stdin")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()?;
        let stdin = child.stdin.as_mut().expect("piped stdin");
        w.to_writer(stdin)?;
        let stdout = child.wait_with_output()?.stdout;
        if let Some(b'\n') = stdout.last() {
            Ok(str::from_utf8(&stdout[0..stdout.len() - 1])?.parse()?)
        } else {
            Err(anyhow!("missing newline"))
        }
    }

    fn read(&self, _objtype: &[u8], id: &UntypedId) -> Result<Box<dyn Read>> {
        let child = Command::new("./kgit")
            .arg("cat-file")
            .arg("blob")
            .arg(id.hex())
            .stdout(Stdio::piped())
            .spawn()?;
        // It seems that dropping child doesn't kill the process.
        Ok(Box::new(child.stdout.expect("piped stdout")))
    }
}
