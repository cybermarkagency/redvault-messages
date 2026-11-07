#!/usr/bin/env bash
# Builds the engine and every test driver for the cross-platform run, from
# Git Bash on Windows: the Windows C++ driver (MinGW), the JNI library for the
# desktop JVM and for Android (NDK r29), the WebAssembly packages (browser and
# Node), the Android dex driver, and (through WSL Ubuntu) the Linux C++ driver.
# Needs: Rust 1.99.1 (rustup) with the wasm32 and Android targets,
# wasm-bindgen-cli 0.2.129, MSYS2 UCRT64 g++, JBR 21, Android SDK build-tools 36,
# NDK 29.0.14206865, and Rust inside WSL Ubuntu.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRATE="$(cd "$HERE/.." && pwd)"
export PATH="$HOME/.cargo/bin:/c/msys64/ucrt64/bin:$PATH"
SDK="$LOCALAPPDATA/Android/Sdk"
TC="$SDK/ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin"
cd "$CRATE"

echo "== tests"
cargo test --release --locked 2>&1 | grep -E "test result" || true

echo "== windows (C ABI + JNI for the JVM)"
cargo build --release --locked --features ffi,jni
mkdir -p "$HERE/build/windows"
g++ -std=c++20 -O2 -I "$CRATE/include" "$HERE/drivers/cpp_driver.cpp" "$CRATE/target/release/libredvault_e2ee.a" \
  -lws2_32 -luserenv -lntdll -lbcrypt -ladvapi32 -o "$HERE/build/windows/cpp_driver.exe"

echo "== web (WebAssembly)"
RUSTFLAGS='--cfg getrandom_backend="wasm_js"' cargo build --release --locked --target wasm32-unknown-unknown --features wasm
wasm-bindgen --target web --out-dir "$HERE/build/web" target/wasm32-unknown-unknown/release/redvault_e2ee.wasm
wasm-bindgen --target nodejs --out-dir "$HERE/build/node" target/wasm32-unknown-unknown/release/redvault_e2ee.wasm

echo "== android (JNI, four ABIs, 16 KB pages)"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$TC/aarch64-linux-android26-clang.cmd"
export CARGO_TARGET_ARMV7_LINUX_ANDROIDEABI_LINKER="$TC/armv7a-linux-androideabi26-clang.cmd"
export CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER="$TC/x86_64-linux-android26-clang.cmd"
export CARGO_TARGET_I686_LINUX_ANDROID_LINKER="$TC/i686-linux-android26-clang.cmd"
for target in aarch64-linux-android armv7-linux-androideabi x86_64-linux-android i686-linux-android; do
  RUSTFLAGS="-C link-arg=-Wl,-z,max-page-size=16384" cargo build --release --locked --target "$target" --features jni
done
JAVA_HOME="$HOME/.jdks/jbr-21.0.11"
mkdir -p "$HERE/build/android/classes"
"$JAVA_HOME/bin/javac" --release 11 -d "$HERE/build/android/classes" \
  "$CRATE/bindings/android/com/redvaultsystems/e2ee/NativeE2ee.java" "$CRATE/bindings/android/com/redvaultsystems/e2ee/Driver.java"
(cd "$HERE/build/android/classes" && JAVA_HOME="$JAVA_HOME" PATH="$JAVA_HOME/bin:$PATH" "$SDK/build-tools/36.0.0/d8.bat" --release --min-api 26 \
  --output "$HERE/build/android" com/redvaultsystems/e2ee/NativeE2ee.class com/redvaultsystems/e2ee/Driver.class)

echo "== linux (WSL Ubuntu)"
WSL_SCRIPT="$(cygpath -w "$HERE/wsl-build.sh" | sed 's#\\#/#g')"
powershell.exe -NoProfile -Command "\$p = (wsl.exe -d Ubuntu -- wslpath -u '$WSL_SCRIPT') -join ''; \$p = \$p.Trim([char]0).Trim(); wsl.exe -d Ubuntu -- bash \$p" | tr -d '\0'
echo "done"
