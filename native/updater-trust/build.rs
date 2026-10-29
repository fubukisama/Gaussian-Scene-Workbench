// SPDX-License-Identifier: GPL-3.0-or-later
use std::{env, fs, io::Read, path::PathBuf};

fn embed(output: &std::path::Path, variable: &str, destination: &str) {
    let source = PathBuf::from(env::var_os(variable).expect("profile feature needs an explicit compile-time profile path"));
    assert!(source.is_absolute(), "profile must be an absolute build input");
    let mut bytes = Vec::new();
    fs::File::open(&source).expect("open profile build input").take(2 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes).expect("read profile build input");
    assert!(bytes.len() <= 2 * 1024 * 1024, "profile build input too large");
    println!("cargo:rerun-if-changed={}", source.display());
    fs::write(output.join(destination), bytes).expect("embed profile build input");
}

fn main() {
    println!("cargo:rerun-if-env-changed=GSW_UPDATER_FIXTURE_PROFILE_PATH");
    println!("cargo:rerun-if-env-changed=GSW_UPDATER_PRODUCTION_PROFILE_PATH");
    let output = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo OUT_DIR"));
    let fixture = env::var_os("CARGO_FEATURE_FIXTURE_PROFILE").is_some();
    let production = env::var_os("CARGO_FEATURE_PRODUCTION_PROFILE").is_some();
    assert!(!(fixture && production), "production-profile and fixture-profile are mutually exclusive");
    if fixture {
        embed(&output, "GSW_UPDATER_FIXTURE_PROFILE_PATH", "fixture-profile.json");
    }
    if production {
        embed(&output, "GSW_UPDATER_PRODUCTION_PROFILE_PATH", "production-profile.json");
    }
}

