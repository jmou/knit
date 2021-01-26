use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{prelude::*, BufReader};
use std::str;

use serde::Serialize;
use sha1::Digest;
use stable_eyre::eyre::{anyhow, bail, Result};
use walkdir::WalkDir;

use crate::attributes::StrExt;
use crate::{cas, object::*};

#[derive(Debug, PartialEq)]
pub struct Plan {
    pub steps: HashMap<String, Step>,
}

#[derive(Debug, Serialize, PartialEq)]
pub struct Step {
    #[serde(rename = "_pos")]
    pub pos: Option<String>,
    pub process: Process,

    // TODO DRY with Production
    #[serde(rename = "_exit_code")]
    pub exit_code: Option<i32>,
    #[serde(rename = "_production")]
    pub production: Option<Id>,
    #[serde(rename = "_source")]
    pub source: Option<String>,

    #[serde(flatten)]
    pub inputs: HashMap<String, Input>,
    #[serde(flatten)]
    pub dependencies: HashMap<String, Id>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum Input {
    #[serde(rename = "")]
    Id(Id),
    File(String),
    #[serde(rename = "_pos")]
    Pos(String, String),
    // TODO reconcile name
    #[serde(rename = "inline")]
    Value(String),
}

impl Plan {
    fn emit_identity_step(
        &mut self,
        store: &mut dyn cas::Store,
        source: String,
        data: &[u8],
    ) -> Result<Input> {
        let name = hex::encode(sha1::Sha1::digest(source.as_bytes()));
        let id = store.write("resource", data)?;
        let mut inputs = HashMap::new();
        inputs.insert("in/_".into(), Input::Id(id));
        // This step only exists to propagate source.
        let step = Step {
            pos: Some(name.clone()),
            process: Process::Identity,
            exit_code: None,
            production: None,
            source: Some(source),
            inputs,
            dependencies: HashMap::new(),
        };
        self.steps.insert(name.clone(), step);
        Ok(Input::Pos(name, "out/_".into()))
    }

    fn interpret_input(
        &mut self,
        store: &mut dyn cas::Store,
        key: &str,
        input: Input,
        inputs: &mut HashMap<String, Input>,
    ) -> Result<()> {
        match &input {
            Input::File(path) => {
                // This is awkward to use in nested contexts because it only takes paths
                // from the repository root.
                if key.ends_with('/') && path.ends_with('/') {
                    for entry in WalkDir::new(path).follow_links(true) {
                        let entry = entry?;
                        if !entry.file_type().is_file() {
                            continue;
                        }
                        if let Some(stem) = entry.path().strip_prefix(path)?.to_str() {
                            let source = format!("file:{}", entry.path().display());
                            let data = fs::read(entry.path())?;
                            let step = self.emit_identity_step(store, source, &data)?;
                            inputs.insert(format!("{}{}", key, stem), step);
                        } else {
                            bail!("non-UTF-8 path");
                        }
                    }
                } else {
                    let source = format!("file:{}", path);
                    let step = self.emit_identity_step(store, source, &fs::read(path)?)?;
                    inputs.insert(key.into(), step);
                }
            }
            Input::Value(value) => {
                let mut data = value.as_bytes().to_owned();
                // Newline for compatibility is kind of weird.
                data.push(b'\n');
                let step = self.emit_identity_step(store, format!("inline:{}", value), &data)?;
                inputs.insert(key.into(), step);
            }
            _ => {
                inputs.insert(key.into(), input);
            }
        }
        Ok(())
    }

    fn translate_unit(
        &mut self,
        store: &mut dyn cas::Store,
        unit: &str,
        step: &str,
        index: &mut i32,
    ) -> Result<String> {
        let pos = if *index >= 0 {
            format!("{}@{}", step, *index)
        } else {
            step.to_string()
        };
        let mut process = None;
        let mut inputs = HashMap::new();
        {
            let file = File::open(unit)?;
            for line in BufReader::new(file).lines() {
                let line = line?;
                if line.starts_with('#') || line.chars().all(char::is_whitespace) {
                    continue;
                }

                let (key, value) = line
                    .split_once_ext('=')
                    .ok_or_else(|| anyhow!("expected ="))?;
                if key == "process" {
                    process = Some(value.parse()?);
                } else if key.starts_with("in/") || key.starts_with("inref/") {
                    let input = match value.strip_prefix("unit:") {
                        Some(suffix) => {
                            let (input_unit, path) = suffix
                                .split_once_ext(':')
                                .ok_or_else(|| anyhow!("expected :"))?;
                            *index += 1;
                            let pos = self.translate_unit(store, input_unit, step, index)?;
                            Input::Pos(pos, path.into())
                        }
                        None => {
                            let input = value.parse()?;
                            if let Input::Pos(pos, _) = &input {
                                if pos != "_param" {
                                    eprintln!("warning: passing through _pos input in {}", unit);
                                }
                            }
                            input
                        }
                    };
                    self.interpret_input(store, key, input, &mut inputs)?;
                } else {
                    bail!("unsupported key: {}", key);
                }
            }
        }
        let step = Step {
            pos: Some(pos.clone()),
            process: process.ok_or_else(|| anyhow!("missing process"))?,
            exit_code: None,
            production: None,
            source: Some(format!("unit:{}", unit)),
            inputs,
            dependencies: HashMap::new(),
        };
        self.steps.insert(pos.clone(), step);
        Ok(pos)
    }

    pub fn from_unit_file(store: &mut dyn cas::Store, unit: &str, root_pos: &str) -> Result<Plan> {
        let mut plan = Plan {
            steps: HashMap::new(),
        };
        plan.translate_unit(store, unit, root_pos, &mut -1)?;
        Ok(plan)
    }
}
