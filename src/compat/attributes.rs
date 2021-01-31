//! key=value serialization format.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read};
use std::str::FromStr;

use serde::Serialize;
use stable_eyre::eyre::{anyhow, bail, Result};

pub use crate::compat::ser::to_writer;

// Similar function in Rust nightly.
pub trait StrExt {
    fn split_once_ext(&self, delimiter: char) -> Option<(&str, &str)>;
}
impl StrExt for str {
    fn split_once_ext(&self, delimiter: char) -> Option<(&str, &str)> {
        let start = self.find(delimiter)?;
        let end = start + delimiter.len_utf8();
        Some((&self[..start], &self[end..]))
    }
}

#[derive(Clone)]
pub struct Attributes(HashMap<String, String>);

impl Attributes {
    pub fn consume<T>(&mut self, key: &str) -> Result<T>
    where
        T: FromStr,
        <T as FromStr>::Err: 'static + std::error::Error,
        <T as FromStr>::Err: Send,
        <T as FromStr>::Err: Sync,
    {
        Ok(self
            .0
            .remove(key)
            .ok_or_else(|| anyhow!("missing key {}", key))?
            .parse::<T>()?)
    }

    pub fn from_reader(r: &mut dyn Read) -> Result<Self> {
        let mut attrs: HashMap<String, String> = HashMap::new();
        for line in BufReader::new(r).lines() {
            if let Some((key, value)) = line?.split_once_ext('=') {
                attrs.insert(key.into(), value.into());
            } else {
                bail!("malformed line without =");
            }
        }
        Ok(Self(attrs))
    }
}

impl IntoIterator for Attributes {
    type Item = <HashMap<String, String> as IntoIterator>::Item;
    type IntoIter = <HashMap<String, String> as IntoIterator>::IntoIter;
    fn into_iter(self) -> Self::IntoIter {
        self.0.into_iter()
    }
}

pub fn to_bytes(value: &impl Serialize) -> Vec<u8> {
    let mut buf = Vec::new();
    to_writer(&mut buf, value).expect("in-memory serialization");
    buf
}
