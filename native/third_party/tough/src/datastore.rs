// Copyright 2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: MIT OR Apache-2.0

use crate::error::{self, Result};
use crate::gsw_store::Journal;
use jiff::Timestamp;
use log::debug;
use serde::Serialize;
use snafu::{ensure, ResultExt};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tempfile::TempDir;
use tokio::sync::{Mutex, RwLock, RwLockReadGuard, RwLockWriteGuard};

/// `Datastore` persists TUF metadata files.
#[derive(Debug, Clone)]
pub(crate) struct Datastore {
    /// A lock around retrieving the datastore path.
    path_lock: Arc<RwLock<DatastorePath>>,
    /// A lock to treat the `system_time` function as a critical section.
    time_lock: Arc<Mutex<()>>,
    namespace: String,
}

impl Datastore {
    pub(crate) fn new(path: Option<PathBuf>, namespace: String) -> Result<Self> {
        let path = match path {
                None => DatastorePath::TempDir(TempDir::new().context(error::DatastoreInitSnafu)?),
                Some(p) => DatastorePath::Path(p),
            };
        Journal::initialize(path.path()).context(error::DatastoreInitSnafu)?;
        Ok(Self {
            path_lock: Arc::new(RwLock::new(path)),
            time_lock: Arc::new(Mutex::new(())),
            namespace,
        })
    }

    async fn read(&self) -> RwLockReadGuard<'_, DatastorePath> {
        self.path_lock.read().await
    }

    async fn write(&self) -> RwLockWriteGuard<'_, DatastorePath> {
        self.path_lock.write().await
    }

    /// Get contents of a file in the datastore. This function is thread safe.
    ///
    /// TODO: [provide a thread safe interface](https://github.com/awslabs/tough/issues/602)
    ///
    pub(crate) async fn bytes(&self, file: &str) -> Result<Option<Vec<u8>>> {
        let lock = &self.read().await;
        let path = lock.path();
        let journal = Journal::open(path).context(error::DatastoreOpenSnafu { path })?;
        journal.get(&self.key(file)).map(|v| v.map(String::into_bytes))
            .context(error::DatastoreOpenSnafu { path })
    }

    fn key(&self, file: &str) -> String {
        if file == "latest_known_time.json" { return "clock/latest_known_time.json".into(); }
        format!("tuf/{}/{file}", self.namespace)
    }

    /// Writes a JSON metadata file in the datastore. This function is thread safe.
    pub(crate) async fn create<T: Serialize>(&self, file: &str, value: &T) -> Result<()> {
        let lock = &self.write().await;
        let path = lock.path();
        let bytes = serde_json::to_vec(value).with_context(|_| error::DatastoreSerializeSnafu {
            what: format!("{file} in datastore"),
            path: path.to_owned(),
        })?;
        let text = String::from_utf8(bytes).expect("serde JSON is UTF-8");
        let journal = Journal::open(path).context(error::DatastoreCreateSnafu { path })?;
        let key = self.key(file);
        journal.transact(file, |entries| {
            if file == "root.json" {
                let value: serde_json::Value = serde_json::from_str(&text)?;
                let version = value["signed"]["version"].as_u64().ok_or_else(|| std::io::Error::other("invalid root version"))?;
                let history = self.key(&format!("root-history/{version}.json"));
                if let Some(existing) = entries.get(&history) {
                    let prior: serde_json::Value = serde_json::from_str(existing)?;
                    if prior != value { return Err(std::io::Error::other("root history conflict")); }
                } else {
                    entries.insert(history, text.clone());
                }
            }
            entries.insert(key, text);
            Ok(())
        }).context(error::DatastoreCreateSnafu { path })
    }

    /// Deletes a file from the datastore. This function is thread safe.
    pub(crate) async fn remove(&self, file: &str) -> Result<()> {
        let lock = self.write().await;
        let path = lock.path();
        debug!("removing journal metadata '{file}'");
        let journal = Journal::open(path).context(error::DatastoreRemoveSnafu { path })?;
        journal.transact(file, |entries| { entries.remove(&self.key(file)); Ok(()) })
            .context(error::DatastoreRemoveSnafu { path })
    }

    /// Ensures that system time has not stepped backward since it was last sampled. This function
    /// is protected by a lock guard to ensure thread safety.
    pub(crate) async fn system_time(&self) -> Result<Timestamp> {
        // Treat this function as a critical section. This lock is not used for anything else.
        let lock = self.time_lock.lock().await;

        let file = "latest_known_time.json";
        // Load the latest known system time, if it exists
        let poss_latest_known_time = self
            .bytes(file)
            .await?
            .map(|b| serde_json::from_slice::<Timestamp>(&b));

        // Get 'current' system time
        let sys_time = Timestamp::now();

        if let Some(latest_known_time) = poss_latest_known_time {
            let clock_path = self.read().await.path().to_owned();
            let latest_known_time = latest_known_time.with_context(|_| error::DatastoreSerializeSnafu {
                what: "latest known time in datastore".to_owned(),
                path: clock_path,
            })?;
            // Make sure the sampled system time did not go back in time
            ensure!(
                sys_time >= latest_known_time,
                error::SystemTimeSteppedBackwardSnafu {
                    sys_time,
                    latest_known_time
                }
            );
        }
        // Store the latest known time
        // Serializes RFC3339 time string and store to datastore
        self.create(file, &sys_time).await?;

        // Explicitly drop the lock to avoid any compiler optimization.
        drop(lock);
        Ok(sys_time)
    }
}

/// Because `TempDir` is an RAII object, we need to hold on to it. This private enum allows us to
/// hold either a `TempDir` or a `PathBuf` depending on whether or not the user wants to manage the
/// directory.
#[derive(Debug)]
enum DatastorePath {
    /// Path to a user-managed directory.
    Path(PathBuf),
    /// A `TempDir` that we created on the user's behalf.
    TempDir(TempDir),
}

impl DatastorePath {
    /// Provides convenient access to the underlying filepath.
    fn path(&self) -> &Path {
        match self {
            DatastorePath::Path(p) => p,
            DatastorePath::TempDir(t) => t.path(),
        }
    }
}
