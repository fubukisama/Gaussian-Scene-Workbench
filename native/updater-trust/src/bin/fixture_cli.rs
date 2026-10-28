// SPDX-License-Identifier: GPL-3.0-or-later
//! Remote-test driver of the EXACT static-library C ABI. Never installed.
use gsw_updater_trust::{gsw_update_trust_free, gsw_update_trust_request};
use std::ffi::{c_int, c_void};
use std::io::{Read, Write};
use std::time::{Duration, Instant};

struct Cancel { start: Instant, after: Option<Duration> }
unsafe extern "C" fn cancelled(context: *mut c_void) -> c_int {
    let state = &*(context as *const Cancel);
    c_int::from(state.after.is_some_and(|after| state.start.elapsed() >= after))
}
fn main() {
    let arguments: Vec<_> = std::env::args_os().collect();
    if arguments.len() != 2 {
        eprintln!("fixture CLI requires one request JSON file, or - for stdin");
        std::process::exit(64);
    }
    let mut bytes = Vec::new();
    let read = if arguments[1] == "-" {
        std::io::stdin().lock().take(16 * 1024 + 1).read_to_end(&mut bytes)
    } else {
        std::fs::File::open(&arguments[1]).and_then(|file| file.take(16 * 1024 + 1).read_to_end(&mut bytes))
    };
    if read.is_err() { eprintln!("fixture request unavailable"); std::process::exit(66); }
    let mut state = Cancel { start: Instant::now(), after: std::env::var("GSW_UPDATE_FIXTURE_CANCEL_AFTER_MS")
        .ok().and_then(|v| v.parse::<u64>().ok()).map(Duration::from_millis) };
    let buffer = gsw_update_trust_request(bytes.as_ptr(), bytes.len(), Some(cancelled), &mut state as *mut Cancel as *mut c_void);
    let result = unsafe { std::slice::from_raw_parts(buffer.data, buffer.length) };
    let ok = serde_json::from_slice::<serde_json::Value>(result).ok().is_some_and(|v| v["ok"] == true);
    let written = std::io::stdout().lock().write_all(result);
    unsafe { gsw_update_trust_free(buffer); }
    if written.is_err() { std::process::exit(74); }
    if !ok { std::process::exit(2); }
}
