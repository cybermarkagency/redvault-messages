//! Signal session state as stored inside the engine's encrypted state, device
//! certificates, signed device capabilities, and the downgrade guard for
//! protocol 3's Olm.
//!
//! This file is compiled in every build, with or without the `signal` feature,
//! and it uses no libsignal code. That matters for three reasons:
//! - a build without libsignal must still carry Signal state forward
//!   untouched, because dropping it would silently forget Signal sessions and
//!   downgrade pins;
//! - every client (the web app included) must verify device lists that carry
//!   capabilities;
//! - a trusted device without libsignal can still add a capability to its
//!   account's list.
//!
//! The libsignal code itself lives in `src/signal.rs`.

use std::collections::BTreeMap;

use base64::{engine::general_purpose::STANDARD_NO_PAD as B64_NO_PAD, Engine as _};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use vodozemac::{Ed25519PublicKey, Ed25519Signature};

use crate::engine::{DeviceIdentity, Error, Result, State};
use crate::identity::SignedDeviceList;

/// The protocol id of Signal sessions. It appears in the outer envelope, in the
/// authenticated inner payload (the two must match), in certificates and in
/// list capabilities.
pub const SIGNAL_PROTOCOL: &str = "rv-signal-v1";
const CERT_CONTEXT: &str = "rv-signal-device-cert-v1";
const CAPABILITIES_CONTEXT: &[u8] = b"redvault-device-list-capabilities-v1\0";
/// One capability per device and protocol, at most this many per list (10 devices × known protocols).
const MAX_CAPABILITIES: usize = 10;

// --- Stored state ---

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub(crate) struct KyberEntry {
    pub record: String,
    /// The signed last-resort key. It is kept after use. One-time keys are deleted when used.
    pub last_resort: bool,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SignalState {
    /// libsignal IdentityKeyPair, serialized. Never leaves the device.
    pub identity: String,
    pub registration_id: u32,
    pub next_prekey_id: u32,
    pub next_signed_id: u32,
    pub next_kyber_id: u32,
    pub current_signed_id: Option<u32>,
    pub current_last_resort_id: Option<u32>,
    /// One-time EC prekeys: id -> PreKeyRecord.
    pub prekeys: BTreeMap<u32, String>,
    /// Signed EC prekeys: id -> SignedPreKeyRecord (the previous one is kept for late messages).
    pub signed_prekeys: BTreeMap<u32, String>,
    /// Kyber prekeys (one-time and last-resort): id -> record.
    pub kyber: BTreeMap<u32, KyberEntry>,
    /// Base keys already accepted with each last-resort Kyber key ("kyberId:signedId" -> base keys),
    /// so a replayed first message is refused once its session is gone.
    pub kyber_seen: BTreeMap<String, Vec<String>>,
    /// Sessions by peer address (the peer's protocol-3 Curve25519 key): SessionRecord.
    pub sessions: BTreeMap<String, String>,
    /// The Signal identity key each peer address was certified with (from its device certificate).
    pub identities: BTreeMap<String, String>,
    /// Groups (M6): sender-key records by "peer address|distribution id".
    #[serde(default)]
    pub sender_keys: BTreeMap<String, String>,
    /// This device's current sender key per group.
    #[serde(default)]
    pub group_out: BTreeMap<String, OutboundSenderKey>,
    /// Sender keys received, by distribution id: which device, group and roster epoch each belongs to.
    #[serde(default)]
    pub group_in: BTreeMap<String, InboundSenderKey>,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub(crate) struct OutboundSenderKey {
    pub distribution_id: String,
    pub epoch: u64,
    pub sent: u32,
    /// Curve25519 keys of the member devices this key was sealed to.
    pub shared_with: Vec<String>,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub(crate) struct InboundSenderKey {
    pub group_id: String,
    pub epoch: u64,
    pub sender_curve: String,
    pub sender_user: String,
}

/// The `signal` part of the engine state. `pins` lives outside `session` so that it
/// survives even if the Signal sessions are reset.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SignalPart {
    #[serde(default)]
    pub session: Option<SignalState>,
    /// Downgrade pins. Maps the protocol-3 Curve25519 key of every peer device
    /// whose verified certificate says it speaks rv-signal-v1 to the Signal
    /// identity key it was certified with.
    #[serde(default)]
    pub pins: BTreeMap<String, String>,
    /// Signal was turned off for this device: its own capability left the account's signed list
    /// (`clearSignalCapability`, on this device or another trusted one). It then stops insisting on
    /// Signal (Olm chat is allowed again) and the apps' upkeep doesn't switch it back on; only an
    /// explicit `signalEnable` with `reenable` does. Messages that still arrive over Signal open as before.
    #[serde(default)]
    pub disabled: bool,
}

/// Whether this device currently speaks Signal: it has a Signal identity and it hasn't been turned off.
pub(crate) fn signal_active(state: &State) -> bool {
    state.signal.session.is_some() && !state.signal.disabled
}

// --- Device certificates ---

/// Binds a device's Signal identity key to its protocol-3 identity. It is signed
/// by the device's Ed25519 key, which is the key its account's signed list names.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DeviceCertificate {
    pub v: u32,
    pub protocol: String,
    pub user_id: String,
    pub device_id: String,
    pub curve25519: String,
    pub ed25519: String,
    pub identity_key: String,
    pub registration_id: u32,
    pub signature: String,
}

fn put(out: &mut Vec<u8>, value: &[u8]) {
    out.extend_from_slice(&(value.len() as u32).to_be_bytes());
    out.extend_from_slice(value);
}

/// The signed bytes. Every field is length-prefixed, so two different
/// certificates can't produce the same bytes.
/// The server repeats this construction in server/src/crypto/signalDirectory.ts.
pub(crate) fn certificate_input(c: &DeviceCertificate) -> Vec<u8> {
    let mut out = Vec::new();
    put(&mut out, CERT_CONTEXT.as_bytes());
    put(&mut out, &c.v.to_be_bytes());
    put(&mut out, c.protocol.as_bytes());
    put(&mut out, c.user_id.as_bytes());
    put(&mut out, c.device_id.as_bytes());
    put(&mut out, c.curve25519.as_bytes());
    put(&mut out, c.ed25519.as_bytes());
    put(&mut out, c.identity_key.as_bytes());
    put(&mut out, &c.registration_id.to_be_bytes());
    out
}

/// What a list capability pins: SHA-256 over the certificate's signed bytes and
/// its signature, in unpadded standard base64 (43 characters).
pub fn certificate_hash(c: &DeviceCertificate) -> String {
    let mut hash = Sha256::new();
    hash.update(certificate_input(c));
    hash.update(c.signature.as_bytes());
    B64_NO_PAD.encode(hash.finalize())
}

/// Checks that the certificate names exactly `device` and is signed by its Ed25519 key.
/// It doesn't check that the device is in a verified list; callers do that.
pub(crate) fn verify_certificate_signature(device: &DeviceIdentity, cert: &DeviceCertificate) -> Result<()> {
    if cert.v != 1 || cert.protocol != SIGNAL_PROTOCOL {
        return Err(Error::new("bad_certificate", "unknown certificate version or protocol"));
    }
    if cert.user_id != device.user_id || cert.device_id != device.device_id || cert.curve25519 != device.curve25519 || cert.ed25519 != device.ed25519 {
        return Err(Error::new("bad_certificate", "the certificate names a different device"));
    }
    let signer = Ed25519PublicKey::from_base64(&device.ed25519).map_err(|_| Error::new("bad_request", "ed25519 isn't a key"))?;
    let signature = Ed25519Signature::from_base64(&cert.signature).map_err(|_| Error::new("bad_certificate", "unreadable signature"))?;
    signer
        .verify(&certificate_input(cert), &signature)
        .map_err(|_| Error::new("bad_certificate", "the certificate isn't signed by this device"))
}

// --- Capabilities in the signed device list ---

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DeviceCapability {
    pub device_id: String,
    pub protocol: String,
    pub certificate_hash: String,
}

/// Appends the capabilities to a list's signed bytes. Nothing is appended when
/// there are none, so lists without capabilities sign the same bytes as before.
/// The caller passes them sorted by device id, then protocol (as `sign_list` does,
/// and as `check_capabilities` requires).
pub(crate) fn append_capabilities(out: &mut Vec<u8>, capabilities: &[DeviceCapability]) {
    if capabilities.is_empty() {
        return;
    }
    out.extend_from_slice(CAPABILITIES_CONTEXT);
    out.extend_from_slice(&(capabilities.len() as u32).to_be_bytes());
    for c in capabilities {
        put(out, c.device_id.as_bytes());
        put(out, c.protocol.as_bytes());
        put(out, c.certificate_hash.as_bytes());
    }
}

/// Each capability must name a listed device and a known protocol, once, in
/// sorted order. An unknown protocol makes the whole list invalid rather than
/// being skipped.
pub(crate) fn check_capabilities(list: &SignedDeviceList) -> Result<()> {
    let bad = |m: &str| Error::new("bad_device_list", m.to_owned());
    if list.capabilities.len() > MAX_CAPABILITIES {
        return Err(bad("too many capabilities"));
    }
    let mut previous: Option<(&str, &str)> = None;
    for c in &list.capabilities {
        if c.protocol != SIGNAL_PROTOCOL {
            return Err(bad("a capability names an unknown protocol"));
        }
        if !list.devices.iter().any(|d| d.device_id == c.device_id) {
            return Err(bad("a capability names a device that isn't listed"));
        }
        if c.certificate_hash.len() != 43 || B64_NO_PAD.decode(&c.certificate_hash).map(|h| h.len()) != Ok(32) {
            return Err(bad("unreadable certificate hash"));
        }
        let key = (c.device_id.as_str(), c.protocol.as_str());
        if previous.is_some_and(|p| p >= key) {
            return Err(bad("capabilities must be sorted and unique"));
        }
        previous = Some(key);
    }
    Ok(())
}

/// The certificate hash a verified list (this account's own list, or a contact's
/// last verified one) pins for the device with this Curve25519 key.
pub(crate) fn listed_capability(state: &State, curve25519: &str) -> Option<String> {
    let lists = state.own_list.iter().chain(state.contacts.values().filter_map(|c| c.list.as_ref()));
    for list in lists {
        if let Some(device) = list.devices.iter().find(|d| d.curve25519 == curve25519) {
            return list
                .capabilities
                .iter()
                .find(|c| c.device_id == device.device_id && c.protocol == SIGNAL_PROTOCOL)
                .map(|c| c.certificate_hash.clone());
        }
    }
    None
}

// --- Downgrade guard ---

/// Refuses an Olm chat message between two devices that both speak Signal: this
/// one (Signal enabled) and the peer (pinned from a verified certificate, or
/// declared in its account's verified signed list).
///
/// A device without Signal, such as the web app (owner decision 2026-11-07),
/// is never refused. It keeps chatting over Olm.
/// Room keys, links and story keys still travel over Olm: groups and stories
/// stay on protocol 3 until groups are decided (plan §5, decision 4).
pub(crate) fn refuse_olm_chat(state: &State, peer_curve25519: &str) -> Result<()> {
    if !signal_active(state) {
        return Ok(());
    }
    if state.signal.pins.contains_key(peer_curve25519) || listed_capability(state, peer_curve25519).is_some() {
        return Err(Error::new(
            "downgrade_refused",
            "both devices speak rv-signal-v1: chat messages between them over redvault-olm-v1 are refused",
        ));
    }
    Ok(())
}

/// A device left its account's signed list (revoked or removed): its Signal
/// session and recorded identity are deleted, so nothing new is encrypted to it.
/// Its downgrade pin is kept. An unlisted device is refused anyway, and if it
/// ever comes back with the same key, the pin still stops it from being
/// contacted over Olm.
pub(crate) fn forget_device(state: &mut State, curve25519: &str) {
    if let Some(st) = state.signal.session.as_mut() {
        st.sessions.remove(curve25519);
        st.identities.remove(curve25519);
    }
}

/// What a newly verified list changed in its devices' Signal capabilities, compared with the
/// list verified before it.
/// - A device whose capability the account removed loses its downgrade pin, so chat with it goes
///   back to Olm, and the apps tell the person. The removal is signed, so only a holder of the
///   account key can make it.
/// - A device whose pinned certificate was replaced (a new Signal identity) loses its pin, its
///   Signal session and its recorded identity, so only the new certificate is accepted from then on.
/// Returns the device ids (added, removed, replaced).
pub(crate) fn apply_capability_change(
    state: &mut State,
    old: Option<&SignedDeviceList>,
    new: &SignedDeviceList,
) -> (Vec<String>, Vec<String>, Vec<String>) {
    let before: Vec<DeviceCapability> = old.map(|l| l.capabilities.clone()).unwrap_or_default();
    let (mut added, mut removed, mut replaced) = (Vec::new(), Vec::new(), Vec::new());
    for c in &new.capabilities {
        match before.iter().find(|b| b.device_id == c.device_id && b.protocol == c.protocol) {
            None => added.push(c.device_id.clone()),
            Some(b) if b.certificate_hash != c.certificate_hash => replaced.push(c.device_id.clone()),
            Some(_) => {}
        }
    }
    for b in &before {
        let still_listed = new.devices.iter().any(|d| d.device_id == b.device_id);
        let kept = new.capabilities.iter().any(|c| c.device_id == b.device_id && c.protocol == b.protocol);
        if still_listed && !kept {
            removed.push(b.device_id.clone());
        }
    }
    // This device's own capability was removed: Signal is turned off here.
    if new.user_id == state.user_id && removed.contains(&state.device_id) {
        state.signal.disabled = true;
    }
    for id in removed.iter().chain(replaced.iter()) {
        if let Some(device) = new.devices.iter().find(|d| &d.device_id == id) {
            let curve = device.curve25519.clone();
            state.signal.pins.remove(&curve);
            if replaced.contains(id) {
                forget_device(state, &curve);
            }
        }
    }
    (added, removed, replaced)
}
