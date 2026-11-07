// The platform drivers shared by run.mjs (relay) and server-run.mjs (the real API).
// Each runs the engine through that platform's own binding, one JSON request per
// line in and one response per line out:
//   web      - WebAssembly in a real browser (the page at the relay's /, via its job queue)
//   node     - the same WebAssembly module in Node (a second web runtime)
//   windows  - the C++ driver on the C ABI (MinGW build)
//   linux    - the C++ driver on the C ABI, in WSL Ubuntu
//   android  - the Java driver on JNI, on the emulator (ART, app_process)
import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { createInterface } from "node:readline";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

export const HERE = fileURLToPath(new URL(".", import.meta.url));
export const RELAY = process.env.RELAY ?? "http://127.0.0.1:8787";
const ADB = join(process.env.LOCALAPPDATA ?? "", "Android", "Sdk", "platform-tools", "adb.exe");

export class LineDriver {
  constructor(name, command, args) {
    this.name = name;
    this.command = command;
    this.args = args;
    this.start();
  }
  start() {
    this.child = spawn(this.command, this.args, { stdio: ["pipe", "pipe", "inherit"], windowsHide: true });
    this.pending = [];
    createInterface({ input: this.child.stdout }).on("line", (line) => {
      const next = this.pending.shift();
      if (next) next(line.trim());
    });
  }
  call(request) {
    return new Promise((resolve) => {
      this.pending.push(resolve);
      this.child.stdin.write(`${request}\n`);
    });
  }
  /** A restart: the process goes; all that survives is the state on disk. */
  restart() {
    this.child.stdin.end();
    this.child.kill();
    this.start();
  }
  stop() {
    this.child.stdin.end();
    this.child.kill();
  }
}

export class BrowserDriver {
  name = "web";
  async call(request) {
    const id = randomUUID();
    await fetch(`${RELAY}/jobs/web`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ id, request }) });
    for (;;) {
      const res = await fetch(`${RELAY}/results/${id}`);
      if (res.status === 200) return (await res.json()).response;
    }
  }
  restart() {}
  stop() {}
}

export function driverFor(platform) {
  switch (platform) {
    case "web":
      return new BrowserDriver();
    case "node":
      return new LineDriver("node", process.execPath, [join(HERE, "drivers", "node_driver.cjs")]);
    case "windows":
      return new LineDriver("windows", join(HERE, "build", "windows", "cpp_driver.exe"), []);
    case "linux":
      return new LineDriver("linux", "wsl.exe", ["-d", "Ubuntu", "--", "/home/winchester/rv-bin/cpp_driver"]);
    case "android":
      return new LineDriver("android", ADB, [
        "shell",
        "CLASSPATH=/data/local/tmp/rv-driver.dex app_process -Drv.lib=/data/local/tmp/libredvault_e2ee.so /data/local/tmp com.redvaultsystems.e2ee.Driver",
      ]);
    default:
      throw new Error(`unknown platform ${platform}`);
  }
}

/** One character of the base64 changed (still valid base64). */
export function flip(b64) {
  const i = Math.floor(b64.length / 2);
  return b64.slice(0, i) + (b64[i] === "A" ? "B" : "A") + b64.slice(i + 1);
}
