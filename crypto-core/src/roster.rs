// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Signed group rosters with history (M6, docs/signal-integration/03-group-design.md §6).
//!
//! A roster says which accounts are in a group at one epoch. A member device
//! signs it with its account key, and it names the hash of the roster before
//! it, so the rosters form a chain from the first one (epoch 1). A device
//! accepts a roster only if:
//! - it continues the chain it already has, or a full chain from epoch 1 for a
//!   group it doesn't know yet;
//! - its issuer is a member of the previous roster (any member, for epoch 1);
//! - its issuer's account key is the one this device knows for that account;
//! - its signature checks.
//! Two different rosters for the same epoch are refused (equivocation).
//!
//! This makes membership something a server can't change alone. It is our own
//! composition of standard pieces (Ed25519, SHA-256, length-prefixed fields),
//! so it needs independent review before use. Compiled in every build: it has
//! no libsignal code.

use std::collections::BTreeMap;

use base64::{engine::general_purpose::STANDARD_NO_PAD as B64_NO_PAD, Engine as _};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use vodozemac::{Ed25519PublicKey, Ed25519SecretKey, Ed25519Signature};

use crate::engine::{bad_request, str_field, Error, Result, State};

const ROSTER_CONTEXT: &str = "rv-group-roster-v1";
/// Member lists per epoch kept for checking late messages. Older epochs are forgotten.
const EPOCHS_KEPT: usize = 20;
const MAX_MEMBERS: usize = 500;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct GroupRoster {
    pub v: u32,
    pub group_id: String,
    pub epoch: u64,
    /// Member account ids, sorted and unique.
    pub members: Vec<String>,
    pub issuer: String,
    /// The issuer's account key (Ed25519), as its signed device list names it.
    pub issuer_account_key: String,
    /// The hash of the previous roster; absent for epoch 1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub prev: Option<String>,
    pub signature: String,
}

/// What this device knows about one group.
#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub(crate) struct RosterState {
    pub latest: GroupRoster,
    pub latest_hash: String,
    /// Member lists of the most recent epochs (for late messages).
    pub epochs: BTreeMap<u64, Vec<String>>,
}

fn put(out: &mut Vec<u8>, value: &[u8]) {
    out.extend_from_slice(&(value.len() as u32).to_be_bytes());
    out.extend_from_slice(value);
}

fn signing_input(r: &GroupRoster) -> Vec<u8> {
    let mut out = Vec::new();
    put(&mut out, ROSTER_CONTEXT.as_bytes());
    put(&mut out, &r.v.to_be_bytes());
    put(&mut out, r.group_id.as_bytes());
    put(&mut out, &r.epoch.to_be_bytes());
    put(&mut out, &(r.members.len() as u32).to_be_bytes());
    for m in &r.members {
        put(&mut out, m.as_bytes());
    }
    put(&mut out, r.issuer.as_bytes());
    put(&mut out, r.issuer_account_key.as_bytes());
    put(&mut out, r.prev.as_deref().unwrap_or("").as_bytes());
    out
}

/// SHA-256 over the signed bytes and the signature, unpadded base64: what the next roster's `prev` names.
pub fn roster_hash(r: &GroupRoster) -> String {
    let mut h = Sha256::new();
    h.update(signing_input(r));
    h.update(r.signature.as_bytes());
    B64_NO_PAD.encode(h.finalize())
}

fn normalized(members: &[String]) -> Result<Vec<String>> {
    let mut out: Vec<String> = members.to_vec();
    out.sort();
    out.dedup();
    if out.len() != members.len() {
        return Err(bad_request("members must be unique"));
    }
    if out.is_empty() || out.len() > MAX_MEMBERS || out.iter().any(String::is_empty) {
        return Err(bad_request(format!("a roster has 1 to {MAX_MEMBERS} member accounts")));
    }
    Ok(out)
}

/// The account key this device knows for `user`: its own, or a contact's pinned one.
fn known_account_key(state: &State, user: &str) -> Option<String> {
    if user == state.user_id {
        return state.own_list.as_ref().map(|l| l.account_key.clone());
    }
    state.contacts.get(user).map(|c| c.account_key.clone())
}

fn sign(state: &State, group_id: &str, epoch: u64, members: Vec<String>, prev: Option<String>) -> Result<GroupRoster> {
    let secret = state.account_key.clone().ok_or_else(|| Error::new("not_trusted", "this device doesn't hold the account key"))?;
    let key = Ed25519SecretKey::from_base64(&secret).map_err(|_| Error::new("bad_state", "unreadable account key"))?;
    let mut roster = GroupRoster {
        v: 1,
        group_id: group_id.to_owned(),
        epoch,
        members,
        issuer: state.user_id.clone(),
        issuer_account_key: key.public_key().to_base64(),
        prev,
        signature: String::new(),
    };
    roster.signature = key.sign(&signing_input(&roster)).to_base64();
    Ok(roster)
}

/// Checks one roster on its own: shape, the issuer's known key, the signature.
fn check_one(state: &State, r: &GroupRoster) -> Result<()> {
    let bad = |m: &str| Error::new("bad_roster", m.to_owned());
    if r.v != 1 {
        return Err(bad("unknown roster version"));
    }
    if normalized(&r.members)? != r.members {
        return Err(bad("members must be sorted"));
    }
    if !r.members.contains(&r.issuer) {
        return Err(bad("the issuer must be a member of the roster it signs"));
    }
    if (r.epoch == 1) != r.prev.is_none() {
        return Err(bad("only epoch 1 has no previous roster"));
    }
    match known_account_key(state, &r.issuer) {
        Some(k) if k == r.issuer_account_key => {}
        Some(_) => return Err(Error::new("account_key_changed", "the roster's issuer signed with an account key this device doesn't know for them")),
        None => return Err(Error::new("unknown_issuer", "verify the issuer's device list before their rosters")),
    }
    let key = Ed25519PublicKey::from_base64(&r.issuer_account_key).map_err(|_| bad("unreadable issuer key"))?;
    let signature = Ed25519Signature::from_base64(&r.signature).map_err(|_| bad("unreadable signature"))?;
    key.verify(&signing_input(r), &signature).map_err(|_| bad("the roster isn't signed by its issuer"))
}

fn remember(state: &mut State, r: &GroupRoster) {
    let entry = state.rosters.entry(r.group_id.clone()).or_insert_with(|| RosterState {
        latest: r.clone(),
        latest_hash: String::new(),
        epochs: BTreeMap::new(),
    });
    entry.latest = r.clone();
    entry.latest_hash = roster_hash(r);
    entry.epochs.insert(r.epoch, r.members.clone());
    while entry.epochs.len() > EPOCHS_KEPT {
        let oldest = *entry.epochs.keys().next().expect("non-empty");
        entry.epochs.remove(&oldest);
    }
}

fn members_field(request: &Value) -> Result<Vec<String>> {
    let members: Vec<String> = serde_json::from_value(request.get("members").cloned().unwrap_or(Value::Null))
        .map_err(|_| bad_request("members must be account ids"))?;
    normalized(&members)
}

/// The first roster of a group (epoch 1), signed by this account. This device's account must be a member.
pub(crate) fn roster_create(state: &mut State, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?.to_owned();
    if state.rosters.contains_key(&group_id) {
        return Err(Error::new("roster_exists", "this group already has a roster here"));
    }
    let members = members_field(request)?;
    if !members.contains(&state.user_id) {
        return Err(bad_request("the roster must include this account"));
    }
    let roster = sign(state, &group_id, 1, members, None)?;
    remember(state, &roster);
    Ok(json!({ "roster": roster }))
}

/// The next roster (epoch + 1) after the latest one this device verified. This account must be in the latest one.
pub(crate) fn roster_update(state: &mut State, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?.to_owned();
    let current = state.rosters.get(&group_id).cloned().ok_or_else(|| Error::new("roster_unknown", "no roster for this group here"))?;
    if !current.latest.members.contains(&state.user_id) {
        return Err(Error::new("not_a_member", "only a member can change the roster"));
    }
    let members = members_field(request)?;
    let roster = sign(state, &group_id, current.latest.epoch + 1, members, Some(current.latest_hash.clone()))?;
    let (added, removed) = diff(&current.latest.members, &roster.members);
    remember(state, &roster);
    Ok(json!({ "roster": roster, "added": added, "removed": removed }))
}

fn diff(old: &[String], new: &[String]) -> (Vec<String>, Vec<String>) {
    (
        new.iter().filter(|m| !old.contains(m)).cloned().collect(),
        old.iter().filter(|m| !new.contains(m)).cloned().collect(),
    )
}

/// Verifies rosters the server handed out, oldest first. For a group this device knows, they must
/// continue its chain; rosters it already has must be identical (else equivocation). For a new
/// group they must start at epoch 1: the full history, so a new device can't be shown a member
/// list the server chose.
pub(crate) fn roster_verify(state: &mut State, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?.to_owned();
    let chain: Vec<GroupRoster> = serde_json::from_value(request.get("chain").cloned().unwrap_or(Value::Null))
        .map_err(|_| Error::new("bad_roster", "chain must be a list of rosters"))?;
    if chain.is_empty() {
        return Err(bad_request("the chain is empty"));
    }
    let known = state.rosters.get(&group_id).cloned();
    let before = known.as_ref().map(|k| k.latest.members.clone()).unwrap_or_default();
    // Where this device's chain stands: (epoch, hash, members).
    let mut tip: Option<(u64, String, Vec<String>)> = known.as_ref().map(|k| (k.latest.epoch, k.latest_hash.clone(), k.latest.members.clone()));
    let mut accepted = 0;
    for r in &chain {
        if r.group_id != group_id {
            return Err(Error::new("bad_roster", "a roster for another group"));
        }
        if let Some((epoch, hash, _)) = &tip {
            if r.epoch <= *epoch {
                // Already part of the chain here: only the latest can still be compared byte for byte.
                if r.epoch == *epoch && roster_hash(r) != *hash {
                    return Err(Error::new("roster_equivocation", "two different rosters for the same epoch"));
                }
                continue;
            }
        }
        check_one(state, r)?;
        match &tip {
            None => {
                if r.epoch != 1 {
                    return Err(Error::new("roster_history_missing", "a group this device doesn't know needs its rosters from epoch 1"));
                }
            }
            Some((epoch, hash, members)) => {
                if r.epoch != epoch + 1 {
                    return Err(Error::new("roster_gap", "a roster is missing in the chain"));
                }
                if r.prev.as_deref() != Some(hash.as_str()) {
                    return Err(Error::new("roster_fork", "this roster doesn't follow the one this device has"));
                }
                if !members.contains(&r.issuer) {
                    return Err(Error::new("bad_roster_issuer", "the issuer wasn't a member of the previous roster"));
                }
            }
        }
        remember(state, r);
        tip = Some((r.epoch, roster_hash(r), r.members.clone()));
        accepted += 1;
    }
    let latest = state.rosters.get(&group_id).ok_or_else(|| Error::new("roster_unknown", "no roster for this group"))?.latest.clone();
    let (added, removed) = diff(&before, &latest.members);
    Ok(json!({
        "groupId": group_id,
        "epoch": latest.epoch,
        "members": latest.members,
        "accepted": accepted,
        "added": added,
        "removed": removed,
        "isMember": latest.members.contains(&state.user_id),
    }))
}

/// The members of `group_id` at `epoch`, if this device verified that epoch and still remembers it.
#[cfg_attr(not(feature = "signal"), allow(dead_code))]
pub(crate) fn members_at<'a>(state: &'a State, group_id: &str, epoch: u64) -> Option<&'a Vec<String>> {
    state.rosters.get(group_id).and_then(|r| r.epochs.get(&epoch))
}

/// The latest verified roster of `group_id`.
#[cfg_attr(not(feature = "signal"), allow(dead_code))]
pub(crate) fn latest<'a>(state: &'a State, group_id: &str) -> Option<&'a GroupRoster> {
    state.rosters.get(group_id).map(|r| &r.latest)
}
