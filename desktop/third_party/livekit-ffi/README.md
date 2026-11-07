# livekit-ffi protocol

The `.proto` files in `protocol/` are LiveKit's livekit-ffi protocol, copied unchanged from
https://github.com/livekit/rust-sdks at the tag `livekit-ffi/v0.12.83` (`livekit-ffi/protocol/`). They are
compiled with `protoc` by `desktop/groupcall/CMakeLists.txt` and must always match the prebuilt library
that CMake downloads from the same release (`ffi-windows-x86_64.zip`, `ffi-linux-x86_64.zip`): when
you move to another release, change the version and both SHA-256 values there and re-copy these files from
the same tag.

Licensed under the Apache License 2.0 (`LICENSE-APACHE-2.0.txt`, copyright LiveKit, Inc.). The prebuilt
library's own license text ships with the app as `licenses/livekit-ffi-LICENSE.md`.
