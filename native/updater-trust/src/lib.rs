// SPDX-License-Identifier: GPL-3.0-or-later
//! Native TUF adapter. The public build deliberately has no release trust root
//! until an authorized production profile is provisioned. It never accepts a
//! root, CA, feed URL or unsafe verification switch from runtime requests.

use std::ffi::{c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Mutex;

mod engine;
mod network;
mod policy;

// The caller's installation lock is cross-process. This additionally prevents
// re-entrant C ABI callers in one helper from interleaving journal transactions.
static REQUEST_LOCK: Mutex<()> = Mutex::new(());

#[repr(C)]
pub struct GswTrustBuffer {
    pub data: *mut u8,
    pub length: usize,
}

type Cancel = Option<unsafe extern "C" fn(*mut c_void) -> c_int>;

fn buffer(bytes: &[u8]) -> GswTrustBuffer {
    let owned = bytes.to_vec().into_boxed_slice();
    let length = owned.len();
    GswTrustBuffer { data: Box::into_raw(owned) as *mut u8, length }
}

#[no_mangle]
pub extern "C" fn gsw_update_trust_request(
    json: *const u8, length: usize, cancel: Cancel, context: *mut c_void,
) -> GswTrustBuffer {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if json.is_null() || length == 0 || length > 16 * 1024 {
            return buffer(br#"{"schema":1,"ok":false,"code":"INVALID_REQUEST"}"#);
        }
        if let Some(callback) = cancel {
            if unsafe { callback(context) } != 0 {
                return buffer(br#"{"schema":1,"ok":false,"code":"CANCELLED"}"#);
            }
        }
        let bytes = unsafe { std::slice::from_raw_parts(json, length) };
        let outcome = (|| {
            let request = policy::Request::parse(bytes)?;
            let profile = policy::Profile::compiled()?.ok_or("NOT_CONFIGURED")?;
            // Do not make a cancelled/re-entrant caller wait behind another
            // bounded refresh. The public helper serializes intents already.
            let _guard = REQUEST_LOCK.try_lock().map_err(|_| "TRUST_BUSY")?;
            engine::execute(request, profile, network::Cancellation { callback: cancel, context: context as usize })
        })();
        let response = match outcome {
            Ok(value) => value,
            Err(code) => serde_json::json!({"schema":1,"ok":false,"code":code}),
        };
        match serde_json::to_vec(&response) {
            Ok(bytes) if bytes.len() <= 4 * 1024 * 1024 => buffer(&bytes),
            _ => buffer(br#"{"schema":1,"ok":false,"code":"RESPONSE_LIMIT"}"#),
        }
    }));
    result.unwrap_or_else(|_| buffer(br#"{"schema":1,"ok":false,"code":"INTERNAL_ERROR"}"#))
}

#[no_mangle]
pub unsafe extern "C" fn gsw_update_trust_free(value: GswTrustBuffer) {
    if !value.data.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(value.data, value.length)));
    }
}

