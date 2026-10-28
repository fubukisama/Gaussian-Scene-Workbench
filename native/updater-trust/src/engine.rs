// SPDX-License-Identifier: GPL-3.0-or-later
use crate::network::{Cancellation, SecureTransport};
use crate::policy::{is_hex, safe_token, validate_metadata, Offer, Profile, Request, Result, DESCRIPTOR_LIMIT, METADATA_LIMIT};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::time::Duration;
use tough::gsw_store::{durable_anchor, sha256, strict_json, Journal};
use tough::schema::{Root, Signed};
use tough::{ExpirationEnforcement, IntoVec, Limits, RepositoryLoader, TargetName};

const LEDGER: &str = "gsw/accepted";
const PROFILE: &str = "gsw/profile";
const MAX_HISTORY: usize = 1024;

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct HighWater { release_sequence: u64, metadata_revision: u64, build_id: String, descriptor_sha256: String }
#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Accepted { descriptor_sha256: String, descriptor_sha256s: BTreeSet<String>, immutable_identity: String, offer: Offer }
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Ledger {
    highest_by_channel: BTreeMap<String, HighWater>,
    revoked_build_ids: BTreeSet<String>,
    accepted: BTreeMap<String, Accepted>,
}
impl Ledger {
    fn parse(entries: &BTreeMap<String, String>, profile: &Profile) -> Result<Self> {
        let text = entries.get(LEDGER).ok_or("TRUST_STATE_CORRUPT")?;
        let value = strict_json(text.as_bytes(), 3 * 1024 * 1024).map_err(|_| "TRUST_STATE_CORRUPT")?;
        let ledger: Self = serde_json::from_value(value).map_err(|_| "TRUST_STATE_CORRUPT")?;
        if ledger.accepted.len() > MAX_HISTORY || ledger.revoked_build_ids.len() > 8192
            || ledger.revoked_build_ids.iter().any(|id| !safe_token(id)) { return Err("TRUST_STATE_CORRUPT"); }
        for (channel, water) in &ledger.highest_by_channel {
            if !matches!(channel.as_str(), "stable" | "preview") || water.release_sequence == 0 || water.metadata_revision == 0
                || !safe_token(&water.build_id) || !ledger.accepted.get(&water.build_id)
                    .is_some_and(|accepted| accepted.descriptor_sha256s.contains(&water.descriptor_sha256))
                || !is_hex(&water.descriptor_sha256, 64) { return Err("TRUST_STATE_CORRUPT"); }
        }
        for (id, accepted) in &ledger.accepted {
            accepted.offer.validate(profile, &accepted.offer.channel).map_err(|_| "TRUST_STATE_CORRUPT")?;
            if !matches!(accepted.offer.channel.as_str(), "stable" | "preview") || id != &accepted.offer.build_id
                || !is_hex(&accepted.descriptor_sha256, 64) || !accepted.descriptor_sha256s.contains(&accepted.descriptor_sha256)
                || accepted.descriptor_sha256s.is_empty() || accepted.descriptor_sha256s.len() > 64
                || accepted.descriptor_sha256s.iter().any(|digest| !is_hex(digest, 64))
                || accepted.immutable_identity != accepted.offer.immutable_identity()? {
                return Err("TRUST_STATE_CORRUPT");
            }
        }
        Ok(ledger)
    }
    fn accept(&mut self, offer: &Offer, descriptor_sha256: &str) -> Result<()> {
        if let Some(water) = self.highest_by_channel.get(&offer.channel) {
            if offer.release_sequence < water.release_sequence || offer.metadata_revision < water.metadata_revision {
                return Err("RELEASE_ROLLBACK_REJECTED");
            }
            if offer.metadata_revision == water.metadata_revision && descriptor_sha256 != water.descriptor_sha256 {
                return Err("RELEASE_EQUIVOCATION_REJECTED");
            }
            if offer.release_sequence == water.release_sequence && offer.build_id != water.build_id {
                return Err("RELEASE_EQUIVOCATION_REJECTED");
            }
        }
        let identity = offer.immutable_identity()?;
        if let Some(accepted) = self.accepted.get(&offer.build_id) {
            if accepted.immutable_identity != identity { return Err("BUILD_ID_REUSED"); }
        } else if self.accepted.len() >= MAX_HISTORY { return Err("TRUST_HISTORY_FULL"); }
        self.revoked_build_ids.extend(offer.revoked_build_ids.iter().cloned());
        if self.revoked_build_ids.len() > 8192 { return Err("TRUST_HISTORY_FULL"); }
        self.highest_by_channel.insert(offer.channel.clone(), HighWater {
            release_sequence: offer.release_sequence, metadata_revision: offer.metadata_revision,
            build_id: offer.build_id.clone(), descriptor_sha256: descriptor_sha256.into(),
        });
        let accepted = self.accepted.entry(offer.build_id.clone()).or_insert_with(|| Accepted {
            descriptor_sha256: descriptor_sha256.into(), descriptor_sha256s: BTreeSet::new(),
            immutable_identity: identity, offer: offer.ledger_offer(),
        });
        accepted.descriptor_sha256s.insert(descriptor_sha256.into());
        if accepted.descriptor_sha256s.len() > 64 { return Err("TRUST_HISTORY_FULL"); }
        Ok(())
    }
}

fn check_tree(path: &Path) -> Result<()> {
    let mut current = PathBuf::new();
    for component in path.components() {
        if matches!(component, Component::ParentDir | Component::CurDir) { return Err("INSTALLATION_PATH_REJECTED"); }
        current.push(component);
        if matches!(component, Component::Prefix(_)) { continue; }
        match fs::symlink_metadata(&current) {
            Ok(metadata) => {
                if !metadata.is_dir() || metadata.file_type().is_symlink() { return Err("INSTALLATION_PATH_REJECTED"); }
                #[cfg(windows)] { use std::os::windows::fs::MetadataExt;
                    if metadata.file_attributes() & 0x400 != 0 { return Err("INSTALLATION_PATH_REJECTED"); }
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err("INSTALLATION_PATH_REJECTED"),
        }
    }
    Ok(())
}
fn store_path(request: &Request) -> Result<PathBuf> {
    let path = PathBuf::from(&request.installation_root);
    if !path.is_absolute() || !path.is_dir() || request.installation_root.contains('\0') { return Err("INSTALLATION_PATH_REJECTED"); }
    #[cfg(windows)] {
        use std::path::Prefix;
        if !matches!(path.components().next(), Some(Component::Prefix(prefix)) if matches!(prefix.kind(), Prefix::Disk(_))) {
            return Err("INSTALLATION_PATH_REJECTED");
        }
    }
    check_tree(&path)?;
    let store = path.join("updates").join("trust");
    check_tree(&store)?;
    Ok(store)
}

fn root_from(text: &[u8]) -> Result<Signed<Root>> {
    let value = strict_json(text, METADATA_LIMIT).map_err(|_| "TRUST_STATE_CORRUPT")?;
    validate_metadata(&value, "root").map_err(|_| "TRUST_STATE_CORRUPT")?;
    serde_json::from_value(value).map_err(|_| "TRUST_STATE_CORRUPT")
}

fn initialize(store: &Journal, profile: &Profile) -> Result<()> {
    let entries = store.entries().map_err(|_| "TRUST_STATE_CORRUPT")?;
    if !entries.is_empty() { return Ok(()); }
    let seed_bytes = profile.seed_bytes()?;
    let seed = root_from(&seed_bytes)?;
    seed.signed.verify_role(&seed).map_err(|_| "PROFILE_INVALID")?;
    let identity = profile.identity()?;
    let seed_text = String::from_utf8(seed_bytes).map_err(|_| "PROFILE_INVALID")?;
    let ledger = serde_json::to_string(&Ledger::default()).map_err(|_| "INTERNAL_ERROR")?;
    store.transact("bootstrap", |entries| {
        if !entries.is_empty() { return Err(std::io::Error::other("initialization conflict")); }
        entries.insert(PROFILE.into(), identity);
        entries.insert(LEDGER.into(), ledger);
        for channel in ["stable", "preview"] {
            entries.insert(format!("tuf/{channel}/root.json"), seed_text.clone());
            entries.insert(format!("tuf/{channel}/root-history/{}.json", seed.signed.version), seed_text.clone());
        }
        Ok(())
    }).map_err(|_| "TRUST_STATE_WRITE_FAILED")
}

/// Replays the root chain from the compiled public anchor with Tough's old AND
/// new threshold verifier. Offline accepted-history inspection deliberately does
/// not assert the freshness of an old offer for a new network installation.
fn validate_store(entries: &BTreeMap<String, String>, profile: &Profile, channel: &str) -> Result<Vec<u8>> {
    if entries.get(PROFILE) != Some(&profile.identity()?) { return Err("TRUST_PROFILE_MISMATCH"); }
    let ledger = Ledger::parse(entries, profile)?;
    let seed_bytes = profile.seed_bytes()?;
    let seed = root_from(&seed_bytes)?;
    seed.signed.verify_role(&seed).map_err(|_| "PROFILE_INVALID")?;
    let mut selected = None;
    for current_channel in ["stable", "preview"] {
        let prefix = format!("tuf/{current_channel}/root-history/");
        let mut roots = BTreeMap::new();
        for (key, text) in entries.iter().filter(|(key, _)| key.starts_with(&prefix)) {
            let version = key.strip_prefix(&prefix).and_then(|v| v.strip_suffix(".json"))
                .and_then(|v| v.parse::<u64>().ok()).ok_or("TRUST_STATE_CORRUPT")?;
            let root = root_from(text.as_bytes())?;
            if root.signed.version.get() != version || roots.insert(version, (root, text)).is_some() { return Err("TRUST_STATE_CORRUPT"); }
        }
        if roots.is_empty() || roots.len() > 4096 { return Err("TRUST_STATE_CORRUPT"); }
        let initial = roots.get(&seed.signed.version.get()).ok_or("TRUST_STATE_CORRUPT")?;
        if initial.0 != seed { return Err("TRUST_STATE_CORRUPT"); }
        let mut previous = &seed;
        let mut version = seed.signed.version.get();
        for (n, (root, _)) in roots.range((std::ops::Bound::Excluded(version), std::ops::Bound::Unbounded)) {
            if *n != version + 1 { return Err("TRUST_ROOT_CHAIN_GAP"); }
            previous.signed.verify_role(root).map_err(|_| "TRUST_ROOT_CHAIN_REJECTED")?;
            root.signed.verify_role(root).map_err(|_| "TRUST_ROOT_CHAIN_REJECTED")?;
            previous = root;
            version = *n;
        }
        if roots.keys().any(|v| *v < seed.signed.version.get()) { return Err("TRUST_STATE_CORRUPT"); }
        let current = entries.get(&format!("tuf/{current_channel}/root.json")).ok_or("TRUST_STATE_CORRUPT")?;
        if root_from(current.as_bytes())? != *previous { return Err("TRUST_STATE_CORRUPT"); }
        if current_channel == channel { selected = Some(current.as_bytes().to_vec()); }
        let raw_count = ["timestamp", "snapshot", "targets", "release"].iter()
            .filter(|role| entries.contains_key(&format!("raw/{current_channel}/{role}"))).count();
        if raw_count != 0 && raw_count != 4 { return Err("TRUST_STATE_CORRUPT"); }
    }
    for (key, text) in entries {
        if key == PROFILE || key == LEDGER || key.contains("/root-history/") || key.ends_with("/root.json") { continue; }
        if key == "clock/latest_known_time.json" {
            let value = strict_json(text.as_bytes(), 256).map_err(|_| "TRUST_STATE_CORRUPT")?;
            let _: jiff::Timestamp = serde_json::from_value(value).map_err(|_| "TRUST_STATE_CORRUPT")?;
            continue;
        }
        let mut known = false;
        for channel in ["stable", "preview"] {
            for role in ["timestamp", "snapshot", "targets"] {
                if key == &format!("tuf/{channel}/{role}.json") || key == &format!("raw/{channel}/{role}") {
                    let value = strict_json(text.as_bytes(), METADATA_LIMIT).map_err(|_| "TRUST_STATE_CORRUPT")?;
                    validate_metadata(&value, role).map_err(|_| "TRUST_STATE_CORRUPT")?;
                    known = true;
                }
            }
            if key == &format!("raw/{channel}/release") {
                Offer::parse(text.as_bytes(), profile, channel).map_err(|_| "TRUST_STATE_CORRUPT")?;
                known = true;
            }
        }
        if !known { return Err("TRUST_STATE_CORRUPT"); }
    }
    if !ledger.highest_by_channel.is_empty() && !entries.contains_key("clock/latest_known_time.json") { return Err("TRUST_STATE_CORRUPT"); }
    selected.ok_or("TRUST_STATE_CORRUPT")
}

pub fn execute(request: Request, profile: Profile, cancel: Cancellation) -> Result<Value> {
    cancel.check()?;
    let directory = store_path(&request)?;
    let anchor_path = Path::new(&request.installation_root).join(".update-trust-anchor");
    let anchor = format!("gsw.trust_anchor/1\n{}\n", profile.identity()?);
    let anchor_exists = match fs::symlink_metadata(&anchor_path) {
        Ok(_) => true,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => false,
        Err(_) => return Err("TRUST_STATE_UNAVAILABLE"),
    };
    // inspect_accepted is strictly read-only, including on a fresh installation.
    let store = if request.operation == "inspect_accepted" || anchor_exists {
        durable_anchor(&anchor_path, anchor.as_bytes(), false).map_err(|_| "TRUST_ANCHOR_REJECTED")?;
        Journal::open(&directory).map_err(|_| "TRUST_STATE_UNAVAILABLE")?
    } else {
        if directory.try_exists().map_err(|_| "TRUST_STATE_UNAVAILABLE")?
            && fs::read_dir(&directory).map_err(|_| "TRUST_STATE_UNAVAILABLE")?.next().is_some() {
            return Err("TRUST_ANCHOR_REJECTED");
        }
        // Write the external sentinel FIRST. Partial first initialization is an
        // explicit fail-closed repair case, never an implicit fresh trust reset.
        durable_anchor(&anchor_path, anchor.as_bytes(), true).map_err(|_| "TRUST_STATE_WRITE_FAILED")?;
        let store = Journal::initialize(&directory).map_err(|_| "TRUST_STATE_CORRUPT")?;
        initialize(&store, &profile)?;
        store
    };
    let entries = store.entries().map_err(|_| "TRUST_STATE_CORRUPT")?;
    let root = validate_store(&entries, &profile, &request.channel)?;
    if request.operation == "inspect_accepted" {
        let ledger = Ledger::parse(&entries, &profile)?;
        return Ok(json!({"schema":1,"ok":true,"code":"ACCEPTED_STATE","acceptedState":ledger}));
    }
    let cached = if request.operation == "verify_cached" {
        let mut raw = BTreeMap::new();
        for role in ["timestamp", "snapshot", "targets", "release"] {
            raw.insert(role.to_owned(), entries.get(&format!("raw/{}/{role}", request.channel))
                .ok_or("CACHED_METADATA_UNAVAILABLE")?.as_bytes().to_vec());
        }
        Some(raw)
    } else { None };
    let transport = SecureTransport::new(profile.clone(), &request.channel, cancel, cached)?;
    let endpoints = profile.channels.get(&request.channel).ok_or("PROFILE_INVALID")?;
    let metadata_url = profile.url(&endpoints.metadata_base)?;
    let targets_url = profile.url(&endpoints.targets_base)?;
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().map_err(|_| "INTERNAL_ERROR")?;
    let target_bytes = runtime.block_on(async {
        tokio::time::timeout(Duration::from_secs(90), async {
            let repository = RepositoryLoader::new(&root, metadata_url, targets_url)
                .datastore(directory.clone()).gsw_datastore_namespace(&request.channel)
                .expiration_enforcement(ExpirationEnforcement::Safe)
                .limits(Limits { max_root_size: METADATA_LIMIT as u64, max_timestamp_size: METADATA_LIMIT as u64,
                    max_snapshot_size: METADATA_LIMIT as u64, max_targets_size: METADATA_LIMIT as u64, max_root_updates: 64 })
                .transport(transport.clone()).load().await.map_err(|_| "TUF_REJECTED")?;
            cancel.check()?;
            let name = TargetName::new("release.json").map_err(|_| "INTERNAL_ERROR")?;
            let target = repository.read_target(&name).await.map_err(|_| "TUF_REJECTED")?.ok_or("TARGET_UNAVAILABLE")?;
            // IMPORTANT: collect until the final successful digest check. No
            // Offer is parsed or acted on from a partially verified stream.
            let bytes = target.into_vec().await.map_err(|_| "TUF_REJECTED")?;
            if bytes.len() > DESCRIPTOR_LIMIT { return Err("RESPONSE_LIMIT"); }
            Ok(bytes)
        }).await.map_err(|_| "TRUST_OPERATION_TIMEOUT")?
    });
    cancel.check()?;
    let target_bytes = match target_bytes {
        Ok(bytes) => bytes,
        Err(code) => return Err(transport.failure.lock().map_err(|_| "INTERNAL_ERROR")?.unwrap_or(code)),
    };
    let offer = Offer::parse(&target_bytes, &profile, &request.channel)?;
    let descriptor_sha256 = sha256(&target_bytes);
    let current = store.entries().map_err(|_| "TRUST_STATE_CORRUPT")?;
    validate_store(&current, &profile, &request.channel)?;
    let mut ledger = Ledger::parse(&current, &profile)?;
    if request.operation == "verify_cached" && !ledger.accepted.get(&offer.build_id)
        .is_some_and(|accepted| accepted.descriptor_sha256s.contains(&descriptor_sha256)) { return Err("CACHED_OFFER_NOT_ACCEPTED"); }
    ledger.accept(&offer, &descriptor_sha256)?;
    let raw = if request.operation == "resolve" {
        let raw = transport.raw.lock().map_err(|_| "INTERNAL_ERROR")?.clone();
        if ["timestamp", "snapshot", "targets", "release"].iter().any(|role| !raw.contains_key(*role)) { return Err("CACHED_METADATA_UNAVAILABLE"); }
        Some(raw)
    } else { None };
    let serialized = serde_json::to_string(&ledger).map_err(|_| "INTERNAL_ERROR")?;
    if serialized.len() > 3 * 1024 * 1024 { return Err("TRUST_HISTORY_FULL"); }
    store.transact("accepted-offer", |entries| {
        entries.insert(LEDGER.into(), serialized);
        if let Some(raw) = raw {
            for role in ["timestamp", "snapshot", "targets", "release"] {
                let bytes = raw.get(role).ok_or_else(|| std::io::Error::other("missing validated metadata"))?;
                let text = std::str::from_utf8(bytes).map_err(std::io::Error::other)?;
                entries.insert(format!("raw/{}/{role}", request.channel), text.to_owned());
            }
        }
        Ok(())
    }).map_err(|_| "TRUST_STATE_WRITE_FAILED")?;
    // Persisted revocations and high water survive a mismatching caller intent
    // or a revoked candidate, and every later download/activation failure.
    if ledger.revoked_build_ids.contains(&offer.build_id) { return Err("BUILD_REVOKED"); }
    if request.expected_build_id.as_ref().is_some_and(|id| id != &offer.build_id) { return Err("BUILD_ID_MISMATCH"); }
    let transport_policy = json!({"approvedHosts":profile.approved_hosts});
    #[cfg(feature = "fixture-profile")]
    let transport_policy = {
        let mut policy = transport_policy;
        policy["fixtureCaPem"] = json!(profile.fixture_ca_pem);
        policy
    };
    Ok(json!({"schema":1,"ok":true,"code":"VERIFIED","offer":offer,"descriptorSha256":descriptor_sha256,
        "transportPolicy":transport_policy}))
}
