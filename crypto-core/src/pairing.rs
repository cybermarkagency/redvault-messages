//! Pairing a new device with a trusted one when neither can scan a QR code:
//! vodozemac's SAS (the short authentication string Matrix uses to verify
//! devices across an untrusted server).
//!
//! 1. The new device starts and sends a *commitment* to its ephemeral key
//!    (`sasStart`, role "new"). The trusted device starts and sends its key.
//! 2. The new device reveals its key; the trusted device checks it against the
//!    commitment, so neither side (nor the server between them) can pick a key
//!    after seeing the other's to make the codes match (`sasEstablish`).
//! 3. Both screens show the same three numbers if, and only if, both devices
//!    see the same keys and identities. The **person** compares them.
//! 4. Only after the person confirms, each device sends a MAC over its own
//!    device identity (`sasMac`); the other checks it (`sasConfirm`). The
//!    trusted device may then approve that exact device; the new device will
//!    then only accept a link from that exact trusted device.
//!
//! The ephemeral secrets live in memory only (a vodozemac `Sas` can't be
//! saved): if the app restarts mid-pairing, pairing starts again.

use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use vodozemac::sas::{EstablishedSas, Mac, Sas};
use vodozemac::Curve25519PublicKey;

use crate::engine::{bad_request, identity_field, own_identity, str_field, DeviceIdentity, Error, Result, State};

const COMMIT_CONTEXT: &str = "redvault-sas-commit-v1";
const SAS_INFO: &str = "REDVAULT_SAS_V1";
const MAC_INFO: &str = "REDVAULT_SAS_MAC_V1";

enum Phase {
    Started(Sas),
    Established(EstablishedSas),
}

struct Session {
    pairing_id: String,
    role: Role,
    our_public: String,
    phase: Phase,
    new_device: Option<DeviceIdentity>,
    trusted_device: Option<DeviceIdentity>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Role {
    New,
    Trusted,
}

impl Role {
    fn name(self) -> &'static str {
        match self {
            Role::New => "new",
            Role::Trusted => "trusted",
        }
    }
}

fn sessions() -> &'static Mutex<HashMap<String, Session>> {
    static SESSIONS: OnceLock<Mutex<HashMap<String, Session>>> = OnceLock::new();
    SESSIONS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn commitment(pairing_id: &str, public_key: &str) -> String {
    let digest = Sha256::digest(format!("{COMMIT_CONTEXT}\0{pairing_id}\0{public_key}").as_bytes());
    B64.encode(digest)
}

fn identity_line(device: &DeviceIdentity) -> String {
    format!("{}|{}|{}|{}", device.user_id, device.device_id, device.curve25519, device.ed25519)
}

/// Starts a pairing on this device. The new device's answer carries a commitment, not its key yet.
pub(crate) fn sas_start(request: &Value) -> Result<Value> {
    let pairing_id = str_field(request, "pairingId")?.to_owned();
    let role = match str_field(request, "role")? {
        "new" => Role::New,
        "trusted" => Role::Trusted,
        _ => return Err(bad_request("role must be \"new\" or \"trusted\"")),
    };
    let sas = Sas::new();
    let public = sas.public_key().to_base64();
    let mut id = [0u8; 16];
    getrandom::fill(&mut id).map_err(|e| Error::new("internal", e.to_string()))?;
    let sas_id = id.iter().map(|b| format!("{b:02x}")).collect::<String>();
    let mut response = json!({ "sasId": sas_id });
    match role {
        Role::New => response["commitment"] = json!(commitment(&pairing_id, &public)),
        Role::Trusted => response["publicKey"] = json!(public),
    }
    let mut map = sessions().lock().map_err(|_| Error::new("internal", "pairing lock"))?;
    map.insert(
        sas_id,
        Session { pairing_id, role, our_public: public, phase: Phase::Started(sas), new_device: None, trusted_device: None },
    );
    Ok(response)
}

/// The new device's key, once it has seen the trusted device's (the commitment came first).
pub(crate) fn sas_reveal(request: &Value) -> Result<Value> {
    let sas_id = str_field(request, "sasId")?;
    let map = sessions().lock().map_err(|_| Error::new("internal", "pairing lock"))?;
    let session = map.get(sas_id).ok_or_else(|| Error::new("unknown_pairing", "no such pairing on this device (it may have restarted)"))?;
    if session.role != Role::New {
        return Err(bad_request("only the new device reveals its key after committing"));
    }
    Ok(json!({ "publicKey": session.our_public }))
}

/// Computes the shared secret and the numbers to compare, binding both identities and both keys.
pub(crate) fn sas_establish(request: &Value) -> Result<Value> {
    let sas_id = str_field(request, "sasId")?.to_owned();
    let their_public = str_field(request, "theirPublicKey")?.to_owned();
    let new_device = identity_field(request, "newDevice")?;
    let trusted_device = identity_field(request, "trustedDevice")?;
    if new_device.user_id != trusted_device.user_id {
        return Err(Error::new("wrong_account", "both devices must belong to the same account"));
    }
    let mut map = sessions().lock().map_err(|_| Error::new("internal", "pairing lock"))?;
    let session = map.remove(&sas_id).ok_or_else(|| Error::new("unknown_pairing", "no such pairing on this device (it may have restarted)"))?;
    let Phase::Started(sas) = session.phase else {
        return Err(Error::new("bad_pairing", "this pairing was already established"));
    };
    if session.role == Role::Trusted {
        // The new device committed to its key before it saw ours.
        let expected = str_field(request, "theirCommitment")?;
        if !bool::from(commitment(&session.pairing_id, &their_public).as_bytes().ct_eq(expected.as_bytes())) {
            return Err(Error::new("bad_commitment", "the new device's key doesn't match what it committed to: stop pairing"));
        }
    }
    let (new_public, trusted_public) =
        if session.role == Role::New { (session.our_public.clone(), their_public.clone()) } else { (their_public.clone(), session.our_public.clone()) };
    Curve25519PublicKey::from_base64(&their_public).map_err(|_| bad_request("theirPublicKey isn't a key"))?;
    let established = sas.diffie_hellman_with_raw(&their_public).map_err(|_| Error::new("bad_pairing", "unsafe key"))?;
    let info = format!(
        "{SAS_INFO}|{}|{}|{}|{new_public}|{trusted_public}",
        session.pairing_id,
        identity_line(&new_device),
        identity_line(&trusted_device)
    );
    let bytes = established.bytes(&info);
    let (a, b, c) = bytes.decimals();
    let emoji = bytes.emoji_indices();
    map.insert(
        sas_id,
        Session {
            pairing_id: session.pairing_id,
            role: session.role,
            our_public: session.our_public,
            phase: Phase::Established(established),
            new_device: Some(new_device),
            trusted_device: Some(trusted_device),
        },
    );
    Ok(json!({ "decimals": [a, b, c], "emoji": emoji }))
}

/// After the person confirmed the numbers match: a MAC over this device's own identity.
pub(crate) fn sas_mac(state: &State, account: &vodozemac::olm::Account, request: &Value) -> Result<Value> {
    let sas_id = str_field(request, "sasId")?;
    let map = sessions().lock().map_err(|_| Error::new("internal", "pairing lock"))?;
    let session = map.get(sas_id).ok_or_else(|| Error::new("unknown_pairing", "no such pairing on this device"))?;
    let Phase::Established(established) = &session.phase else {
        return Err(Error::new("bad_pairing", "compare the numbers first"));
    };
    let own = own_identity(state, account);
    let expected = if session.role == Role::New { session.new_device.as_ref() } else { session.trusted_device.as_ref() };
    if expected != Some(&own) {
        return Err(Error::new("bad_pairing", "this pairing was set up for another device"));
    }
    let mac = established.calculate_mac(&identity_line(&own), &format!("{MAC_INFO}|{}|{}", session.pairing_id, session.role.name()));
    Ok(json!({ "mac": mac.to_base64() }))
}

/// Checks the other device's MAC. On success the pairing is remembered in this device's state.
pub(crate) fn sas_confirm(state: &mut State, request: &Value) -> Result<Value> {
    let sas_id = str_field(request, "sasId")?.to_owned();
    let mac_text = str_field(request, "mac")?;
    let mut map = sessions().lock().map_err(|_| Error::new("internal", "pairing lock"))?;
    let session = map.remove(&sas_id).ok_or_else(|| Error::new("unknown_pairing", "no such pairing on this device"))?;
    let Phase::Established(established) = &session.phase else {
        return Err(Error::new("bad_pairing", "compare the numbers first"));
    };
    let (their, their_role) = match session.role {
        Role::New => (session.trusted_device.clone(), Role::Trusted),
        Role::Trusted => (session.new_device.clone(), Role::New),
    };
    let their = their.ok_or_else(|| Error::new("bad_pairing", "incomplete pairing"))?;
    let mac = Mac::from_base64(mac_text).map_err(|_| bad_request("mac isn't base64"))?;
    established
        .verify_mac(&identity_line(&their), &format!("{MAC_INFO}|{}|{}", session.pairing_id, their_role.name()), &mac)
        .map_err(|_| Error::new("bad_pairing", "the other device's identity doesn't match: don't approve it"))?;
    match session.role {
        Role::Trusted => {
            state.sas_verified.insert(their.curve25519.clone(), their.clone());
        }
        Role::New => {
            state.pending_link_approver = Some(their.clone());
        }
    }
    Ok(json!({ "verified": true, "device": their }))
}
