//! Best effort parsing from attribute format.

use std::collections::HashMap;
use std::io::prelude::*;
use std::str::FromStr;

use bstr::ByteSlice;
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
        let annotated_plan = attrs.consume("_annotated_plan")?;
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
            annotated_plan,
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
                "file" => Ok(Input::File(suffix.into())),
                "_pos" => {
                    if let Some((pos, path)) = suffix.split_once_ext(':') {
                        Ok(Input::Pos(pos.into(), path.into()))
                    } else {
                        Err(anyhow!("expected : in _pos input"))
                    }
                }
                "inline" => Ok(Input::Value(suffix.into())),
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
        }
        let process = if let Some((kind, suffix)) = s.split_once_ext(':') {
            match kind {
                "command" => Process::Command(suffix.into()),
                "nested" => Process::Nested(suffix.into()),
                "composite" => Process::Composite(suffix.into()),
                _ => bail!("unsupported process"),
            }
        } else {
            bail!("malformed process");
        };
        Ok(process)
    }
}

impl Step {
    pub fn from_reader(r: &mut impl Read) -> Result<Self> {
        let mut attrs = Attributes::from_reader(r)?;
        let pos = attrs.consume("_pos").ok();
        let process = attrs.consume::<String>("process")?.parse()?;
        let exit_code = attrs.consume("_exit_code").ok();
        let production = attrs.consume("_production").ok();
        let source = attrs.consume("_source").ok();
        let mut inputs = HashMap::new();
        let mut dependencies = HashMap::new();
        for (key, value) in attrs {
            if key.starts_with("in/") || key.starts_with("inref/") {
                inputs.insert(key.clone(), value.parse()?);
            } else if key.starts_with("_dep:") {
                dependencies.insert(key.clone(), value.parse()?);
            } else {
                bail!("unknown key");
            }
        }
        Ok(Step {
            pos,
            process,
            exit_code,
            production,
            source,
            inputs,
            dependencies,
        })
    }
}

impl Storable for Plan {
    fn objtype() -> &'static [u8] {
        b"production"
    }

    fn to_writer(&self, w: &mut dyn Write) -> Result<()> {
        let mut keys: Vec<&String> = self.steps.keys().collect();
        keys.sort();
        for key in keys {
            attributes::to_writer(w, &self.steps[key])?;
            w.write_all(b"\n")?;
        }
        Ok(())
    }

    fn from_reader(mut r: Box<dyn Read>) -> Result<Self> {
        let mut steps = HashMap::new();
        let mut buf = Vec::new();
        r.read_to_end(&mut buf)?;
        for mut step_buf in buf.split_str(b"\n\n") {
            if step_buf.is_empty() {
                continue;
            }
            let step = Step::from_reader(&mut step_buf)?;
            steps.insert(step.pos.clone().unwrap(), step);
        }
        Ok(Plan { steps })
    }
}

#[cfg(test)]
mod tests {
    use crate::object::Id;

    use super::*;

    #[test]
    fn parse_invocation() {
        let raw = b"_annotated_plan=bc21dd3f5fefde24510e7b3b07a3eada55476e5d
production=5a85adff7fc597bdb1c2efa56a3a7d758854ced5
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
                annotated_plan: "bc21dd3f5fefde24510e7b3b07a3eada55476e5d".parse().unwrap(),
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
            "file:foo/bar".parse::<Input>().unwrap(),
            Input::File("foo/bar".into())
        );
        assert_eq!(
            "_pos:main@0:out/_".parse::<Input>().unwrap(),
            Input::Pos("main@0".into(), "out/_".into())
        );
    }

    #[test]
    fn parse_step() {
        let raw = b"_exit_code=0
_production=6067a9bbab7995feadd4c09fdf0c76920a393543
_pos=main@0
_source=unit:flow/basic/source.unit
process=command:cat in/file > out/_
_dep:in/file=7c31e039fa719bccf92e192e500dcf1bc109b9d7
in/file=01e79c32a8c99c557f0757da7cb6d65b3414466d
";
        assert_eq!(
            Step::from_reader(&mut raw.as_ref()).unwrap(),
            Step {
                pos: Some("main@0".into()),
                process: Process::Command("cat in/file > out/_".into()),
                exit_code: Some(0),
                production: Some("6067a9bbab7995feadd4c09fdf0c76920a393543".parse().unwrap()),
                source: Some("unit:flow/basic/source.unit".into()),
                inputs: [(
                    "in/file".into(),
                    "01e79c32a8c99c557f0757da7cb6d65b3414466d".parse().unwrap()
                )]
                .iter()
                .cloned()
                .collect(),
                dependencies: [(
                    "_dep:in/file".into(),
                    "7c31e039fa719bccf92e192e500dcf1bc109b9d7".parse().unwrap()
                )]
                .iter()
                .cloned()
                .collect(),
            }
        );
    }

    #[test]
    fn parse_plan() {
        let raw = b"_exit_code=0
_production=7c31e039fa719bccf92e192e500dcf1bc109b9d7
_pos=1472c372edfbb7e2b7fedf5d314548db64248f85
_source=file:flow/basic/source.txt
process=identity
in/_=01e79c32a8c99c557f0757da7cb6d65b3414466d

_exit_code=0
_production=6067a9bbab7995feadd4c09fdf0c76920a393543
_pos=main@0
_source=unit:flow/basic/source.unit
process=command:cat in/file > out/_
_dep:in/file=7c31e039fa719bccf92e192e500dcf1bc109b9d7
in/file=01e79c32a8c99c557f0757da7cb6d65b3414466d

_exit_code=0
_production=e5385bc10618962dbe798dd99b88207e2df8e3ec
_pos=main
_source=unit:flow/basic/tac.unit
process=command:perl -e 'print reverse <>' in/data > out/_
_dep:in/data=6067a9bbab7995feadd4c09fdf0c76920a393543
in/data=01e79c32a8c99c557f0757da7cb6d65b3414466d

";
        let mut steps = HashMap::new();
        steps.insert(
            "1472c372edfbb7e2b7fedf5d314548db64248f85".into(),
            Step {
                pos: Some("1472c372edfbb7e2b7fedf5d314548db64248f85".into()),
                process: Process::Identity,
                exit_code: Some(0),
                production: Some("7c31e039fa719bccf92e192e500dcf1bc109b9d7".parse().unwrap()),
                source: Some("file:flow/basic/source.txt".into()),
                inputs: [(
                    "in/_".into(),
                    "01e79c32a8c99c557f0757da7cb6d65b3414466d".parse().unwrap(),
                )]
                .iter()
                .cloned()
                .collect(),
                dependencies: HashMap::new(),
            },
        );
        steps.insert(
            "main@0".into(),
            Step {
                pos: Some("main@0".into()),
                process: Process::Command("cat in/file > out/_".into()),
                exit_code: Some(0),
                production: Some("6067a9bbab7995feadd4c09fdf0c76920a393543".parse().unwrap()),
                source: Some("unit:flow/basic/source.unit".into()),
                inputs: [(
                    "in/file".into(),
                    "01e79c32a8c99c557f0757da7cb6d65b3414466d".parse().unwrap(),
                )]
                .iter()
                .cloned()
                .collect(),
                dependencies: [(
                    "_dep:in/file".into(),
                    "7c31e039fa719bccf92e192e500dcf1bc109b9d7".parse().unwrap(),
                )]
                .iter()
                .cloned()
                .collect(),
            },
        );
        steps.insert(
            "main".into(),
            Step {
                pos: Some("main".into()),
                process: Process::Command("perl -e 'print reverse <>' in/data > out/_".into()),
                exit_code: Some(0),
                production: Some("e5385bc10618962dbe798dd99b88207e2df8e3ec".parse().unwrap()),
                source: Some("unit:flow/basic/tac.unit".into()),
                inputs: [(
                    "in/data".into(),
                    "01e79c32a8c99c557f0757da7cb6d65b3414466d".parse().unwrap(),
                )]
                .iter()
                .cloned()
                .collect(),
                dependencies: [(
                    "_dep:in/data".into(),
                    "6067a9bbab7995feadd4c09fdf0c76920a393543".parse().unwrap(),
                )]
                .iter()
                .cloned()
                .collect(),
            },
        );
        assert_eq!(
            Plan::from_reader(Box::new(raw.as_ref())).unwrap(),
            Plan { steps }
        );
    }
}
