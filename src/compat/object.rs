//! Best effort parsing from attribute format.

use std::collections::HashMap;
use std::io::prelude::*;
use std::str::FromStr;

use stable_eyre::eyre::{anyhow, bail, ensure, Error, Result};

use crate::attributes::{self, Attributes, StrExt};
use crate::cas::Storable;
use crate::object::*;

impl Storable for Invocation {
    fn objtype() -> &'static [u8] {
        b"invocation"
    }

    fn to_writer(&self, w: &mut dyn Write) -> Result<()> {
        Ok(w.write_all(&attributes::to_bytes(self))?)
    }

    fn from_reader(mut r: Box<dyn Read>) -> Result<Self> {
        let mut attrs = Attributes::from_reader(&mut r)?;
        let production = attrs.consume("production").ok();
        let plan = attrs.consume("plan")?;
        let status = match attrs.consume::<String>("status")?.as_str() {
            "ok" => InvocationStatus::Ok,
            "fail" => InvocationStatus::Fail,
            _ => bail!("invalid status"),
        };
        let mut partial_productions = HashMap::new();
        for (key, value) in attrs {
            ensure!(key.starts_with("partial_production:"), "unknown key");
            partial_productions.insert(key, value.parse()?);
        }
        Ok(Invocation {
            production,
            partial_productions,
            status,
            plan,
        })
    }
}

impl Storable for Production {
    fn objtype() -> &'static [u8] {
        b"production"
    }

    fn to_writer(&self, w: &mut dyn Write) -> Result<()> {
        Ok(w.write_all(&attributes::to_bytes(self))?)
    }

    fn from_reader(mut r: Box<dyn Read>) -> Result<Self> {
        let mut attrs = Attributes::from_reader(&mut r)?;
        let job = attrs.consume("job")?;
        // Does not distinguish between missing and other parse errors.
        let invocation = attrs.consume("invocation").ok();
        let cache = attrs.consume("cache").ok();
        let exit_code = attrs.consume("exit_code")?;
        let log = attrs.consume("log").ok();
        let source = attrs.consume("_source").ok();
        let start_ts = attrs.consume("start_ts").ok();
        let end_ts = attrs.consume("end_ts").ok();
        let mut outputs = HashMap::new();
        let mut dependencies = HashMap::new();
        for (key, value) in attrs {
            if key.starts_with("dep:") {
                dependencies.insert(key.clone(), value.parse()?);
            } else if key.starts_with("out/") {
                outputs.insert(key.clone(), value.parse()?);
            } else {
                bail!("unknown key: {}", &key);
            }
        }
        Ok(Production {
            job,
            invocation,
            cache,
            outputs,
            exit_code,
            log,
            dependencies,
            source,
            start_ts,
            end_ts,
        })
    }
}

impl Storable for Job {
    fn objtype() -> &'static [u8] {
        b"job"
    }

    fn to_writer(&self, w: &mut dyn Write) -> Result<()> {
        Ok(w.write_all(&attributes::to_bytes(self))?)
    }

    fn from_reader(mut r: Box<dyn Read>) -> Result<Self> {
        let mut attrs = Attributes::from_reader(&mut r)?;
        let process = attrs.consume::<String>("process")?.parse()?;
        let mut inputs = HashMap::new();
        for (key, value) in attrs {
            if key.starts_with("in/") || key.starts_with("inref/") {
                inputs.insert(key.clone(), value.parse()?);
            } else {
                bail!("unknown key");
            }
        }
        Ok(Job { process, inputs })
    }
}

impl FromStr for Input {
    type Err = Error;

    fn from_str(s: &str) -> std::result::Result<Self, Self::Err> {
        if let Some((prefix, suffix)) = s.split_once_ext(':') {
            match prefix {
                "_pos" => {
                    if let Some((pos, path)) = suffix.split_once_ext(':') {
                        Ok(Input::Pos(pos.into(), path.into()))
                    } else {
                        Err(anyhow!("expected : in _pos input"))
                    }
                }
                // This seems like the wrong way to do this
                "param" => Ok(Input::Pos("_param".into(), suffix.into())),
                _ => Err(anyhow!("unknown input type: {}", prefix)),
            }
        } else {
            Ok(Input::Id(s.parse()?))
        }
    }
}

impl FromStr for Process {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        if s == "identity" {
            return Ok(Process::Identity);
        } else if s == "dynamic" {
            return Ok(Process::Dynamic);
        }
        let process = if let Some((kind, suffix)) = s.split_once_ext(':') {
            match kind {
                "command" => Process::Command(suffix.into()),
                "nested" => Process::Nested(suffix.into()),
                _ => bail!("unsupported process"),
            }
        } else {
            bail!("malformed process");
        };
        Ok(process)
    }
}

#[cfg(test)]
mod tests {
    use crate::object::Id;

    use super::*;

    #[test]
    fn parse_invocation() {
        let raw = b"production=5a85adff7fc597bdb1c2efa56a3a7d758854ced5
plan=00478c2684ff7c617cf87fd103c89114342adddb
status=ok
";
        assert_eq!(
            Invocation::from_reader(Box::new(raw.as_ref())).unwrap(),
            Invocation {
                production: Some("5a85adff7fc597bdb1c2efa56a3a7d758854ced5".parse().unwrap()),
                partial_productions: HashMap::new(),
                status: InvocationStatus::Ok,
                plan: "00478c2684ff7c617cf87fd103c89114342adddb".parse().unwrap(),
            }
        );
    }

    #[test]
    fn parse_production() {
        let raw = b"_source=unit:flow/basic/tac.unit
dep:in/data=f16725e71499854fcda3059ac4a2611bfd3a5237
end_ts=2021-01-02T04:32:43-0800
exit_code=0
job=4233117e9199336269c23534c78a7088dc5e4893
out/_=2d6976f9b54866fa6afeb9080bfd843098f107bb
start_ts=2021-01-02T04:32:43-0800
";
        assert_eq!(
            Production::from_reader(Box::new(raw.as_ref())).unwrap(),
            Production {
                job: "4233117e9199336269c23534c78a7088dc5e4893".parse().unwrap(),
                exit_code: 0,
                outputs: [(
                    "out/_".into(),
                    "2d6976f9b54866fa6afeb9080bfd843098f107bb".parse().unwrap()
                )]
                .iter()
                .cloned()
                .collect(),
                dependencies: [(
                    "dep:in/data".into(),
                    "f16725e71499854fcda3059ac4a2611bfd3a5237"
                        .parse::<Id<Production>>()
                        .unwrap()
                )]
                .iter()
                .cloned()
                .collect(),
                log: None,
                invocation: None,
                cache: None,
                source: Some("unit:flow/basic/tac.unit".into()),
                start_ts: Some("2021-01-02T04:32:43-0800".parse().unwrap()),
                end_ts: Some("2021-01-02T04:32:43-0800".parse().unwrap()),
            }
        );
    }

    #[test]
    fn parse_job() {
        let raw = b"in/data=01e79c32a8c99c557f0757da7cb6d65b3414466d
process=command:perl -e 'print reverse <>' in/data > out/_
";
        assert_eq!(
            Job::from_reader(Box::new(raw.as_ref())).unwrap(),
            Job {
                process: Process::Command("perl -e 'print reverse <>' in/data > out/_".into()),
                inputs: [(
                    "in/data".into(),
                    "01e79c32a8c99c557f0757da7cb6d65b3414466d".parse().unwrap()
                )]
                .iter()
                .cloned()
                .collect(),
            }
        );
    }

    #[test]
    fn parse_input() {
        assert_eq!(
            "01e79c32a8c99c557f0757da7cb6d65b3414466d"
                .parse::<Input>()
                .unwrap(),
            Input::Id("01e79c32a8c99c557f0757da7cb6d65b3414466d".parse().unwrap())
        );
        assert_eq!(
            "_pos:main@0:out/_".parse::<Input>().unwrap(),
            Input::Pos("main@0".into(), "out/_".into())
        );
    }
}
