// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Where the gateway may go: an exact list of v3 onion services, plus at most
//! one HTTPS website reached through a Tor exit.
//!
//! The SOCKS port is the only way the apps reach the network for messaging, so
//! these lists are the routing policy. They are checked on every request;
//! anything that isn't one of these `host:port` pairs is refused.
//!
//! - **Onion targets:** `"<v3>.onion:<port>"`, as always.
//! - **Exit targets** (optional, the sealed backup route of
//!   docs/tor-sealed-fallback.md): one DNS hostname, port 443 only. Requests to
//!   it are already sealed (Oblivious HTTP) and inside TLS, so the exit relay
//!   learns only that someone reaches that site. An app that doesn't list one
//!   never uses an exit relay.
//!
//! IP literals can never be listed, so a hostname the app resolved itself (a
//! DNS leak upstream) can't get through; exit hostnames are resolved by the
//! exit relay, inside Tor.

use std::collections::BTreeSet;
use std::str::FromStr;

use tor_hscrypto::pk::HsId;

/// One allowed destination: a v3 onion address (lowercase, with `.onion`) and a port.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct OnionTarget {
    pub host: String,
    pub port: u16,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum PolicyError {
    #[error("no allowed targets were given")]
    Empty,
    #[error("allowed target {0:?} is not host:port")]
    BadFormat(String),
    #[error("allowed target {0:?} is not a valid v3 onion address")]
    NotOnion(String),
    #[error("exit target {0:?} is not <hostname>:443 (a DNS name, not an IP address or onion)")]
    BadExit(String),
    #[error("at most {MAX_EXIT_TARGETS} exit target may be given")]
    TooManyExits,
}

/// The sealed backup route needs exactly one website; nothing else may use an exit.
pub const MAX_EXIT_TARGETS: usize = 1;

#[derive(Debug, Clone)]
pub struct TargetPolicy {
    targets: BTreeSet<OnionTarget>,
    exits: BTreeSet<OnionTarget>,
}

impl TargetPolicy {
    /// Parses `"<56 base32 chars>.onion:<port>"` entries. Every entry must be a
    /// valid v3 address (checksum and version are verified).
    pub fn parse<S: AsRef<str>>(entries: &[S]) -> Result<Self, PolicyError> {
        if entries.is_empty() {
            return Err(PolicyError::Empty);
        }
        let mut targets = BTreeSet::new();
        for entry in entries {
            let entry = entry.as_ref().trim();
            let (host, port) = entry
                .rsplit_once(':')
                .ok_or_else(|| PolicyError::BadFormat(entry.to_owned()))?;
            let port: u16 = port
                .parse()
                .ok()
                .filter(|p| *p != 0)
                .ok_or_else(|| PolicyError::BadFormat(entry.to_owned()))?;
            let host = host.to_ascii_lowercase();
            if HsId::from_str(&host).is_err() {
                return Err(PolicyError::NotOnion(entry.to_owned()));
            }
            targets.insert(OnionTarget { host, port });
        }
        Ok(Self { targets, exits: BTreeSet::new() })
    }

    /// [`parse`](Self::parse) plus the exit targets: `"<hostname>:443"`, at most
    /// [`MAX_EXIT_TARGETS`]. Empty `exits` is exactly [`parse`](Self::parse).
    pub fn parse_with_exits<S: AsRef<str>, E: AsRef<str>>(entries: &[S], exits: &[E]) -> Result<Self, PolicyError> {
        let mut policy = Self::parse(entries)?;
        if exits.len() > MAX_EXIT_TARGETS {
            return Err(PolicyError::TooManyExits);
        }
        for entry in exits {
            let entry = entry.as_ref().trim();
            let bad = || PolicyError::BadExit(entry.to_owned());
            let (host, port) = entry.rsplit_once(':').ok_or_else(bad)?;
            if port != "443" {
                return Err(bad());
            }
            let host = host.to_ascii_lowercase();
            if !is_dns_hostname(&host) {
                return Err(bad());
            }
            policy.exits.insert(OnionTarget { host, port: 443 });
        }
        Ok(policy)
    }

    /// True only for an exact (case-insensitive) match of host and port.
    pub fn allows(&self, host: &str, port: u16) -> bool {
        let wanted = OnionTarget { host: host.to_ascii_lowercase(), port };
        self.targets.contains(&wanted) || self.exits.contains(&wanted)
    }

    /// True when `host:port` is an exit target (reached through a Tor exit relay).
    pub fn is_exit(&self, host: &str, port: u16) -> bool {
        self.exits.contains(&OnionTarget { host: host.to_ascii_lowercase(), port })
    }

    pub fn targets(&self) -> impl Iterator<Item = &OnionTarget> {
        self.targets.iter()
    }
}

/// A public DNS name: at least two labels of `a-z`, `0-9` and inner hyphens, a
/// letter in the last label (so never an IPv4 literal), not `.onion`, not
/// `localhost` or a single label.
fn is_dns_hostname(host: &str) -> bool {
    if host.is_empty() || host.len() > 253 || host.ends_with(".onion") || host.ends_with(".localhost") {
        return false;
    }
    let labels: Vec<&str> = host.split('.').collect();
    if labels.len() < 2 {
        return false;
    }
    let label_ok = |l: &&str| {
        !l.is_empty()
            && l.len() <= 63
            && !l.starts_with('-')
            && !l.ends_with('-')
            && l.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
    };
    labels.iter().all(label_ok) && labels.last().is_some_and(|tld| tld.bytes().any(|b| b.is_ascii_lowercase()))
}

#[cfg(test)]
mod tests {
    use super::*;

    // The Tor Project's own onion service (a well-known, valid v3 address).
    const VALID: &str = "2gzyxa5ihm7nsggfxnu52rck2vv4rvmdlkiu3zzui5du4xyclen53wid.onion";

    #[test]
    fn accepts_exact_v3_targets_only() {
        let policy = TargetPolicy::parse(&[format!("{VALID}:80")]).unwrap();
        assert!(policy.allows(VALID, 80));
        assert!(policy.allows(&VALID.to_ascii_uppercase(), 80));
        assert!(!policy.allows(VALID, 443));
        assert!(!policy.allows("www.redvaultsystems.com", 80));
        assert!(!policy.allows(&format!("www.{VALID}"), 80));
        assert!(!policy.allows("127.0.0.1", 80));
    }

    #[test]
    fn refuses_anything_that_is_not_a_v3_onion() {
        assert_eq!(TargetPolicy::parse::<&str>(&[]).unwrap_err(), PolicyError::Empty);
        for bad in [
            "www.redvaultsystems.com:443",
            "127.0.0.1:80",
            "[::1]:80",
            "expyuzz4wqqyqhjn.onion:80", // v2, retired
            "2gzyxa5ihm7nsggfxnu52rck2vv4rvmdlkiu3zzui5du4xyclen53wia.onion:80", // bad checksum
        ] {
            assert!(
                matches!(TargetPolicy::parse(&[bad]), Err(PolicyError::NotOnion(_))),
                "{bad} should be refused"
            );
        }
        for bad in [VALID, &format!("{VALID}:0"), &format!("{VALID}:http")] {
            assert!(matches!(TargetPolicy::parse(&[bad]), Err(PolicyError::BadFormat(_))), "{bad}");
        }
    }

    #[test]
    fn no_exit_targets_means_onion_only() {
        let none: [&str; 0] = [];
        let policy = TargetPolicy::parse_with_exits(&[format!("{VALID}:80")], &none).unwrap();
        assert!(policy.allows(VALID, 80));
        assert!(!policy.allows("www.redvaultsystems.com", 443));
    }

    #[test]
    fn one_exact_https_website_may_be_an_exit_target() {
        let policy =
            TargetPolicy::parse_with_exits(&[format!("{VALID}:80")], &["WWW.RedVaultSystems.com:443"]).unwrap();
        assert!(policy.allows(VALID, 80));
        assert!(policy.allows("www.redvaultsystems.com", 443));
        assert!(policy.is_exit("www.redvaultsystems.com", 443));
        assert!(!policy.is_exit(VALID, 80));
        assert!(!policy.allows("www.redvaultsystems.com", 80));
        assert!(!policy.allows("redvaultsystems.com", 443));
        assert!(!policy.allows("evil.www.redvaultsystems.com", 443));
        assert!(!policy.allows("example.com", 443));
    }

    #[test]
    fn exit_targets_are_https_hostnames_only() {
        let onion_443 = format!("{VALID}:443");
        for bad in [
            "www.redvaultsystems.com:80",
            "www.redvaultsystems.com",
            "www.redvaultsystems.com:0443",
            "104.21.0.1:443",
            "[::1]:443",
            "localhost:443",
            "app.localhost:443",
            "intranet:443",
            onion_443.as_str(),
            "-bad.example.com:443",
            "under_score.example.com:443",
            "example.123:443",
            "*.example.com:443",
            "",
        ] {
            let result = TargetPolicy::parse_with_exits(&[format!("{VALID}:80")], &[bad]);
            assert!(matches!(result, Err(PolicyError::BadExit(_))), "{bad:?} should be refused");
        }
    }

    #[test]
    fn at_most_one_exit_target() {
        let result =
            TargetPolicy::parse_with_exits(&[format!("{VALID}:80")], &["a.example.com:443", "b.example.com:443"]);
        assert_eq!(result.unwrap_err(), PolicyError::TooManyExits);
    }
}
