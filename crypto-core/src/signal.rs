// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Signal sessions for protocol 3 devices: libsignal's PQXDH and Triple Ratchet
//! (Double Ratchet + SPQR), between two devices that protocol 3 already knows.
//!
//! LICENCE: this module links libsignal-protocol and spqr, which are
//! AGPL-3.0-only. It is built only with the `signal` cargo feature, and only
//! for development (docs/signal-integration/01-audit-and-plan.md §5).
//!
//! What this module adds, and what it reuses:
//! - Identity. Each device gets its own libsignal identity key. That key is
//!   bound to the device's protocol-3 identity by a *device certificate*,
//!   signed with the device's vodozemac Ed25519 key. That Ed25519 key is the
//!   one the account's signed device list names. So the chain of trust is:
//!   account key -> signed list -> device Ed25519 key -> Signal identity key.
//! - Trust. Before a session is started with a peer device, or a first message
//!   from it is accepted, the device must be in a verified signed device list:
//!   `identity::check_listed` must return true. There is no trust on first use.
//! - Protocol binding. The inner payload repeats the protocol id, the sender
//!   and the recipient. The ratchet's AEAD authenticates it, and the outer
//!   envelope must match it.
//! - Downgrade. A device with a verified certificate is pinned
//!   (`signal_state::refuse_olm_chat`). Its chat messages are then refused
//!   over Olm.
//! - Crash safety. As with every engine call, the new state is returned with
//!   the ciphertext. The host must store the state before it sends the
//!   ciphertext. A failed call returns no state.
//!
//! The cryptography is libsignal's, unchanged. This module only implements
//! libsignal's store traits over the engine's encrypted state.

use std::collections::BTreeMap;
use std::time::SystemTime;

use async_trait::async_trait;
use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use libsignal_protocol::{
    kem, message_decrypt, message_encrypt, process_prekey_bundle, CiphertextMessage, CiphertextMessageType, DeviceId,
    Direction, GenericSignedPreKey, IdentityChange, IdentityKey, IdentityKeyPair, IdentityKeyStore, KeyPair,
    KyberPreKeyId, KyberPreKeyRecord, KyberPreKeyStore, PreKeyBundle, PreKeyId, PreKeyRecord, PreKeySignalMessage,
    PreKeyStore, ProtocolAddress, PublicKey, SessionRecord, SessionStore, SignalMessage, SignalProtocolError,
    SignedPreKeyId, SignedPreKeyRecord, SignedPreKeyStore, Timestamp,
};
use rand09::{Rng as _, TryRngCore as _};
use serde_json::{json, Value};
use vodozemac::olm::Account;
use zeroize::Zeroizing;

use crate::engine::{bad_request, identity_field, own_identity, pin_device, str_field, DeviceIdentity, Error, Result, State};
use crate::signal_state::{
    certificate_hash, certificate_input, listed_capability, verify_certificate_signature, DeviceCertificate, KyberEntry, SignalState, SIGNAL_PROTOCOL,
};

/// The KEM that Signal's own clients use for PQXDH prekeys (the libsignal bridge's `KYBER_KEY_TYPE`).
const KYBER_KEY_TYPE: kem::KeyType = kem::KeyType::Kyber1024;
/// The most unused one-time keys of each kind kept. This bounds the size of the state.
const MAX_ONE_TIME_KEYS: usize = 100;
/// Base keys remembered per last-resort Kyber key, to refuse replayed first messages.
const MAX_SEEN_BASE_KEYS: usize = 2000;
/// Prekey ids stay below 2^24, as in Signal's clients.
const MAX_KEY_ID: u32 = 0x00FF_FFFF;

type SpResult<T> = std::result::Result<T, SignalProtocolError>;

// --- Errors and small helpers ---

/// Maps libsignal errors to stable engine codes. The messages name no secrets.
fn sig_err(error: SignalProtocolError) -> Error {
    match error {
        SignalProtocolError::DuplicatedMessage(..) => Error::new("duplicate", "this message was already decrypted"),
        SignalProtocolError::UntrustedIdentity(_) => {
            Error::new("untrusted_identity", "the Signal identity key isn't the one this device was certified with")
        }
        SignalProtocolError::SignatureValidationFailed => {
            Error::new("bad_prekey_signature", "a prekey isn't signed by this device's identity key")
        }
        SignalProtocolError::InvalidPreKeyId
        | SignalProtocolError::InvalidKyberPreKeyId
        | SignalProtocolError::InvalidSignedPreKeyId => {
            Error::new("unknown_prekey", "the message uses a prekey this device doesn't have (used already, or never made)")
        }
        // Not "no_session": hosts answer that one by claiming Olm keys.
        SignalProtocolError::SessionNotFound(_) => Error::new("no_signal_session", "no Signal session with this device yet"),
        SignalProtocolError::InvalidMessage(_, _)
        | SignalProtocolError::InvalidProtobufEncoding
        | SignalProtocolError::CiphertextMessageTooShort(_)
        | SignalProtocolError::LegacyCiphertextVersion(_)
        | SignalProtocolError::UnrecognizedCiphertextVersion(_)
        | SignalProtocolError::UnrecognizedMessageVersion(_) => Error::new("decrypt_failed", "this message can't be opened"),
        SignalProtocolError::BadKeyType(_)
        | SignalProtocolError::BadKeyLength(..)
        | SignalProtocolError::NoKeyTypeIdentifier
        | SignalProtocolError::BadKEMKeyType(_)
        | SignalProtocolError::WrongKEMKeyType(..)
        | SignalProtocolError::BadKEMKeyLength(..)
        | SignalProtocolError::InvalidKeyAgreement => Error::new("bad_key", "a key is malformed or of the wrong type"),
        _ => Error::new("signal_error", "the Signal operation failed"),
    }
}

fn b64(bytes: &[u8]) -> String {
    B64.encode(bytes)
}

fn unb64(text: &str, what: &str) -> Result<Vec<u8>> {
    B64.decode(text).map_err(|_| bad_request(format!("{what} isn't base64")))
}

fn stored(text: &str) -> SpResult<Vec<u8>> {
    B64.decode(text).map_err(|_| SignalProtocolError::InvalidState("store", "damaged record".into()))
}

fn rng() -> rand09::rngs::OsRng {
    rand09::rngs::OsRng
}

fn now_millis() -> u64 {
    SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

// --- The libsignal stores, over the engine's encrypted state ---
//
// libsignal takes each store as its own `&mut` borrow. Each store below
// borrows a different field of `SignalState`, so all of them can be lent at once.

struct Identities<'a> {
    known: &'a mut BTreeMap<String, String>,
    pair: IdentityKeyPair,
    registration_id: u32,
}

#[async_trait(?Send)]
impl IdentityKeyStore for Identities<'_> {
    async fn get_identity_key_pair(&self) -> SpResult<IdentityKeyPair> {
        Ok(self.pair)
    }
    async fn get_local_registration_id(&self) -> SpResult<u32> {
        Ok(self.registration_id)
    }
    async fn save_identity(&mut self, address: &ProtocolAddress, identity: &IdentityKey) -> SpResult<IdentityChange> {
        let encoded = b64(&identity.serialize());
        let previous = self.known.insert(address.name().to_owned(), encoded.clone());
        Ok(IdentityChange::from_changed(previous.is_some_and(|p| p != encoded)))
    }
    /// Only the key a verified device certificate named is trusted. An unknown address is not trusted.
    async fn is_trusted_identity(&self, address: &ProtocolAddress, identity: &IdentityKey, _direction: Direction) -> SpResult<bool> {
        Ok(self.known.get(address.name()).is_some_and(|known| *known == b64(&identity.serialize())))
    }
    async fn get_identity(&self, address: &ProtocolAddress) -> SpResult<Option<IdentityKey>> {
        match self.known.get(address.name()) {
            Some(text) => Ok(Some(IdentityKey::decode(&stored(text)?)?)),
            None => Ok(None),
        }
    }
}

struct PreKeys<'a>(&'a mut BTreeMap<u32, String>);

#[async_trait(?Send)]
impl PreKeyStore for PreKeys<'_> {
    async fn get_pre_key(&self, id: PreKeyId) -> SpResult<PreKeyRecord> {
        let text = self.0.get(&u32::from(id)).ok_or(SignalProtocolError::InvalidPreKeyId)?;
        PreKeyRecord::deserialize(&stored(text)?)
    }
    async fn save_pre_key(&mut self, id: PreKeyId, record: &PreKeyRecord) -> SpResult<()> {
        self.0.insert(id.into(), b64(&record.serialize()?));
        Ok(())
    }
    async fn remove_pre_key(&mut self, id: PreKeyId) -> SpResult<()> {
        self.0.remove(&u32::from(id));
        Ok(())
    }
}

struct SignedPreKeys<'a>(&'a mut BTreeMap<u32, String>);

#[async_trait(?Send)]
impl SignedPreKeyStore for SignedPreKeys<'_> {
    async fn get_signed_pre_key(&self, id: SignedPreKeyId) -> SpResult<SignedPreKeyRecord> {
        let text = self.0.get(&u32::from(id)).ok_or(SignalProtocolError::InvalidSignedPreKeyId)?;
        SignedPreKeyRecord::deserialize(&stored(text)?)
    }
    async fn save_signed_pre_key(&mut self, id: SignedPreKeyId, record: &SignedPreKeyRecord) -> SpResult<()> {
        self.0.insert(id.into(), b64(&record.serialize()?));
        Ok(())
    }
}

struct KyberPreKeys<'a> {
    keys: &'a mut BTreeMap<u32, KyberEntry>,
    seen: &'a mut BTreeMap<String, Vec<String>>,
}

#[async_trait(?Send)]
impl KyberPreKeyStore for KyberPreKeys<'_> {
    async fn get_kyber_pre_key(&self, id: KyberPreKeyId) -> SpResult<KyberPreKeyRecord> {
        let entry = self.keys.get(&u32::from(id)).ok_or(SignalProtocolError::InvalidKyberPreKeyId)?;
        KyberPreKeyRecord::deserialize(&stored(&entry.record)?)
    }
    async fn save_kyber_pre_key(&mut self, id: KyberPreKeyId, record: &KyberPreKeyRecord) -> SpResult<()> {
        let last_resort = self.keys.get(&u32::from(id)).is_some_and(|e| e.last_resort);
        self.keys.insert(id.into(), KyberEntry { record: b64(&record.serialize()?), last_resort });
        Ok(())
    }
    /// A one-time Kyber key is deleted on first use. For the last-resort key,
    /// each first message's base key is remembered and a repeat is refused.
    async fn mark_kyber_pre_key_used(&mut self, id: KyberPreKeyId, ec_id: SignedPreKeyId, base_key: &PublicKey) -> SpResult<()> {
        let kyber_id = u32::from(id);
        let last_resort = self.keys.get(&kyber_id).ok_or(SignalProtocolError::InvalidKyberPreKeyId)?.last_resort;
        if !last_resort {
            self.keys.remove(&kyber_id);
            return Ok(());
        }
        let seen = self.seen.entry(format!("{kyber_id}:{}", u32::from(ec_id))).or_default();
        let encoded = b64(&base_key.serialize());
        if seen.contains(&encoded) {
            return Err(SignalProtocolError::InvalidMessage(CiphertextMessageType::PreKey, "reused base key".to_owned()));
        }
        if seen.len() >= MAX_SEEN_BASE_KEYS {
            seen.remove(0);
        }
        seen.push(encoded);
        Ok(())
    }
}

struct Sessions<'a>(&'a mut BTreeMap<String, String>);

#[async_trait(?Send)]
impl SessionStore for Sessions<'_> {
    async fn load_session(&self, address: &ProtocolAddress) -> SpResult<Option<SessionRecord>> {
        match self.0.get(address.name()) {
            Some(text) => Ok(Some(SessionRecord::deserialize(&stored(text)?)?)),
            None => Ok(None),
        }
    }
    async fn store_session(&mut self, address: &ProtocolAddress, record: &SessionRecord) -> SpResult<()> {
        self.0.insert(address.name().to_owned(), b64(&record.serialize()?));
        Ok(())
    }
}

/// All the stores at once, each borrowing its own field.
struct Stores<'a> {
    identities: Identities<'a>,
    prekeys: PreKeys<'a>,
    signed: SignedPreKeys<'a>,
    kyber: KyberPreKeys<'a>,
    sessions: Sessions<'a>,
}

fn signal_state(state: &mut State) -> Result<&mut SignalState> {
    state
        .signal
        .session
        .as_mut()
        .ok_or_else(|| Error::new("signal_not_enabled", "Signal isn't enabled on this device: call signalEnable first"))
}

fn identity_pair(st: &SignalState) -> Result<IdentityKeyPair> {
    let raw = Zeroizing::new(stored(&st.identity).map_err(|_| Error::new("bad_state", "damaged Signal identity"))?);
    IdentityKeyPair::try_from(raw.as_slice()).map_err(|_| Error::new("bad_state", "damaged Signal identity"))
}

fn open_stores(st: &mut SignalState) -> Result<Stores<'_>> {
    let pair = identity_pair(st)?;
    let SignalState { registration_id, prekeys, signed_prekeys, kyber, kyber_seen, sessions, identities, .. } = st;
    Ok(Stores {
        identities: Identities { known: identities, pair, registration_id: *registration_id },
        prekeys: PreKeys(prekeys),
        signed: SignedPreKeys(signed_prekeys),
        kyber: KyberPreKeys { keys: kyber, seen: kyber_seen },
        sessions: Sessions(sessions),
    })
}

/// Peers are addressed by their protocol-3 Curve25519 key, which is unique, and
/// which `pin_device` binds to one user and device. libsignal's device id is always 1.
fn address(device: &DeviceIdentity) -> ProtocolAddress {
    ProtocolAddress::new(device.curve25519.clone(), DeviceId::try_from(1u8).expect("1 is a valid device id"))
}

// --- Device certificates (format and hashing in src/signal_state.rs) ---

fn own_certificate(state: &State, account: &Account) -> Result<DeviceCertificate> {
    let st = state.signal.session.as_ref().ok_or_else(|| Error::new("signal_not_enabled", "Signal isn't enabled on this device"))?;
    let me = own_identity(state, account);
    let pair = identity_pair(st)?;
    let mut cert = DeviceCertificate {
        v: 1,
        protocol: SIGNAL_PROTOCOL.to_owned(),
        user_id: me.user_id,
        device_id: me.device_id,
        curve25519: me.curve25519,
        ed25519: me.ed25519,
        identity_key: b64(&pair.identity_key().serialize()),
        registration_id: st.registration_id,
        signature: String::new(),
    };
    cert.signature = account.sign(certificate_input(&cert)).to_base64();
    Ok(cert)
}

/// Checks that a certificate is signed by `device`'s Ed25519 key and names exactly
/// that device, that the device is in a verified signed device list, and, when
/// that list pins a certificate for the device, that this is that certificate.
/// Returns the certificate and its parsed identity key.
fn verify_certificate(state: &State, device: &DeviceIdentity, value: Option<&Value>) -> Result<(DeviceCertificate, IdentityKey)> {
    let value = value.filter(|v| !v.is_null()).ok_or_else(|| bad_request("certificate is required"))?;
    let cert: DeviceCertificate =
        serde_json::from_value(value.clone()).map_err(|_| Error::new("bad_certificate", "not a device certificate"))?;
    verify_certificate_signature(device, &cert)?;
    if !crate::identity::check_listed(state, device)? {
        return Err(Error::new(
            "unverified_device",
            "this device's account has no verified signed device list here: verify the list before using Signal with it",
        ));
    }
    if let Some(pinned) = listed_capability(state, &device.curve25519) {
        if pinned != certificate_hash(&cert) {
            return Err(Error::new("certificate_mismatch", "the signed device list pins another certificate for this device"));
        }
    }
    let identity = IdentityKey::decode(&unb64(&cert.identity_key, "certificate.identityKey")?)
        .map_err(|_| Error::new("bad_key", "the certified identity key is malformed"))?;
    Ok((cert, identity))
}

/// Records the certified identity for this device and pins it as Signal-capable.
/// A different identity than the one recorded before is a hard error.
fn record_identity(state: &mut State, device: &DeviceIdentity, identity: &IdentityKey) -> Result<()> {
    pin_device(state, device)?;
    let encoded = b64(&identity.serialize());
    let changed = || Error::new("identity_changed", "this device was certified with a different Signal identity key before");
    if state.signal.pins.get(&device.curve25519).is_some_and(|pinned| *pinned != encoded) {
        return Err(changed());
    }
    let st = signal_state(state)?;
    if st.identities.get(&device.curve25519).is_some_and(|known| *known != encoded) {
        return Err(changed());
    }
    st.identities.insert(device.curve25519.clone(), encoded.clone());
    state.signal.pins.insert(device.curve25519.clone(), encoded);
    Ok(())
}

// --- Commands ---

/// Creates this device's Signal identity (once) and returns its signed certificate.
/// After Signal was turned off for this device, only `reenable: true` (the person asked) switches it back on.
pub(crate) fn enable(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    if state.signal.disabled {
        if request.get("reenable").and_then(Value::as_bool) != Some(true) {
            return Err(Error::new("signal_turned_off", "Signal was turned off for this device: turn it back on explicitly"));
        }
        state.signal.disabled = false;
    }
    if state.signal.session.is_none() {
        let mut rng = rng().unwrap_err();
        let pair = IdentityKeyPair::generate(&mut rng);
        state.signal.session = Some(SignalState {
            identity: b64(&Zeroizing::new(pair.serialize().to_vec())),
            // 14 bits and never 0, as in Signal's clients.
            registration_id: rng.random_range(1..=0x3FFF),
            next_prekey_id: 1,
            next_signed_id: 1,
            next_kyber_id: 1,
            ..Default::default()
        });
    }
    Ok(json!({ "certificate": own_certificate(state, account)?, "protocol": SIGNAL_PROTOCOL }))
}

fn next_id(counter: &mut u32) -> u32 {
    let id = (*counter).clamp(1, MAX_KEY_ID);
    *counter = if id >= MAX_KEY_ID { 1 } else { id + 1 };
    id
}

/// Makes the keys a device publishes: a signed EC prekey, a signed last-resort
/// Kyber key, and up to `count` one-time keys of each kind.
/// With `rotate`, the signed and last-resort keys are replaced. The previous
/// ones are kept for messages already on their way; older ones are deleted.
pub(crate) fn prekeys(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let count = request.get("count").and_then(Value::as_u64).unwrap_or(20).min(MAX_ONE_TIME_KEYS as u64) as usize;
    let rotate = request.get("rotate").and_then(Value::as_bool).unwrap_or(false);
    let st = signal_state(state)?;
    let pair = identity_pair(st)?;
    let mut rng = rng().unwrap_err();
    let ts = Timestamp::from_epoch_millis(now_millis());

    if rotate || st.current_signed_id.is_none() {
        let id = next_id(&mut st.next_signed_id);
        let kp = KeyPair::generate(&mut rng);
        let signature = pair.private_key().calculate_signature(&kp.public_key.serialize(), &mut rng).map_err(|_| Error::new("internal", "prekey signing failed"))?;
        let record = SignedPreKeyRecord::new(id.into(), ts, &kp, &signature);
        if let Some(previous) = st.current_signed_id {
            st.signed_prekeys.retain(|k, _| *k == previous);
        }
        st.signed_prekeys.insert(id, b64(&record.serialize().map_err(sig_err)?));
        st.current_signed_id = Some(id);
    }
    if rotate || st.current_last_resort_id.is_none() {
        let id = next_id(&mut st.next_kyber_id);
        let kp = kem::KeyPair::generate(KYBER_KEY_TYPE, &mut rng);
        let signature = pair.private_key().calculate_signature(&kp.public_key.serialize(), &mut rng).map_err(|_| Error::new("internal", "prekey signing failed"))?;
        let record = KyberPreKeyRecord::new(id.into(), ts, &kp, &signature);
        if let Some(previous) = st.current_last_resort_id {
            st.kyber.retain(|k, e| !e.last_resort || *k == previous);
            let keep = format!("{previous}:");
            st.kyber_seen.retain(|k, _| k.starts_with(&keep));
        }
        st.kyber.insert(id, KyberEntry { record: b64(&record.serialize().map_err(sig_err)?), last_resort: true });
        st.current_last_resort_id = Some(id);
    }

    let ec_room = MAX_ONE_TIME_KEYS.saturating_sub(st.prekeys.len()).min(count);
    let mut one_time = Vec::new();
    for _ in 0..ec_room {
        let id = next_id(&mut st.next_prekey_id);
        let kp = KeyPair::generate(&mut rng);
        st.prekeys.insert(id, b64(&PreKeyRecord::new(id.into(), &kp).serialize().map_err(sig_err)?));
        one_time.push(json!({ "id": id, "key": b64(&kp.public_key.serialize()) }));
    }
    let kyber_one_time = st.kyber.values().filter(|e| !e.last_resort).count();
    let kyber_room = MAX_ONE_TIME_KEYS.saturating_sub(kyber_one_time).min(count);
    let mut kyber_keys = Vec::new();
    for _ in 0..kyber_room {
        let id = next_id(&mut st.next_kyber_id);
        let kp = kem::KeyPair::generate(KYBER_KEY_TYPE, &mut rng);
        let signature = pair.private_key().calculate_signature(&kp.public_key.serialize(), &mut rng).map_err(|_| Error::new("internal", "prekey signing failed"))?;
        let record = KyberPreKeyRecord::new(id.into(), ts, &kp, &signature);
        st.kyber.insert(id, KyberEntry { record: b64(&record.serialize().map_err(sig_err)?), last_resort: false });
        kyber_keys.push(json!({ "id": id, "key": b64(&kp.public_key.serialize()), "signature": b64(&signature) }));
    }

    let signed_id = st.current_signed_id.expect("set above");
    let signed = SignedPreKeyRecord::deserialize(&stored(&st.signed_prekeys[&signed_id]).map_err(sig_err)?).map_err(sig_err)?;
    let last_id = st.current_last_resort_id.expect("set above");
    let last = KyberPreKeyRecord::deserialize(&stored(&st.kyber[&last_id].record).map_err(sig_err)?).map_err(sig_err)?;
    let registration_id = st.registration_id;
    Ok(json!({
        "upload": {
            "protocol": SIGNAL_PROTOCOL,
            "certificate": own_certificate(state, account)?,
            "registrationId": registration_id,
            "identityKey": b64(&pair.identity_key().serialize()),
            "signedPreKey": {
                "id": signed_id,
                "key": b64(&signed.public_key().map_err(sig_err)?.serialize()),
                "signature": b64(&signed.signature().map_err(sig_err)?),
            },
            "lastResortKyberPreKey": {
                "id": last_id,
                "key": b64(&last.public_key().map_err(sig_err)?.serialize()),
                "signature": b64(&last.signature().map_err(sig_err)?),
            },
            "preKeys": one_time,
            "kyberPreKeys": kyber_keys,
        }
    }))
}

fn u32_field(value: &Value, name: &str) -> Result<u32> {
    value
        .get(name)
        .and_then(Value::as_u64)
        .and_then(|v| u32::try_from(v).ok())
        .ok_or_else(|| bad_request(format!("{name} must be a 32-bit number")))
}

fn key_field(value: &Value, name: &str) -> Result<Vec<u8>> {
    unb64(value.get(name).and_then(Value::as_str).ok_or_else(|| bad_request(format!("{name} is required")))?, name)
}

/// Starts a session with a peer device from its published bundle (PQXDH).
/// The request has `peer` (its protocol-3 identity), `certificate`, and `bundle`:
/// `{ registrationId, identityKey, signedPreKey{id,key,signature}, kyberPreKey{id,key,signature}, preKey?{id,key} }`.
/// `kyberPreKey` is one of the peer's one-time Kyber keys, or its last-resort key when none are left.
pub(crate) fn start_session(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    signal_state(state)?;
    let peer = identity_field(request, "peer")?;
    let (cert, certified) = verify_certificate(state, &peer, request.get("certificate"))?;
    let bundle = request.get("bundle").ok_or_else(|| bad_request("bundle is required"))?;
    let bundle_identity =
        IdentityKey::decode(&key_field(bundle, "identityKey")?).map_err(|_| Error::new("bad_key", "bundle.identityKey is malformed"))?;
    if bundle_identity != certified {
        return Err(Error::new("identity_mismatch", "the bundle's identity key isn't the certified one"));
    }
    let registration_id = u32_field(bundle, "registrationId")?;
    if cert.registration_id != registration_id {
        return Err(Error::new("identity_mismatch", "the bundle's registration id isn't the certified one"));
    }
    let signed = bundle.get("signedPreKey").ok_or_else(|| bad_request("bundle.signedPreKey is required"))?;
    let kyber = bundle.get("kyberPreKey").ok_or_else(|| bad_request("bundle.kyberPreKey is required (PQXDH)"))?;
    let pre_key = match bundle.get("preKey") {
        None | Some(Value::Null) => None,
        Some(p) => Some((
            PreKeyId::from(u32_field(p, "id")?),
            PublicKey::deserialize(&key_field(p, "key")?).map_err(|_| Error::new("bad_key", "bundle.preKey.key is malformed"))?,
        )),
    };
    let built = PreKeyBundle::new(
        registration_id,
        DeviceId::try_from(1u8).expect("valid"),
        pre_key,
        SignedPreKeyId::from(u32_field(signed, "id")?),
        PublicKey::deserialize(&key_field(signed, "key")?).map_err(|_| Error::new("bad_key", "bundle.signedPreKey.key is malformed"))?,
        key_field(signed, "signature")?,
        KyberPreKeyId::from(u32_field(kyber, "id")?),
        kem::PublicKey::deserialize(&key_field(kyber, "key")?).map_err(|_| Error::new("bad_key", "bundle.kyberPreKey.key is malformed"))?,
        key_field(kyber, "signature")?,
        bundle_identity,
    )
    .map_err(sig_err)?;

    record_identity(state, &peer, &certified)?;
    let me = own_identity(state, account);
    let mut stores = open_stores(signal_state(state)?)?;
    let mut rng = rng().unwrap_err();
    futures_executor::block_on(process_prekey_bundle(
        &address(&peer),
        &address(&me),
        &mut stores.sessions,
        &mut stores.identities,
        &built,
        SystemTime::now(),
        &mut rng,
    ))
    .map_err(sig_err)?;
    Ok(json!({ "protocol": SIGNAL_PROTOCOL, "peer": { "userId": peer.user_id, "deviceId": peer.device_id } }))
}

fn names(v: &Value, who: &DeviceIdentity) -> bool {
    v.get("userId").and_then(Value::as_str) == Some(who.user_id.as_str())
        && v.get("deviceId").and_then(Value::as_str) == Some(who.device_id.as_str())
        && v.get("ed25519").and_then(Value::as_str) == Some(who.ed25519.as_str())
}

/// Encrypts `plaintext` for a peer device that already has a session.
pub(crate) fn encrypt(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let peer = identity_field(request, "peer")?;
    let text = str_field(request, "plaintext")?;
    encrypt_payload(state, account, &peer, json!({ "t": "msg", "body": text }))
}

/// Seals a typed payload to a peer device that already has a session. `fields` carries `t`
/// ("msg", or "sender_key" for groups, M6) and its contents; protocol, sender and recipient are added here.
fn encrypt_payload(state: &mut State, account: &Account, peer: &DeviceIdentity, fields: Value) -> Result<Value> {
    let peer = peer.clone();
    crate::identity::check_listed(state, &peer)?;
    pin_device(state, &peer)?;
    let me = own_identity(state, account);
    let mut payload = json!({
        "v": 1,
        "p": SIGNAL_PROTOCOL,
        "sender": { "userId": me.user_id, "deviceId": me.device_id, "ed25519": me.ed25519 },
        "recipient": { "userId": peer.user_id, "deviceId": peer.device_id, "ed25519": peer.ed25519 },
    });
    if let (Some(target), Value::Object(extra)) = (payload.as_object_mut(), fields) {
        for (k, v) in extra {
            if !matches!(k.as_str(), "v" | "p" | "sender" | "recipient") {
                target.insert(k, v);
            }
        }
    }
    let inner = Zeroizing::new(serde_json::to_vec(&payload).map_err(|e| Error::new("internal", e.to_string()))?);
    let mut stores = open_stores(signal_state(state)?)?;
    let mut rng = rng().unwrap_err();
    let message = futures_executor::block_on(message_encrypt(
        &inner,
        &address(&peer),
        &address(&me),
        &mut stores.sessions,
        &mut stores.identities,
        SystemTime::now(),
        &mut rng,
    ))
    .map_err(sig_err)?;
    let kind = match message.message_type() {
        CiphertextMessageType::PreKey => "prekey",
        CiphertextMessageType::Whisper => "signal",
        _ => return Err(Error::new("internal", "unexpected message type")),
    };
    Ok(json!({
        "protocol": SIGNAL_PROTOCOL,
        "message": { "protocol": SIGNAL_PROTOCOL, "type": kind, "body": b64(message.serialize()) },
    }))
}

/// Opens a message from a peer device. If no identity is recorded for that
/// device yet, its first message (`type: "prekey"`) must come with its `certificate`.
pub(crate) fn decrypt(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    signal_state(state)?;
    let sender = identity_field(request, "sender")?;
    let message = request.get("message").ok_or_else(|| bad_request("message is required"))?;
    if message.get("protocol").and_then(Value::as_str) != Some(SIGNAL_PROTOCOL) {
        return Err(Error::new("protocol_mismatch", "this isn't an rv-signal-v1 message"));
    }
    let body = key_field(message, "body")?;
    let parsed = match message.get("type").and_then(Value::as_str) {
        Some("prekey") => CiphertextMessage::PreKeySignalMessage(
            PreKeySignalMessage::try_from(body.as_slice()).map_err(|_| Error::new("malformed_message", "not a Signal prekey message"))?,
        ),
        Some("signal") => CiphertextMessage::SignalMessage(
            SignalMessage::try_from(body.as_slice()).map_err(|_| Error::new("malformed_message", "not a Signal message"))?,
        ),
        _ => return Err(Error::new("malformed_message", "unknown message type")),
    };
    crate::identity::check_listed(state, &sender)?;
    let known = state.signal.session.as_ref().is_some_and(|st| st.identities.contains_key(&sender.curve25519));
    if known {
        pin_device(state, &sender)?;
    } else {
        let (_, certified) = verify_certificate(state, &sender, request.get("certificate"))?;
        record_identity(state, &sender, &certified)?;
    }
    let me = own_identity(state, account);
    let mut stores = open_stores(signal_state(state)?)?;
    let mut rng = rng().unwrap_err();
    let plaintext = Zeroizing::new(
        futures_executor::block_on(message_decrypt(
            &parsed,
            &address(&sender),
            &address(&me),
            &mut stores.sessions,
            &mut stores.identities,
            &mut stores.prekeys,
            &stores.signed,
            &mut stores.kyber,
            &mut rng,
        ))
        .map_err(sig_err)?,
    );
    let inner: Value = serde_json::from_slice(&plaintext).map_err(|_| Error::new("malformed_payload", "not a payload"))?;
    if inner.get("v").and_then(Value::as_u64) != Some(1) || inner.get("p").and_then(Value::as_str) != Some(SIGNAL_PROTOCOL) {
        return Err(Error::new("protocol_mismatch", "the authenticated payload names another protocol"));
    }
    if !inner.get("sender").is_some_and(|s| names(s, &sender)) || !inner.get("recipient").is_some_and(|r| names(r, &me)) {
        return Err(Error::new("identity_mismatch", "the payload doesn't name these two devices"));
    }
    // Typed payloads (M6). An untyped one is a chat message, as before.
    match inner.get("t").and_then(Value::as_str) {
        None | Some("msg") => {}
        Some("sender_key") => {
            let key = inner.get("senderKey").ok_or_else(|| Error::new("malformed_payload", "no sender key"))?;
            return crate::signal_group::receive_sender_key(state, &sender, key);
        }
        Some(_) => return Err(Error::new("malformed_payload", "unknown payload type")),
    }
    let body = inner.get("body").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no body"))?;
    Ok(json!({ "protocol": SIGNAL_PROTOCOL, "kind": "message", "plaintext": body }))
}

// --- Routing for the apps (encryptMany and decrypt in src/engine.rs) ---

/// Whether chat to `peer` must go over Signal: this device has Signal, and the peer is pinned
/// (verified certificate) or declared Signal-capable in its account's verified signed list.
/// Exactly the pairs `signal_state::refuse_olm_chat` refuses over Olm.
pub(crate) fn wants_signal(state: &State, peer_curve25519: &str) -> bool {
    crate::signal_state::signal_active(state)
        && (state.signal.pins.contains_key(peer_curve25519) || listed_capability(state, peer_curve25519).is_some())
}

/// One recipient of `encryptMany` over Signal. Without a session, `claimed` must be what the
/// server's `/signal/claim` returned for this device (`{certificate, bundle}`); otherwise the
/// result is `no_signal_session` and the app claims a bundle and tries again. A first message
/// carries this device's certificate, so the recipient can check it against the signed list.
pub(crate) fn encrypt_for(state: &mut State, account: &Account, recipient: &DeviceIdentity, claimed: Option<&Value>, text: &str) -> Result<Value> {
    encrypt_payload_for(state, account, recipient, claimed, json!({ "t": "msg", "body": text }))
}

/// [`encrypt_for`] with a typed payload (see [`encrypt_payload`]).
pub(crate) fn encrypt_payload_for(state: &mut State, account: &Account, recipient: &DeviceIdentity, claimed: Option<&Value>, fields: Value) -> Result<Value> {
    let peer = serde_json::to_value(recipient).map_err(|e| Error::new("internal", e.to_string()))?;
    let has_session = state.signal.session.as_ref().is_some_and(|st| st.sessions.contains_key(&recipient.curve25519));
    if !has_session {
        let claimed = claimed.ok_or_else(|| Error::new("no_signal_session", "no Signal session with this device yet: claim its bundle"))?;
        if claimed.get("certificate").is_none_or(Value::is_null) || claimed.get("bundle").is_none_or(Value::is_null) {
            // The list says this device speaks Signal but the server has no keys for it: never fall back to Olm.
            return Err(Error::new("signal_keys_missing", "the server has no Signal keys for a device that speaks Signal"));
        }
        start_session(state, account, &json!({ "peer": peer, "certificate": claimed["certificate"], "bundle": claimed["bundle"] }))?;
    }
    let sealed = encrypt_payload(state, account, recipient, fields)?;
    let mut message = sealed["message"].clone();
    if message["type"] == "prekey" {
        message["certificate"] = serde_json::to_value(own_certificate(state, account)?).map_err(|e| Error::new("internal", e.to_string()))?;
    }
    Ok(message)
}

/// Reports whether Signal is on, the key counts, and the sessions and pins this device has.
pub(crate) fn status(state: &State) -> Result<Value> {
    let Some(st) = state.signal.session.as_ref() else {
        return Ok(json!({ "enabled": false, "disabled": state.signal.disabled, "pinned": state.signal.pins.len() }));
    };
    Ok(json!({
        "enabled": true,
        "disabled": state.signal.disabled,
        "protocol": SIGNAL_PROTOCOL,
        "oneTimePreKeys": st.prekeys.len(),
        "oneTimeKyberPreKeys": st.kyber.values().filter(|e| !e.last_resort).count(),
        "sessions": st.sessions.len(),
        "pinned": state.signal.pins.len(),
    }))
}
