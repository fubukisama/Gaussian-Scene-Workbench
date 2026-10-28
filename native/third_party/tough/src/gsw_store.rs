// Copyright 2026 Gaussian Scene Workbench contributors.
// SPDX-License-Identifier: MIT
//! GSW's durable storage adapter, not a replacement TUF implementation.
//! The native installation owner holds its cross-process publish lock. All
//! accepted roles, roots, clock samples and application high-water marks share
//! one atomic journal. The digest detects accidental corruption, NOT an attacker
//! who can rewrite this installation with the same user's privileges.

use aws_lc_rs::digest::{digest, SHA256};
use serde::de::{DeserializeSeed, MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Number, Value};
use std::collections::{BTreeMap, HashSet};
use std::fmt;
use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

/// Largest integer admitted by the GSW wire profile without JSON ambiguity.
pub const MAX_SAFE_INTEGER: u64 = 9_007_199_254_740_991;
const MAX_JOURNAL: usize = 32 * 1024 * 1024;
const JOURNAL: &str = "trust-journal.json";
const MARKER: &str = "trust-initialized";
const MARKER_BYTES: &[u8] = b"gsw.atomic_trust_store/1\n";

fn invalid() -> io::Error { io::Error::new(io::ErrorKind::InvalidData, "invalid trust state") }

/// Strict bounded JSON used before any TUF/application deserializer: decoded
/// duplicate object keys, floating point numbers, unsafe integers, excessive
/// nesting and trailing content are rejected instead of being normalized away.
pub fn strict_json(bytes: &[u8], maximum: usize) -> io::Result<Value> {
    if bytes.is_empty() || bytes.len() > maximum { return Err(invalid()); }
    let mut deserializer = serde_json::Deserializer::from_slice(bytes);
    let value = StrictSeed(0).deserialize(&mut deserializer).map_err(|_| invalid())?;
    deserializer.end().map_err(|_| invalid())?;
    Ok(value)
}

struct StrictSeed(usize);
impl<'de> DeserializeSeed<'de> for StrictSeed {
    type Value = Value;
    fn deserialize<D: serde::Deserializer<'de>>(self, d: D) -> Result<Value, D::Error> {
        if self.0 > 48 { return Err(serde::de::Error::custom("JSON depth limit")); }
        d.deserialize_any(StrictVisitor(self.0))
    }
}
struct StrictVisitor(usize);
impl<'de> Visitor<'de> for StrictVisitor {
    type Value = Value;
    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { f.write_str("strict JSON") }
    fn visit_bool<E: serde::de::Error>(self, v: bool) -> Result<Value, E> { Ok(Value::Bool(v)) }
    fn visit_unit<E: serde::de::Error>(self) -> Result<Value, E> { Ok(Value::Null) }
    fn visit_i64<E: serde::de::Error>(self, v: i64) -> Result<Value, E> {
        if v.unsigned_abs() > MAX_SAFE_INTEGER { return Err(E::custom("unsafe integer")); }
        Ok(Value::Number(Number::from(v)))
    }
    fn visit_u64<E: serde::de::Error>(self, v: u64) -> Result<Value, E> {
        if v > MAX_SAFE_INTEGER { return Err(E::custom("unsafe integer")); }
        Ok(Value::Number(Number::from(v)))
    }
    fn visit_f64<E: serde::de::Error>(self, _: f64) -> Result<Value, E> { Err(E::custom("non-integer JSON")) }
    fn visit_str<E: serde::de::Error>(self, v: &str) -> Result<Value, E> {
        if v.len() > 8 * 1024 * 1024 { return Err(E::custom("string limit")); }
        Ok(Value::String(v.to_owned()))
    }
    fn visit_string<E: serde::de::Error>(self, v: String) -> Result<Value, E> { self.visit_str(&v) }
    fn visit_seq<A: SeqAccess<'de>>(self, mut a: A) -> Result<Value, A::Error> {
        let mut values = Vec::new();
        while let Some(v) = a.next_element_seed(StrictSeed(self.0 + 1))? {
            if values.len() >= 16_384 { return Err(serde::de::Error::custom("array limit")); }
            values.push(v);
        }
        Ok(Value::Array(values))
    }
    fn visit_map<A: MapAccess<'de>>(self, mut a: A) -> Result<Value, A::Error> {
        let mut keys = HashSet::new();
        let mut values = Map::new();
        while let Some(key) = a.next_key::<String>()? {
            if key.len() > 1024 || keys.len() >= 16_384 || !keys.insert(key.clone()) {
                return Err(serde::de::Error::custom("duplicate key or object limit"));
            }
            values.insert(key, a.next_value_seed(StrictSeed(self.0 + 1))?);
        }
        Ok(Value::Object(values))
    }
}

/// SHA-256 through the upstream cryptographic library, as lowercase hex.
pub fn sha256(bytes: &[u8]) -> String {
    digest(&SHA256, bytes).as_ref().iter().map(|b| format!("{b:02x}")).collect()
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Envelope {
    schema: String,
    entries: BTreeMap<String, String>,
    sha256: String,
}

#[derive(Debug, Clone)]
/// A single durable journal whose external owner serializes all mutations.
pub struct Journal { directory: PathBuf }

impl Journal {
    /// Initialization is deliberately fail-closed across a crash: the independent
    /// sentinel makes a missing journal distinguishable from a new installation.
    /// A partial first initialization requires explicit repair, never trust reset.
    pub fn initialize(directory: &Path) -> io::Result<Self> {
        fs::create_dir_all(directory)?;
        let store = Self { directory: directory.to_owned() };
        let journal = directory.join(JOURNAL);
        let marker = directory.join(MARKER);
        match (journal.try_exists()?, marker.try_exists()?) {
            (false, false) => {
                store.save(&BTreeMap::new(), "initialize")?;
                atomic_write(&marker, MARKER_BYTES, "initialize-marker")?;
            }
            (true, true) => { store.entries()?; }
            _ => return Err(invalid()),
        }
        Ok(store)
    }

    /// Open existing initialized state; never treats missing state as fresh.
    pub fn open(directory: &Path) -> io::Result<Self> {
        let store = Self { directory: directory.to_owned() };
        store.entries()?;
        Ok(store)
    }

    /// Read and integrity-check the entire journal, rejecting partial content.
    pub fn entries(&self) -> io::Result<BTreeMap<String, String>> {
        if read_bounded(&self.directory.join(MARKER), 128)? != MARKER_BYTES { return Err(invalid()); }
        let bytes = read_bounded(&self.directory.join(JOURNAL), MAX_JOURNAL)?;
        let value = strict_json(&bytes, MAX_JOURNAL)?;
        let envelope: Envelope = serde_json::from_value(value).map_err(|_| invalid())?;
        if envelope.schema != "gsw.atomic_trust_store/1" || envelope.entries.len() > 16_384 { return Err(invalid()); }
        let canonical = serde_json::to_vec(&envelope.entries).map_err(|_| invalid())?;
        if envelope.sha256 != sha256(&canonical) { return Err(invalid()); }
        Ok(envelope.entries)
    }

    /// Read one entry only after checking the complete journal.
    pub fn get(&self, key: &str) -> io::Result<Option<String>> { Ok(self.entries()?.remove(key)) }

    /// Atomically replace all affected entries together; no rollback on later
    /// caller failures. `tag` is used only by compile-time fixture fault hooks.
    pub fn transact<F>(&self, tag: &str, action: F) -> io::Result<()>
    where F: FnOnce(&mut BTreeMap<String, String>) -> io::Result<()> {
        let mut entries = self.entries()?;
        action(&mut entries)?;
        self.save(&entries, tag)
    }

    fn save(&self, entries: &BTreeMap<String, String>, tag: &str) -> io::Result<()> {
        let canonical = serde_json::to_vec(entries).map_err(|_| invalid())?;
        let value = Envelope { schema: "gsw.atomic_trust_store/1".into(), entries: entries.clone(), sha256: sha256(&canonical) };
        let bytes = serde_json::to_vec(&value).map_err(|_| invalid())?;
        if bytes.len() > MAX_JOURNAL || entries.len() > 16_384 { return Err(invalid()); }
        strict_json(&bytes, MAX_JOURNAL)?;
        atomic_write(&self.directory.join(JOURNAL), &bytes, tag)
    }
}

fn read_bounded(path: &Path, limit: usize) -> io::Result<Vec<u8>> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_file() || metadata.file_type().is_symlink() || metadata.len() > limit as u64 { return Err(invalid()); }
    #[cfg(windows)] {
        use std::os::windows::fs::MetadataExt;
        if metadata.file_attributes() & 0x400 != 0 { return Err(invalid()); }
    }
    let mut bytes = Vec::new();
    fs::File::open(path)?.take(limit as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > limit { return Err(invalid()); }
    Ok(bytes)
}

fn fixture_fault(tag: &str, phase: &str) -> io::Result<()> {
    #[cfg(feature = "gsw-fixture-store-faults")]
    if std::env::var("GSW_UPDATE_FIXTURE_STORE_FAULT").ok().as_deref() == Some(&format!("{tag}:{phase}")) {
        return Err(io::Error::other("fixture storage fault"));
    }
    let _ = (tag, phase);
    Ok(())
}

fn atomic_write(destination: &Path, bytes: &[u8], tag: &str) -> io::Result<()> {
    let parent = destination.parent().ok_or_else(invalid)?;
    let mut temporary = tempfile::Builder::new().prefix(".gsw-trust-").tempfile_in(parent)?;
    temporary.write_all(bytes)?;
    temporary.as_file().sync_all()?;
    fixture_fault(tag, "before-replace")?;
    #[cfg(windows)] {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::Storage::FileSystem::{MoveFileExW, SetFileAttributesW, FILE_ATTRIBUTE_NORMAL, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH};
        let from: Vec<u16> = temporary.path().as_os_str().encode_wide().chain(Some(0)).collect();
        let to: Vec<u16> = destination.as_os_str().encode_wide().chain(Some(0)).collect();
        // Match tempfile's persist guarantee: do not leave durable state marked
        // FILE_ATTRIBUTE_TEMPORARY (Windows may otherwise defer its writeback).
        if unsafe { SetFileAttributesW(from.as_ptr(), FILE_ATTRIBUTE_NORMAL) } == 0 { return Err(io::Error::last_os_error()); }
        temporary.as_file().sync_all()?;
        if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) } == 0 {
            return Err(io::Error::last_os_error());
        }
    }
    #[cfg(not(windows))] {
        temporary.persist(destination).map_err(|e| e.error)?;
        fs::File::open(parent)?.sync_all()?;
    }
    fixture_fault(tag, "after-replace")
}
