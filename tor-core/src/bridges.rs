// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Bridges: hidden entrances to Tor for networks that block it.
//!
//! Still Tor all the way (the "always Tor" rule holds): a bridge is just a
//! relay that isn't in the public list, reached through a pluggable
//! transport that disguises the traffic. The transports are separate
//! programs from the Tor Project, run by Arti as child processes:
//!
//! * `lyrebird`: obfs4 (looks like random bytes) and WebTunnel (looks like HTTPS);
//! * `snowflake-client`: Snowflake (looks like a WebRTC video call; no bridge
//!   addresses to block, works in the hardest places).
//!
//! The built-in lists are Tor Browser's (`builtin_bridges.json`, from the Tor
//! Project's bridge service), so every app ships the same ones.

use std::collections::BTreeSet;
use std::path::PathBuf;

use arti_client::config::pt::TransportConfigBuilder;
use arti_client::config::{BoolOrAuto, BridgeConfigBuilder, CfgPath, TorClientConfigBuilder};
use serde::Deserialize;

/// At most this many custom bridge lines, each at most this long: a pasted
/// list is small; anything bigger is a mistake.
pub const MAX_CUSTOM_BRIDGES: usize = 50;
pub const MAX_BRIDGE_LINE: usize = 2048;

const BUILTIN_JSON: &str = include_str!("builtin_bridges.json");

#[derive(Deserialize)]
struct Builtin {
    obfs4: Vec<String>,
    snowflake: Vec<String>,
}

fn builtin() -> Builtin {
    serde_json::from_str(BUILTIN_JSON).expect("builtin_bridges.json is valid")
}

/// How to reach Tor.
#[derive(Debug, Clone, Default, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum BridgeMode {
    /// Straight to the public Tor network (the normal case).
    #[default]
    Off,
    /// Tor Browser's built-in Snowflake bridges.
    Snowflake,
    /// Tor Browser's built-in obfs4 bridges.
    Obfs4,
    /// Bridge lines the user pasted (from bridges.torproject.org, email, a friend).
    Custom,
}

/// Bridge settings from the app.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct BridgeSettings {
    #[serde(default)]
    pub mode: BridgeMode,
    /// Only for `Custom`: one bridge per entry, with or without a leading "Bridge ".
    #[serde(default)]
    pub lines: Vec<String>,
    /// The bundled lyrebird program (obfs4, WebTunnel).
    #[serde(default)]
    pub lyrebird_path: Option<PathBuf>,
    /// The bundled snowflake-client program.
    #[serde(default)]
    pub snowflake_path: Option<PathBuf>,
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum BridgeError {
    #[error("no bridges given")]
    NoBridges,
    #[error("too many bridges (at most {MAX_CUSTOM_BRIDGES})")]
    TooMany,
    #[error("bridge {0} isn't a valid bridge line")]
    BadLine(usize),
    #[error("bridge {0} uses \"{1}\", which this app doesn't support (obfs4, webtunnel, snowflake or none)")]
    UnsupportedTransport(usize, String),
    #[error("the {0} program isn't included with this app")]
    MissingProgram(&'static str),
}

/// Which bundled program runs a transport.
fn program_for(transport: &str) -> Option<&'static str> {
    match transport {
        "obfs4" | "webtunnel" => Some("lyrebird"),
        "snowflake" => Some("snowflake"),
        _ => None,
    }
}

/// The bridge lines this mode stands for, trimmed, without a leading "Bridge ".
pub fn resolved_lines(settings: &BridgeSettings) -> Result<Vec<String>, BridgeError> {
    let raw: Vec<String> = match settings.mode {
        BridgeMode::Off => return Ok(Vec::new()),
        BridgeMode::Snowflake => builtin().snowflake,
        BridgeMode::Obfs4 => builtin().obfs4,
        BridgeMode::Custom => {
            if settings.lines.len() > MAX_CUSTOM_BRIDGES * 4 {
                return Err(BridgeError::TooMany);
            }
            settings.lines.clone()
        }
    };
    let mut lines = Vec::new();
    for line in raw {
        let line = line.trim();
        let line = line.strip_prefix("Bridge ").or_else(|| line.strip_prefix("bridge ")).unwrap_or(line).trim();
        // Blank lines and comments in a pasted list are fine.
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if lines.iter().any(|l| l == line) {
            continue;
        }
        lines.push(line.to_owned());
    }
    if lines.is_empty() {
        return Err(BridgeError::NoBridges);
    }
    if lines.len() > MAX_CUSTOM_BRIDGES {
        return Err(BridgeError::TooMany);
    }
    Ok(lines)
}

/// The transport a bridge line uses ("obfs4", "snowflake", ...), or None for
/// a plain bridge (an address and fingerprint, no disguise).
fn transport_of(line: &str) -> Option<&str> {
    let first = line.split_whitespace().next()?;
    // A plain bridge starts with its address: a digit, or "[" for IPv6.
    if first.starts_with(|c: char| c.is_ascii_digit() || c == '[') {
        None
    } else {
        Some(first)
    }
}

/// Checks the settings without starting anything: what the app's settings
/// screen calls before saving a pasted list. Returns how many bridges there are.
pub fn validate(settings: &BridgeSettings) -> Result<usize, BridgeError> {
    let lines = resolved_lines(settings)?;
    for (i, line) in lines.iter().enumerate() {
        if line.len() > MAX_BRIDGE_LINE {
            return Err(BridgeError::BadLine(i + 1));
        }
        if let Some(t) = transport_of(line) {
            if program_for(t).is_none() {
                return Err(BridgeError::UnsupportedTransport(i + 1, t.to_owned()));
            }
        }
        line.parse::<BridgeConfigBuilder>().map_err(|_| BridgeError::BadLine(i + 1))?;
    }
    Ok(lines.len())
}

/// Puts the bridges and the transports they need into Arti's config.
/// `Off` leaves the config as it is (no bridges, no transports).
pub fn apply(settings: &BridgeSettings, builder: &mut TorClientConfigBuilder) -> Result<(), BridgeError> {
    if settings.mode == BridgeMode::Off {
        return Ok(());
    }
    validate(settings)?;
    let lines = resolved_lines(settings)?;

    let mut transports = BTreeSet::new();
    for line in &lines {
        let bridge: BridgeConfigBuilder = line.parse().map_err(|_| BridgeError::BadLine(0))?;
        builder.bridges().bridges().push(bridge);
        if let Some(t) = transport_of(line) {
            transports.insert(t.to_owned());
        }
    }

    // One lyrebird process serves obfs4 and WebTunnel; Snowflake has its own.
    let lyrebird: Vec<&String> = transports.iter().filter(|t| program_for(t) == Some("lyrebird")).collect();
    if !lyrebird.is_empty() {
        let path = settings.lyrebird_path.clone().ok_or(BridgeError::MissingProgram("lyrebird"))?;
        let mut t = TransportConfigBuilder::default();
        t.protocols(lyrebird.iter().map(|p| p.parse().expect("known transport name")).collect())
            .path(CfgPath::new_literal(path))
            .run_on_startup(false);
        builder.bridges().transports().push(t);
    }
    if transports.contains("snowflake") {
        let path = settings.snowflake_path.clone().ok_or(BridgeError::MissingProgram("snowflake"))?;
        let mut t = TransportConfigBuilder::default();
        t.protocols(vec!["snowflake".parse().expect("known transport name")])
            .path(CfgPath::new_literal(path))
            .run_on_startup(false);
        builder.bridges().transports().push(t);
    }
    builder.bridges().enabled(BoolOrAuto::Explicit(true));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn with_programs(mode: BridgeMode, lines: &[&str]) -> BridgeSettings {
        BridgeSettings {
            mode,
            lines: lines.iter().map(|s| s.to_string()).collect(),
            lyrebird_path: Some(PathBuf::from("/opt/rv/lyrebird")),
            snowflake_path: Some(PathBuf::from("/opt/rv/snowflake-client")),
        }
    }

    fn built(settings: &BridgeSettings) -> Result<arti_client::config::TorClientConfig, String> {
        let mut b = TorClientConfigBuilder::from_directories("/tmp/rv-state", "/tmp/rv-cache");
        apply(settings, &mut b).map_err(|e| e.to_string())?;
        b.build().map_err(|e| e.to_string())
    }

    #[test]
    fn builtin_lists_parse_and_build() {
        assert!(validate(&with_programs(BridgeMode::Snowflake, &[])).unwrap() >= 1);
        assert!(validate(&with_programs(BridgeMode::Obfs4, &[])).unwrap() >= 1);
        built(&with_programs(BridgeMode::Snowflake, &[])).unwrap();
        built(&with_programs(BridgeMode::Obfs4, &[])).unwrap();
    }

    #[test]
    fn off_changes_nothing() {
        let settings = BridgeSettings::default();
        assert_eq!(resolved_lines(&settings).unwrap(), Vec::<String>::new());
        built(&settings).unwrap();
    }

    #[test]
    fn custom_lines_accept_the_bridge_prefix_blanks_and_comments() {
        let s = with_programs(
            BridgeMode::Custom,
            &[
                "# from bridges.torproject.org",
                "",
                "Bridge obfs4 192.0.2.55:38114 316E643333645F6D79216558614D3931657A5F5F cert=YXJlIGZyZXF1ZW50bHkgZnVsbCBvZiBsaXR0bGUgbWVzc2FnZXMgeW91IGNhbiBmaW5kLg iat-mode=0",
                "  webtunnel [2001:db8::1]:443 7DD62766BF2052432051D7B7E08A22F7E34A4543 url=https://example.com/abc ver=0.0.1  ",
            ],
        );
        assert_eq!(validate(&s).unwrap(), 2);
        built(&s).unwrap();
    }

    #[test]
    fn duplicates_count_once() {
        let line = "obfs4 192.0.2.55:38114 316E643333645F6D79216558614D3931657A5F5F cert=YXJlIGZyZXF1ZW50bHkgZnVsbCBvZiBsaXR0bGUgbWVzc2FnZXMgeW91IGNhbiBmaW5kLg iat-mode=0";
        assert_eq!(validate(&with_programs(BridgeMode::Custom, &[line, line])).unwrap(), 1);
    }

    #[test]
    fn plain_bridges_need_no_program() {
        let mut s = with_programs(BridgeMode::Custom, &["192.0.2.66:443 316E643333645F6D79216558614D3931657A5F5F"]);
        s.lyrebird_path = None;
        s.snowflake_path = None;
        built(&s).unwrap();
    }

    #[test]
    fn refuses_empty_garbage_and_unknown_transports() {
        assert_eq!(validate(&with_programs(BridgeMode::Custom, &["", "# nothing"])), Err(BridgeError::NoBridges));
        assert_eq!(validate(&with_programs(BridgeMode::Custom, &["obfs4 not-an-address"])), Err(BridgeError::BadLine(1)));
        assert_eq!(
            validate(&with_programs(BridgeMode::Custom, &["meek_lite 192.0.2.20:80 url=https://x.example front=y.example"])),
            Err(BridgeError::UnsupportedTransport(1, "meek_lite".into()))
        );
        let many: Vec<String> = (0..=MAX_CUSTOM_BRIDGES).map(|i| format!("192.0.2.{}:443 316E643333645F6D79216558614D3931657A5F5F", i % 250)).collect();
        let refs: Vec<&str> = many.iter().map(|s| s.as_str()).collect();
        // Distinct addresses up to 250, so 51 lines stay 51 after de-duplication.
        assert_eq!(validate(&with_programs(BridgeMode::Custom, &refs)), Err(BridgeError::TooMany));
    }

    #[test]
    fn says_which_program_is_missing() {
        let mut s = with_programs(BridgeMode::Snowflake, &[]);
        s.snowflake_path = None;
        assert_eq!(built(&s).unwrap_err(), "the snowflake program isn't included with this app");
        let mut s = with_programs(BridgeMode::Obfs4, &[]);
        s.lyrebird_path = None;
        assert_eq!(built(&s).unwrap_err(), "the lyrebird program isn't included with this app");
    }
}
