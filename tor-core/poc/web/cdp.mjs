// A small Chrome DevTools Protocol client and browser launcher (Edge or Chrome, headless, fake microphone and
// camera), with no dependencies: Node's built-in WebSocket. Used by call_relay_test.mjs to drive the real web app.
import { spawn } from "node:child_process";
import { mkdirSync, rmSync } from "node:fs";

export const EDGE = "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class Page {
  constructor(ws) {
    this.ws = ws;
    this.nextId = 0;
    this.pending = new Map();
    ws.addEventListener("message", (event) => {
      const message = JSON.parse(event.data);
      if (message.id && this.pending.has(message.id)) {
        const { resolve, reject } = this.pending.get(message.id);
        this.pending.delete(message.id);
        if (message.error) reject(new Error(message.error.message));
        else resolve(message.result);
      }
    });
  }

  send(method, params = {}) {
    const id = ++this.nextId;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }

  /** Evaluates an expression (it may be async) in the page and returns its value. */
  async eval(expression) {
    const { result, exceptionDetails } = await this.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
    if (exceptionDetails) throw new Error(exceptionDetails.exception?.description?.split("\n")[0] ?? exceptionDetails.text);
    return result.value;
  }

  async navigate(url) {
    await this.send("Page.navigate", { url });
    await sleep(1500);
  }

  close() {
    this.ws.close();
  }
}

/** Starts a headless browser with its own profile and returns { page, stop }. `initScript` runs before every page's own scripts. */
export async function launch(name, port, { profileRoot, initScript, exe = EDGE } = {}) {
  const profile = `${profileRoot}\\${name}`;
  rmSync(profile, { recursive: true, force: true });
  mkdirSync(profile, { recursive: true });
  const child = spawn(
    exe,
    [
      "--headless=new",
      "--window-size=1280,900",
      `--user-data-dir=${profile}`,
      `--remote-debugging-port=${port}`,
      "--remote-allow-origins=*",
      "--use-fake-device-for-media-stream",
      "--use-fake-ui-for-media-stream",
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-background-timer-throttling",
      "--disable-renderer-backgrounding",
      "--disable-backgrounding-occluded-windows",
      "about:blank",
    ],
    { stdio: "ignore", detached: false }
  );
  let targets;
  for (let i = 0; i < 60; i++) {
    try {
      targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      if (targets.some((t) => t.type === "page")) break;
    } catch {
      /* still starting */
    }
    await sleep(500);
  }
  const target = targets?.find((t) => t.type === "page");
  if (!target) throw new Error(`${name}: no page target on port ${port}`);
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    ws.addEventListener("open", resolve);
    ws.addEventListener("error", reject);
  });
  const page = new Page(ws);
  await page.send("Page.enable");
  await page.send("Runtime.enable");
  if (initScript) await page.send("Page.addScriptToEvaluateOnNewDocument", { source: initScript });
  return {
    page,
    pid: child.pid,
    stop() {
      page.close();
      child.kill();
    },
  };
}

export { sleep };
