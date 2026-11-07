//! Regression test for the Windows bootstrap hang (Arti issues #2678 and #2726):
//! saturating-time 0.5.0 loops forever on Windows the first time a SystemTime
//! limit is needed, which Arti hits while parsing the consensus. The fix is
//! vendored in patches/saturating-time (see PATCHED.md). On Windows this test
//! times out (fails) if the patch is dropped while upstream is still unfixed.

use std::sync::mpsc;
use std::thread;
use std::time::{Duration, SystemTime};

use saturating_time::SaturatingTime;

fn finishes_within<T: Send + 'static>(limit: Duration, f: impl FnOnce() -> T + Send + 'static) -> Option<T> {
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let _ = tx.send(f());
    });
    rx.recv_timeout(limit).ok()
}

#[test]
fn ordinary_saturating_sub_returns_promptly() {
    // The upstream reproducer: an ordinary subtraction (no saturation needed).
    let now = SystemTime::now();
    let result = finishes_within(Duration::from_secs(10), move || SaturatingTime::saturating_sub(now, Duration::from_secs(1)))
        .expect("saturating_sub hung (the Windows find_limit loop)");
    assert_eq!(result, now - Duration::from_secs(1));
}

#[test]
fn system_time_limits_are_found_and_saturate() {
    let (min, max) = finishes_within(Duration::from_secs(10), || {
        (<SystemTime as SaturatingTime>::min_value(), <SystemTime as SaturatingTime>::max_value())
    })
    .expect("finding SystemTime's limits hung");
    assert!(min < SystemTime::UNIX_EPOCH);
    assert!(max > SystemTime::now());
    assert_eq!(SaturatingTime::saturating_sub(min, Duration::from_secs(1)), min);
    assert_eq!(SaturatingTime::saturating_add(max, Duration::from_secs(1)), max);
    // Nothing below the platform's resolution can be subtracted from the minimum.
    assert!(min.checked_sub(Duration::from_secs(1)).is_none());
}
