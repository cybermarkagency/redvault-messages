//! Account identity and device management (docs/vodozemac-poc.md, "Identity").
//!
//! - **Account signing key.** An Ed25519 key per account, made on the first
//!   device and held only by trusted devices (inside their encrypted state); it
//!   never goes to the server. Its public half is the account's identity.
//! - **Signed device lists.** The account key signs the list of the account's
//!   devices, with a version number. Other accounts pin the account key on first
//!   sight and from then on accept a list only if that key signed it: a device
//!   the server adds on its own isn't in a signed list, so it can't receive or
//!   send messages (`unlisted_device`). A lower version (rollback), a different
//!   list for the same version (equivocation) and a different account key are all
//!   refused, never accepted silently.
//! - **Linking.** A new device shows a QR code with its identity, a signed
//!   one-time key and a secret nonce. A trusted device scans it, adds the device
//!   to a new list version and sends it the account key over an Olm session
//!   started from that one-time key; the nonce, which only the scanner saw,
//!   proves to the new device who sent it.
//! - **Revocation** removes a device from the next list version.
//! - **Fingerprints** follow Signal's documented numeric fingerprint
//!   construction (iterated SHA-512 over a version, the key and a stable id),
//!   over the account signing keys, with a QR form to compare by scanning.
//! - **Recovery backup**: the account key encrypted (XChaCha20-Poly1305) under
//!   the 32-byte random recovery key, for a device that has no other.

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{XChaCha20Poly1305, XNonce};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha512};
use subtle::ConstantTimeEq;
use vodozemac::olm::Account;
use vodozemac::{Ed25519PublicKey, Ed25519SecretKey, Ed25519Signature};
use zeroize::Zeroizing;

use crate::engine::{
    bad_request, decode_pickle_key, identity_field, new_state, olm_encrypt, own_identity, payload, prekey_signing_input, seal_state,
    str_field, DeviceIdentity, Error, Result, SignedKey, State,
};

const DEVICE_LIST_CONTEXT: &[u8] = b"redvault-device-list-v1\0";
const BACKUP_AAD: &str = "redvault-account-backup-v1";
const FINGERPRINT_VERSION: u16 = 1;
const FINGERPRINT_ITERATIONS: usize = 5200;
const FINGERPRINT_QR_MAGIC: &[u8] = b"RVFP";
/// What the apps put in the QR code: this prefix, then the QR bytes in base64. The prefix keeps it
/// apart from the older account safety number's code, which the same Verify screens scan.
const FINGERPRINT_QR_TEXT_PREFIX: &str = "RVDK1:";
/// The key type byte before an Ed25519 key in fingerprints (Signal uses 0x05 for its Curve25519 keys).
const FINGERPRINT_KEY_TYPE: u8 = 0xed;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct SignedDeviceList {
    pub user_id: String,
    pub version: u64,
    pub devices: Vec<DeviceIdentity>,
    /// The account signing key (Ed25519, vodozemac's unpadded base64).
    pub account_key: String,
    pub signature: String,
    /// The account's legacy signing key (migration 030) vouching for `account_key` (not part of the signed bytes).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub legacy_certificate: Option<LegacyCertificate>,
    /// Protocols listed devices speak beyond Olm, each pinned to one certificate
    /// (src/signal_state.rs). Signed with the list, so a server can neither hide
    /// nor swap them. An empty list keeps the original signed bytes.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub capabilities: Vec<crate::signal_state::DeviceCapability>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct LegacyCertificate {
    /// Raw 32-byte Ed25519 public key, standard base64 (as the apps registered it with the server).
    pub legacy_signing_key: String,
    pub signature: String,
}

const CERTIFICATE_CONTEXT: &str = "redvault-account-key-certificate-v1";

// Statements the account key signs for the server's policy (server/src/crypto/deviceList.ts has the same layouts).
const ROTATION_CONTEXT: &str = "redvault-account-key-rotation-v1";
const RESET_CANCEL_CONTEXT: &str = "redvault-account-reset-cancel-v1";
const BACKUP_SIGNATURE_CONTEXT: &str = "redvault-recovery-backup-v1";

fn statement(context: &str, user_id: &str, subject: &str) -> Vec<u8> {
    format!("{context}\0{user_id}\0{subject}").into_bytes()
}

fn sign_statement(secret: &str, context: &str, user_id: &str, subject: &str) -> Result<String> {
    let key = Ed25519SecretKey::from_base64(secret).map_err(|_| Error::new("bad_state", "unreadable account key"))?;
    Ok(key.sign(&statement(context, user_id, subject)).to_base64())
}

fn certificate_input(user_id: &str, account_key: &str) -> Vec<u8> {
    format!("{CERTIFICATE_CONTEXT}\0{user_id}\0{account_key}").into_bytes()
}

/// The legacy signing key the apps derive (migration 030): libsodium's crypto_kdf_derive_from_key(32, 1,
/// "rvsignk1", accountPrivateKey), i.e. BLAKE2b keyed with the account key, salt = subkey id (LE) and
/// personal = context, then Ed25519 from that seed.
fn legacy_signing_key(legacy_account_key: &str) -> Result<Ed25519SecretKey> {
    use blake2::digest::{consts::U32, Mac as _};
    let master = Zeroizing::new(B64.decode(legacy_account_key).map_err(|_| bad_request("legacyAccountKey isn't base64"))?);
    if master.len() != 32 {
        return Err(bad_request("legacyAccountKey must be 32 bytes"));
    }
    let mut salt = [0u8; 16];
    salt[..8].copy_from_slice(&1u64.to_le_bytes());
    let mut personal = [0u8; 16];
    personal[..8].copy_from_slice(b"rvsignk1");
    let mut mac = blake2::Blake2bMac::<U32>::new_with_salt_and_personal(Some(master.as_slice()), &salt, &personal)
        .map_err(|_| Error::new("internal", "kdf setup failed"))?;
    mac.update(&[]);
    let mut seed = Zeroizing::new([0u8; 32]);
    seed.copy_from_slice(mac.finalize().into_bytes().as_slice());
    Ok(Ed25519SecretKey::from_slice(&seed))
}

fn certify(legacy_account_key: &str, user_id: &str, account_key: &str) -> Result<LegacyCertificate> {
    let legacy = legacy_signing_key(legacy_account_key)?;
    let signature = legacy.sign(&certificate_input(user_id, account_key)).to_base64();
    Ok(LegacyCertificate { legacy_signing_key: B64.encode(legacy.public_key().as_bytes()), signature })
}

/// A legacy key arriving in a link or a backup must be the one the account's list is certified with.
fn check_legacy_matches(list: &SignedDeviceList, legacy_account_key: &str) -> Result<()> {
    let Some(certificate) = &list.legacy_certificate else { return Ok(()) };
    let derived = B64.encode(legacy_signing_key(legacy_account_key)?.public_key().as_bytes());
    if !bool::from(derived.as_bytes().ct_eq(certificate.legacy_signing_key.as_bytes())) {
        return Err(Error::new("legacy_key_mismatch", "the legacy account key doesn't match the account's certificate"));
    }
    Ok(())
}

fn check_certificate(list: &SignedDeviceList) -> Result<Option<String>> {
    let Some(certificate) = &list.legacy_certificate else { return Ok(None) };
    let raw = B64.decode(&certificate.legacy_signing_key).map_err(|_| Error::new("bad_device_list", "unreadable legacy key"))?;
    let raw: [u8; 32] = raw.try_into().map_err(|_| Error::new("bad_device_list", "unreadable legacy key"))?;
    let key = Ed25519PublicKey::from_slice(&raw).map_err(|_| Error::new("bad_device_list", "unreadable legacy key"))?;
    let signature = Ed25519Signature::from_base64(&certificate.signature).map_err(|_| Error::new("bad_device_list", "unreadable certificate"))?;
    key.verify(&certificate_input(&list.user_id, &list.account_key), &signature)
        .map_err(|_| Error::new("bad_device_list", "the legacy certificate doesn't verify"))?;
    Ok(Some(certificate.legacy_signing_key.clone()))
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct ContactTrust {
    pub account_key: String,
    /// Compared in person (QR or safety number).
    pub verified: bool,
    pub list: Option<SignedDeviceList>,
}

// --- Device lists ---

fn length_prefixed(out: &mut Vec<u8>, value: &str) {
    out.extend_from_slice(&(value.len() as u32).to_be_bytes());
    out.extend_from_slice(value.as_bytes());
}

/// What the account key signs: every field length-prefixed, devices in device-id order,
/// then the capabilities (only when there are any, so lists without them sign the same bytes as before).
fn list_signing_input(user_id: &str, version: u64, devices: &[DeviceIdentity], capabilities: &[crate::signal_state::DeviceCapability]) -> Vec<u8> {
    let mut sorted: Vec<&DeviceIdentity> = devices.iter().collect();
    sorted.sort_by(|a, b| a.device_id.cmp(&b.device_id));
    let mut out = DEVICE_LIST_CONTEXT.to_vec();
    length_prefixed(&mut out, user_id);
    out.extend_from_slice(&version.to_be_bytes());
    out.extend_from_slice(&(sorted.len() as u32).to_be_bytes());
    for device in sorted {
        length_prefixed(&mut out, &device.user_id);
        length_prefixed(&mut out, &device.device_id);
        length_prefixed(&mut out, &device.curve25519);
        length_prefixed(&mut out, &device.ed25519);
    }
    crate::signal_state::append_capabilities(&mut out, capabilities);
    out
}

fn account_secret(state: &State) -> Result<Zeroizing<String>> {
    state.account_key.clone().map(Zeroizing::new).ok_or_else(|| Error::new("not_trusted", "this device doesn't hold the account key"))
}

/// Signs a list. Capabilities of devices that aren't in `devices` are dropped.
fn sign_list(
    secret: &str,
    user_id: &str,
    version: u64,
    devices: Vec<DeviceIdentity>,
    capabilities: Vec<crate::signal_state::DeviceCapability>,
) -> Result<SignedDeviceList> {
    let key = Ed25519SecretKey::from_base64(secret).map_err(|_| Error::new("bad_state", "unreadable account key"))?;
    let mut capabilities: Vec<_> = capabilities.into_iter().filter(|c| devices.iter().any(|d| d.device_id == c.device_id)).collect();
    capabilities.sort_by(|a, b| (&a.device_id, &a.protocol).cmp(&(&b.device_id, &b.protocol)));
    let signature = key.sign(&list_signing_input(user_id, version, &devices, &capabilities)).to_base64();
    Ok(SignedDeviceList {
        user_id: user_id.to_owned(),
        version,
        devices,
        account_key: key.public_key().to_base64(),
        signature,
        legacy_certificate: None,
        capabilities,
    })
}

/// Checks a list's signature and shape (not whether its key is the one to trust).
pub(crate) fn check_list(list: &SignedDeviceList) -> Result<()> {
    let key = Ed25519PublicKey::from_base64(&list.account_key).map_err(|_| Error::new("bad_device_list", "unreadable account key"))?;
    let signature = Ed25519Signature::from_base64(&list.signature).map_err(|_| Error::new("bad_device_list", "unreadable signature"))?;
    key.verify(&list_signing_input(&list.user_id, list.version, &list.devices, &list.capabilities), &signature)
        .map_err(|_| Error::new("bad_device_list", "the list isn't signed by its account key"))?;
    let mut ids = std::collections::BTreeSet::new();
    let mut keys = std::collections::BTreeSet::new();
    for device in &list.devices {
        if device.user_id != list.user_id || !ids.insert(device.device_id.clone()) || !keys.insert(device.curve25519.clone()) {
            return Err(Error::new("bad_device_list", "a device is listed twice or for another account"));
        }
        vodozemac::Curve25519PublicKey::from_base64(&device.curve25519).map_err(|_| Error::new("bad_device_list", "unreadable device key"))?;
        Ed25519PublicKey::from_base64(&device.ed25519).map_err(|_| Error::new("bad_device_list", "unreadable device key"))?;
    }
    crate::signal_state::check_capabilities(list)?;
    check_certificate(list)?;
    Ok(())
}

/// Signs the next version of this account's list, carrying the legacy certificate forward.
fn next_list(state: &State, secret: &str, devices: Vec<DeviceIdentity>) -> Result<SignedDeviceList> {
    let current = state.own_list.as_ref().ok_or_else(|| Error::new("not_linked", "no device list"))?;
    let mut list = sign_list(secret, &state.user_id, current.version + 1, devices, current.capabilities.clone())?;
    if list.account_key == current.account_key {
        list.legacy_certificate = current.legacy_certificate.clone();
    }
    Ok(list)
}

/// A trusted device stops a pending account reset (one the server holds for a delay, see
/// server/src/services/v3DeviceService.ts) by signing its refusal with the current account key.
pub(crate) fn cancel_reset(state: &State, request: &Value) -> Result<Value> {
    let secret = account_secret(state)?;
    let pending = str_field(request, "pendingAccountKey")?;
    Ed25519PublicKey::from_base64(pending).map_err(|_| bad_request("pendingAccountKey isn't a key"))?;
    Ok(json!({ "signature": sign_statement(&secret, RESET_CANCEL_CONTEXT, &state.user_id, pending)? }))
}

/// A new device gives up waiting to be linked (another method, or a pairing the person abandoned).
pub(crate) fn cancel_link(state: &mut State) -> Result<Value> {
    state.pending_link = None;
    state.pending_link_approver = None;
    Ok(json!({ "cancelled": true }))
}

/// The legacy account key this device holds, for the app's protocol-1, Files, vault and calls.
pub(crate) fn legacy_keys(state: &State) -> Result<Value> {
    Ok(json!({ "legacyAccountKey": state.legacy_account_key }))
}

fn list_field(request: &Value, name: &str) -> Result<SignedDeviceList> {
    serde_json::from_value(request.get(name).cloned().unwrap_or(Value::Null)).map_err(|_| bad_request(format!("{name} isn't a signed device list")))
}

/// When a signed list is known for the device's account, the device must be in it.
pub(crate) fn check_listed(state: &State, device: &DeviceIdentity) -> Result<bool> {
    let list = if device.user_id == state.user_id {
        state.own_list.as_ref()
    } else {
        state.contacts.get(&device.user_id).and_then(|c| c.list.as_ref())
    };
    match list {
        Some(list) if list.devices.contains(device) => Ok(true),
        Some(_) => Err(Error::new("unlisted_device", "this device isn't in its account's signed device list")),
        None => Ok(false),
    }
}

fn diff(old: &[DeviceIdentity], new: &[DeviceIdentity]) -> (Vec<DeviceIdentity>, Vec<DeviceIdentity>) {
    let added = new.iter().filter(|d| !old.contains(d)).cloned().collect();
    let removed = old.iter().filter(|d| !new.contains(d)).cloned().collect();
    (added, removed)
}

// --- Commands ---

/// The first device of a new account: its device keys, the account key and list version 1.
pub(crate) fn create_account(request: &Value) -> Result<Value> {
    let key = decode_pickle_key(request.get("pickleKey").unwrap_or(&Value::Null))?;
    let account = Account::new();
    let mut state = new_state(str_field(request, "userId")?.to_owned(), str_field(request, "deviceId")?.to_owned(), &account);
    let secret = Ed25519SecretKey::new();
    let own = own_identity(&state, &account);
    let mut list = sign_list(&secret.to_base64(), &state.user_id, 1, vec![own.clone()], Vec::new())?;
    // An existing account moving to per-device identities: its legacy signing key, which
    // contacts already pinned, vouches for the new account key.
    if let Some(legacy) = request.get("legacyAccountKey").and_then(Value::as_str) {
        list.legacy_certificate = Some(certify(legacy, &state.user_id, &list.account_key)?);
        state.legacy_account_key = Some(legacy.to_owned());
    }
    state.account_key = Some(secret.to_base64());
    state.own_list = Some(list.clone());
    Ok(json!({ "state": seal_state(&state, &key)?, "identity": own, "deviceList": list, "accountKey": list.account_key }))
}

pub(crate) fn device_list(state: &State) -> Result<Value> {
    let list = state.own_list.clone().ok_or_else(|| Error::new("not_linked", "this device isn't linked to its account yet"))?;
    Ok(json!({ "deviceList": list, "trusted": state.account_key.is_some() }))
}

/// A new device's QR code: its identity, a fresh signed one-time key, and a secret nonce.
pub(crate) fn link_offer(state: &mut State, account: &mut Account) -> Result<Value> {
    account.generate_one_time_keys(1);
    let (key_id, key) = account
        .one_time_keys()
        .into_iter()
        .max_by_key(|(id, _)| id.to_base64())
        .ok_or_else(|| Error::new("internal", "no one-time key"))?;
    account.mark_keys_as_published();
    let key_id = key_id.to_base64();
    let key = key.to_base64();
    let signature = account.sign(prekey_signing_input(&state.user_id, &state.device_id, &key_id, &key)).to_base64();
    let mut nonce = [0u8; 16];
    getrandom::fill(&mut nonce).map_err(|e| Error::new("internal", e.to_string()))?;
    let nonce = B64.encode(nonce);
    state.pending_link = Some(nonce.clone());
    let qr = json!({
        "v": 1,
        "userId": state.user_id,
        "device": own_identity(state, account),
        "oneTimeKey": SignedKey { key_id, key, signature },
        "nonce": nonce,
    });
    Ok(json!({ "qr": qr, "qrText": B64.encode(qr.to_string()) }))
}

/// A trusted device adds a scanned device to the next list version and sends it the account key.
pub(crate) fn approve_device(state: &mut State, account: &mut Account, request: &Value) -> Result<Value> {
    let secret = account_secret(state)?;
    let qr: Value = match request.get("qr") {
        Some(Value::String(text)) => serde_json::from_slice(&B64.decode(text).map_err(|_| bad_request("qr isn't base64"))?)
            .map_err(|_| bad_request("qr isn't a link offer"))?,
        Some(value) => value.clone(),
        None => return Err(bad_request("qr is required")),
    };
    if qr.get("v").and_then(Value::as_u64) != Some(1) {
        return Err(bad_request("unknown link offer version"));
    }
    let device = identity_field(&qr, "device")?;
    if device.user_id != state.user_id || qr.get("userId").and_then(Value::as_str) != Some(state.user_id.as_str()) {
        return Err(Error::new("wrong_account", "this QR code is for another account"));
    }
    let nonce = qr.get("nonce").and_then(Value::as_str).ok_or_else(|| bad_request("the QR code has no nonce"))?;
    let otk: SignedKey = serde_json::from_value(qr.get("oneTimeKey").cloned().unwrap_or(Value::Null)).map_err(|_| bad_request("no one-time key"))?;
    let signer = Ed25519PublicKey::from_base64(&device.ed25519).map_err(|_| bad_request("unreadable device key"))?;
    let signature = Ed25519Signature::from_base64(&otk.signature).map_err(|_| Error::new("bad_prekey_signature", "unreadable signature"))?;
    signer
        .verify(&prekey_signing_input(&device.user_id, &device.device_id, &otk.key_id, &otk.key), &signature)
        .map_err(|_| Error::new("bad_prekey_signature", "the one-time key isn't signed by this device"))?;

    // The app must say how the person checked the device: a QR code scanned off its screen (the
    // offer itself, with its secret nonce, came from the camera), or codes compared (src/pairing.rs),
    // in which case only the device verified that way is accepted.
    match request.get("verifiedBy").and_then(Value::as_str) {
        Some("qr") => {}
        Some("sas") => {
            if state.sas_verified.get(&device.curve25519) != Some(&device) {
                return Err(Error::new("not_verified", "compare the pairing codes with this device first"));
            }
        }
        _ => return Err(bad_request("verifiedBy must be \"qr\" or \"sas\"")),
    }
    let current = state.own_list.clone().ok_or_else(|| Error::new("not_linked", "no device list"))?;
    if current.devices.iter().any(|d| d.device_id == device.device_id && *d != device) {
        return Err(Error::new("device_id_taken", "another device already uses this device id"));
    }
    let mut devices = current.devices.clone();
    if !devices.contains(&device) {
        devices.push(device.clone());
    }
    let list = next_list(state, &secret, devices)?;
    state.own_list = Some(list.clone());
    // The code comparison stays valid until a list that includes the device is adopted: if this
    // list loses a race on the server, the approval is redone on top of the winner.
    let mut fields = json!({ "nonce": nonce, "accountKey": secret.as_str(), "deviceList": list });
    if let Some(legacy) = &state.legacy_account_key {
        fields["legacyAccountKey"] = json!(legacy);
    }
    let inner = payload(state, account, &device, "link", fields);
    let message = olm_encrypt(state, account, &device, Some(&serde_json::to_value(&otk).unwrap_or(Value::Null)), &inner)?;
    Ok(json!({ "deviceList": list, "message": message, "added": device }))
}

/// The new device receives the account key and the list (from `decrypt`).
pub(crate) fn receive_link(state: &mut State, account: &Account, sender: &DeviceIdentity, inner: &Value) -> Result<Value> {
    let expected = state.pending_link.clone().ok_or_else(|| Error::new("no_pending_link", "this device isn't waiting to be linked"))?;
    let nonce = inner.get("nonce").and_then(Value::as_str).unwrap_or("");
    if !bool::from(expected.as_bytes().ct_eq(nonce.as_bytes())) {
        return Err(Error::new("bad_link", "the link didn't come from the device that scanned this QR code"));
    }
    // Paired by comparing codes: the nonce went through the server, so only the verified device may link us.
    if let Some(approver) = &state.pending_link_approver {
        if approver != sender {
            return Err(Error::new("bad_link", "the link didn't come from the device whose codes you compared"));
        }
    }
    let secret = Zeroizing::new(inner.get("accountKey").and_then(Value::as_str).ok_or_else(|| Error::new("bad_link", "no account key"))?.to_owned());
    let list: SignedDeviceList =
        serde_json::from_value(inner.get("deviceList").cloned().unwrap_or(Value::Null)).map_err(|_| Error::new("bad_link", "no device list"))?;
    check_list(&list)?;
    let public = Ed25519SecretKey::from_base64(&secret).map_err(|_| Error::new("bad_link", "unreadable account key"))?.public_key().to_base64();
    let own = own_identity(state, account);
    if public != list.account_key || list.user_id != state.user_id || !list.devices.contains(&own) || !list.devices.contains(sender) {
        return Err(Error::new("bad_link", "the account key, the list and the devices don't match"));
    }
    let legacy = inner.get("legacyAccountKey").and_then(Value::as_str);
    if let Some(legacy) = legacy {
        check_legacy_matches(&list, legacy)?;
    }
    state.account_key = Some(secret.to_string());
    state.own_list = Some(list.clone());
    state.pending_link = None;
    state.pending_link_approver = None;
    if let Some(legacy) = legacy {
        state.legacy_account_key = Some(legacy.to_owned());
    }
    Ok(json!({ "kind": "link", "deviceList": list, "hasLegacyKey": state.legacy_account_key.is_some() }))
}

/// A trusted device removes another device from the next list version.
pub(crate) fn revoke_device(state: &mut State, request: &Value) -> Result<Value> {
    let secret = account_secret(state)?;
    let device_id = str_field(request, "deviceId")?;
    if device_id == state.device_id {
        return Err(bad_request("a device can't revoke itself; rotate the account key from another device"));
    }
    let current = state.own_list.clone().ok_or_else(|| Error::new("not_linked", "no device list"))?;
    let revoked: Vec<DeviceIdentity> = current.devices.iter().filter(|d| d.device_id == device_id).cloned().collect();
    if revoked.is_empty() {
        return Err(Error::new("unknown_device", "no such device in the list"));
    }
    let devices = current.devices.iter().filter(|d| d.device_id != device_id).cloned().collect();
    let list = next_list(state, &secret, devices)?;
    state.own_list = Some(list.clone());
    for device in &revoked {
        state.sessions.remove(&device.curve25519);
        crate::signal_state::forget_device(state, &device.curve25519);
    }
    Ok(json!({ "deviceList": list, "revoked": revoked }))
}

/// A trusted device declares, in the signed list, that one of the account's devices speaks
/// rv-signal-v1 with exactly this certificate. Contacts then refuse Olm chat with that device
/// (when they speak Signal themselves) and accept no other certificate for it.
/// Needs no libsignal: the certificate is checked with the device's listed Ed25519 key.
pub(crate) fn set_signal_capability(state: &mut State, request: &Value) -> Result<Value> {
    let secret = account_secret(state)?;
    let current = state.own_list.clone().ok_or_else(|| Error::new("not_linked", "no device list"))?;
    let cert: crate::signal_state::DeviceCertificate = serde_json::from_value(request.get("certificate").cloned().unwrap_or(Value::Null))
        .map_err(|_| Error::new("bad_certificate", "not a device certificate"))?;
    let device = current
        .devices
        .iter()
        .find(|d| d.device_id == cert.device_id)
        .cloned()
        .ok_or_else(|| Error::new("unknown_device", "the certificate's device isn't in this account's list"))?;
    crate::signal_state::verify_certificate_signature(&device, &cert)?;
    let capability = crate::signal_state::DeviceCapability {
        device_id: device.device_id.clone(),
        protocol: crate::signal_state::SIGNAL_PROTOCOL.to_owned(),
        certificate_hash: crate::signal_state::certificate_hash(&cert),
    };
    if current.capabilities.contains(&capability) {
        return Ok(json!({ "deviceList": current, "changed": false }));
    }
    let mut capabilities: Vec<_> = current.capabilities.iter().filter(|c| c.device_id != capability.device_id || c.protocol != capability.protocol).cloned().collect();
    capabilities.push(capability);
    let mut list = sign_list(&secret, &state.user_id, current.version + 1, current.devices.clone(), capabilities)?;
    list.legacy_certificate = current.legacy_certificate.clone();
    state.own_list = Some(list.clone());
    Ok(json!({ "deviceList": list, "changed": true }))
}

/// A trusted device turns Signal off for one of the account's devices, in the signed list.
/// Contacts that verify this list stop requiring Signal for that device, and are told so; chat
/// with it goes back to Olm. This is a deliberate, signed step, never something a server can do.
pub(crate) fn clear_signal_capability(state: &mut State, request: &Value) -> Result<Value> {
    let secret = account_secret(state)?;
    let device_id = str_field(request, "deviceId")?;
    let current = state.own_list.clone().ok_or_else(|| Error::new("not_linked", "no device list"))?;
    if !current.capabilities.iter().any(|c| c.device_id == device_id) {
        return Ok(json!({ "deviceList": current, "changed": false }));
    }
    let capabilities = current.capabilities.iter().filter(|c| c.device_id != device_id).cloned().collect();
    let mut list = sign_list(&secret, &state.user_id, current.version + 1, current.devices.clone(), capabilities)?;
    list.legacy_certificate = current.legacy_certificate.clone();
    crate::signal_state::apply_capability_change(state, Some(&current), &list);
    state.own_list = Some(list.clone());
    Ok(json!({ "deviceList": list, "changed": true }))
}

/// A new account key (after a trusted device was lost): contacts will see an identity change.
pub(crate) fn rotate_account_key(state: &mut State) -> Result<Value> {
    let current = state.own_list.clone().ok_or_else(|| Error::new("not_linked", "no device list"))?;
    let old_secret = account_secret(state)?;
    let secret = Ed25519SecretKey::new();
    let mut list = sign_list(&secret.to_base64(), &state.user_id, current.version + 1, current.devices, current.capabilities)?;
    if let Some(legacy) = state.legacy_account_key.clone() {
        list.legacy_certificate = Some(certify(&legacy, &state.user_id, &list.account_key)?);
    }
    // The old key vouches for the new one, so the server can tell a rotation by a trusted device
    // from a reset (which it delays and announces). Contacts are warned either way.
    let rotation_proof = sign_statement(&old_secret, ROTATION_CONTEXT, &state.user_id, &list.account_key)?;
    state.account_key = Some(secret.to_base64());
    state.own_list = Some(list.clone());
    Ok(json!({ "deviceList": list, "accountKey": list.account_key, "rotationProof": rotation_proof }))
}

/// Another account's list (from the server). Pins its account key on first sight; afterwards
/// refuses a different key, an older version or a different list for the same version.
pub(crate) fn verify_device_list(state: &mut State, request: &Value) -> Result<Value> {
    let list = list_field(request, "deviceList")?;
    check_list(&list)?;
    let certified_by = check_certificate(&list)?;
    // The app already pinned this account's legacy signing key (migration 030): it must vouch for the account key.
    if let Some(pinned) = request.get("legacySigningKey").and_then(Value::as_str) {
        if certified_by.as_deref() != Some(pinned) {
            return Err(Error::new("uncertified_account_key", "this account key isn't vouched for by the account's known signing key"));
        }
    }
    if list.user_id == state.user_id {
        let own = state.own_list.clone().ok_or_else(|| Error::new("not_linked", "no device list"))?;
        if own.account_key != list.account_key {
            return Err(Error::new("account_key_changed", "this account's key isn't the one this device holds"));
        }
        if list.version < own.version {
            return Err(Error::new("rollback", "an outdated device list"));
        }
        // The same version, signed by this account's own key, but not the list this device signed: another
        // trusted device's change won the server's compare-and-set, and this device's never went out. Adopt it.
        let replaced_unpublished = list.version == own.version && list != own;
        // Another trusted device changed the list (e.g. after a compare-and-set conflict on the server).
        let (added, removed) = diff(&own.devices, &list.devices);
        for device in &removed {
            state.sessions.remove(&device.curve25519);
            crate::signal_state::forget_device(state, &device.curve25519);
        }
        let this_device_listed = list.devices.iter().any(|d| d.device_id == state.device_id);
        state.sas_verified.retain(|_, verified| !list.devices.contains(verified));
        let (signal_added, signal_removed, signal_replaced) = crate::signal_state::apply_capability_change(state, Some(&own), &list);
        state.own_list = Some(list.clone());
        return Ok(json!({
            "userId": list.user_id, "version": list.version, "devices": list.devices, "added": added, "removed": removed,
            "firstSeen": false, "verified": true, "legacyCertified": certified_by.is_some(), "thisDeviceListed": this_device_listed,
            "replacedUnpublished": replaced_unpublished,
            "signalAdded": signal_added, "signalRemoved": signal_removed, "signalReplaced": signal_replaced,
        }));
    }
    let (first_seen, added, removed, verified) = match state.contacts.get(&list.user_id) {
        None => (true, list.devices.clone(), Vec::new(), false),
        Some(trust) if trust.account_key != list.account_key => {
            return Err(Error {
                code: "account_key_changed",
                message: format!("{}'s account key changed: confirm it (compare safety numbers) before trusting it", list.user_id),
            })
        }
        Some(trust) => match &trust.list {
            Some(old) if list.version < old.version => return Err(Error::new("rollback", "an older device list than one already seen")),
            Some(old) if list.version == old.version && list != *old => {
                return Err(Error::new("equivocation", "two different device lists with the same version"))
            }
            Some(old) => {
                let (added, removed) = diff(&old.devices, &list.devices);
                (false, added, removed, trust.verified)
            }
            None => (false, list.devices.clone(), Vec::new(), trust.verified),
        },
    };
    let previous = state.contacts.get(&list.user_id).and_then(|t| t.list.clone());
    let (signal_added, signal_removed, signal_replaced) = crate::signal_state::apply_capability_change(state, previous.as_ref(), &list);
    let trust = state.contacts.entry(list.user_id.clone()).or_insert(ContactTrust { account_key: list.account_key.clone(), verified: false, list: None });
    trust.list = Some(list.clone());
    for device in &removed {
        state.sessions.remove(&device.curve25519);
        crate::signal_state::forget_device(state, &device.curve25519);
    }
    Ok(json!({
        "userId": list.user_id,
        "version": list.version,
        "devices": list.devices,
        "added": added,
        "removed": removed,
        "firstSeen": first_seen,
        "verified": verified,
        "legacyCertified": certified_by.is_some(),
        // The Signal capabilities this list added, removed or moved to a new certificate (a new
        // Signal identity). A removal is signed, and chat with that device goes back to Olm: the
        // person should be told.
        "signalAdded": signal_added,
        "signalRemoved": signal_removed,
        "signalReplaced": signal_replaced,
    }))
}

/// The person accepted another account's (new) key, e.g. after comparing safety numbers.
pub(crate) fn trust_account_key(state: &mut State, request: &Value) -> Result<Value> {
    let user_id = str_field(request, "userId")?.to_owned();
    let account_key = str_field(request, "accountKey")?.to_owned();
    Ed25519PublicKey::from_base64(&account_key).map_err(|_| bad_request("accountKey isn't a key"))?;
    let verified = request.get("verified").and_then(Value::as_bool).unwrap_or(false);
    let previous = state.contacts.insert(user_id.clone(), ContactTrust { account_key: account_key.clone(), verified, list: None });
    if let Some(previous) = previous.filter(|p| p.account_key != account_key) {
        if let Some(list) = previous.list {
            for device in list.devices {
                state.sessions.remove(&device.curve25519);
                crate::signal_state::forget_device(state, &device.curve25519);
            }
        }
    }
    Ok(json!({ "userId": user_id, "accountKey": account_key, "verified": verified }))
}

// --- Fingerprints (safety numbers) ---

fn fingerprint_bytes(user_id: &str, account_key: &str) -> Result<[u8; 32]> {
    let key = Ed25519PublicKey::from_base64(account_key).map_err(|_| bad_request("accountKey isn't a key"))?;
    let mut key_bytes = vec![FINGERPRINT_KEY_TYPE];
    key_bytes.extend_from_slice(key.as_bytes());
    let mut hash = Sha512::new();
    hash.update(FINGERPRINT_VERSION.to_be_bytes());
    hash.update(&key_bytes);
    hash.update(user_id.as_bytes());
    let mut digest = hash.finalize();
    for _ in 0..FINGERPRINT_ITERATIONS {
        let mut next = Sha512::new();
        next.update(&digest);
        next.update(&key_bytes);
        digest = next.finalize();
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&digest[..32]);
    Ok(out)
}

fn displayable(fingerprint: &[u8; 32]) -> String {
    (0..6)
        .map(|chunk| {
            let bytes = &fingerprint[chunk * 5..chunk * 5 + 5];
            let value = bytes.iter().fold(0u64, |acc, b| (acc << 8) | u64::from(*b));
            format!("{:05}", value % 100_000)
        })
        .collect()
}

fn remote_key(state: &State, request: &Value, user_id: &str) -> Result<String> {
    match request.get("accountKey").and_then(Value::as_str) {
        Some(key) => Ok(key.to_owned()),
        None => state.contacts.get(user_id).map(|c| c.account_key.clone()).ok_or_else(|| bad_request("no account key known for this user")),
    }
}

fn own_account_key(state: &State) -> Result<String> {
    state.own_list.as_ref().map(|l| l.account_key.clone()).ok_or_else(|| Error::new("not_linked", "this device isn't linked to its account yet"))
}

/// The safety number for this account and another, and the QR code to scan.
pub(crate) fn fingerprint(state: &State, request: &Value) -> Result<Value> {
    let user_id = str_field(request, "userId")?;
    let local = fingerprint_bytes(&state.user_id, &own_account_key(state)?)?;
    let remote_account_key = remote_key(state, request, user_id)?;
    let remote = fingerprint_bytes(user_id, &remote_account_key)?;
    let (a, b) = (displayable(&local), displayable(&remote));
    let digits = if a <= b { format!("{a}{b}") } else { format!("{b}{a}") };
    let groups: Vec<String> = (0..12).map(|i| digits[i * 5..i * 5 + 5].to_owned()).collect();
    let mut qr = FINGERPRINT_QR_MAGIC.to_vec();
    qr.push(1);
    qr.extend_from_slice(&local);
    qr.extend_from_slice(&remote);
    // Verified only for the pinned key; a number over a different (new) key is a key change to compare.
    let pinned = state.contacts.get(user_id);
    let verified = pinned.is_some_and(|c| c.verified && c.account_key == remote_account_key);
    let key_changed = pinned.is_some_and(|c| c.account_key != remote_account_key);
    let qr = B64.encode(qr);
    let qr_text = format!("{FINGERPRINT_QR_TEXT_PREFIX}{qr}");
    Ok(json!({ "safetyNumber": groups, "qr": qr, "qrText": qr_text, "verified": verified, "keyChanged": key_changed, "accountKey": remote_account_key }))
}

/// The QR bytes from `qr` (base64) or `qrText` (what a camera read off the other person's screen).
fn scanned_qr(request: &Value) -> Result<Vec<u8>> {
    let encoded = match (request.get("qrText").and_then(Value::as_str), request.get("qr").and_then(Value::as_str)) {
        (Some(text), _) => text.trim().strip_prefix(FINGERPRINT_QR_TEXT_PREFIX).ok_or_else(|| bad_request("not a safety number QR code"))?,
        (None, Some(qr)) => qr,
        (None, None) => return Err(bad_request("qr or qrText is required")),
    };
    B64.decode(encoded).map_err(|_| bad_request("not a safety number QR code"))
}

/// Compares the other person's scanned QR code with this device's view; on a match the key is marked verified.
pub(crate) fn verify_fingerprint_qr(state: &mut State, request: &Value) -> Result<Value> {
    let user_id = str_field(request, "userId")?.to_owned();
    let key = remote_key(state, request, &user_id)?;
    let scanned = scanned_qr(request)?;
    if scanned.len() != FINGERPRINT_QR_MAGIC.len() + 1 + 64 || &scanned[..4] != FINGERPRINT_QR_MAGIC || scanned[4] != 1 {
        return Err(bad_request("not a safety number QR code"));
    }
    let local = fingerprint_bytes(&state.user_id, &own_account_key(state)?)?;
    let remote = fingerprint_bytes(&user_id, &key)?;
    // Their QR: their key first, then what they think ours is.
    let matches = bool::from(scanned[5..37].ct_eq(&remote)) && bool::from(scanned[37..69].ct_eq(&local));
    if matches {
        let accept_new = request.get("acceptNewKey").and_then(Value::as_bool).unwrap_or(false);
        let trust = state.contacts.entry(user_id.clone()).or_insert(ContactTrust { account_key: key.clone(), verified: false, list: None });
        if trust.account_key != key {
            if !accept_new {
                return Err(Error::new("account_key_changed", "the scanned key isn't the pinned one: accept the new key first"));
            }
            // Their new key, confirmed by the scan: pin it (dropping the old key's sessions) as verified.
            trust_account_key(state, &json!({ "userId": user_id, "accountKey": key, "verified": true }))?;
            return Ok(json!({ "verified": true, "acceptedNewKey": true }));
        }
        trust.verified = true;
    }
    Ok(json!({ "verified": matches }))
}

/// The person compared the digits and says they match (or takes that back). Only for a contact whose
/// account key is already pinned; the cached device list is kept. A new account key starts unverified.
pub(crate) fn set_contact_verified(state: &mut State, request: &Value) -> Result<Value> {
    let user_id = str_field(request, "userId")?.to_owned();
    let verified = request.get("verified").and_then(Value::as_bool).ok_or_else(|| bad_request("verified must be true or false"))?;
    let trust = state.contacts.get_mut(&user_id).ok_or_else(|| Error::new("unknown_contact", "no account key is pinned for this user yet"))?;
    if let Some(expected) = request.get("accountKey").and_then(Value::as_str) {
        if expected != trust.account_key {
            return Err(Error::new("account_key_changed", "the pinned key isn't the one shown: compare the new number first"));
        }
    }
    trust.verified = verified;
    Ok(json!({ "userId": user_id, "verified": verified }))
}

// --- Recovery backup ---

fn recovery_cipher(request: &Value) -> Result<XChaCha20Poly1305> {
    let key = decode_pickle_key(request.get("recoveryKey").unwrap_or(&Value::Null)).map_err(|_| bad_request("recoveryKey must be 32 bytes, base64"))?;
    XChaCha20Poly1305::new_from_slice(&key[..]).map_err(|_| bad_request("recoveryKey must be 32 bytes"))
}

/// The account key encrypted under the recovery key, for the server to keep (it can't open it).
pub(crate) fn export_account_backup(state: &State, request: &Value) -> Result<Value> {
    let secret = account_secret(state)?;
    let cipher = recovery_cipher(request)?;
    let mut nonce = [0u8; 24];
    getrandom::fill(&mut nonce).map_err(|e| Error::new("internal", e.to_string()))?;
    let aad = format!("{BACKUP_AAD}\0{}", state.user_id);
    let contents = Zeroizing::new(json!({ "accountKey": secret.as_str(), "legacyAccountKey": state.legacy_account_key }).to_string());
    let sealed = cipher
        .encrypt(&XNonce::from(nonce), Payload { msg: contents.as_bytes(), aad: aad.as_bytes() })
        .map_err(|_| Error::new("internal", "backup encryption failed"))?;
    // Format 2: JSON with the account key and the legacy key (format 1 held the account key alone).
    let mut out = vec![2u8];
    out.extend_from_slice(&nonce);
    out.extend_from_slice(&sealed);
    let backup = B64.encode(out);
    // Lets the server refuse to replace the stored backup with one the account key didn't make.
    let signature = sign_statement(&secret, BACKUP_SIGNATURE_CONTEXT, &state.user_id, &backup)?;
    Ok(json!({ "backup": backup, "signature": signature }))
}

/// A device with no other device left: opens the backup with the recovery key and adds itself
/// to the next version of the account's current list (which the restored key must have signed).
pub(crate) fn restore_account_backup(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let cipher = recovery_cipher(request)?;
    let raw = B64.decode(str_field(request, "backup")?).map_err(|_| bad_request("backup isn't base64"))?;
    if raw.len() < 1 + 24 + 16 || !(raw[0] == 1 || raw[0] == 2) {
        return Err(Error::new("bad_backup", "unknown backup format"));
    }
    let aad = format!("{BACKUP_AAD}\0{}", state.user_id);
    let secret = Zeroizing::new(
        String::from_utf8(
            cipher
                .decrypt(&XNonce::try_from(&raw[1..25]).map_err(|_| Error::new("bad_backup", "bad nonce"))?, Payload { msg: &raw[25..], aad: aad.as_bytes() })
                .map_err(|_| Error::new("bad_backup", "wrong recovery key, or the backup is for another account"))?,
        )
        .map_err(|_| Error::new("bad_backup", "damaged backup"))?,
    );
    let (secret, legacy) = if raw[0] == 1 {
        (secret, None)
    } else {
        let contents: Value = serde_json::from_str(&secret).map_err(|_| Error::new("bad_backup", "damaged backup"))?;
        let account_key = contents.get("accountKey").and_then(Value::as_str).ok_or_else(|| Error::new("bad_backup", "no account key"))?;
        (Zeroizing::new(account_key.to_owned()), contents.get("legacyAccountKey").and_then(Value::as_str).map(str::to_owned))
    };
    let current = list_field(request, "deviceList")?;
    check_list(&current)?;
    let public = Ed25519SecretKey::from_base64(&secret).map_err(|_| Error::new("bad_backup", "unreadable account key"))?.public_key().to_base64();
    if current.account_key != public || current.user_id != state.user_id {
        return Err(Error::new("account_key_changed", "the current device list isn't signed by the backed-up key"));
    }
    if let Some(legacy) = &legacy {
        check_legacy_matches(&current, legacy)?;
    }
    let own = own_identity(state, account);
    let mut devices = current.devices.clone();
    if !devices.contains(&own) {
        devices.push(own);
    }
    let mut list = sign_list(&secret, &state.user_id, current.version + 1, devices, current.capabilities.clone())?;
    list.legacy_certificate = current.legacy_certificate.clone();
    state.account_key = Some(secret.to_string());
    state.own_list = Some(list.clone());
    state.pending_link = None;
    if legacy.is_some() {
        state.legacy_account_key = legacy;
    }
    Ok(json!({ "deviceList": list, "hasLegacyKey": state.legacy_account_key.is_some() }))
}
