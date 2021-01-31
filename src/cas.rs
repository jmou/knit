//! Core content addressable storage abstractions.

use std::fmt;
use std::io::prelude::*;
use std::marker::PhantomData;
use std::str::{self, FromStr};

use derivative::Derivative;
use hex::FromHexError;
use serde::Serialize;
use stable_eyre::eyre::Result;

/// Typesafe access to objects in the CAS. The untyped backing store uses a
/// trait object to avoid proliferating generic type parameters.
pub struct Store(pub Box<dyn UntypedStore>);

impl Store {
    /// Write a [`Storable`] to the CAS, returning its unique content [`Id`].
    pub fn write<T: Storable>(&self, value: &T) -> Result<Id<T>> {
        let untyped_id = self.0.write(T::objtype(), value)?;
        Ok(Id::new(untyped_id))
    }

    /// Read a [`Storable`] from the CAS identified by its unique content [`Id`].
    pub fn read<T: Storable>(&self, id: &Id<T>) -> Result<T> {
        let reader = self.0.read(T::objtype(), &id.untyped_id)?;
        T::from_reader(reader)
    }

    /// Slightly more efficient alternative to `write` for [`Resource`] bytes.
    pub fn write_resource(&self, value: &[u8]) -> Result<Id<Resource>> {
        let untyped_id = self.0.write(Resource::objtype(), &value)?;
        Ok(Id::new(untyped_id))
    }

    /// Slightly more efficient alternative to `read` for [`Resource`] bytes.
    pub fn read_resource(&self, id: &Id<Resource>) -> Result<Box<dyn Read>> {
        self.0.read(Resource::objtype(), &id.untyped_id)
    }
}

/// Trait for objects that can be stored in the CAS.
pub trait Storable {
    /// Byte string unique for this object type, for the [`Store`] to
    /// disambiguate object types.
    fn objtype() -> &'static [u8];
    /// Serialize the object.
    fn to_writer(&self, w: &mut dyn Write) -> Result<()>;
    /// Deserialize the object.
    fn from_reader(r: Box<dyn Read>) -> Result<Self>
    where
        Self: Sized;
}

/// CAS object of arbitrary bytes.
pub struct Resource(Vec<u8>);

impl Storable for Resource {
    fn objtype() -> &'static [u8] {
        b"resource"
    }

    fn to_writer(&self, w: &mut dyn Write) -> Result<()> {
        Ok(w.write_all(&self.0)?)
    }

    fn from_reader(mut r: Box<dyn Read>) -> Result<Self> {
        let mut buf = Vec::new();
        r.read_to_end(&mut buf)?;
        Ok(Resource(buf))
    }
}

/// Object-safe trait for CAS backing stores. Most users should use [`Store`].
pub trait UntypedStore {
    fn write(&self, objtype: &[u8], w: &dyn Writable) -> Result<UntypedId>;
    fn read(&self, objtype: &[u8], id: &UntypedId) -> Result<Box<dyn Read>>;
}

/// Object-safe adapter for writing [`Storable`] trait.
pub trait Writable {
    fn to_writer(&self, w: &mut dyn Write) -> Result<()>;
}

impl<T: Storable> Writable for T {
    fn to_writer(&self, w: &mut dyn Write) -> Result<()> {
        self.to_writer(w)
    }
}

/// Special case for `Resource` bytes. This must be implemented on `&[u8]` and
/// not `[u8]` because a trait object may not be unsized.
/// https://github.com/rust-lang/rust/issues/42923
impl Writable for &[u8] {
    fn to_writer(&self, w: &mut dyn Write) -> Result<()> {
        w.write_all(self)?;
        Ok(())
    }
}

/// Typesafe CAS object id. Uniquely identifies the referenced object.
#[derive(Derivative, Serialize)]
// derive uses wrong trait bounds for PhantomData
// https://github.com/rust-lang/rust/issues/26925
#[derivative(Clone, Copy, PartialEq)]
#[serde(transparent)]
pub struct Id<T> {
    untyped_id: UntypedId,
    // Associate type parameter T without indicating ownership (no drop check).
    phantom: PhantomData<*const T>,
}

impl<T> Id<T> {
    pub fn new(id: UntypedId) -> Self {
        Id {
            untyped_id: id,
            phantom: PhantomData,
        }
    }
}

impl<T> FromStr for Id<T> {
    type Err = FromHexError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        Ok(Id::new(UntypedId::from_str(s)?))
    }
}

impl<T> fmt::Display for Id<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.untyped_id.fmt(f)
    }
}

impl<T> fmt::Debug for Id<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.untyped_id.fmt(f)
    }
}

/// Raw content hash. Most users should use [`Id`].
#[derive(Clone, Copy, Serialize, PartialEq)]
pub struct UntypedId(#[serde(with = "hex")] pub [u8; 20]);

impl UntypedId {
    pub fn hex(&self) -> String {
        hex::encode(self.0)
    }
}

impl FromStr for UntypedId {
    type Err = FromHexError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut id = [0u8; 20];
        hex::decode_to_slice(s, &mut id)?;
        Ok(UntypedId(id))
    }
}

impl fmt::Display for UntypedId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.hex())
    }
}

impl fmt::Debug for UntypedId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, f)
    }
}
