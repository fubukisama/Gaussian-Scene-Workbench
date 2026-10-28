// SPDX-License-Identifier: GPL-3.0-or-later
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use tough::gsw_store::{sha256, strict_json};
use url::Url;

pub type Result<T> = std::result::Result<T, &'static str>;
pub const METADATA_LIMIT: usize = 1024 * 1024;
pub const DESCRIPTOR_LIMIT: usize = 256 * 1024;
#[cfg(feature = "fixture-profile")]
pub const FIXTURE_PRODUCT: &str = "Fubukisama.GaussianSceneWorkbench.UpdateFixture";

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
    pub schema: u64,
    pub operation: String,
    pub installation_root: String,
    pub channel: String,
    pub expected_build_id: Option<String>,
}
impl Request {
    pub fn parse(bytes: &[u8]) -> Result<Self> {
        let value = strict_json(bytes, 16 * 1024).map_err(|_| "INVALID_REQUEST")?;
        let request: Self = serde_json::from_value(value).map_err(|_| "INVALID_REQUEST")?;
        if request.schema != 1 || !matches!(request.operation.as_str(), "resolve" | "verify_cached" | "inspect_accepted")
            || !matches!(request.channel.as_str(), "stable" | "preview") || request.installation_root.len() > 1024
            || request.expected_build_id.as_deref().is_some_and(|v| !safe_token(v))
            || (request.operation == "verify_cached" && request.expected_build_id.is_none()) {
            return Err("INVALID_REQUEST");
        }
        Ok(request)
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Channel {
    pub metadata_base: String,
    pub targets_base: String,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
#[cfg_attr(not(feature = "fixture-profile"), allow(dead_code))]
pub struct Profile {
    pub schema: String,
    pub product: String,
    pub platform: String,
    pub channels: BTreeMap<String, Channel>,
    pub approved_hosts: Vec<String>,
    pub seed_root: Value,
    #[serde(default)]
    pub fixture_ca_pem: Option<String>,
}
impl Profile {
    pub fn compiled() -> Result<Option<Self>> {
        #[cfg(not(feature = "fixture-profile"))] { Ok(None) }
        #[cfg(feature = "fixture-profile")] {
            let bytes = include_bytes!(concat!(env!("OUT_DIR"), "/fixture-profile.json"));
            let value = strict_json(bytes, 2 * 1024 * 1024).map_err(|_| "PROFILE_INVALID")?;
            let profile: Self = serde_json::from_value(value).map_err(|_| "PROFILE_INVALID")?;
            profile.validate()?;
            Ok(Some(profile))
        }
    }
    #[cfg(feature = "fixture-profile")]
    fn validate(&self) -> Result<()> {
        if self.schema != "gsw.update_profile/1" || self.product != FIXTURE_PRODUCT || self.platform != "windows-x64"
            || self.channels.len() != 2 || !self.channels.contains_key("stable") || !self.channels.contains_key("preview")
            || self.approved_hosts.is_empty() || self.approved_hosts.len() > 16
            || self.approved_hosts.iter().any(|h| h.is_empty() || h.len() > 253 || h != &h.to_ascii_lowercase()
                || !h.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-'))
            || self.fixture_ca_pem.as_ref().is_none_or(|v| v.is_empty() || v.len() > 64 * 1024) {
            return Err("PROFILE_INVALID");
        }
        for channel in self.channels.values() {
            for base in [&channel.metadata_base, &channel.targets_base] {
                let url = self.url(base)?;
                if url.query().is_some() || !url.path().ends_with('/') { return Err("PROFILE_INVALID"); }
            }
        }
        validate_metadata(&self.seed_root, "root")?;
        Ok(())
    }
    pub fn url(&self, value: &str) -> Result<Url> {
        if value.len() > 2048 { return Err("URL_REJECTED"); }
        let url = Url::parse(value).map_err(|_| "URL_REJECTED")?;
        if url.scheme() != "https" || !url.username().is_empty() || url.password().is_some() || url.fragment().is_some()
            || !url.host_str().is_some_and(|h| self.approved_hosts.iter().any(|approved| approved == h)) {
            return Err("URL_REJECTED");
        }
        #[cfg(not(feature = "fixture-profile"))]
        if url.port().is_some_and(|port| port != 443) { return Err("URL_REJECTED"); }
        Ok(url)
    }
    pub fn seed_bytes(&self) -> Result<Vec<u8>> { serde_json::to_vec(&self.seed_root).map_err(|_| "PROFILE_INVALID") }
    pub fn identity(&self) -> Result<String> {
        Ok(format!("{}:{}:{}", self.product, self.platform, sha256(&self.seed_bytes()?)))
    }
}

pub fn safe_token(value: &str) -> bool {
    !value.is_empty() && value.len() <= 96 && value.as_bytes()[0].is_ascii_alphanumeric()
        && value.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_' || b == b'.')
        && !value.ends_with('.') && !value.contains("..")
        && !matches!(value.split('.').next().unwrap_or("").to_ascii_uppercase().as_str(),
            "CON" | "PRN" | "AUX" | "NUL" | "COM1" | "COM2" | "COM3" | "COM4" | "COM5" | "COM6" | "COM7" | "COM8" | "COM9" |
            "LPT1" | "LPT2" | "LPT3" | "LPT4" | "LPT5" | "LPT6" | "LPT7" | "LPT8" | "LPT9")
}
pub fn is_hex(value: &str, length: usize) -> bool { value.len() == length && value.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) }
fn exact(value: &Value, keys: &[&str]) -> Result<()> {
    let object = value.as_object().ok_or("METADATA_PROFILE_REJECTED")?;
    if object.len() != keys.len() || object.keys().any(|k| !keys.contains(&k.as_str())) { return Err("METADATA_PROFILE_REJECTED"); }
    Ok(())
}
fn positive(value: &Value, maximum: u64) -> Result<u64> {
    let n = value.as_u64().ok_or("METADATA_PROFILE_REJECTED")?;
    if n == 0 || n > maximum { return Err("METADATA_PROFILE_REJECTED"); }
    Ok(n)
}
fn hash_object(value: &Value) -> Result<()> {
    exact(value, &["sha256"])?;
    if !value["sha256"].as_str().is_some_and(|v| is_hex(v, 64)) { return Err("METADATA_PROFILE_REJECTED"); }
    Ok(())
}
fn metafile(value: &Value) -> Result<()> {
    exact(value, &["version", "length", "hashes"])?;
    positive(&value["version"], tough::gsw_store::MAX_SAFE_INTEGER)?;
    positive(&value["length"], METADATA_LIMIT as u64)?;
    hash_object(&value["hashes"])
}

/// Explicit GSW profile: Ed25519 top-level roles, SHA-256, no delegations or
/// unknown critical fields. Tough still performs all signature/role/hash/time
/// verification; this only bounds the supported wire language.
pub fn validate_metadata(value: &Value, role: &str) -> Result<()> {
    exact(value, &["signed", "signatures"])?;
    let signed = &value["signed"];
    let keys: &[&str] = match role {
        "root" => &["_type", "spec_version", "version", "expires", "consistent_snapshot", "keys", "roles"],
        "timestamp" | "snapshot" => &["_type", "spec_version", "version", "expires", "meta"],
        "targets" => &["_type", "spec_version", "version", "expires", "targets"],
        _ => return Err("METADATA_PROFILE_REJECTED"),
    };
    exact(signed, keys)?;
    if signed["_type"].as_str() != Some(role) || !signed["expires"].as_str().is_some_and(|v| v.len() <= 64)
        || !signed["spec_version"].as_str().is_some_and(|v| {
            let parts: Vec<_> = v.split('.').collect();
            parts.len() == 3 && parts[0] == "1" && parts[1] == "0" && parts[2].parse::<u64>().is_ok_and(|n| n <= 37)
        }) { return Err("METADATA_PROFILE_REJECTED"); }
    positive(&signed["version"], tough::gsw_store::MAX_SAFE_INTEGER)?;
    let signatures = value["signatures"].as_array().ok_or("METADATA_PROFILE_REJECTED")?;
    if signatures.is_empty() || signatures.len() > 64 { return Err("METADATA_PROFILE_REJECTED"); }
    for signature in signatures {
        exact(signature, &["keyid", "sig"])?;
        if !signature["keyid"].as_str().is_some_and(|v| is_hex(v, 64)) || !signature["sig"].as_str().is_some_and(|v| is_hex(v, 128)) {
            return Err("METADATA_PROFILE_REJECTED");
        }
    }
    match role {
        "root" => {
            if !signed["consistent_snapshot"].is_boolean() { return Err("METADATA_PROFILE_REJECTED"); }
            let keys = signed["keys"].as_object().ok_or("METADATA_PROFILE_REJECTED")?;
            if keys.is_empty() || keys.len() > 64 { return Err("METADATA_PROFILE_REJECTED"); }
            for (keyid, key) in keys {
                exact(key, &["keytype", "scheme", "keyval"])?;
                exact(&key["keyval"], &["public"])?;
                if !is_hex(keyid, 64) || key["keytype"] != "ed25519" || key["scheme"] != "ed25519"
                    || !key["keyval"]["public"].as_str().is_some_and(|v| is_hex(v, 64)) { return Err("METADATA_PROFILE_REJECTED"); }
            }
            exact(&signed["roles"], &["root", "timestamp", "snapshot", "targets"])?;
            for role in ["root", "timestamp", "snapshot", "targets"] {
                let role = &signed["roles"][role];
                exact(role, &["keyids", "threshold"])?;
                let ids = role["keyids"].as_array().ok_or("METADATA_PROFILE_REJECTED")?;
                let threshold = positive(&role["threshold"], 16)?;
                let mut unique = BTreeSet::new();
                if ids.is_empty() || ids.len() > 16 || threshold > ids.len() as u64 { return Err("METADATA_PROFILE_REJECTED"); }
                for id in ids {
                    let id = id.as_str().ok_or("METADATA_PROFILE_REJECTED")?;
                    if !keys.contains_key(id) || !unique.insert(id) { return Err("METADATA_PROFILE_REJECTED"); }
                }
            }
        }
        "timestamp" => { exact(&signed["meta"], &["snapshot.json"])?; metafile(&signed["meta"]["snapshot.json"])?; }
        "snapshot" => { exact(&signed["meta"], &["targets.json"])?; metafile(&signed["meta"]["targets.json"])?; }
        "targets" => {
            exact(&signed["targets"], &["release.json"])?;
            let target = &signed["targets"]["release.json"];
            exact(target, &["length", "hashes"])?;
            positive(&target["length"], DESCRIPTOR_LIMIT as u64)?;
            hash_object(&target["hashes"])?;
        }
        _ => unreachable!(),
    }
    Ok(())
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Offer {
    pub schema: String,
    pub product: String,
    pub platform: String,
    pub channel: String,
    pub display_version: String,
    pub build_id: String,
    pub release_sequence: u64,
    pub metadata_revision: u64,
    pub source_commit: String,
    pub source_tree: String,
    pub archive_url: String,
    pub archive_length: u64,
    pub archive_sha256: String,
    pub payload_manifest_sha256: String,
    pub corresponding_source_sha256: String,
    pub unpacked_length: u64,
    pub file_count: u64,
    pub minimum_bootstrap_protocol: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub release_notes: Option<BTreeMap<String, String>>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub revoked_build_ids: Vec<String>,
}
impl Offer {
    pub fn parse(bytes: &[u8], profile: &Profile, channel: &str) -> Result<Self> {
        let value = strict_json(bytes, DESCRIPTOR_LIMIT).map_err(|_| "OFFER_REJECTED")?;
        let offer: Self = serde_json::from_value(value).map_err(|_| "OFFER_REJECTED")?;
        offer.validate(profile, channel)?;
        Ok(offer)
    }
    pub fn validate(&self, profile: &Profile, channel: &str) -> Result<()> {
        if self.schema != "gsw.update_offer/1" || self.product != profile.product || self.platform != profile.platform
            || self.channel != channel || !safe_token(&self.build_id) || self.display_version.is_empty()
            || self.display_version.len() > 128 || self.display_version.chars().any(char::is_control)
            || self.release_sequence == 0 || self.metadata_revision == 0
            || self.release_sequence > tough::gsw_store::MAX_SAFE_INTEGER || self.metadata_revision > tough::gsw_store::MAX_SAFE_INTEGER
            || !is_hex(&self.source_commit, 40) || !is_hex(&self.source_tree, 40)
            || !is_hex(&self.archive_sha256, 64) || !is_hex(&self.payload_manifest_sha256, 64) || !is_hex(&self.corresponding_source_sha256, 64)
            || self.archive_length == 0 || self.archive_length > 4 * 1024 * 1024 * 1024
            || self.unpacked_length == 0 || self.unpacked_length > 16 * 1024 * 1024 * 1024
            || self.file_count == 0 || self.file_count > 100_000 || self.minimum_bootstrap_protocol != 2
            || self.revoked_build_ids.len() > 1024 || self.revoked_build_ids.iter().any(|v| !safe_token(v)) {
            return Err("OFFER_REJECTED");
        }
        profile.url(&self.archive_url)?;
        if let Some(notes) = &self.release_notes {
            if notes.len() != 3 || ["zh_CN", "en_US", "ja_JP"].iter().any(|key| !notes.contains_key(*key))
                || notes.values().any(|s| s.len() > 4096 || s.chars().any(|c| c.is_control() && c != '\n' && c != '\t')) {
                return Err("OFFER_REJECTED");
            }
        }
        Ok(())
    }
    pub fn immutable_identity(&self) -> Result<String> {
        let mut value = serde_json::to_value(self).map_err(|_| "OFFER_REJECTED")?;
        let map = value.as_object_mut().ok_or("OFFER_REJECTED")?;
        for key in ["channel", "releaseSequence", "metadataRevision", "releaseNotes", "revokedBuildIds"] { map.remove(key); }
        Ok(sha256(&serde_json::to_vec(&value).map_err(|_| "OFFER_REJECTED")?))
    }
    pub fn ledger_offer(&self) -> Self { let mut result = self.clone(); result.release_notes = None; result }
}
