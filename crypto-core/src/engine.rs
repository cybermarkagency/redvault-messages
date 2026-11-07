//! The engine behind every binding: one JSON request in, one JSON response out.
//!
//! The engine keeps no state between calls. Each request carries the device's
//! encrypted state and the key that opens it (the platform keeps that key in
//! its own secure storage: Android Keystore, Windows DPAPI / libsecret, a
//! non-extractable WebCrypto key). Each successful response carries the new
//! encrypted state, which the platform must write *before* it sends anything
//! the call produced, so a crash can never make a key be used twice. A failed
//! call returns no state: the caller keeps the old one, untouched.
//!
//! Protocols, all from vodozemac (Apache-2.0, audited by Least Authority in 2022):
//! - Olm (3DH + Double Ratchet) between two devices, SessionConfig version 1.
//! - Megolm (a sender ratchet, Ed25519-signed messages) for groups; each
//!   sending device has its own outbound session, whose key reaches each
//!   member device only inside an Olm message, so the receiver knows which
//!   device it belongs to.
//!
//! None of this is post-quantum, and Megolm gives no post-compromise security
//! within a session (see docs/vodozemac-poc.md).

use std::collections::BTreeMap;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{XChaCha20Poly1305, XNonce};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use vodozemac::megolm::{
    GroupSession, GroupSessionPickle, InboundGroupSession, InboundGroupSessionPickle, MegolmMessage,
    SessionConfig as MegolmConfig, SessionKey,
};
use vodozemac::olm::{Account, AccountPickle, OlmMessage, Session, SessionConfig, SessionPickle};
use vodozemac::{Curve25519PublicKey, Ed25519PublicKey, Ed25519Signature};
use zeroize::Zeroizing;

/// Identifies this engine's message formats; carried in every response that produces a message.
pub const PROTOCOL: &str = "redvault-olm-v1";
const STATE_VERSION: u32 = 1;
const STATE_AAD: &[u8] = b"redvault-e2ee-state-v1";
const PREKEY_CONTEXT: &str = "redvault-prekey-v1";
/// Olm sessions kept per peer device (newest first); older ones still open late messages.
const MAX_SESSIONS_PER_DEVICE: usize = 5;
/// A sending group session is replaced after this many messages (and whenever a member leaves).
const MAX_MESSAGES_PER_GROUP_SESSION: u32 = 100;
/// Message indexes remembered per inbound group session, to refuse replays.
const MAX_SEEN_INDEXES: usize = 5000;

// --- Errors ---

#[derive(Debug)]
pub struct Error {
    pub code: &'static str,
    pub message: String,
}

impl Error {
    pub(crate) fn new(code: &'static str, message: impl Into<String>) -> Self {
        Error { code, message: message.into() }
    }
}

pub(crate) type Result<T> = std::result::Result<T, Error>;

pub(crate) fn bad_request(message: impl Into<String>) -> Error {
    Error::new("bad_request", message)
}

// --- Public data shapes ---

/// A device's public identity: who it belongs to and its two vodozemac keys.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct DeviceIdentity {
    pub user_id: String,
    pub device_id: String,
    pub curve25519: String,
    pub ed25519: String,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct SignedKey {
    pub key_id: String,
    pub key: String,
    pub signature: String,
}

// --- Encrypted state ---

#[derive(Serialize, Deserialize)]
pub(crate) struct State {
    pub(crate) version: u32,
    pub(crate) user_id: String,
    pub(crate) device_id: String,
    pub(crate) account: AccountPickle,
    /// Peer device Curve25519 key -> its sessions, newest first.
    pub(crate) sessions: BTreeMap<String, Vec<SessionPickle>>,
    /// Peer device Curve25519 key -> who it was first seen as (pinned).
    pub(crate) known_devices: BTreeMap<String, DeviceIdentity>,
    pub(crate) groups: BTreeMap<String, GroupState>,
    /// Inbound group session id -> session.
    pub(crate) inbound: BTreeMap<String, InboundGroup>,
    /// Account identity (src/identity.rs): the account signing key, held only by trusted devices.
    #[serde(default)]
    pub(crate) account_key: Option<String>,
    /// This account's current signed device list.
    #[serde(default)]
    pub(crate) own_list: Option<crate::identity::SignedDeviceList>,
    /// A device waiting to be linked: the secret nonce its QR code carries.
    #[serde(default)]
    pub(crate) pending_link: Option<String>,
    /// Other accounts: pinned account key, whether it was verified, their last device list.
    #[serde(default)]
    pub(crate) contacts: BTreeMap<String, crate::identity::ContactTrust>,
    /// Stories this device published (src/stories.rs), kept as an encrypted archive.
    #[serde(default)]
    pub(crate) stories_out: BTreeMap<String, crate::stories::OwnStory>,
    /// Story keys received from publishers.
    #[serde(default)]
    pub(crate) stories_in: BTreeMap<String, crate::stories::ReceivedStory>,
    /// New devices this trusted device verified by comparing codes (src/pairing.rs), by Curve25519 key.
    #[serde(default)]
    pub(crate) sas_verified: BTreeMap<String, DeviceIdentity>,
    /// On a new device: the trusted device it verified by comparing codes; only it may link this device.
    #[serde(default)]
    pub(crate) pending_link_approver: Option<DeviceIdentity>,
    /// The account's legacy X25519 private key (protocol 1, Files, vault, calls), carried to new devices
    /// inside links and recovery backups so the server's password-wrapped copy can go later.
    #[serde(default)]
    pub(crate) legacy_account_key: Option<String>,
    /// Signal sessions and downgrade pins (src/signal_state.rs). Present in every
    /// build so that a build without the `signal` feature never drops them.
    #[serde(default)]
    pub(crate) signal: crate::signal_state::SignalPart,
    /// Verified group rosters (src/roster.rs), by group id.
    #[serde(default)]
    pub(crate) rosters: BTreeMap<String, crate::roster::RosterState>,
}

#[derive(Serialize, Deserialize, Default)]
pub(crate) struct GroupState {
    /// Every member device, this one included.
    members: Vec<DeviceIdentity>,
    outbound: Option<OutboundGroup>,
}

#[derive(Serialize, Deserialize)]
pub(crate) struct OutboundGroup {
    session: GroupSessionPickle,
    messages_sent: u32,
    /// Curve25519 keys of the member devices this session's key was sent to.
    shared_with: Vec<String>,
}

#[derive(Serialize, Deserialize)]
pub(crate) struct InboundGroup {
    group_id: String,
    /// The device whose Olm session delivered the key: the only possible sender.
    sender: DeviceIdentity,
    session: InboundGroupSessionPickle,
    seen: Vec<u32>,
    /// The sender left the group (or was removed): nothing more is accepted from this session.
    revoked: bool,
}

pub(crate) fn decode_pickle_key(value: &Value) -> Result<Zeroizing<[u8; 32]>> {
    let text = value.as_str().ok_or_else(|| bad_request("pickleKey is required"))?;
    let bytes = Zeroizing::new(B64.decode(text).map_err(|_| bad_request("pickleKey isn't base64"))?);
    if bytes.len() != 32 {
        return Err(bad_request("pickleKey must be 32 bytes"));
    }
    let mut key = Zeroizing::new([0u8; 32]);
    key.copy_from_slice(&bytes);
    Ok(key)
}

pub(crate) fn seal_state(state: &State, key: &[u8; 32]) -> Result<String> {
    let plain = Zeroizing::new(serde_json::to_vec(state).map_err(|e| Error::new("internal", e.to_string()))?);
    let mut nonce = [0u8; 24];
    getrandom::fill(&mut nonce).map_err(|e| Error::new("internal", e.to_string()))?;
    let cipher = XChaCha20Poly1305::new(key.into());
    let sealed = cipher
        .encrypt(&XNonce::from(nonce), Payload { msg: &plain, aad: STATE_AAD })
        .map_err(|_| Error::new("internal", "state encryption failed"))?;
    let mut out = Vec::with_capacity(1 + 24 + sealed.len());
    out.push(STATE_VERSION as u8);
    out.extend_from_slice(&nonce);
    out.extend_from_slice(&sealed);
    Ok(B64.encode(out))
}

fn open_state(value: &Value, key: &[u8; 32]) -> Result<State> {
    let text = value.as_str().ok_or_else(|| bad_request("state is required"))?;
    let raw = B64.decode(text).map_err(|_| Error::new("bad_state", "state isn't base64"))?;
    if raw.len() < 1 + 24 + 16 || raw[0] != STATE_VERSION as u8 {
        return Err(Error::new("bad_state", "unknown state format"));
    }
    let cipher = XChaCha20Poly1305::new(key.into());
    let plain = Zeroizing::new(
        cipher
            .decrypt(
                &XNonce::try_from(&raw[1..25]).map_err(|_| Error::new("bad_state", "unknown state format"))?,
                Payload { msg: &raw[25..], aad: STATE_AAD },
            )
            .map_err(|_| Error::new("bad_state", "wrong pickle key or damaged state"))?,
    );
    serde_json::from_slice(&plain).map_err(|_| Error::new("bad_state", "damaged state"))
}

// --- Helpers ---

pub(crate) fn str_field<'a>(request: &'a Value, name: &str) -> Result<&'a str> {
    request.get(name).and_then(Value::as_str).ok_or_else(|| bad_request(format!("{name} is required")))
}

pub(crate) fn identity_field(request: &Value, name: &str) -> Result<DeviceIdentity> {
    let value = request.get(name).ok_or_else(|| bad_request(format!("{name} is required")))?;
    let identity: DeviceIdentity = serde_json::from_value(value.clone()).map_err(|_| bad_request(format!("{name} isn't a device identity")))?;
    Curve25519PublicKey::from_base64(&identity.curve25519).map_err(|_| bad_request(format!("{name}.curve25519 isn't a key")))?;
    Ed25519PublicKey::from_base64(&identity.ed25519).map_err(|_| bad_request(format!("{name}.ed25519 isn't a key")))?;
    if identity.user_id.is_empty() || identity.device_id.is_empty() {
        return Err(bad_request(format!("{name} needs a userId and deviceId")));
    }
    Ok(identity)
}

pub(crate) fn own_identity(state: &State, account: &Account) -> DeviceIdentity {
    let keys = account.identity_keys();
    DeviceIdentity {
        user_id: state.user_id.clone(),
        device_id: state.device_id.clone(),
        curve25519: keys.curve25519.to_base64(),
        ed25519: keys.ed25519.to_base64(),
    }
}

/// The bytes a device signs for each prekey it publishes: binds the key to that user and device.
pub(crate) fn prekey_signing_input(user_id: &str, device_id: &str, key_id: &str, key: &str) -> Vec<u8> {
    format!("{PREKEY_CONTEXT}\0{user_id}\0{device_id}\0{key_id}\0{key}").into_bytes()
}

/// Remembers who a device key belongs to the first time it's seen; refuses a different answer later.
pub(crate) fn pin_device(state: &mut State, identity: &DeviceIdentity) -> Result<()> {
    match state.known_devices.get(&identity.curve25519) {
        Some(known) if known != identity => Err(Error::new(
            "identity_mismatch",
            "this device key was first seen with a different identity",
        )),
        Some(_) => Ok(()),
        None => {
            state.known_devices.insert(identity.curve25519.clone(), identity.clone());
            Ok(())
        }
    }
}

fn olm_to_json(message: &OlmMessage) -> Value {
    let (message_type, body) = message.to_parts();
    json!({ "type": message_type, "body": B64.encode(body) })
}

fn olm_from_json(value: &Value) -> Result<OlmMessage> {
    let message_type = value.get("type").and_then(Value::as_u64).ok_or_else(|| bad_request("message.type is required"))?;
    let body = value.get("body").and_then(Value::as_str).ok_or_else(|| bad_request("message.body is required"))?;
    let bytes = B64.decode(body).map_err(|_| bad_request("message.body isn't base64"))?;
    OlmMessage::from_parts(message_type as usize, &bytes).map_err(|_| Error::new("malformed_message", "not an Olm message"))
}

// --- Olm: one device to another ---

/// Olm-encrypts `inner` (JSON) to `recipient`, starting a session from its signed one-time key when there's none yet.
pub(crate) fn olm_encrypt(state: &mut State, account: &Account, recipient: &DeviceIdentity, one_time_key: Option<&Value>, inner: &Value) -> Result<Value> {
    crate::identity::check_listed(state, recipient)?;
    pin_device(state, recipient)?;
    let mut sessions: Vec<Session> = state
        .sessions
        .get(&recipient.curve25519)
        .map(|pickles| pickles.iter().map(clone_pickle).collect::<Result<Vec<SessionPickle>>>())
        .transpose()?
        .unwrap_or_default()
        .into_iter()
        .map(Session::from_pickle)
        .collect();
    if sessions.is_empty() {
        let otk_value = one_time_key.ok_or_else(|| Error::new("no_session", "no session with this device yet: a one-time key is needed"))?;
        let otk: SignedKey = serde_json::from_value(otk_value.clone()).map_err(|_| bad_request("oneTimeKey isn't a signed key"))?;
        let signer = Ed25519PublicKey::from_base64(&recipient.ed25519).map_err(|_| bad_request("recipient.ed25519 isn't a key"))?;
        let signature = Ed25519Signature::from_base64(&otk.signature).map_err(|_| Error::new("bad_prekey_signature", "unreadable signature"))?;
        signer
            .verify(&prekey_signing_input(&recipient.user_id, &recipient.device_id, &otk.key_id, &otk.key), &signature)
            .map_err(|_| Error::new("bad_prekey_signature", "the one-time key isn't signed by this device"))?;
        let otk_key = Curve25519PublicKey::from_base64(&otk.key).map_err(|_| bad_request("oneTimeKey.key isn't a key"))?;
        let identity_key = Curve25519PublicKey::from_base64(&recipient.curve25519).map_err(|_| bad_request("recipient.curve25519 isn't a key"))?;
        let session = account
            .create_outbound_session(SessionConfig::version_1(), identity_key, otk_key)
            .map_err(|_| Error::new("bad_prekey", "unsafe one-time key"))?;
        sessions.push(session);
    }
    let plaintext = Zeroizing::new(serde_json::to_vec(inner).map_err(|e| Error::new("internal", e.to_string()))?);
    let message = sessions[0].encrypt(plaintext.as_slice()).map_err(|_| Error::new("encrypt_failed", "session can't encrypt"))?;
    state.sessions.insert(recipient.curve25519.clone(), sessions.iter().map(Session::pickle).collect());
    Ok(olm_to_json(&message))
}

/// Opens an Olm message from `sender` and checks the authenticated payload names both devices.
fn olm_decrypt(state: &mut State, account: &mut Account, sender: &DeviceIdentity, message: &OlmMessage) -> Result<Value> {
    crate::identity::check_listed(state, sender)?;
    pin_device(state, sender)?;
    let mut sessions: Vec<Session> = state
        .sessions
        .remove(&sender.curve25519)
        .unwrap_or_default()
        .into_iter()
        .map(Session::from_pickle)
        .collect();

    let mut plaintext: Option<Vec<u8>> = None;
    let mut duplicate = false;
    for index in 0..sessions.len() {
        // A failed attempt must leave the session as it was: try on a copy.
        let mut candidate = Session::from_pickle(sessions[index].pickle());
        match candidate.decrypt(message) {
            Ok(bytes) => {
                sessions[index] = candidate;
                // The session that just worked goes first: it's the one to reply with.
                let used = sessions.remove(index);
                sessions.insert(0, used);
                plaintext = Some(bytes);
                break;
            }
            Err(vodozemac::olm::DecryptionError::MissingMessageKey(_)) => duplicate = true,
            Err(_) => {}
        }
    }
    if plaintext.is_none() {
        if let OlmMessage::PreKey(pre_key) = message {
            let identity_key = Curve25519PublicKey::from_base64(&sender.curve25519).map_err(|_| bad_request("sender.curve25519 isn't a key"))?;
            match account.create_inbound_session(SessionConfig::version_1(), identity_key, pre_key) {
                Ok(created) => {
                    sessions.insert(0, created.session);
                    plaintext = Some(created.plaintext);
                }
                // Its one-time key was already used: this is a replay of a message already read.
                Err(vodozemac::olm::SessionCreationError::MissingOneTimeKey(_)) => duplicate = true,
                Err(_) => {}
            }
        }
    }
    let plaintext = Zeroizing::new(match plaintext {
        Some(bytes) => bytes,
        None if duplicate => return Err(Error::new("duplicate", "this message was already decrypted")),
        None => return Err(Error::new("decrypt_failed", "no session opens this message")),
    });
    sessions.truncate(MAX_SESSIONS_PER_DEVICE);
    state.sessions.insert(sender.curve25519.clone(), sessions.iter().map(Session::pickle).collect());

    let inner: Value = serde_json::from_slice(&plaintext).map_err(|_| Error::new("malformed_payload", "not a payload"))?;
    // Matrix-style binding: the encrypted payload names sender and recipient,
    // so a message can't be presented as coming from (or meant for) another device.
    let own = own_identity(state, account);
    let names = |v: &Value, who: &DeviceIdentity| {
        v.get("userId").and_then(Value::as_str) == Some(who.user_id.as_str())
            && v.get("deviceId").and_then(Value::as_str) == Some(who.device_id.as_str())
            && v.get("ed25519").and_then(Value::as_str) == Some(who.ed25519.as_str())
    };
    if inner.get("v").and_then(Value::as_u64) != Some(1)
        || !inner.get("sender").is_some_and(|s| names(s, sender))
        || !inner.get("recipient").is_some_and(|r| names(r, &own))
    {
        return Err(Error::new("identity_mismatch", "the payload doesn't name these two devices"));
    }
    Ok(inner)
}

pub(crate) fn payload(state: &State, account: &Account, recipient: &DeviceIdentity, kind: &str, fields: Value) -> Value {
    let own = own_identity(state, account);
    let mut out = json!({
        "v": 1,
        "type": kind,
        "sender": { "userId": own.user_id, "deviceId": own.device_id, "ed25519": own.ed25519 },
        "recipient": { "userId": recipient.user_id, "deviceId": recipient.device_id, "ed25519": recipient.ed25519 },
    });
    if let (Some(target), Value::Object(extra)) = (out.as_object_mut(), fields) {
        target.extend(extra);
    }
    out
}

// --- Commands ---

/// A new device's state (also used by createAccount in src/identity.rs).
pub(crate) fn new_state(user_id: String, device_id: String, account: &Account) -> State {
    State {
        version: STATE_VERSION,
        user_id,
        device_id,
        account: account.pickle(),
        sessions: BTreeMap::new(),
        known_devices: BTreeMap::new(),
        groups: BTreeMap::new(),
        inbound: BTreeMap::new(),
        account_key: None,
        own_list: None,
        pending_link: None,
        contacts: BTreeMap::new(),
        stories_out: BTreeMap::new(),
        stories_in: BTreeMap::new(),
        sas_verified: BTreeMap::new(),
        pending_link_approver: None,
        legacy_account_key: None,
        signal: Default::default(),
        rosters: BTreeMap::new(),
    }
}

fn create_device(request: &Value) -> Result<Value> {
    let key = decode_pickle_key(request.get("pickleKey").unwrap_or(&Value::Null))?;
    let user_id = str_field(request, "userId")?.to_owned();
    let device_id = str_field(request, "deviceId")?.to_owned();
    let account = Account::new();
    let state = new_state(user_id, device_id, &account);
    let identity = own_identity(&state, &account);
    Ok(json!({ "state": seal_state(&state, &key)?, "identity": identity }))
}

fn prekeys(state: &mut State, account: &mut Account, request: &Value) -> Result<Value> {
    let count = request.get("count").and_then(Value::as_u64).unwrap_or(20).min(100) as usize;
    account.generate_one_time_keys(count);
    if account.fallback_key().is_empty() {
        account.generate_fallback_key();
    }
    let sign = |key_id: String, key: String| {
        let signature = account.sign(prekey_signing_input(&state.user_id, &state.device_id, &key_id, &key)).to_base64();
        SignedKey { key_id, key, signature }
    };
    let mut one_time: Vec<SignedKey> = account.one_time_keys().into_iter().map(|(id, key)| sign(id.to_base64(), key.to_base64())).collect();
    one_time.sort_by(|a, b| a.key_id.cmp(&b.key_id));
    let fallback = account.fallback_key().into_iter().next().map(|(id, key)| sign(id.to_base64(), key.to_base64()));
    account.mark_keys_as_published();
    Ok(json!({ "upload": { "identity": own_identity(state, account), "oneTimeKeys": one_time, "fallbackKey": fallback } }))
}

fn encrypt(state: &mut State, account: &mut Account, request: &Value) -> Result<Value> {
    let recipient = identity_field(request, "recipient")?;
    let text = str_field(request, "plaintext")?;
    crate::signal_state::refuse_olm_chat(state, &recipient.curve25519)?;
    let inner = payload(state, account, &recipient, "msg", json!({ "body": text }));
    let message = olm_encrypt(state, account, &recipient, request.get("oneTimeKey"), &inner)?;
    Ok(json!({ "message": message, "protocol": PROTOCOL }))
}

/// One plaintext to many devices (a contact's devices and this account's other devices).
/// Each recipient succeeds or fails on its own; the state keeps every success.
fn encrypt_many(state: &mut State, account: &mut Account, request: &Value) -> Result<Value> {
    let text = str_field(request, "plaintext")?;
    let recipients = request.get("recipients").and_then(Value::as_array).ok_or_else(|| bad_request("recipients is required"))?;
    let mut results = Vec::new();
    for entry in recipients {
        let recipient = match identity_field(entry, "identity") {
            Ok(identity) => identity,
            Err(error) => {
                results.push(json!({ "error": error.code, "message": error.message }));
                continue;
            }
        };
        // rv-signal-v1 when both devices speak it (src/signal.rs); otherwise Olm, as before.
        #[cfg(feature = "signal")]
        if crate::signal::wants_signal(state, &recipient.curve25519) {
            match crate::identity::check_listed(state, &recipient)
                .and_then(|_| crate::signal::encrypt_for(state, account, &recipient, entry.get("signalBundle"), text))
            {
                Ok(message) => results.push(json!({
                    "userId": recipient.user_id, "deviceId": recipient.device_id, "message": message,
                    "protocol": crate::signal_state::SIGNAL_PROTOCOL,
                })),
                Err(error) => results.push(json!({ "userId": recipient.user_id, "deviceId": recipient.device_id, "error": error.code, "message": error.message })),
            }
            continue;
        }
        if let Err(error) = crate::signal_state::refuse_olm_chat(state, &recipient.curve25519) {
            results.push(json!({ "userId": recipient.user_id, "deviceId": recipient.device_id, "error": error.code, "message": error.message }));
            continue;
        }
        let inner = payload(state, account, &recipient, "msg", json!({ "body": text }));
        match olm_encrypt(state, account, &recipient, entry.get("oneTimeKey"), &inner) {
            Ok(message) => results.push(json!({ "userId": recipient.user_id, "deviceId": recipient.device_id, "message": message })),
            Err(error) => results.push(json!({ "userId": recipient.user_id, "deviceId": recipient.device_id, "error": error.code, "message": error.message })),
        }
    }
    Ok(json!({ "results": results, "protocol": PROTOCOL }))
}

/// Which protocol a chat message to each of `recipients` would use, so the apps can show it
/// honestly: "rv-signal-v1" where both devices speak Signal, "redvault-olm-v1" elsewhere.
fn chat_protocols(state: &State, request: &Value) -> Result<Value> {
    let recipients = request.get("recipients").and_then(Value::as_array).ok_or_else(|| bad_request("recipients is required"))?;
    let mut out = Vec::new();
    for entry in recipients {
        let identity = identity_field(entry, "identity")?;
        #[cfg(feature = "signal")]
        let signal = crate::signal::wants_signal(state, &identity.curve25519);
        #[cfg(not(feature = "signal"))]
        let signal = false;
        out.push(json!({
            "userId": identity.user_id,
            "deviceId": identity.device_id,
            "protocol": if signal { crate::signal_state::SIGNAL_PROTOCOL } else { PROTOCOL },
        }));
    }
    Ok(json!({ "protocols": out, "thisDeviceSignal": crate::signal_state::signal_active(state) }))
}

fn decrypt(state: &mut State, account: &mut Account, request: &Value) -> Result<Value> {
    let sender = identity_field(request, "sender")?;
    let message_value = request.get("message").ok_or_else(|| bad_request("message is required"))?;
    // Olm messages carry no protocol tag. Anything tagged must be a protocol this engine knows,
    // and goes only to that protocol's decrypter: no message is tried under two protocols.
    match message_value.get("protocol") {
        None => {}
        Some(Value::String(p)) if p == crate::signal_state::SIGNAL_PROTOCOL => {
            #[cfg(feature = "signal")]
            {
                let signal_request = json!({
                    "sender": request["sender"],
                    "message": message_value,
                    "certificate": message_value.get("certificate").cloned().unwrap_or(Value::Null),
                });
                return crate::signal::decrypt(state, account, &signal_request);
            }
            #[cfg(not(feature = "signal"))]
            return Err(Error::new("unsupported_protocol", "this build can't open rv-signal-v1 messages"));
        }
        Some(_) => return Err(Error::new("unknown_protocol", "a message tagged with a protocol this engine doesn't know")),
    }
    let message = olm_from_json(message_value)?;
    let inner = olm_decrypt(state, account, &sender, &message)?;
    match inner.get("type").and_then(Value::as_str) {
        Some("msg") => {
            crate::signal_state::refuse_olm_chat(state, &sender.curve25519)?;
            let body = inner.get("body").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no body"))?;
            Ok(json!({ "kind": "message", "plaintext": body }))
        }
        Some("room_key") => receive_room_key(state, &sender, &inner),
        Some("link") => crate::identity::receive_link(state, account, &sender, &inner),
        Some("story_key") => crate::stories::receive_story_key(state, &sender, &inner),
        _ => Err(Error::new("malformed_payload", "unknown payload type")),
    }
}

/// A group session key that arrived inside an Olm message: stored as belonging to that sending device.
fn receive_room_key(state: &mut State, sender: &DeviceIdentity, inner: &Value) -> Result<Value> {
    let group_id = inner.get("groupId").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no groupId"))?;
    let session_id = inner.get("sessionId").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no sessionId"))?;
    let key_text = inner.get("sessionKey").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no sessionKey"))?;
    let key = SessionKey::from_base64(key_text).map_err(|_| Error::new("malformed_payload", "bad sessionKey"))?;
    let session = InboundGroupSession::new(&key, MegolmConfig::version_1());
    if session.session_id() != session_id {
        return Err(Error::new("malformed_payload", "sessionId doesn't match the key"));
    }
    if let Some(existing) = state.inbound.get(session_id) {
        if existing.sender != *sender || existing.group_id != group_id {
            return Err(Error::new("room_key_conflict", "this group session already belongs to another sender or group"));
        }
        return Ok(json!({ "kind": "roomKey", "groupId": group_id, "sessionId": session_id }));
    }
    let member = state
        .groups
        .get(group_id)
        .is_some_and(|g| g.members.iter().any(|m| m == sender));
    if !member {
        return Err(Error::new("not_a_member", "a group key from a device that isn't in the group"));
    }
    state.inbound.insert(
        session_id.to_owned(),
        InboundGroup { group_id: group_id.to_owned(), sender: sender.clone(), session: session.pickle(), seen: Vec::new(), revoked: false },
    );
    Ok(json!({ "kind": "roomKey", "groupId": group_id, "sessionId": session_id }))
}

/// Sets a group's member devices. Anyone gone: this device's sending session
/// is replaced (they never get the next one) and their sessions are revoked.
fn group_set_members(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?.to_owned();
    let members: Vec<DeviceIdentity> = serde_json::from_value(request.get("members").cloned().unwrap_or(Value::Null))
        .map_err(|_| bad_request("members must be device identities"))?;
    for member in &members {
        crate::identity::check_listed(state, member)?;
        pin_device(state, member)?;
    }
    let own = own_identity(state, account);
    if !members.contains(&own) {
        return Err(bad_request("members must include this device"));
    }
    let group = state.groups.entry(group_id.clone()).or_default();
    let removed: Vec<DeviceIdentity> = group.members.iter().filter(|m| !members.contains(m)).cloned().collect();
    group.members = members;
    let rotated = !removed.is_empty() && group.outbound.is_some();
    if !removed.is_empty() {
        group.outbound = None;
    }
    let mut revoked = Vec::new();
    for (session_id, inbound) in state.inbound.iter_mut() {
        if inbound.group_id == group_id && removed.contains(&inbound.sender) && !inbound.revoked {
            inbound.revoked = true;
            revoked.push(session_id.clone());
        }
    }
    Ok(json!({ "rotated": rotated, "revokedSessions": revoked }))
}

/// This device's current sending session for the group (made if needed), and who still needs its key.
fn group_session(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?;
    let own = own_identity(state, account);
    let group = state.groups.get_mut(group_id).ok_or_else(|| Error::new("unknown_group", "set the group's members first"))?;
    let worn_out = group.outbound.as_ref().is_some_and(|o| o.messages_sent >= MAX_MESSAGES_PER_GROUP_SESSION);
    if group.outbound.is_none() || worn_out || request.get("rotate").and_then(Value::as_bool) == Some(true) {
        let session = GroupSession::new(MegolmConfig::version_1());
        // This device reads its own messages too.
        let inbound = InboundGroupSession::new(&session.session_key(), MegolmConfig::version_1());
        state.inbound.insert(
            session.session_id(),
            InboundGroup { group_id: group_id.to_owned(), sender: own.clone(), session: inbound.pickle(), seen: Vec::new(), revoked: false },
        );
        group.outbound = Some(OutboundGroup { session: session.pickle(), messages_sent: 0, shared_with: vec![own.curve25519.clone()] });
    }
    let outbound = group.outbound.as_ref().expect("just ensured");
    let unshared: Vec<&DeviceIdentity> = group.members.iter().filter(|m| !outbound.shared_with.contains(&m.curve25519)).collect();
    let session_id = GroupSession::from_pickle(clone_pickle(&outbound.session)?).session_id();
    Ok(json!({ "sessionId": session_id, "unshared": unshared }))
}

pub(crate) fn clone_pickle<T: Serialize + for<'de> Deserialize<'de>>(pickle: &T) -> Result<T> {
    let value = serde_json::to_value(pickle).map_err(|e| Error::new("internal", e.to_string()))?;
    serde_json::from_value(value).map_err(|e| Error::new("internal", e.to_string()))
}

/// The current sending session's key, Olm-encrypted for one member device.
fn share_room_key(state: &mut State, account: &mut Account, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?.to_owned();
    let recipient = identity_field(request, "recipient")?;
    let group = state.groups.get(&group_id).ok_or_else(|| Error::new("unknown_group", "set the group's members first"))?;
    if !group.members.contains(&recipient) {
        return Err(Error::new("not_a_member", "the recipient isn't in the group"));
    }
    let outbound = group.outbound.as_ref().ok_or_else(|| Error::new("no_group_session", "call groupSession first"))?;
    let session = GroupSession::from_pickle(clone_pickle(&outbound.session)?);
    let inner = payload(
        state,
        account,
        &recipient,
        "room_key",
        json!({ "groupId": group_id, "sessionId": session.session_id(), "sessionKey": session.session_key().to_base64() }),
    );
    let message = olm_encrypt(state, account, &recipient, request.get("oneTimeKey"), &inner)?;
    let group = state.groups.get_mut(&group_id).expect("checked above");
    if let Some(outbound) = group.outbound.as_mut() {
        if !outbound.shared_with.contains(&recipient.curve25519) {
            outbound.shared_with.push(recipient.curve25519.clone());
        }
    }
    Ok(json!({ "message": message, "protocol": PROTOCOL }))
}

fn group_encrypt(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?;
    let text = str_field(request, "plaintext")?;
    let own = own_identity(state, account);
    let group = state.groups.get_mut(group_id).ok_or_else(|| Error::new("unknown_group", "set the group's members first"))?;
    let outbound = group.outbound.as_mut().ok_or_else(|| Error::new("no_group_session", "call groupSession first"))?;
    if outbound.messages_sent >= MAX_MESSAGES_PER_GROUP_SESSION {
        return Err(Error::new("no_group_session", "this session is used up: call groupSession again"));
    }
    if group.members.iter().any(|m| !outbound.shared_with.contains(&m.curve25519)) {
        return Err(Error::new("unshared_members", "share the session key with every member first"));
    }
    let mut session = GroupSession::from_pickle(clone_pickle(&outbound.session)?);
    let inner = Zeroizing::new(
        serde_json::to_vec(&json!({ "v": 1, "groupId": group_id, "body": text })).map_err(|e| Error::new("internal", e.to_string()))?,
    );
    let message = session.encrypt(inner.as_slice());
    outbound.session = session.pickle();
    outbound.messages_sent += 1;
    Ok(json!({
        "groupId": group_id,
        "sessionId": session.session_id(),
        "ciphertext": message.to_base64(),
        "sender": { "userId": own.user_id, "deviceId": own.device_id },
        "protocol": PROTOCOL,
    }))
}

fn group_decrypt(state: &mut State, request: &Value) -> Result<Value> {
    let group_id = str_field(request, "groupId")?;
    let session_id = str_field(request, "sessionId")?;
    let message = MegolmMessage::from_base64(str_field(request, "ciphertext")?).map_err(|_| Error::new("malformed_message", "not a Megolm message"))?;
    let inbound = state.inbound.get_mut(session_id).ok_or_else(|| Error::new("unknown_session", "no key for this group session (yet)"))?;
    if inbound.group_id != group_id {
        return Err(Error::new("wrong_group", "this session belongs to another group"));
    }
    if inbound.revoked {
        return Err(Error::new("revoked", "the sender is no longer in the group"));
    }
    if inbound.seen.contains(&message.message_index()) {
        return Err(Error::new("duplicate", "this group message was already decrypted"));
    }
    let mut session = InboundGroupSession::from_pickle(clone_pickle(&inbound.session)?);
    let decrypted = session.decrypt(&message).map_err(|e| match e {
        vodozemac::megolm::DecryptionError::UnknownMessageIndex(..) => Error::new("unknown_index", "this message is from before this device got the key"),
        _ => Error::new("decrypt_failed", "the group message doesn't verify"),
    })?;
    let inner: Value = serde_json::from_slice(&Zeroizing::new(decrypted.plaintext)).map_err(|_| Error::new("malformed_payload", "not a payload"))?;
    if inner.get("v").and_then(Value::as_u64) != Some(1) || inner.get("groupId").and_then(Value::as_str) != Some(group_id) {
        return Err(Error::new("wrong_group", "the payload names another group"));
    }
    let body = inner.get("body").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no body"))?.to_owned();
    inbound.seen.push(decrypted.message_index);
    if inbound.seen.len() > MAX_SEEN_INDEXES {
        inbound.seen.remove(0);
    }
    inbound.session = session.pickle();
    Ok(json!({
        "plaintext": body,
        "messageIndex": decrypted.message_index,
        "sender": { "userId": inbound.sender.user_id, "deviceId": inbound.sender.device_id, "ed25519": inbound.sender.ed25519 },
    }))
}

/// Runs one request against the device's state. Returns the response, with
/// the new state on success; on failure `{ok: false, error, message}` and no state.
pub fn handle(request_text: &str) -> String {
    let response = match serde_json::from_str::<Value>(request_text) {
        Ok(request) => run(&request),
        Err(_) => Err(bad_request("the request isn't JSON")),
    };
    match response {
        Ok(mut value) => {
            if let Some(object) = value.as_object_mut() {
                object.insert("ok".into(), Value::Bool(true));
            }
            value.to_string()
        }
        Err(error) => json!({ "ok": false, "error": error.code, "message": error.message }).to_string(),
    }
}

fn run(request: &Value) -> Result<Value> {
    let op = str_field(request, "op")?;
    match op {
        "version" => {
            return Ok(json!({
                "engine": env!("CARGO_PKG_VERSION"),
                "vodozemac": "0.11.2",
                "protocol": PROTOCOL,
                "signal": if cfg!(feature = "signal") { json!({ "libsignal": "v0.106.2", "protocol": crate::signal_state::SIGNAL_PROTOCOL }) } else { Value::Null },
            }))
        }
        "createDevice" => return create_device(request),
        "createAccount" => return crate::identity::create_account(request),
        // Pairing secrets live in memory only (src/pairing.rs): no stored state involved.
        "sasStart" => return crate::pairing::sas_start(request),
        "sasReveal" => return crate::pairing::sas_reveal(request),
        "sasEstablish" => return crate::pairing::sas_establish(request),
        _ => {}
    }
    let key = decode_pickle_key(request.get("pickleKey").unwrap_or(&Value::Null))?;
    let mut state = open_state(request.get("state").unwrap_or(&Value::Null), &key)?;
    let mut account = Account::from_pickle(clone_pickle(&state.account)?);
    let mut result = match op {
        "identity" => json!({ "identity": own_identity(&state, &account) }),
        "prekeys" => prekeys(&mut state, &mut account, request)?,
        "encrypt" => encrypt(&mut state, &mut account, request)?,
        "decrypt" => decrypt(&mut state, &mut account, request)?,
        "groupSetMembers" => group_set_members(&mut state, &account, request)?,
        "groupSession" => group_session(&mut state, &account, request)?,
        "shareRoomKey" => share_room_key(&mut state, &mut account, request)?,
        "groupEncrypt" => group_encrypt(&mut state, &account, request)?,
        "groupDecrypt" => group_decrypt(&mut state, request)?,
        "encryptMany" => encrypt_many(&mut state, &mut account, request)?,
        "sasMac" => crate::pairing::sas_mac(&state, &account, request)?,
        "sasConfirm" => crate::pairing::sas_confirm(&mut state, request)?,
        "legacyKeys" => crate::identity::legacy_keys(&state)?,
        "cancelLink" => crate::identity::cancel_link(&mut state)?,
        "cancelReset" => crate::identity::cancel_reset(&state, request)?,
        "deviceList" => crate::identity::device_list(&state)?,
        "linkOffer" => crate::identity::link_offer(&mut state, &mut account)?,
        "approveDevice" => crate::identity::approve_device(&mut state, &mut account, request)?,
        "revokeDevice" => crate::identity::revoke_device(&mut state, request)?,
        "rotateAccountKey" => crate::identity::rotate_account_key(&mut state)?,
        "setSignalCapability" => crate::identity::set_signal_capability(&mut state, request)?,
        "clearSignalCapability" => crate::identity::clear_signal_capability(&mut state, request)?,
        "rosterCreate" => crate::roster::roster_create(&mut state, request)?,
        "rosterUpdate" => crate::roster::roster_update(&mut state, request)?,
        "rosterVerify" => crate::roster::roster_verify(&mut state, request)?,
        #[cfg(feature = "signal")]
        "groupSignalShare" => crate::signal_group::share(&mut state, &account, request)?,
        #[cfg(feature = "signal")]
        "groupSignalEncrypt" => crate::signal_group::encrypt(&mut state, &account, request)?,
        #[cfg(feature = "signal")]
        "groupSignalDecrypt" => crate::signal_group::decrypt(&mut state, &account, request)?,
        "chatProtocols" => chat_protocols(&state, request)?,
        "verifyDeviceList" => crate::identity::verify_device_list(&mut state, request)?,
        "trustAccountKey" => crate::identity::trust_account_key(&mut state, request)?,
        "fingerprint" => crate::identity::fingerprint(&state, request)?,
        "verifyFingerprintQr" => crate::identity::verify_fingerprint_qr(&mut state, request)?,
        "setContactVerified" => crate::identity::set_contact_verified(&mut state, request)?,
        "exportAccountBackup" => crate::identity::export_account_backup(&state, request)?,
        "restoreAccountBackup" => crate::identity::restore_account_backup(&mut state, &account, request)?,
        "storyCreate" => crate::stories::story_create(&mut state, &account, request)?,
        "storyShareKey" => crate::stories::story_share_key(&mut state, &mut account, request)?,
        "storySetAudience" => crate::stories::story_set_audience(&mut state, request)?,
        "storyOpen" => crate::stories::story_open(&mut state, request)?,
        "storyExpire" => crate::stories::story_expire(&mut state, request)?,
        "storyDelete" => crate::stories::story_delete(&mut state, request)?,
        "storyArchive" => crate::stories::story_archive(&state)?,
        #[cfg(feature = "signal")]
        "signalEnable" => crate::signal::enable(&mut state, &account, request)?,
        #[cfg(feature = "signal")]
        "signalPrekeys" => crate::signal::prekeys(&mut state, &account, request)?,
        #[cfg(feature = "signal")]
        "signalStartSession" => crate::signal::start_session(&mut state, &account, request)?,
        #[cfg(feature = "signal")]
        "signalEncrypt" => crate::signal::encrypt(&mut state, &account, request)?,
        #[cfg(feature = "signal")]
        "signalDecrypt" => crate::signal::decrypt(&mut state, &account, request)?,
        #[cfg(feature = "signal")]
        "signalStatus" => crate::signal::status(&state)?,
        _ => return Err(bad_request(format!("unknown op {op}"))),
    };
    state.account = account.pickle();
    if let Some(object) = result.as_object_mut() {
        object.insert("state".into(), Value::String(seal_state(&state, &key)?));
    }
    Ok(result)
}
