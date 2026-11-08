// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Signal groups (M6, docs/signal-integration/03-group-design.md §6): libsignal sender keys,
//! delivered to each member device inside its pairwise rv-signal-v1 session (PQXDH + SPQR), for
//! groups whose membership is a verified signed roster (src/roster.rs).
//!
//! LICENCE: links libsignal (AGPL-3.0-only). The `signal` feature only, development only.
//!
//! How it fits together:
//! - **Sharing.** `groupSignalShare` makes (or reuses) this device's sender key for the group's
//!   current roster epoch, and seals its distribution message to each recipient device that
//!   doesn't have it yet. A new roster epoch, or 1,000 messages, means a new sender key, so a
//!   removed member never gets the next one. Recipients must belong to accounts in the latest roster.
//! - **Sending.** `groupSignalEncrypt` encrypts one message under the current sender key. Its
//!   authenticated plaintext names the group, the epoch and the sending device.
//! - **Receiving a key.** A distribution message is accepted only inside a Signal session with the
//!   device it belongs to, from a member of that roster epoch. It is bound to that device, group
//!   and epoch.
//! - **Receiving a message.** `groupSignalDecrypt` accepts a message only under a key bound to the
//!   claimed sender, for a group and epoch this device verified, from an account that is still in
//!   the latest roster, at the latest epoch or the one before it (messages in flight).
//!
//! libsignal does the cryptography; this file binds keys to devices, groups and epochs.

use std::collections::BTreeMap;

use async_trait::async_trait;
use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use libsignal_protocol::{
    create_sender_key_distribution_message, group_decrypt, group_encrypt, process_sender_key_distribution_message,
    ProtocolAddress, SenderKeyDistributionMessage, SenderKeyRecord, SenderKeyStore, SignalProtocolError,
};
use rand09::TryRngCore as _;
use serde_json::{json, Value};
use uuid::Uuid;
use vodozemac::olm::Account;
use zeroize::Zeroizing;

use crate::engine::{bad_request, identity_field, own_identity, str_field, DeviceIdentity, Error, Result, State};
use crate::signal_state::{InboundSenderKey, OutboundSenderKey, SignalState};

pub const GROUP_PROTOCOL: &str = "rv-signal-group-v1";
/// A sender key is replaced after this many messages, as well as at every new roster epoch.
const MAX_MESSAGES_PER_SENDER_KEY: u32 = 1000;

type SpResult<T> = std::result::Result<T, SignalProtocolError>;

struct SenderKeys<'a>(&'a mut BTreeMap<String, String>);

fn sender_key_id(sender: &ProtocolAddress, distribution_id: Uuid) -> String {
    format!("{}|{}", sender.name(), distribution_id)
}

#[async_trait(?Send)]
impl SenderKeyStore for SenderKeys<'_> {
    async fn store_sender_key(&mut self, sender: &ProtocolAddress, distribution_id: Uuid, record: &SenderKeyRecord) -> SpResult<()> {
        self.0.insert(sender_key_id(sender, distribution_id), B64.encode(record.serialize()?));
        Ok(())
    }
    async fn load_sender_key(&mut self, sender: &ProtocolAddress, distribution_id: Uuid) -> SpResult<Option<SenderKeyRecord>> {
        match self.0.get(&sender_key_id(sender, distribution_id)) {
            Some(text) => {
                let bytes = B64.decode(text).map_err(|_| SignalProtocolError::InvalidState("sender key", "damaged record".into()))?;
                Ok(Some(SenderKeyRecord::deserialize(&bytes)?))
            }
            None => Ok(None),
        }
    }
}

fn group_err(error: SignalProtocolError) -> Error {
    match error {
        SignalProtocolError::DuplicatedMessage(..) => Error::new("duplicate", "this group message was already decrypted"),
        SignalProtocolError::NoSenderKeyState { .. } => Error::new("no_sender_key", "no sender key from this device for this message yet"),
        SignalProtocolError::InvalidSenderKeySession { .. } => Error::new("no_sender_key", "the sender key for this message is gone"),
        SignalProtocolError::SignatureValidationFailed => Error::new("decrypt_failed", "the group message's signature doesn't check"),
        _ => Error::new("decrypt_failed", "this group message can't be opened"),
    }
}

fn signal_state(state: &mut State) -> Result<&mut SignalState> {
    if !crate::signal_state::signal_active(state) {
        return Err(Error::new("signal_not_enabled", "Signal isn't on for this device"));
    }
    state.signal.session.as_mut().ok_or_else(|| Error::new("signal_not_enabled", "Signal isn't on for this device"))
}

fn address(device: &DeviceIdentity) -> ProtocolAddress {
    ProtocolAddress::new(device.curve25519.clone(), libsignal_protocol::DeviceId::try_from(1u8).expect("valid"))
}

fn names(v: &Value, who: &DeviceIdentity) -> bool {
    v.get("userId").and_then(Value::as_str) == Some(who.user_id.as_str())
        && v.get("deviceId").and_then(Value::as_str) == Some(who.device_id.as_str())
        && v.get("ed25519").and_then(Value::as_str) == Some(who.ed25519.as_str())
}

/// Makes sure this device has a current sender key for the group (new at each roster epoch or after
/// the message limit). Returns the key's distribution id and the distribution message bytes.
fn current_sender_key(state: &mut State, account: &Account, group_id: &str, epoch: u64) -> Result<(Uuid, Vec<u8>)> {
    let me = own_identity(state, account);
    let st = signal_state(state)?;
    let reuse = st.group_out.get(group_id).filter(|o| o.epoch == epoch && o.sent < MAX_MESSAGES_PER_SENDER_KEY).cloned();
    let distribution = match &reuse {
        Some(o) => Uuid::parse_str(&o.distribution_id).map_err(|_| Error::new("bad_state", "damaged sender key id"))?,
        None => Uuid::new_v4(),
    };
    let mut rng = rand09::rngs::OsRng.unwrap_err();
    let mut store = SenderKeys(&mut st.sender_keys);
    let skdm = futures_executor::block_on(create_sender_key_distribution_message(&address(&me), distribution, &mut store, &mut rng))
        .map_err(group_err)?;
    if reuse.is_none() {
        st.group_out.insert(
            group_id.to_owned(),
            OutboundSenderKey { distribution_id: distribution.to_string(), epoch, sent: 0, shared_with: Vec::new() },
        );
    }
    Ok((distribution, skdm.serialized().to_vec()))
}

/// Seals this device's current sender key for the group to each recipient device that doesn't have
/// it yet. Request: `groupId`, and `recipients: [{identity, signalBundle?}]` (the member devices; a
/// device without a Signal session needs its claimed bundle, as in encryptMany). Each result is a
/// pairwise rv-signal-v1 message to deliver, `{shared: true}` when the device already has the key,
/// or an error for that device alone.
pub(crate) fn share(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?.to_owned();
    let roster = crate::roster::latest(state, &group_id).cloned().ok_or_else(|| Error::new("roster_unknown", "verify the group's roster first"))?;
    if !roster.members.contains(&state.user_id) {
        return Err(Error::new("not_a_member", "this account isn't in the group's roster"));
    }
    let recipients = request.get("recipients").and_then(Value::as_array).ok_or_else(|| bad_request("recipients is required"))?.clone();
    let (distribution, skdm) = current_sender_key(state, account, &group_id, roster.epoch)?;
    let mut results = Vec::new();
    for entry in &recipients {
        let recipient = match identity_field(entry, "identity") {
            Ok(r) => r,
            Err(e) => {
                results.push(json!({ "error": e.code, "message": e.message }));
                continue;
            }
        };
        let result = (|| -> Result<Value> {
            if !roster.members.contains(&recipient.user_id) {
                return Err(Error::new("not_a_member", "this device's account isn't in the group's roster"));
            }
            if !crate::identity::check_listed(state, &recipient)? {
                return Err(Error::new("unverified_device", "verify this device's account list first"));
            }
            let already = state.signal.session.as_ref().and_then(|st| st.group_out.get(&group_id)).is_some_and(|o| o.shared_with.contains(&recipient.curve25519));
            if already {
                return Ok(json!({ "shared": true }));
            }
            let payload = json!({
                "t": "sender_key",
                "senderKey": { "groupId": group_id, "epoch": roster.epoch, "distributionId": distribution.to_string(), "skdm": B64.encode(&skdm) },
            });
            let message = crate::signal::encrypt_payload_for(state, account, &recipient, entry.get("signalBundle"), payload)?;
            if let Some(o) = state.signal.session.as_mut().and_then(|st| st.group_out.get_mut(&group_id)) {
                o.shared_with.push(recipient.curve25519.clone());
            }
            Ok(json!({ "message": message }))
        })();
        let mut out = match result {
            Ok(v) => v,
            Err(e) => json!({ "error": e.code, "message": e.message }),
        };
        out["userId"] = json!(recipient.user_id);
        out["deviceId"] = json!(recipient.device_id);
        results.push(out);
    }
    Ok(json!({ "groupId": group_id, "epoch": roster.epoch, "distributionId": distribution.to_string(), "results": results }))
}

/// Encrypts one message to the group under this device's current sender key. Share first:
/// a key from an older roster epoch is refused (`share_first`).
pub(crate) fn encrypt(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?.to_owned();
    let text = str_field(request, "plaintext")?;
    let roster = crate::roster::latest(state, &group_id).cloned().ok_or_else(|| Error::new("roster_unknown", "verify the group's roster first"))?;
    let me = own_identity(state, account);
    let st = signal_state(state)?;
    let out = st
        .group_out
        .get(&group_id)
        .filter(|o| o.epoch == roster.epoch && o.sent < MAX_MESSAGES_PER_SENDER_KEY)
        .cloned()
        .ok_or_else(|| Error::new("share_first", "share a sender key for the group's current roster first"))?;
    let distribution = Uuid::parse_str(&out.distribution_id).map_err(|_| Error::new("bad_state", "damaged sender key id"))?;
    let inner = Zeroizing::new(
        serde_json::to_vec(&json!({
            "v": 1,
            "p": GROUP_PROTOCOL,
            "groupId": group_id,
            "epoch": roster.epoch,
            "sender": { "userId": me.user_id, "deviceId": me.device_id, "ed25519": me.ed25519 },
            "body": text,
        }))
        .map_err(|e| Error::new("internal", e.to_string()))?,
    );
    let mut rng = rand09::rngs::OsRng.unwrap_err();
    let mut store = SenderKeys(&mut st.sender_keys);
    let message = futures_executor::block_on(group_encrypt(&mut store, &address(&me), distribution, &inner, &mut rng)).map_err(group_err)?;
    if let Some(o) = st.group_out.get_mut(&group_id) {
        o.sent += 1;
    }
    Ok(json!({
        "message": {
            "protocol": GROUP_PROTOCOL,
            "groupId": group_id,
            "epoch": roster.epoch,
            "distributionId": out.distribution_id,
            "body": B64.encode(message.serialized()),
        }
    }))
}

/// A sender key that arrived inside a Signal session with `sender` (called from signal::decrypt).
pub(crate) fn receive_sender_key(state: &mut State, sender: &DeviceIdentity, key: &Value) -> Result<Value> {
    let group_id = key.get("groupId").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no group"))?.to_owned();
    let epoch = key.get("epoch").and_then(Value::as_u64).ok_or_else(|| Error::new("malformed_payload", "no epoch"))?;
    let distribution_id = key.get("distributionId").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no distribution id"))?;
    let distribution = Uuid::parse_str(distribution_id).map_err(|_| Error::new("malformed_payload", "bad distribution id"))?;
    let skdm_bytes = B64
        .decode(key.get("skdm").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no key"))?)
        .map_err(|_| Error::new("malformed_payload", "bad key"))?;
    let members = crate::roster::members_at(state, &group_id, epoch).ok_or_else(|| Error::new("roster_unknown", "verify the group's roster at this epoch first"))?;
    if !members.contains(&sender.user_id) {
        return Err(Error::new("not_a_member", "a group key from an account that isn't in that roster"));
    }
    let skdm = SenderKeyDistributionMessage::try_from(skdm_bytes.as_slice()).map_err(|_| Error::new("malformed_payload", "not a sender key"))?;
    if skdm.distribution_id().map_err(|_| Error::new("malformed_payload", "bad sender key"))? != distribution {
        return Err(Error::new("malformed_payload", "the sender key names another distribution"));
    }
    let st = signal_state(state)?;
    if let Some(bound) = st.group_in.get(distribution_id) {
        if bound.sender_curve != sender.curve25519 || bound.group_id != group_id || bound.epoch != epoch {
            return Err(Error::new("sender_key_conflict", "this sender key already belongs to another device, group or epoch"));
        }
    }
    let mut store = SenderKeys(&mut st.sender_keys);
    futures_executor::block_on(process_sender_key_distribution_message(&address(sender), &skdm, &mut store)).map_err(group_err)?;
    st.group_in.insert(
        distribution_id.to_owned(),
        InboundSenderKey { group_id: group_id.clone(), epoch, sender_curve: sender.curve25519.clone(), sender_user: sender.user_id.clone() },
    );
    Ok(json!({ "kind": "senderKey", "groupId": group_id, "epoch": epoch }))
}

/// Opens a group message from `sender`.
pub(crate) fn decrypt(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let sender = identity_field(request, "sender")?;
    let message = request.get("message").ok_or_else(|| bad_request("message is required"))?;
    if message.get("protocol").and_then(Value::as_str) != Some(GROUP_PROTOCOL) {
        return Err(Error::new("protocol_mismatch", "this isn't an rv-signal-group-v1 message"));
    }
    let group_id = str_field(message, "groupId")?.to_owned();
    let epoch = message.get("epoch").and_then(Value::as_u64).ok_or_else(|| bad_request("epoch is required"))?;
    let distribution_id = str_field(message, "distributionId")?.to_owned();
    let body = B64.decode(str_field(message, "body")?).map_err(|_| bad_request("body isn't base64"))?;
    let latest = crate::roster::latest(state, &group_id).cloned().ok_or_else(|| Error::new("roster_unknown", "verify the group's roster first"))?;
    if epoch > latest.epoch {
        return Err(Error::new("roster_unknown", "a message from a roster epoch this device hasn't verified yet"));
    }
    if epoch + 1 < latest.epoch {
        return Err(Error::new("stale_epoch", "a message from an older roster epoch"));
    }
    if !latest.members.contains(&sender.user_id) {
        return Err(Error::new("not_a_member", "the sender's account isn't in the group's latest roster"));
    }
    let me = own_identity(state, account);
    let st = signal_state(state)?;
    let bound = st.group_in.get(&distribution_id).cloned().ok_or_else(|| Error::new("no_sender_key", "no sender key from this device for this message yet"))?;
    if bound.sender_curve != sender.curve25519 || bound.group_id != group_id || bound.epoch != epoch {
        return Err(Error::new("identity_mismatch", "this sender key belongs to another device, group or epoch"));
    }
    let mut store = SenderKeys(&mut st.sender_keys);
    let plaintext = Zeroizing::new(futures_executor::block_on(group_decrypt(&body, &mut store, &address(&sender))).map_err(group_err)?);
    let inner: Value = serde_json::from_slice(&plaintext).map_err(|_| Error::new("malformed_payload", "not a payload"))?;
    if inner.get("v").and_then(Value::as_u64) != Some(1)
        || inner.get("p").and_then(Value::as_str) != Some(GROUP_PROTOCOL)
        || inner.get("groupId").and_then(Value::as_str) != Some(group_id.as_str())
        || inner.get("epoch").and_then(Value::as_u64) != Some(epoch)
        || !inner.get("sender").is_some_and(|s| names(s, &sender))
    {
        return Err(Error::new("identity_mismatch", "the authenticated payload names another group, epoch or sender"));
    }
    let _ = me;
    let text = inner.get("body").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no body"))?;
    Ok(json!({ "kind": "message", "protocol": GROUP_PROTOCOL, "groupId": group_id, "epoch": epoch, "plaintext": text }))
}
