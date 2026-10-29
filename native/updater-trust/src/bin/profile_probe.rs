// SPDX-License-Identifier: GPL-3.0-or-later
//! Developer-only observation of the compiled profile. No input adoption,
//! networking, installation state or public-package entry point is provided.

#[path = "../policy.rs"]
#[allow(dead_code)]
mod policy;

fn main() {
    if std::env::args_os().len() != 1 {
        println!(r#"{{"schema":"gsw.profile_probe/1","ok":false,"code":"INVALID_ARGUMENTS"}}"#);
        std::process::exit(64);
    }
    let response = match policy::Profile::compiled() {
        Ok(None) => serde_json::json!({"schema":"gsw.profile_probe/1","ok":true,"code":"NOT_CONFIGURED"}),
        Ok(Some(profile)) => match profile.identity() {
            Ok(identity) => serde_json::json!({"schema":"gsw.profile_probe/1","ok":true,
                "code":"CONFIGURED","product":profile.product,"platform":profile.platform,"identity":identity}),
            Err(code) => serde_json::json!({"schema":"gsw.profile_probe/1","ok":false,"code":code}),
        },
        Err(code) => serde_json::json!({"schema":"gsw.profile_probe/1","ok":false,"code":code}),
    };
    println!("{response}");
    if response["ok"] != true { std::process::exit(2); }
}
