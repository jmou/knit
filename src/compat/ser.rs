//! Custom serializer is a hot mess, but we expect to eventually replace it.
//! Only the minimal methods needed for our objects are implemented.
//! Serialize in key=value form.

use std::fmt;
use std::io::Write;

use serde::{ser, Serialize};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum Error {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("ser: {0}")]
    Ser(String),
    #[error("unterminated line")]
    UnterminatedLine,
    #[error("unscoped elision")]
    Elision,
}

impl ser::Error for Error {
    fn custom<T: fmt::Display>(msg: T) -> Self {
        Self::Ser(msg.to_string())
    }
}

pub fn to_writer(writer: &mut impl Write, value: &impl Serialize) -> Result<(), Error> {
    let mut serializer = Serializer {
        lines: vec![vec![]],
    };
    value.serialize(&mut serializer)?;
    // Remove trailing output buffer.
    if let Some(line) = serializer.lines.last() {
        if !line.is_empty() {
            return Err(Error::UnterminatedLine);
        } else {
            serializer.lines.pop();
        }
    }
    serializer.lines.sort();
    for line in serializer.lines {
        writer.write_all(&line)?;
        writer.write_all(b"\n")?;
    }
    Ok(())
}

struct Serializer {
    lines: Vec<Vec<u8>>,
}

impl Write for Serializer {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        // Kind of gross to overload IO errors, but handy.
        if buf.contains(&b'\n') {
            return Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                "newline in buf",
            ));
        }
        match self.lines.last_mut() {
            Some(line) => line.write(buf),
            None => Err(std::io::Error::new(
                std::io::ErrorKind::Other,
                "uninitialized writer",
            )),
        }
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl<'a> ser::Serializer for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    type SerializeSeq = Self;
    type SerializeTuple = Self;
    type SerializeTupleStruct = Self;
    type SerializeTupleVariant = Self;
    type SerializeMap = Self;
    type SerializeStruct = Self;
    type SerializeStructVariant = Self;

    fn serialize_bool(self, _v: bool) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_i8(self, _v: i8) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_i16(self, _v: i16) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    // for exit_code
    fn serialize_i32(self, v: i32) -> Result<Self::Ok, Self::Error> {
        write!(self, "{}", v)?;
        Ok(())
    }

    fn serialize_i64(self, _v: i64) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_u8(self, _v: u8) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_u16(self, _v: u16) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_u32(self, _v: u32) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_u64(self, _v: u64) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_f32(self, _v: f32) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_f64(self, _v: f64) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_char(self, _v: char) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_str(self, v: &str) -> Result<Self::Ok, Self::Error> {
        write!(self, "{}", v)?;
        Ok(())
    }

    fn serialize_bytes(self, _v: &[u8]) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_none(self) -> Result<Self::Ok, Self::Error> {
        Err(Error::Elision) // remove key entirely
    }

    fn serialize_some<T: ?Sized>(self, value: &T) -> Result<Self::Ok, Self::Error>
    where
        T: Serialize,
    {
        value.serialize(self)
    }

    fn serialize_unit(self) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_unit_struct(self, _name: &'static str) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }

    fn serialize_unit_variant(
        self,
        _name: &'static str,
        _variant_index: u32,
        variant: &'static str,
    ) -> Result<Self::Ok, Self::Error> {
        write!(self, "{}", variant)?;
        Ok(())
    }

    fn serialize_newtype_struct<T: ?Sized>(
        self,
        _name: &'static str,
        value: &T,
    ) -> Result<Self::Ok, Self::Error>
    where
        T: Serialize,
    {
        value.serialize(self)
    }

    fn serialize_newtype_variant<T: ?Sized>(
        self,
        _name: &'static str,
        _variant_index: u32,
        variant: &'static str,
        value: &T,
    ) -> Result<Self::Ok, Self::Error>
    where
        T: Serialize,
    {
        variant.serialize(&mut *self)?;
        // One variant may be unnamed, like Input::Id
        if !variant.is_empty() {
            write!(self, ":")?;
        }
        value.serialize(self)
    }

    fn serialize_seq(self, _len: Option<usize>) -> Result<Self::SerializeSeq, Self::Error> {
        unimplemented!()
    }

    fn serialize_tuple(self, _len: usize) -> Result<Self::SerializeTuple, Self::Error> {
        unimplemented!()
    }

    fn serialize_tuple_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleStruct, Self::Error> {
        unimplemented!()
    }

    fn serialize_tuple_variant(
        self,
        _name: &'static str,
        _variant_index: u32,
        variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleVariant, Self::Error> {
        variant.serialize(&mut *self)?;
        Ok(self)
    }

    fn serialize_map(self, _len: Option<usize>) -> Result<Self::SerializeMap, Self::Error> {
        Ok(self)
    }

    fn serialize_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStruct, Self::Error> {
        Ok(self)
    }

    fn serialize_struct_variant(
        self,
        _name: &'static str,
        _variant_index: u32,
        _variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStructVariant, Self::Error> {
        unimplemented!()
    }
}

impl<'a> ser::SerializeSeq for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    fn serialize_element<T: ?Sized>(&mut self, _value: &T) -> Result<(), Self::Error>
    where
        T: Serialize,
    {
        unimplemented!()
    }

    fn end(self) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }
}

impl<'a> ser::SerializeTuple for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    fn serialize_element<T: ?Sized>(&mut self, _value: &T) -> Result<(), Self::Error>
    where
        T: Serialize,
    {
        unimplemented!()
    }

    fn end(self) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }
}

impl<'a> ser::SerializeTupleStruct for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    fn serialize_field<T: ?Sized>(&mut self, _value: &T) -> Result<(), Self::Error>
    where
        T: Serialize,
    {
        unimplemented!()
    }

    fn end(self) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }
}

impl<'a> ser::SerializeTupleVariant for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    fn serialize_field<T: ?Sized>(&mut self, value: &T) -> Result<(), Self::Error>
    where
        T: Serialize,
    {
        write!(self, ":")?;
        value.serialize(&mut **self)
    }

    fn end(self) -> Result<Self::Ok, Self::Error> {
        Ok(())
    }
}

impl<'a> ser::SerializeMap for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    fn serialize_key<T: ?Sized>(&mut self, key: &T) -> Result<(), Self::Error>
    where
        T: Serialize,
    {
        key.serialize(&mut **self)
    }

    // Apparently a struct with a flattened map is serialized with a map, so we
    // need to implement elision.
    fn serialize_value<T: ?Sized>(&mut self, value: &T) -> Result<(), Self::Error>
    where
        T: Serialize,
    {
        write!(self, "=")?;
        match value.serialize(&mut **self) {
            Ok(()) => self.lines.push(Vec::new()),
            Err(Error::Elision) => self.lines.last_mut().unwrap().clear(),
            Err(e) => return Err(e),
        }
        Ok(())
    }

    fn end(self) -> Result<Self::Ok, Self::Error> {
        Ok(())
    }
}

impl<'a> ser::SerializeStruct for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    fn serialize_field<T>(&mut self, key: &'static str, value: &T) -> Result<(), Self::Error>
    where
        T: ?Sized + Serialize,
    {
        key.serialize(&mut **self)?;
        write!(self, "=")?;
        match value.serialize(&mut **self) {
            Ok(()) => self.lines.push(Vec::new()),
            Err(Error::Elision) => self.lines.last_mut().unwrap().clear(),
            Err(e) => return Err(e),
        }
        Ok(())
    }

    fn end(self) -> Result<Self::Ok, Self::Error> {
        Ok(())
    }
}

impl<'a> ser::SerializeStructVariant for &'a mut Serializer {
    type Ok = ();
    type Error = Error;

    fn serialize_field<T: ?Sized>(
        &mut self,
        _key: &'static str,
        _value: &T,
    ) -> Result<(), Self::Error>
    where
        T: Serialize,
    {
        unimplemented!()
    }

    fn end(self) -> Result<Self::Ok, Self::Error> {
        unimplemented!()
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use super::*;

    #[derive(Serialize)]
    struct NewtypeStruct(String);

    #[derive(Serialize)]
    #[serde(rename_all = "snake_case")]
    enum Variant {
        Unit,
        Newtype(String),
    }

    #[derive(Serialize)]
    struct Struct {
        i32: i32,
        #[serde(with = "hex")]
        u8_array: [u8; 4],
        #[serde(rename = "renamed")]
        string: String,
        option: Option<String>,
        variant: Variant,
        newtype_struct: NewtypeStruct,
        #[serde(flatten)]
        map: HashMap<String, String>,
    }

    #[test]
    fn serialize() {
        let mut serialized = Vec::new();
        let mut value = Struct {
            i32: -1,
            u8_array: *b"cows",
            string: "string".into(),
            option: Some("tuple".into()),
            variant: Variant::Unit,
            newtype_struct: NewtypeStruct("struct".into()),
            map: HashMap::new(),
        };
        to_writer(&mut serialized, &value).unwrap();
        assert_eq!(
            std::str::from_utf8(&serialized).unwrap(),
            "i32=-1\n\
            newtype_struct=struct\n\
            option=tuple\n\
            renamed=string\n\
            u8_array=636f7773\n\
            variant=unit\n"
        );

        serialized.clear();
        value.option.take();
        value.variant = Variant::Newtype("inner".into());
        value.map.insert("key1".into(), "value1".into());
        value.map.insert("key2".into(), "value2".into());
        to_writer(&mut serialized, &value).unwrap();
        assert_eq!(
            std::str::from_utf8(&serialized).unwrap(),
            "i32=-1\n\
            key1=value1\n\
            key2=value2\n\
            newtype_struct=struct\n\
            renamed=string\n\
            u8_array=636f7773\n\
            variant=newtype:inner\n"
        );
    }
}
