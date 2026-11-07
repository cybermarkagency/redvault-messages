//! What the apps show: Tor connecting / connected / unavailable, plus counters
//! the leak tests read.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use serde::Serialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum TorState {
    /// Bootstrapping: messages wait in the app's encrypted outbox.
    Connecting,
    /// Tor is up and the last onion stream (if any) worked.
    Connected,
    /// Tor can't make progress, or the onion service keeps failing. Nothing is
    /// sent; the apps keep their outbox and retry when this clears.
    Unavailable,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Stats {
    /// SOCKS connections accepted on the loopback port.
    pub socks_connections: u64,
    /// Refused: missing or wrong SOCKS credentials.
    pub refused_auth: u64,
    /// Refused: destination is not on the onion allowlist.
    pub refused_target: u64,
    /// Refused: IPv4/IPv6 literal instead of a hostname.
    pub refused_address_type: u64,
    /// Refused: BIND / UDP ASSOCIATE.
    pub refused_command: u64,
    /// Streams opened to the onion service over Tor.
    pub tor_streams_opened: u64,
    /// Stream attempts over Tor that failed (Tor down, service unreachable, timeout).
    pub tor_streams_failed: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    pub state: TorState,
    /// Bootstrap progress, 0-100.
    pub progress: u8,
    /// Human-readable detail (Arti's bootstrap summary or the failure reason).
    pub detail: String,
    /// Whether the last stream to the onion service worked (None before the first one).
    pub onion_reachable: Option<bool>,
    pub stats: Stats,
}

/// Consecutive onion stream failures after which the state turns Unavailable.
const FAILURES_BEFORE_UNAVAILABLE: u32 = 2;
/// Not connected this long after starting: show Unavailable (Tor keeps trying).
/// Arti can take a long time to call a network blocked, and a spinner that
/// never ends hides the problem from the user.
const BOOTSTRAP_DEADLINE: std::time::Duration = std::time::Duration::from_secs(45);

struct Inner {
    started: std::time::Instant,
    progress: u8,
    ready: bool,
    summary: String,
    blocked: Option<String>,
    fatal: Option<String>,
    consecutive_failures: u32,
    onion_reachable: Option<bool>,
    stopped: bool,
}

impl Default for Inner {
    fn default() -> Self {
        Inner {
            started: std::time::Instant::now(),
            progress: 0,
            ready: false,
            summary: "Starting Tor".to_owned(),
            blocked: None,
            fatal: None,
            consecutive_failures: 0,
            onion_reachable: None,
            stopped: false,
        }
    }
}

#[derive(Default)]
pub(crate) struct StatusCell {
    inner: Mutex<Inner>,
    socks_connections: AtomicU64,
    refused_auth: AtomicU64,
    refused_target: AtomicU64,
    refused_address_type: AtomicU64,
    refused_command: AtomicU64,
    tor_streams_opened: AtomicU64,
    tor_streams_failed: AtomicU64,
}

pub(crate) enum Counter {
    SocksConnection,
    RefusedAuth,
    RefusedTarget,
    RefusedAddressType,
    RefusedCommand,
}

impl StatusCell {
    fn lock(&self) -> std::sync::MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub(crate) fn on_bootstrap(&self, progress: f32, ready: bool, summary: String, blocked: Option<String>) {
        let mut inner = self.lock();
        inner.progress = (progress.clamp(0.0, 1.0) * 100.0).round() as u8;
        inner.ready = ready;
        inner.summary = summary;
        inner.blocked = blocked;
    }

    pub(crate) fn on_fatal(&self, reason: String) {
        self.lock().fatal = Some(reason);
    }

    pub(crate) fn on_stopped(&self) {
        self.lock().stopped = true;
    }

    pub(crate) fn on_stream(&self, ok: bool) {
        let mut inner = self.lock();
        if ok {
            self.tor_streams_opened.fetch_add(1, Ordering::Relaxed);
            inner.consecutive_failures = 0;
        } else {
            self.tor_streams_failed.fetch_add(1, Ordering::Relaxed);
            inner.consecutive_failures = inner.consecutive_failures.saturating_add(1);
        }
        inner.onion_reachable = Some(ok);
    }

    pub(crate) fn count(&self, counter: Counter) {
        let cell = match counter {
            Counter::SocksConnection => &self.socks_connections,
            Counter::RefusedAuth => &self.refused_auth,
            Counter::RefusedTarget => &self.refused_target,
            Counter::RefusedAddressType => &self.refused_address_type,
            Counter::RefusedCommand => &self.refused_command,
        };
        cell.fetch_add(1, Ordering::Relaxed);
    }

    pub(crate) fn snapshot(&self) -> Status {
        let inner = self.lock();
        let (state, detail) = if inner.stopped {
            (TorState::Unavailable, "Tor is stopped".to_owned())
        } else if let Some(fatal) = &inner.fatal {
            (TorState::Unavailable, fatal.clone())
        } else if let Some(blocked) = &inner.blocked {
            (TorState::Unavailable, blocked.clone())
        } else if !inner.ready && inner.started.elapsed() >= BOOTSTRAP_DEADLINE {
            (TorState::Unavailable, format!("Tor hasn't connected yet ({})", inner.summary))
        } else if !inner.ready {
            (TorState::Connecting, inner.summary.clone())
        } else if inner.consecutive_failures >= FAILURES_BEFORE_UNAVAILABLE {
            (TorState::Unavailable, "The Redvault onion service can't be reached".to_owned())
        } else {
            (TorState::Connected, inner.summary.clone())
        };
        Status {
            state,
            progress: inner.progress,
            detail,
            onion_reachable: inner.onion_reachable,
            stats: Stats {
                socks_connections: self.socks_connections.load(Ordering::Relaxed),
                refused_auth: self.refused_auth.load(Ordering::Relaxed),
                refused_target: self.refused_target.load(Ordering::Relaxed),
                refused_address_type: self.refused_address_type.load(Ordering::Relaxed),
                refused_command: self.refused_command.load(Ordering::Relaxed),
                tor_streams_opened: self.tor_streams_opened.load(Ordering::Relaxed),
                tor_streams_failed: self.tor_streams_failed.load(Ordering::Relaxed),
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_follows_bootstrap_blockage_and_stream_failures() {
        let cell = StatusCell::default();
        assert_eq!(cell.snapshot().state, TorState::Connecting);

        cell.on_bootstrap(0.4, false, "40%".into(), None);
        assert_eq!(cell.snapshot().state, TorState::Connecting);
        assert_eq!(cell.snapshot().progress, 40);

        cell.on_bootstrap(0.4, false, "40%".into(), Some("offline".into()));
        assert_eq!(cell.snapshot().state, TorState::Unavailable);

        cell.on_bootstrap(1.0, true, "done".into(), None);
        assert_eq!(cell.snapshot().state, TorState::Connected);

        cell.on_stream(false);
        assert_eq!(cell.snapshot().state, TorState::Connected, "one failure is tolerated");
        cell.on_stream(false);
        assert_eq!(cell.snapshot().state, TorState::Unavailable);
        assert_eq!(cell.snapshot().onion_reachable, Some(false));
        cell.on_stream(true);
        assert_eq!(cell.snapshot().state, TorState::Connected);
        assert_eq!(cell.snapshot().stats.tor_streams_failed, 2);
        assert_eq!(cell.snapshot().stats.tor_streams_opened, 1);

        cell.on_stopped();
        assert_eq!(cell.snapshot().state, TorState::Unavailable);
    }
}
