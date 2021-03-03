use std::collections::HashMap;
use std::fs;
use std::io::prelude::*;
use std::str::{self, FromStr};

use bstr::ByteSlice;
use serde::Serialize;
use sha1::Digest;
use stable_eyre::eyre::{anyhow, bail, ensure, Context, Error, Result};

use crate::cas;
use crate::compat::attributes::{self, Attributes, StrExt};
use crate::object::*;

#[derive(Debug, PartialEq)]
pub struct TextPlan {
    pub steps: Vec<TextStep>,
}

#[derive(Debug, Serialize, PartialEq)]
pub struct TextStep {
    #[serde(rename = "_pos")]
    pub pos: String,
    #[serde(rename = "_source")]
    pub source: Option<String>,
    pub process: Process,
    #[serde(flatten)]
    pub inputs: HashMap<String, TextInput>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum TextInput {
    #[serde(rename = "")]
    Id(Id<Resource>),
    File(String),
    #[serde(rename = "_pos")]
    Pos(String, String),
    // TODO reconcile name
    #[serde(rename = "inline")]
    Value(String),
}

impl FromStr for TextInput {
    type Err = Error;

    fn from_str(s: &str) -> std::result::Result<Self, Self::Err> {
        if let Some((prefix, suffix)) = s.split_once_ext(':') {
            match prefix {
                "file" => Ok(TextInput::File(suffix.into())),
                "_pos" => {
                    if let Some((pos, path)) = suffix.split_once_ext(':') {
                        Ok(TextInput::Pos(pos.into(), path.into()))
                    } else {
                        Err(anyhow!("expected : in _pos input"))
                    }
                }
                "inline" => Ok(TextInput::Value(suffix.into())),
                // This seems like the wrong way to do this
                "param" => Ok(TextInput::Pos("_param".into(), suffix.into())),
                _ => Err(anyhow!("unknown input type: {}", prefix)),
            }
        } else {
            Ok(TextInput::Id(s.parse()?))
        }
    }
}

impl TextStep {
    fn from_reader(r: &mut impl Read) -> Result<Self> {
        let mut attrs = Attributes::from_reader(r)?;
        let pos = attrs.consume("_pos")?;
        let source = attrs.consume("_source").ok();
        let process = attrs.consume::<String>("process")?.parse()?;
        let mut inputs = HashMap::new();
        for (key, value) in attrs {
            if key.starts_with("in/") || key.starts_with("inref/") {
                inputs.insert(key.clone(), value.parse()?);
            } else {
                bail!("unknown key {}", &key);
            }
        }
        Ok(TextStep {
            pos,
            source,
            process,
            inputs,
        })
    }
}

impl TextPlan {
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut bytes = Vec::new();
        for step in self.steps.iter() {
            attributes::to_writer(&mut bytes, step).expect("serialization failed");
            bytes.push(b'\n');
        }
        bytes
    }

    pub fn from_reader(mut r: Box<dyn Read>) -> Result<Self> {
        let mut steps = Vec::new();
        let mut buf = Vec::new();
        r.read_to_end(&mut buf)?;
        for mut step_buf in buf.split_str(b"\n\n") {
            if step_buf.is_empty() {
                continue;
            }
            steps.push(TextStep::from_reader(&mut step_buf)?);
        }
        Ok(TextPlan { steps })
    }

    fn make_identity_step(store: &cas::Store, source: String, data: &[u8]) -> Result<Step> {
        let name = hex::encode(sha1::Sha1::digest(source.as_bytes()));
        let id = store.write_resource(data)?;
        let mut inputs = HashMap::new();
        inputs.insert("in/_".into(), Input::Id(id));
        // This step only exists to propagate source.
        let step = Step {
            pos: Some(name),
            process: Process::Identity,
            exit_code: None,
            production: None,
            source: Some(source),
            inputs,
            dependencies: HashMap::new(),
        };
        Ok(step)
    }

    pub fn encode(self, store: &cas::Store) -> Result<Plan> {
        let mut steps = HashMap::new();
        for step in self.steps {
            let mut inputs = HashMap::new();
            for (path, text_input) in step.inputs {
                let input = match text_input {
                    TextInput::Id(id) => Input::Id(id),
                    TextInput::File(ref path) => {
                        let source = format!("file:{}", path);
                        // TODO hardcoded to filesystem
                        let data =
                            fs::read(path).with_context(|| format!("could not read {}", path))?;
                        let step = Self::make_identity_step(store, source, &data)?;
                        let name = step.pos.clone().unwrap();
                        steps.insert(name.clone(), step);
                        Input::Pos(name, "out/_".to_string())
                    }
                    TextInput::Pos(step, path) => Input::Pos(step, path),
                    TextInput::Value(mut value) => {
                        let source = format!("value:{}", value);
                        value.push('\n');
                        let step = Self::make_identity_step(store, source, value.as_bytes())?;
                        let name = step.pos.clone().unwrap();
                        steps.insert(name.clone(), step);
                        Input::Pos(name, "out/_".to_string())
                    }
                };
                inputs.insert(path, input);
            }
            steps.insert(
                step.pos.clone(),
                Step {
                    pos: Some(step.pos),
                    process: step.process,
                    exit_code: None,
                    production: None,
                    source: step.source,
                    inputs,
                    dependencies: HashMap::new(),
                },
            );
        }
        Ok(Plan { steps })
    }
}

impl Plan {
    // TODO seems this should be somewhere else
    pub fn check_terminal(&self, terminal: &str) -> Result<()> {
        let mut alldeps = Vec::new();
        let mut frontier = vec![terminal];
        while let Some(pos) = frontier.pop() {
            if alldeps.contains(&pos) {
                continue;
            }
            alldeps.push(pos);
            let step = self
                .steps
                .get(pos)
                .ok_or_else(|| anyhow!("missing step {}", pos))?;
            for input in step.inputs.values() {
                if let Input::Pos(dep, _) = input {
                    frontier.push(dep);
                }
            }
        }

        let extra_steps: Vec<_> = self
            .steps
            .keys()
            .filter(|s| !alldeps.contains(&s.as_str()))
            .collect();
        ensure!(
            extra_steps.is_empty(),
            "{} is not the plan terminal: {:?}",
            terminal,
            extra_steps
        );
        Ok(())
    }
}
