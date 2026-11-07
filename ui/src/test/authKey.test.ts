import { beforeAll, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import { credentialFor, newCredential, pinSignInKeys } from "../lib/authKey.js";
import * as crypto from "../lib/e2eCrypto.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof api>()),
  preLogin: vi.fn(),
}));

const salt = new Uint8Array(16).fill(7);
const answer = (authVersion: 1 | 2, upgrade: boolean) => ({ kdfSalt: crypto.toBase64(salt), authVersion, upgrade });

beforeAll(async () => {
  await crypto.ready();
});

describe("sign-in key pins", () => {
  it("hold for any spelling of the username, and follow a rename", async () => {
    const { movePin, pinSignInKeys } = await import("../lib/authKey.js");
    localStorage.clear();
    pinSignInKeys("Alice");
    expect(localStorage.getItem("redvaultsystem:signInKeys:alice")).toBe("1");
    pinSignInKeys("  BOB ");
    expect(localStorage.getItem("redvaultsystem:signInKeys:bob")).toBe("1");
    movePin("ALICE", "Carol");
    expect(localStorage.getItem("redvaultsystem:signInKeys:carol")).toBe("1");
    movePin("nobody", "dave");
    expect(localStorage.getItem("redvaultsystem:signInKeys:dave")).toBeNull();
    localStorage.clear();
  });
});

describe("stored key-stretching settings", () => {
  it("a stronger setting derives a different key, and still opens what it wrapped", () => {
    const strong = { ops: 3, mem: 128 * 1024 * 1024 };
    expect(crypto.deriveSignInKey("a password", salt, strong)).not.toBe(crypto.deriveSignInKey("a password", salt));
    const pair = crypto.generateKeyPair();
    const wrapKeyed = crypto.wrapPrivateKey(pair.privateKey, "a password");
    expect(wrapKeyed.kdf).toEqual(crypto.KDF_CURRENT);
    expect(crypto.unwrapPrivateKey(wrapKeyed, "a password")).toEqual(pair.privateKey);
  });

  it("refuses settings below the floor: a server can't make a captured sign-in key cheap to attack", () => {
    expect(() => crypto.deriveSignInKey("a password", salt, { ops: 1, mem: 67108864 })).toThrow(/wasn't sent/);
    expect(() => crypto.deriveSignInKey("a password", salt, { ops: 2, mem: 1024 * 1024 })).toThrow(/wasn't sent/);
  });

  it("refuses settings that would hang the app", () => {
    expect(() => crypto.deriveSignInKey("a password", salt, { ops: 2, mem: 8 * 1024 * 1024 * 1024 })).toThrow(/wasn't sent/);
    expect(() => crypto.deriveSignInKey("a password", salt, { ops: 1000, mem: 67108864 })).toThrow(/wasn't sent/);
  });

  it("sign-in follows the account's stored settings", async () => {
    const strong = { ops: 3, mem: 128 * 1024 * 1024 };
    vi.mocked(api.preLogin).mockResolvedValue({ ...answer(2, true), kdfOps: strong.ops, kdfMem: strong.mem });
    expect((await credentialFor("frank", "a password")).password).toBe(crypto.deriveSignInKey("a password", salt, strong));
  });
});

describe("sign-in keys", () => {
  it("matches the vector every client tests against", () => {
    expect(crypto.deriveSignInKey("correct horse battery staple", salt)).toBe("FctalMlnA5L7J4K3YTVRGWqHwQRvCdnMWno0LvXQI2I=");
  });

  it("is not the password, and differs from the unlock key", () => {
    const key = crypto.deriveSignInKey("a password", salt);
    expect(key).not.toContain("a password");
    expect(crypto.fromBase64(key)).not.toEqual(crypto.deriveWrapKey("a password", salt));
  });

  it("an upgraded account never sends the password", async () => {
    vi.mocked(api.preLogin).mockResolvedValue(answer(2, true));
    const credential = await credentialFor("alice", "a password");
    expect(credential.password).not.toBe("a password");
    expect(credential.authVersion).toBe(2);
  });

  it("an old account sends the password once, with the key to move to", async () => {
    vi.mocked(api.preLogin).mockResolvedValue(answer(1, true));
    const credential = await credentialFor("bob", "a password");
    expect(credential.password).toBe("a password");
    expect(credential.upgradeAuthKey).toBe(crypto.deriveSignInKey("a password", salt));
  });

  it("refuses to send the password if a server claims a pinned account went backwards", async () => {
    pinSignInKeys("carol");
    vi.mocked(api.preLogin).mockResolvedValue(answer(1, true));
    await expect(credentialFor("carol", "a password")).rejects.toThrow(/wasn't sent/);
    vi.mocked(api.preLogin).mockResolvedValue(answer(1, false));
    await expect(newCredential("carol", "a password", salt)).rejects.toThrow(/wasn't sent/);
  });

  it("uses a new account's sign-in key once the server allows it", async () => {
    vi.mocked(api.preLogin).mockResolvedValue(answer(2, true));
    expect((await newCredential("dave", "a password", salt)).authVersion).toBe(2);
    vi.mocked(api.preLogin).mockResolvedValue(answer(1, false));
    expect(await newCredential("erin", "a password", salt)).toEqual({ password: "a password" });
  });
});
