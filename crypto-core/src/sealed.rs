// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

//! Sealed requests for the backup route (docs/tor-sealed-fallback.md).
//!
//! When the onion service can't be reached, the apps reach the server through
//! Tor at the HTTPS site. Every API request is then sealed with Oblivious HTTP
//! (RFC 9458) to the gateway's public key, which the apps carry built in, so
//! Cloudflare (which ends TLS for the site) only sees padded opaque blobs sent
//! to one address. The request inside is Binary HTTP (RFC 9292), padded with
//! zero bytes to a size bucket, so lengths say little about what was sent.
//!
//! - App side (`sealed-transport`): [`seal`] a request, keep the
//!   [`PendingReply`], [`PendingReply::open`] the answer. One key pair per
//!   request, never reused.
//! - Gateway side (`sealed-gateway`, only in the server's gateway program):
//!   [`Gateway::open_request`] and [`GatewayReply::seal`].
//!
//! Suite: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20-Poly1305 (the
//! RFC's mandatory suite), from Mozilla's `ohttp`/`bhttp` crates
//! (MIT OR Apache-2.0) with the pure-Rust `hpke` backend.

use bhttp::{Message, Mode, StatusCode};
use serde::{Deserialize, Serialize};

use crate::engine::{Error, Result};

/// The inner request's scheme and authority. The gateway ignores both and only
/// ever forwards to the API's private listener; they're fixed so they say nothing.
const INNER_SCHEME: &[u8] = b"https";
const INNER_AUTHORITY: &[u8] = b"api";

/// Largest sealed message accepted either way (one file part plus headers, padded).
pub const MAX_MESSAGE: usize = 9 * 1024 * 1024;

/// Headers the app may not set on an inner request: the gateway and the
/// outer HTTPS request own these, and none may identify the device.
const FORBIDDEN_HEADERS: &[&str] = &[
    "host", "connection", "keep-alive", "proxy-connection", "transfer-encoding", "te", "upgrade",
    "trailer", "forwarded", "x-forwarded-for", "x-forwarded-host", "x-forwarded-proto", "x-real-ip",
    "via", "user-agent",
];

/// What the app asks for: method, `/api/...` path, and the few headers the API needs.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct RequestHead {
    pub method: String,
    pub path: String,
    #[serde(default)]
    pub headers: Vec<(String, String)>,
}

/// What came back: status and headers (the body travels separately).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct ResponseHead {
    pub status: u16,
    #[serde(default)]
    pub headers: Vec<(String, String)>,
}

fn sealed_error(message: impl Into<String>) -> Error {
    Error::new("sealed", message)
}

/// The size a message of `len` bytes is padded to: 1, 4, 16, 64 or 256 KB, then 256 KB steps.
pub fn padded_len(len: usize) -> usize {
    const BUCKETS: [usize; 5] = [1024, 4096, 16 * 1024, 64 * 1024, 256 * 1024];
    const STEP: usize = 256 * 1024;
    match BUCKETS.iter().find(|&&b| len <= b) {
        Some(&b) => b,
        None => len.div_ceil(STEP) * STEP,
    }
}

fn pad(mut bytes: Vec<u8>) -> Vec<u8> {
    let target = padded_len(bytes.len());
    bytes.resize(target, 0);
    bytes
}

/// Reads a known-length Binary HTTP message followed only by zero padding (RFC 9292 section 3.8).
fn read_padded(bytes: &[u8]) -> Result<Message> {
    let mut cursor = std::io::Cursor::new(bytes);
    let message = Message::read_bhttp(&mut cursor).map_err(|_| sealed_error("not a valid Binary HTTP message"))?;
    let used = usize::try_from(cursor.position()).map_err(|_| sealed_error("message too long"))?;
    if bytes[used..].iter().any(|&b| b != 0) {
        return Err(sealed_error("the padding isn't all zero"));
    }
    Ok(message)
}

fn write(message: &Message) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    message.write_bhttp(Mode::KnownLength, &mut out).map_err(|_| sealed_error("couldn't encode the message"))?;
    Ok(out)
}

fn valid_header(name: &str, value: &str) -> bool {
    !name.is_empty()
        && name.bytes().all(|b| b.is_ascii_graphic() && b != b':' && !b.is_ascii_uppercase())
        && !value.bytes().any(|b| b == b'\r' || b == b'\n' || b == 0)
        && !FORBIDDEN_HEADERS.contains(&name)
}

/// The rule both ends apply to a path: an absolute `/api/...` path with nothing that walks out of it.
pub fn valid_path(path: &str) -> bool {
    path.starts_with("/api/")
        && path.len() <= 2048
        && !path.contains("..")
        && !path.contains("//")
        && !path.contains('\\')
        && path.bytes().all(|b| b.is_ascii_graphic())
        && !path.contains('#')
}

fn valid_method(method: &str) -> bool {
    matches!(method, "GET" | "POST" | "PUT" | "PATCH" | "DELETE")
}

/// Encodes an app request as padded Binary HTTP (exposed for tests and the gateway's checks).
pub fn encode_request(head: &RequestHead, body: &[u8]) -> Result<Vec<u8>> {
    if !valid_method(&head.method) {
        return Err(sealed_error("unsupported method"));
    }
    if !valid_path(&head.path) {
        return Err(sealed_error("only /api/ paths can be sealed"));
    }
    let mut message = Message::request(
        head.method.as_bytes().to_vec(),
        INNER_SCHEME.to_vec(),
        INNER_AUTHORITY.to_vec(),
        head.path.as_bytes().to_vec(),
    );
    for (name, value) in &head.headers {
        let name = name.to_ascii_lowercase();
        if !valid_header(&name, value) {
            return Err(sealed_error(format!("header {name} can't be sealed")));
        }
        message.put_header(name.into_bytes(), value.as_bytes().to_vec());
    }
    message.write_content(body);
    let encoded = pad(write(&message)?);
    if encoded.len() > MAX_MESSAGE {
        return Err(sealed_error("the request is too large to seal; send it in parts"));
    }
    Ok(encoded)
}

/// Decodes padded Binary HTTP into a request, applying the same rules as [`encode_request`].
pub fn decode_request(bytes: &[u8]) -> Result<(RequestHead, Vec<u8>)> {
    let message = read_padded(bytes)?;
    let control = message.control();
    let method = control.method().and_then(|m| std::str::from_utf8(m).ok()).unwrap_or_default().to_owned();
    let path = control.path().and_then(|p| std::str::from_utf8(p).ok()).unwrap_or_default().to_owned();
    if !valid_method(&method) || !valid_path(&path) {
        return Err(sealed_error("not an allowed request"));
    }
    let mut headers = Vec::new();
    for field in message.header().iter() {
        let name = std::str::from_utf8(field.name()).map_err(|_| sealed_error("bad header"))?.to_ascii_lowercase();
        let value = std::str::from_utf8(field.value()).map_err(|_| sealed_error("bad header"))?.to_owned();
        if !valid_header(&name, &value) {
            return Err(sealed_error(format!("header {name} isn't allowed")));
        }
        headers.push((name, value));
    }
    Ok((RequestHead { method, path, headers }, message.content().to_vec()))
}

/// Encodes a response as padded Binary HTTP.
pub fn encode_response(head: &ResponseHead, body: &[u8]) -> Result<Vec<u8>> {
    let status = StatusCode::try_from(head.status).map_err(|_| sealed_error("bad status"))?;
    let mut message = Message::response(status);
    for (name, value) in &head.headers {
        let name = name.to_ascii_lowercase();
        // A reply's hop-by-hop headers stay at the gateway; Set-Cookie and the rest travel sealed.
        if name.is_empty() || matches!(name.as_str(), "connection" | "keep-alive" | "transfer-encoding" | "content-length") {
            continue;
        }
        message.put_header(name.into_bytes(), value.as_bytes().to_vec());
    }
    message.write_content(body);
    let encoded = pad(write(&message)?);
    if encoded.len() > MAX_MESSAGE {
        return Err(sealed_error("the reply is too large to seal"));
    }
    Ok(encoded)
}

/// Decodes a padded Binary HTTP response.
pub fn decode_response(bytes: &[u8]) -> Result<(ResponseHead, Vec<u8>)> {
    let message = read_padded(bytes)?;
    let status = message.control().status().map(u16::from).ok_or_else(|| sealed_error("not a response"))?;
    let mut headers = Vec::new();
    for field in message.header().iter() {
        let name = String::from_utf8_lossy(field.name()).to_ascii_lowercase();
        let value = String::from_utf8_lossy(field.value()).into_owned();
        headers.push((name, value));
    }
    Ok((ResponseHead { status, headers }, message.content().to_vec()))
}

/// The secret needed to open one request's reply. Single use; dropping it discards the secret.
pub struct PendingReply(ohttp::ClientResponse);

/// Seals one request to the gateway's public key configuration (RFC 9458 `KeyConfig`,
/// one entry). Returns the bytes to `POST` (as `message/ohttp-req`) and what opens the reply.
pub fn seal(key_config: &[u8], head: &RequestHead, body: &[u8]) -> Result<(Vec<u8>, PendingReply)> {
    let inner = zeroize::Zeroizing::new(encode_request(head, body)?);
    let client = ohttp::ClientRequest::from_encoded_config(key_config).map_err(|_| sealed_error("unusable gateway key"))?;
    let (sealed, pending) = client.encapsulate(&inner).map_err(|_| sealed_error("couldn't seal the request"))?;
    Ok((sealed, PendingReply(pending)))
}

impl PendingReply {
    /// Opens the gateway's sealed reply (`message/ohttp-res`).
    pub fn open(self, sealed_reply: &[u8]) -> Result<(ResponseHead, Vec<u8>)> {
        if sealed_reply.len() > MAX_MESSAGE + 64 {
            return Err(sealed_error("the reply is too large"));
        }
        let inner = zeroize::Zeroizing::new(
            self.0.decapsulate(sealed_reply).map_err(|_| sealed_error("the reply can't be opened"))?,
        );
        decode_response(&inner)
    }
}

/// The gateway's side: opens requests sealed to one of its keys, seals the replies.
#[cfg(feature = "sealed-gateway")]
pub struct Gateway {
    servers: Vec<ohttp::Server>,
}

/// The secret that seals one reply, kept by the gateway while the API answers.
#[cfg(feature = "sealed-gateway")]
pub struct GatewayReply(ohttp::ServerResponse);

#[cfg(feature = "sealed-gateway")]
impl Gateway {
    /// The suite every key uses.
    fn suite() -> Vec<ohttp::SymmetricSuite> {
        vec![ohttp::SymmetricSuite::new(ohttp::hpke::Kdf::HkdfSha256, ohttp::hpke::Aead::ChaCha20Poly1305)]
    }

    /// A key derived from 32+ bytes of secret input keying material (kept on the server only).
    /// Returns the gateway's view and the public `KeyConfig` to pin in the apps.
    pub fn key_from_secret(key_id: u8, ikm: &[u8]) -> Result<(ohttp::KeyConfig, Vec<u8>)> {
        if ikm.len() < 32 {
            return Err(sealed_error("the key secret must be at least 32 bytes"));
        }
        let config = ohttp::KeyConfig::derive(key_id, ohttp::hpke::Kem::X25519Sha256, Self::suite(), ikm)
            .map_err(|_| sealed_error("couldn't derive the key"))?;
        let public = config.encode().map_err(|_| sealed_error("couldn't encode the key"))?;
        Ok((config, public))
    }

    /// A gateway that accepts requests to any of these keys (current and next).
    pub fn new(configs: Vec<ohttp::KeyConfig>) -> Result<Self> {
        let servers = configs
            .into_iter()
            .map(|c| ohttp::Server::new(c).map_err(|_| sealed_error("unusable key")))
            .collect::<Result<Vec<_>>>()?;
        if servers.is_empty() {
            return Err(sealed_error("no keys"));
        }
        Ok(Gateway { servers })
    }

    /// Opens a sealed request and checks it: a valid `/api/` request with allowed headers.
    pub fn open_request(&self, sealed: &[u8]) -> Result<(RequestHead, Vec<u8>, GatewayReply)> {
        if sealed.len() > MAX_MESSAGE + 64 {
            return Err(sealed_error("too large"));
        }
        for server in &self.servers {
            if let Ok((inner, reply)) = server.decapsulate(sealed) {
                let inner = zeroize::Zeroizing::new(inner);
                let (head, body) = decode_request(&inner)?;
                return Ok((head, body, GatewayReply(reply)));
            }
        }
        Err(sealed_error("can't be opened with any current key"))
    }
}

#[cfg(feature = "sealed-gateway")]
impl GatewayReply {
    /// Seals the API's answer for the app that sent the request.
    pub fn seal(self, head: &ResponseHead, body: &[u8]) -> Result<Vec<u8>> {
        let inner = zeroize::Zeroizing::new(encode_response(head, body)?);
        self.0.encapsulate(&inner).map_err(|_| sealed_error("couldn't seal the reply"))
    }
}
