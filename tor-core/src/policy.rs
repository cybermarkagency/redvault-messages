// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Where the gateway may go: an exact list of v3 onion services.
//!
//! The SOCKS port is the only way the apps reach the network for messaging, so
//! this list is the routing policy. It is checked on every request; anything
//! that isn't one of these `host:port` pairs is refused. Clearnet hosts and IP
//! literals can never be listed, so a Tor exit relay is never used and a
//! hostname the app resolved itself (a DNS leak upstream) can't get through.

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
}

#[derive(Debug, Clone)]
pub struct TargetPolicy {
    targets: BTreeSet<OnionTarget>,
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
        Ok(Self { targets })
    }

    /// True only for an exact (case-insensitive) match of host and port.
    pub fn allows(&self, host: &str, port: u16) -> bool {
        self.targets.contains(&OnionTarget { host: host.to_ascii_lowercase(), port })
    }

    pub fn targets(&self) -> impl Iterator<Item = &OnionTarget> {
        self.targets.iter()
    }
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
}
