//! WebAssembly binding for the web app (wasm-bindgen).

use wasm_bindgen::prelude::*;

use crate::engine;

/// Runs one JSON request; returns the JSON response (see docs/vodozemac-poc.md).
#[wasm_bindgen]
pub fn call(request: &str) -> String {
    engine::handle(request)
}
