// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

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
    /// Tor is up. The onion service may still be failing: `onion_reachable`
    /// says so, and the apps' sealed backup route takes over then.
    Connected,
    /// Tor itself can't make progress (stopped, blocked, fatal, or not
    /// bootstrapped in time). Nothing is sent; the apps keep their outbox and
    /// retry when this clears.
    ///
    /// Onion failures never cause this state: it closes the apps' gate, which
    /// would block the backup route too, and no stream could succeed to clear it
    /// (the 2026-11-16 lockout).
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

/// Consecutive onion stream failures after which the detail says the onion isn't answering.
const ONION_FAILURES_TO_REPORT: u32 = 2;
/// Not connected this long after starting: show Unavailable (Tor keeps trying).
/// Arti can take a long time to call a network blocked, and a spinner that
/// never ends hides the problem from the user.
const BOOTSTRAP_DEADLINE: std::time::Duration = std::time::Duration::from_secs(45);
/// With bridges: Snowflake routinely takes 1-2 minutes, and Arti may call the
/// bootstrap "blocked" on the way (seen at 101 s on a run that connected at
/// 132 s). Neither counts as Unavailable until this long after starting.
pub(crate) const BRIDGE_BOOTSTRAP_DEADLINE: std::time::Duration = std::time::Duration::from_secs(180);

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
    /// How long to show Connecting before Unavailable.
    deadline: std::time::Duration,
    /// Bridges: Arti's "blocked" only counts once the deadline has passed.
    patient: bool,
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
            deadline: BOOTSTRAP_DEADLINE,
            patient: false,
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

    /// Bridges are in use: wait longer before calling Tor unavailable (see BRIDGE_BOOTSTRAP_DEADLINE).
    pub(crate) fn use_bridge_deadline(&self) {
        let mut inner = self.lock();
        inner.deadline = BRIDGE_BOOTSTRAP_DEADLINE;
        inner.patient = true;
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
        } else if let Some(blocked) = inner.blocked.as_ref().filter(|_| !inner.patient || inner.started.elapsed() >= inner.deadline) {
            (TorState::Unavailable, blocked.clone())
        } else if !inner.ready && inner.started.elapsed() >= inner.deadline {
            (TorState::Unavailable, format!("Tor hasn't connected yet ({})", inner.summary))
        } else if !inner.ready {
            (TorState::Connecting, inner.summary.clone())
        } else if inner.consecutive_failures >= ONION_FAILURES_TO_REPORT {
            (TorState::Connected, "Tor is connected; the onion service isn't answering".to_owned())
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
    fn state_follows_bootstrap_and_blockage_not_onion_failures() {
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
        cell.on_stream(false);
        // The onion failing is not Tor failing: the gate stays open for the backup route.
        assert_eq!(cell.snapshot().state, TorState::Connected, "onion failures never close the gate");
        assert!(cell.snapshot().detail.contains("onion service isn't answering"));
        assert_eq!(cell.snapshot().onion_reachable, Some(false));
        cell.on_stream(true);
        assert_eq!(cell.snapshot().state, TorState::Connected);
        assert_eq!(cell.snapshot().stats.tor_streams_failed, 3);
        assert_eq!(cell.snapshot().stats.tor_streams_opened, 1);

        cell.on_stopped();
        assert_eq!(cell.snapshot().state, TorState::Unavailable);
    }

    #[test]
    fn bridges_get_longer_before_unavailable() {
        let started_long_ago = |cell: &StatusCell, secs: u64| {
            cell.lock().started = std::time::Instant::now() - std::time::Duration::from_secs(secs);
        };

        // Without bridges: 45 s, and a blockage counts at once.
        let direct = StatusCell::default();
        started_long_ago(&direct, 60);
        assert_eq!(direct.snapshot().state, TorState::Unavailable);
        let direct = StatusCell::default();
        direct.on_bootstrap(0.15, false, "15%".into(), Some("Can't make progress".into()));
        assert_eq!(direct.snapshot().state, TorState::Unavailable);

        // With bridges: still connecting at 60 s and through a blockage, until 180 s.
        let bridges = StatusCell::default();
        bridges.use_bridge_deadline();
        started_long_ago(&bridges, 101);
        bridges.on_bootstrap(0.15, false, "15%".into(), Some("Can't make progress".into()));
        assert_eq!(bridges.snapshot().state, TorState::Connecting);
        started_long_ago(&bridges, 181);
        assert_eq!(bridges.snapshot().state, TorState::Unavailable);
        assert_eq!(bridges.snapshot().detail, "Can't make progress");
        bridges.on_bootstrap(1.0, true, "done".into(), None);
        assert_eq!(bridges.snapshot().state, TorState::Connected);
    }
}
