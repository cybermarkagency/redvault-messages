# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

"""Drives the real Redvault web app inside Tor Browser over Marionette
(docs/tor-routing-plan.md, 13.4). Each subcommand does one real user action
and prints one JSON line, so a run can be interleaved with another browser
(the second user) and with the test server's fault switch.

    python torbrowser_probe.py <command> [args]

    connect [seconds]            press Connect on a fresh profile and wait for the Tor network
    wait-onion <url> [seconds]   load the app from the onion address (retries while the service publishes)
    env                          what this browser offers the app (WebRTC, WebAssembly, IndexedDB, secure context...)
    register <handle> <password> sign up through the UI and acknowledge the recovery key
    login <handle> <password>    sign in again (after a reload the private key is gone from memory)
    add-contact <handle>         Contacts > Add a contact > Look up > Send friend request
    open-chat <handle>           open the conversation with them
    send <text>                  type in the composer and press Send
    messages                     the conversation as shown (text with each message's status)
    wait-text <text> [seconds]   wait until the page shows it
    call                         press the voice-call button and report what the app says
    settings-network             Settings > Network routing, as shown
    shot <path>                  screenshot of the page
"""
import json
import sys
import time

from marionette import Marionette, MarionetteError


def out(**fields):
    print(json.dumps(fields))  # ASCII-safe: Windows consoles reject some characters the page shows


def click_text(m, tag, text, timeout=30, exact=True):
    cmp = "t === arguments[0]" if exact else "t.includes(arguments[0])"
    end = time.time() + timeout
    while time.time() < end:
        # Matches an element's visible text or, for icon buttons, its accessible label.
        found = m.script(
            f"return [...document.querySelectorAll('{tag}')].find(e => {{ const t = (e.innerText||e.textContent||'').trim() || e.getAttribute('aria-label') || ''; return !e.disabled && ({cmp}); }}) || null",
            [text],
        )
        if found:
            m.click(found)
            return
        time.sleep(0.4)
    raise MarionetteError(f"nothing to click: {tag} '{text}'; page: {m.text()[:200]!r}")


def cmd_connect(m, seconds="180"):
    # A fresh Tor Browser profile waits on "Connect to Tor"; press it as a user would, then wait for the network.
    started = time.time()
    if m.url().startswith("about:torconnect"):
        button = m.find_js("return document.querySelector('#connectButton') || null", timeout=30)
        m.click(button)
    end = time.time() + float(seconds)
    while time.time() < end and m.url().startswith("about:torconnect"):
        time.sleep(2)
    out(ok=not m.url().startswith("about:torconnect"), seconds=round(time.time() - started), url=m.url()[:60])


def cmd_wait_onion(m, url, seconds="600"):
    end = time.time() + float(seconds)
    started = time.time()
    attempts = 0
    while time.time() < end:
        attempts += 1
        try:
            m.navigate(url)
        except MarionetteError:
            pass
        title = m.title()
        if "Redvaultsystem" in title:
            return out(ok=True, seconds=round(time.time() - started), attempts=attempts, url=m.url(), title=title)
        time.sleep(8)
    out(ok=False, attempts=attempts, url=m.url(), title=m.title(), page=m.text()[:200])


def cmd_env(m):
    out(**m.script("""
      const nav = navigator;
      let wasm = false;
      try { wasm = typeof WebAssembly === 'object' && WebAssembly.validate(new Uint8Array([0,97,115,109,1,0,0,0])); } catch (e) {}
      return {
        origin: location.origin,
        secureContext: window.isSecureContext,
        webrtc: typeof RTCPeerConnection !== 'undefined',
        webAssembly: wasm,
        indexedDB: typeof indexedDB !== 'undefined',
        subtle: !!(window.crypto && window.crypto.subtle),
        userAgent: nav.userAgent,
        languages: nav.languages,
        hardwareConcurrency: nav.hardwareConcurrency,
        cookiesEnabled: nav.cookieEnabled,
      };"""))


def cmd_register(m, handle, password):
    # A fresh browser shows "Create Your Identity"; a returning one the login form.
    try:
        click_text(m, "button", "Create a new identity instead", timeout=3)
    except MarionetteError:
        pass
    m.type(m.input("Choose a handle"), handle)
    m.type(m.input("Choose a password (min. 8 characters)"), password)
    m.type(m.input("Confirm password"), password)
    for box in m.script("return [...document.querySelectorAll('input[type=checkbox]')]"):
        m.click(box)
    started = time.time()
    click_text(m, "button", "Create Account")
    # Key generation (Argon2id in WebAssembly) and the first requests, all inside Tor Browser.
    end = time.time() + 240
    while time.time() < end:
        if "saved my recovery key" in m.text().lower():
            break
        time.sleep(0.5)
    else:
        return out(ok=False, step="recovery-key screen never appeared", page=m.text()[:300])
    box = m.find_js("return [...document.querySelectorAll('input[type=checkbox]')].find(c => /saved my recovery key/i.test(c.closest('label')?.innerText || c.parentElement.innerText)) || null")
    m.click(box)
    click_text(m, "button", "Done")
    m.wait_text("Chats", timeout=60)
    out(ok=True, seconds=round(time.time() - started, 1), signedIn=True)


def cmd_login(m, handle, password):
    # A browser that has never signed in shows "Create Your Identity": switch to the login form.
    try:
        m.input("Handle", timeout=3)
    except MarionetteError:
        click_text(m, "button", "Log in", timeout=10, exact=False)
    m.type(m.input("Handle"), handle)
    m.type(m.input("Password"), password)
    started = time.time()
    click_text(m, "button", "Log in")
    m.wait_text("Chats", timeout=120)
    out(ok=True, seconds=round(time.time() - started, 1))


def cmd_add_contact(m, handle):
    click_text(m, "nav button, nav a", "Contacts")
    click_text(m, "button", "Add a contact")
    m.type(m.input("their-handle·xxxx"), handle)
    click_text(m, "button", "Look up")
    click_text(m, "button", "Send friend request", timeout=90)
    m.wait_text("request", timeout=30)
    out(ok=True, page=m.text()[-300:].replace("\n", " | "))


def cmd_open_chat(m, handle):
    click_text(m, "nav button, nav a", "Chats")
    end = time.time() + 120
    while time.time() < end:
        found = m.script(
            "return [...document.querySelectorAll('button, a, [role=button]')].filter(e => (e.innerText||'').includes(arguments[0]) && (e.innerText||'').length < 160 && !e.closest('nav')).sort((a, b) => a.innerText.length - b.innerText.length)[0] || null",
            [handle],
        )
        if found:
            m.click(found)
            m.input("Message")
            return out(ok=True)
        time.sleep(1)
    out(ok=False, page=m.text()[:300])


def cmd_send(m, text):
    box = m.input("Message")
    m.type(box, text)
    started = time.time()
    click_text(m, "button[aria-label=Send]", "", exact=False)
    out(ok=True, clickedAt=round(started))


def cmd_messages(m):
    body = m.text()
    out(text=body[-700:].replace("\n", " | "))


def cmd_wait_text(m, text, seconds="60"):
    try:
        m.wait_text(text, timeout=float(seconds))
        out(ok=True)
    except MarionetteError as e:
        out(ok=False, error=str(e)[:300])


def cmd_call(m):
    click_text(m, "button[aria-label^=Voice]", "", exact=False)
    time.sleep(1.5)
    body = m.text()
    out(ok="aren't available in Tor Browser" in body, shown=[l for l in body.split("\n") if "Tor Browser" in l][:2], webrtc=m.script("return typeof RTCPeerConnection !== 'undefined'"))


def cmd_settings_network(m):
    click_text(m, "nav button, nav a", "Settings")
    click_text(m, "button", "Network routing")
    time.sleep(1.5)
    body = m.text()
    i = body.rfind("Network routing")
    out(ok=True, text=body[i : i + 1100].replace("\n", " | "))


def cmd_shot(m, path):
    m.screenshot(path)
    out(ok=True, path=path)


COMMANDS = {
    "connect": cmd_connect,
    "wait-onion": cmd_wait_onion,
    "env": cmd_env,
    "register": cmd_register,
    "login": cmd_login,
    "add-contact": cmd_add_contact,
    "open-chat": cmd_open_chat,
    "send": cmd_send,
    "messages": cmd_messages,
    "wait-text": cmd_wait_text,
    "call": cmd_call,
    "settings-network": cmd_settings_network,
    "shot": cmd_shot,
}


def ensure_unlocked(m):
    # The app covers itself after a few idle minutes ("is locked"); a button uncovers it.
    if "is locked" in m.text():
        click_text(m, "button", "Unlock", timeout=5)
        time.sleep(1)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(2)
    m = Marionette()
    try:
        if sys.argv[1] not in ("connect", "wait-onion", "env", "shot"):
            ensure_unlocked(m)
        COMMANDS[sys.argv[1]](m, *sys.argv[2:])
    except MarionetteError as e:
        out(ok=False, error=str(e)[:500])
        sys.exit(1)
    finally:
        try:
            m.send("WebDriver:DeleteSession")
        except Exception:
            pass
        m.close()


if __name__ == "__main__":
    main()
