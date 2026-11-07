//! Encrypted Stories (docs/vodozemac-poc.md, "Stories").
//!
//! Each story gets a **fresh random key**; its content is encrypted once with
//! XChaCha20-Poly1305, bound (as associated data) to the story id, the
//! publishing device and the expiry. The key reaches each audience device only
//! inside an Olm message from the publishing device, so a reader knows which
//! device published it, and only devices in the publisher's chosen audience
//! (from signed device lists) get it. This replaces the legacy design, where a
//! private story would have been readable by anyone holding a shared
//! conversation key.
//!
//! What it can't do: a key already delivered can't be taken back. Removing
//! someone from the audience stops them getting the keys of later stories (and
//! the server stops serving them the content), and expiry deletes keys on every
//! honest device — but a device that kept a key and the ciphertext can still
//! read that story. The engine has no clock: callers pass `now` (ms since the
//! epoch) and the server must also delete expired content.

use std::collections::BTreeMap;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{XChaCha20Poly1305, XNonce};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use vodozemac::olm::Account;
use zeroize::Zeroizing;

use crate::engine::{bad_request, identity_field, olm_encrypt, own_identity, payload, str_field, DeviceIdentity, Error, Result, State};
use crate::identity::check_listed;

const STORY_AAD: &str = "redvault-story-v1";

#[derive(Serialize, Deserialize, Clone)]
pub struct OwnStory {
    key: String,
    expires_at: u64,
    content_hash: String,
    audience: Vec<DeviceIdentity>,
    shared_with: Vec<String>,
}

#[derive(Serialize, Deserialize, Clone)]
pub struct ReceivedStory {
    key: String,
    publisher: DeviceIdentity,
    expires_at: u64,
    content_hash: String,
}

fn story_aad(story_id: &str, publisher: &DeviceIdentity, expires_at: u64) -> Vec<u8> {
    let mut out = format!("{STORY_AAD}\0{story_id}\0{}\0{}\0{}\0", publisher.user_id, publisher.device_id, publisher.ed25519).into_bytes();
    out.extend_from_slice(&expires_at.to_be_bytes());
    out
}

fn audience_field(state: &State, request: &Value) -> Result<Vec<DeviceIdentity>> {
    let audience: Vec<DeviceIdentity> =
        serde_json::from_value(request.get("audience").cloned().unwrap_or(Value::Null)).map_err(|_| bad_request("audience must be device identities"))?;
    for device in &audience {
        check_listed(state, device)?;
    }
    Ok(audience)
}

fn u64_field(request: &Value, name: &str) -> Result<u64> {
    request.get(name).and_then(Value::as_u64).ok_or_else(|| bad_request(format!("{name} is required (ms since the epoch)")))
}

fn content_hash(blob: &[u8]) -> String {
    Sha256::digest(blob).iter().map(|b| format!("{b:02x}")).collect()
}

/// Encrypts a new story's content under a fresh key, kept in this device's encrypted archive.
pub(crate) fn story_create(state: &mut State, account: &Account, request: &Value) -> Result<Value> {
    let story_id = str_field(request, "storyId")?.to_owned();
    if state.stories_out.contains_key(&story_id) {
        return Err(bad_request("a story with this id already exists"));
    }
    let content = str_field(request, "content")?;
    let expires_at = u64_field(request, "expiresAt")?;
    let audience = audience_field(state, request)?;
    let mut key = Zeroizing::new([0u8; 32]);
    let mut nonce = [0u8; 24];
    getrandom::fill(key.as_mut()).map_err(|e| Error::new("internal", e.to_string()))?;
    getrandom::fill(&mut nonce).map_err(|e| Error::new("internal", e.to_string()))?;
    let publisher = own_identity(state, account);
    let sealed = XChaCha20Poly1305::new_from_slice(&key[..])
        .map_err(|_| Error::new("internal", "bad story key"))?
        .encrypt(&XNonce::from(nonce), Payload { msg: content.as_bytes(), aad: &story_aad(&story_id, &publisher, expires_at) })
        .map_err(|_| Error::new("internal", "story encryption failed"))?;
    let mut blob = vec![1u8];
    blob.extend_from_slice(&nonce);
    blob.extend_from_slice(&sealed);
    let hash = content_hash(&blob);
    state.stories_out.insert(
        story_id.clone(),
        OwnStory { key: B64.encode(key.as_ref()), expires_at, content_hash: hash.clone(), audience, shared_with: vec![publisher.curve25519.clone()] },
    );
    Ok(json!({ "storyId": story_id, "ciphertext": B64.encode(blob), "contentHash": hash, "expiresAt": expires_at, "publisher": publisher }))
}

/// The story's key, Olm-encrypted for one device in its audience.
pub(crate) fn story_share_key(state: &mut State, account: &mut Account, request: &Value) -> Result<Value> {
    let story_id = str_field(request, "storyId")?.to_owned();
    let recipient = identity_field(request, "recipient")?;
    let story = state.stories_out.get(&story_id).cloned().ok_or_else(|| Error::new("unknown_story", "no such story here"))?;
    if !story.audience.contains(&recipient) {
        return Err(Error::new("not_in_audience", "this device isn't in the story's audience"));
    }
    let inner = payload(
        state,
        account,
        &recipient,
        "story_key",
        json!({ "storyId": story_id, "key": story.key, "expiresAt": story.expires_at, "contentHash": story.content_hash }),
    );
    let message = olm_encrypt(state, account, &recipient, request.get("oneTimeKey"), &inner)?;
    if let Some(own) = state.stories_out.get_mut(&story_id) {
        if !own.shared_with.contains(&recipient.curve25519) {
            own.shared_with.push(recipient.curve25519.clone());
        }
    }
    Ok(json!({ "message": message }))
}

/// A story key that arrived inside an Olm message: stored as published by that device.
pub(crate) fn receive_story_key(state: &mut State, sender: &DeviceIdentity, inner: &Value) -> Result<Value> {
    let story_id = inner.get("storyId").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no storyId"))?;
    let key = inner.get("key").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no key"))?;
    let expires_at = inner.get("expiresAt").and_then(Value::as_u64).ok_or_else(|| Error::new("malformed_payload", "no expiry"))?;
    let hash = inner.get("contentHash").and_then(Value::as_str).ok_or_else(|| Error::new("malformed_payload", "no content hash"))?;
    if B64.decode(key).map(|k| k.len()) != Ok(32) {
        return Err(Error::new("malformed_payload", "bad story key"));
    }
    if let Some(existing) = state.stories_in.get(story_id) {
        if existing.publisher != *sender {
            return Err(Error::new("story_conflict", "another device already published a story with this id"));
        }
    }
    state.stories_in.insert(
        story_id.to_owned(),
        ReceivedStory { key: key.to_owned(), publisher: sender.clone(), expires_at, content_hash: hash.to_owned() },
    );
    Ok(json!({ "kind": "storyKey", "storyId": story_id, "publisher": sender, "expiresAt": expires_at }))
}

/// Changes who future key shares may go to. Keys already delivered stay delivered.
pub(crate) fn story_set_audience(state: &mut State, request: &Value) -> Result<Value> {
    let story_id = str_field(request, "storyId")?;
    let audience = audience_field(state, request)?;
    let story = state.stories_out.get_mut(story_id).ok_or_else(|| Error::new("unknown_story", "no such story here"))?;
    let removed: Vec<DeviceIdentity> = story.audience.iter().filter(|d| !audience.contains(d)).cloned().collect();
    let already_had_key: Vec<&DeviceIdentity> = removed.iter().filter(|d| story.shared_with.contains(&d.curve25519)).collect();
    let already_had_key = serde_json::to_value(already_had_key).unwrap_or(Value::Null);
    story.audience = audience;
    Ok(json!({ "removed": removed, "alreadyHadKey": already_had_key }))
}

/// Opens a story's content: a received one until it expires (then its key is deleted and the
/// answer is `{expired: true}`), an own one (the archive) any time.
pub(crate) fn story_open(state: &mut State, request: &Value) -> Result<Value> {
    let story_id = str_field(request, "storyId")?.to_owned();
    let now = u64_field(request, "now")?;
    let blob = B64.decode(str_field(request, "ciphertext")?).map_err(|_| bad_request("ciphertext isn't base64"))?;
    let own_identity_now = state.own_list.as_ref().and_then(|l| l.devices.iter().find(|d| d.device_id == state.device_id).cloned());
    let (key, publisher, expires_at, hash, own) = if let Some(story) = state.stories_out.get(&story_id) {
        let publisher = own_identity_now.ok_or_else(|| Error::new("not_linked", "this device's identity isn't in its device list"))?;
        (story.key.clone(), publisher, story.expires_at, story.content_hash.clone(), true)
    } else if let Some(story) = state.stories_in.get(&story_id) {
        (story.key.clone(), story.publisher.clone(), story.expires_at, story.content_hash.clone(), false)
    } else {
        return Err(Error::new("unknown_story", "no key for this story"));
    };
    if !own && now >= expires_at {
        // A success, not an error: only successful calls save state, and the deletion must stick.
        state.stories_in.remove(&story_id);
        return Ok(json!({ "storyId": story_id, "expired": true }));
    }
    if content_hash(&blob) != hash {
        return Err(Error::new("content_mismatch", "this isn't the content the publisher sent"));
    }
    if blob.len() < 1 + 24 + 16 || blob[0] != 1 {
        return Err(Error::new("malformed_message", "unknown story format"));
    }
    let key = Zeroizing::new(B64.decode(key).map_err(|_| Error::new("bad_state", "unreadable story key"))?);
    let plain = Zeroizing::new(
        XChaCha20Poly1305::new_from_slice(&key)
            .map_err(|_| Error::new("bad_state", "unreadable story key"))?
            .decrypt(
                &XNonce::try_from(&blob[1..25]).map_err(|_| Error::new("malformed_message", "bad nonce"))?,
                Payload { msg: &blob[25..], aad: &story_aad(&story_id, &publisher, expires_at) },
            )
            .map_err(|_| Error::new("decrypt_failed", "the story doesn't open with its key"))?,
    );
    let content = String::from_utf8(plain.to_vec()).map_err(|_| Error::new("malformed_payload", "not text"))?;
    Ok(json!({ "storyId": story_id, "content": content, "publisher": { "userId": publisher.user_id, "deviceId": publisher.device_id }, "expiresAt": expires_at }))
}

/// Deletes the keys of received stories that expired by `now`.
pub(crate) fn story_expire(state: &mut State, request: &Value) -> Result<Value> {
    let now = u64_field(request, "now")?;
    let expired: Vec<String> = state.stories_in.iter().filter(|(_, s)| now >= s.expires_at).map(|(id, _)| id.clone()).collect();
    for id in &expired {
        state.stories_in.remove(id);
    }
    Ok(json!({ "deleted": expired }))
}

/// Deletes one story's key here (an own story leaves the archive too).
pub(crate) fn story_delete(state: &mut State, request: &Value) -> Result<Value> {
    let story_id = str_field(request, "storyId")?;
    let removed = state.stories_out.remove(story_id).is_some() | state.stories_in.remove(story_id).is_some();
    Ok(json!({ "deleted": removed }))
}

/// This device's own stories (the encrypted archive): ids and expiry only.
pub(crate) fn story_archive(state: &State) -> Result<Value> {
    let stories: BTreeMap<&String, u64> = state.stories_out.iter().map(|(id, s)| (id, s.expires_at)).collect();
    Ok(json!({ "stories": stories.into_iter().map(|(id, at)| json!({ "storyId": id, "expiresAt": at })).collect::<Vec<_>>() }))
}
