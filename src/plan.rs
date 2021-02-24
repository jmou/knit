// TODO refactor so text plan is its own type

use std::collections::HashMap;
use std::fs;
use std::{mem, str};

use sha1::Digest;
use stable_eyre::eyre::{anyhow, ensure, Context, Result};

use crate::cas;
use crate::object::*;

impl Plan {
    fn emit_identity_step(
        &mut self,
        store: &cas::Store,
        source: String,
        data: &[u8],
    ) -> Result<Input> {
        let name = hex::encode(sha1::Sha1::digest(source.as_bytes()));
        let id = store.write_resource(data)?;
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

    pub fn preprocess(&mut self, store: &cas::Store) -> Result<()> {
        let steps = mem::take(&mut self.steps);
        for (pos, mut step) in steps {
            for (_, input) in step.inputs.iter_mut() {
                match input {
                    Input::File(ref path) => {
                        let source = format!("file:{}", path);
                        let data =
                            fs::read(path).with_context(|| format!("could not read {}", path))?;
                        *input = self.emit_identity_step(store, source, &data)?;
                    }
                    Input::Value(value) => {
                        let source = format!("inline:{}", value);
                        value.push('\n');
                        *input = self.emit_identity_step(store, source, value.as_bytes())?;
                    }
                    _ => (),
                }
            }
            self.steps.insert(pos, step);
        }
        Ok(())
    }

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
