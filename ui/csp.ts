/**
 * Production-only Content-Security-Policy: the built app may only talk to,
 * and load anything from, its own origin — so no analytics, fonts, CDNs,
 * or other third parties can ever receive a visitor's IP or usage.
 */
export const CONTENT_SECURITY_POLICY = [
  "default-src 'self'",
  // 'wasm-unsafe-eval' lets the E2E crypto library (libsodium/argon2) compile
  // its own bundled WebAssembly. It does NOT allow JS eval() or any script
  // from another origin.
  "script-src 'self' 'wasm-unsafe-eval'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data: blob:",
  // Voice messages play from a blob: URL of audio decrypted in this tab.
  "media-src 'self' blob:",
  "font-src 'self'",
  "connect-src 'self'",
  // PDF previews in Files: a blob: of a file decrypted (and checked to be a PDF) in this tab.
  "frame-src blob:",
  "object-src 'none'",
  "base-uri 'self'",
  "form-action 'self'",
].join("; ");

/**
 * The public share page (share/index.html, docs/public-share-links.md) holds
 * a link's key in its address, so it gets less: nothing by default, only its
 * own scripts, no forms, no plugins, and blob: frames only for PDF previews of
 * files it decrypted itself. nginx adds frame-ancestors (a <meta> can't).
 */
export const SHARE_CONTENT_SECURITY_POLICY = [
  "default-src 'none'",
  "script-src 'self' 'wasm-unsafe-eval'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data: blob:",
  "media-src 'self' blob:",
  "font-src 'self'",
  "connect-src 'self'",
  "frame-src blob:",
  "object-src 'none'",
  "base-uri 'none'",
  "form-action 'none'",
].join("; ");
