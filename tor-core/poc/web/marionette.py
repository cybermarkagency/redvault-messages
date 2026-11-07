"""A minimal Marionette client (Firefox's remote-control protocol, which Tor
Browser ships), with no dependencies. Used by torbrowser_probe.py to drive the
real web app inside Tor Browser.

Protocol: after connecting, the server sends "<length>:<json>"; each command is
"<length>:[0, id, name, params]" and each answer "<length>:[1, id, error, result]".
"""
import base64
import json
import socket
import time

ELEMENT_KEY = "element-6066-11e4-a52e-4f735466cecf"


class MarionetteError(Exception):
    pass


class Marionette:
    def __init__(self, host="127.0.0.1", port=2828, timeout=120):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.next_id = 0
        self.hello = self._read()
        self.send("WebDriver:NewSession", {"capabilities": {"alwaysMatch": {"acceptInsecureCerts": True}}})

    def _read(self):
        length = b""
        while True:
            c = self.sock.recv(1)
            if not c:
                raise MarionetteError("connection closed")
            if c == b":":
                break
            length += c
        remaining = int(length)
        body = b""
        while remaining > 0:
            chunk = self.sock.recv(min(65536, remaining))
            if not chunk:
                raise MarionetteError("connection closed")
            body += chunk
            remaining -= len(chunk)
        return json.loads(body.decode("utf-8"))

    def send(self, name, params=None):
        self.next_id += 1
        data = json.dumps([0, self.next_id, name, params or {}]).encode("utf-8")
        self.sock.sendall(str(len(data)).encode() + b":" + data)
        while True:
            message = self._read()
            if isinstance(message, list) and message[0] == 1 and message[1] == self.next_id:
                _, _, error, result = message
                if error:
                    raise MarionetteError(f"{name}: {error.get('error')}: {error.get('message', '')[:300]}")
                return result

    # --- navigation and scripting ---
    def navigate(self, url):
        return self.send("WebDriver:Navigate", {"url": url})

    def url(self):
        return self.send("WebDriver:GetCurrentURL")["value"]

    def title(self):
        return self.send("WebDriver:GetTitle")["value"]

    def script(self, source, args=None, timeout_ms=60000):
        """Runs `source` as a function body (use `return`); arguments are `arguments[i]`."""
        self.send("WebDriver:SetTimeouts", {"script": timeout_ms})
        return self.send("WebDriver:ExecuteScript", {"script": source, "args": args or []})["value"]

    def script_async(self, source, args=None, timeout_ms=60000):
        """Like script(), but the last argument is a callback: call it with the result."""
        self.send("WebDriver:SetTimeouts", {"script": timeout_ms})
        return self.send("WebDriver:ExecuteAsyncScript", {"script": source, "args": args or []})["value"]

    def chrome(self, source, args=None):
        """Runs in the browser's own (chrome) context, for prefs and browser state."""
        self.send("Marionette:SetContext", {"value": "chrome"})
        try:
            return self.script(source, args)
        finally:
            self.send("Marionette:SetContext", {"value": "content"})

    # --- elements ---
    def find_js(self, expression, timeout=30):
        """Waits for a DOM expression (a function body returning an element) and returns the element reference."""
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                found = self.script(expression)
                if found:
                    return found
            except MarionetteError as e:
                last = e
            time.sleep(0.4)
        raise MarionetteError(f"element not found: {expression[:120]} ({last})")

    def click(self, element):
        self.send("WebDriver:ElementClick", {"id": element[ELEMENT_KEY]})

    def type(self, element, text):
        self.send("WebDriver:ElementClear", {"id": element[ELEMENT_KEY]})
        self.send("WebDriver:ElementSendKeys", {"id": element[ELEMENT_KEY], "text": text})

    def button(self, text, timeout=30, exact=True):
        cmp = "t === arguments[0]" if exact else "t.includes(arguments[0])"
        end = time.time() + timeout
        while time.time() < end:
            found = self.script(
                f"return [...document.querySelectorAll('button')].find(b => {{ const t = b.textContent.trim(); return !b.disabled && ({cmp}); }}) || null",
                [text],
            )
            if found:
                return found
            time.sleep(0.4)
        raise MarionetteError(f"no enabled button '{text}'")

    def input(self, placeholder, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            found = self.script("return [...document.querySelectorAll('input,textarea')].find(i => i.placeholder === arguments[0]) || null", [placeholder])
            if found:
                return found
            time.sleep(0.4)
        raise MarionetteError(f"no input with placeholder '{placeholder}'")

    def text(self):
        return self.script("return document.body.innerText")

    def wait_text(self, needle, timeout=60, unlock=True):
        end = time.time() + timeout
        while time.time() < end:
            body = self.text()
            if needle in body:
                return body
            if unlock and "is locked" in body:
                # The app covers itself when its window loses focus; press the button that uncovers it.
                button = self.script("return [...document.querySelectorAll('button')].find(b => b.innerText.trim() === 'Unlock') || null")
                if button:
                    self.click(button)
            time.sleep(0.5)
        raise MarionetteError(f"text not shown: {needle!r}; page says: {self.text()[:300]!r}")

    def screenshot(self, path):
        data = self.send("WebDriver:TakeScreenshot", {"full": False})["value"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
