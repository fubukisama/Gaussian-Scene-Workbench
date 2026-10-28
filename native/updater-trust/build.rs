// SPDX-License-Identifier: GPL-3.0-or-later
use std::{env, fs, path::PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=GSW_UPDATER_FIXTURE_PROFILE_PATH");
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo OUT_DIR"));
    if env::var_os("CARGO_FEATURE_FIXTURE_PROFILE").is_some() {
        let source = PathBuf::from(env::var_os("GSW_UPDATER_FIXTURE_PROFILE_PATH")
            .expect("fixture-profile needs a compile-time fixture profile"));
        assert!(source.is_absolute(), "fixture profile must be an absolute build input");
        let bytes = fs::read(&source).expect("read fixture profile build input");
        assert!(bytes.len() <= 2 * 1024 * 1024, "fixture profile build input too large");
        println!("cargo:rerun-if-changed={}", source.display());
        fs::write(output.join("fixture-profile.json"), bytes).expect("embed fixture profile");
    }
}

