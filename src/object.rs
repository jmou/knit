use std::collections::HashMap;
use std::fmt;
use std::str::FromStr;

use chrono::{DateTime, FixedOffset};
use hex::FromHexError;
use serde::Serialize;

#[derive(Debug, Serialize, PartialEq)]
pub struct Invocation {
    pub production: Option<Id>,
    // TODO clean up failed invocations
    #[serde(flatten)]
    pub partial_productions: HashMap<String, Id>,
    pub status: InvocationStatus,
    pub plan: Id,
    #[serde(rename = "_annotated_plan")]
    pub annotated_plan: Id,
}

#[derive(Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum InvocationStatus {
    Ok,
    Fail,
}

#[derive(Debug, Serialize, PartialEq)]
pub struct Production {
    pub job: Id,
    pub exit_code: i32,
    #[serde(flatten)]
    pub outputs: HashMap<String, Id>,
    #[serde(flatten)]
    pub dependencies: HashMap<String, Id>,
    pub log: Option<Id>,
    pub invocation: Option<Id>,
    pub cache: Option<Id>,
    #[serde(rename = "_source")]
    pub source: Option<String>,
    pub start_ts: Option<DateTime<FixedOffset>>,
    pub end_ts: Option<DateTime<FixedOffset>>,
}

#[derive(Debug, Serialize, PartialEq)]
pub struct Job {
    pub process: Process,
    #[serde(flatten)]
    pub inputs: HashMap<String, Id>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum Process {
    // TODO manual
    Identity,
    Command(String),
    Nested(String),
    Composite(String),
}

#[derive(Clone, Copy, Serialize, PartialEq)]
pub struct Id(#[serde(with = "hex")] pub [u8; 20]);

impl Id {
    pub fn hex(&self) -> String {
        hex::encode(self.0)
    }
}

impl FromStr for Id {
    type Err = FromHexError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut id = [0u8; 20];
        hex::decode_to_slice(s, &mut id)?;
        Ok(Id(id))
    }
}

impl fmt::Display for Id {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.hex())
    }
}

impl fmt::Debug for Id {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, f)
    }
}
