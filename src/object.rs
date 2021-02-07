//! Core data structures.

use std::collections::HashMap;

use chrono::{DateTime, FixedOffset};
use serde::Serialize;

pub use crate::cas::{Id, Resource};

#[derive(Debug, Serialize, PartialEq)]
pub struct Invocation {
    pub production: Option<Id<Production>>,
    // TODO clean up failed invocations
    #[serde(flatten)]
    pub partial_productions: HashMap<String, Id<Production>>,
    pub status: InvocationStatus,
    pub plan: Id<Plan>,
    #[serde(rename = "_annotated_plan")]
    pub annotated_plan: Id<Plan>,
}

#[derive(Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum InvocationStatus {
    Ok,
    Fail,
}

#[derive(Debug, Serialize, PartialEq)]
pub struct Production {
    pub job: Id<Job>,
    pub exit_code: i32,
    #[serde(flatten)]
    pub outputs: HashMap<String, Id<Resource>>,
    #[serde(flatten)]
    pub dependencies: HashMap<String, Id<Production>>,
    pub log: Option<Id<Resource>>,
    pub invocation: Option<Id<Invocation>>,
    pub cache: Option<Id<Production>>,
    #[serde(rename = "_source")]
    pub source: Option<String>,
    pub start_ts: Option<DateTime<FixedOffset>>,
    pub end_ts: Option<DateTime<FixedOffset>>,
}

#[derive(Debug, Serialize, PartialEq)]
pub struct Job {
    pub process: Process,
    #[serde(flatten)]
    pub inputs: HashMap<String, Id<Resource>>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum Process {
    // TODO manual
    Identity,
    Command(String),
    Nested(String),
}

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
    pub production: Option<Id<Production>>,
    #[serde(rename = "_source")]
    pub source: Option<String>,

    #[serde(flatten)]
    pub inputs: HashMap<String, Input>,
    #[serde(flatten)]
    pub dependencies: HashMap<String, Id<Production>>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum Input {
    #[serde(rename = "")]
    Id(Id<Resource>),
    File(String),
    #[serde(rename = "_pos")]
    Pos(String, String),
    // TODO reconcile name
    #[serde(rename = "inline")]
    Value(String),
}
