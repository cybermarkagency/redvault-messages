// Node test driver: the same WebAssembly module the browser runs (nodejs
// target of wasm-bindgen), one JSON request per line in, one response out.
const { createInterface } = require("node:readline");
const { call } = require("../build/node/redvault_e2ee.js");

createInterface({ input: process.stdin }).on("line", (line) => {
  if (line.trim()) process.stdout.write(`${call(line.trim())}\n`);
});
