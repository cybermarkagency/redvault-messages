# Third-party notices

Each RedVault client ships its own generated notices file: every third-party
component, its exact version and licence, followed by the component's own
licence and copyright files.

| Client | File (bundled in the app) | Components |
|---|---|---|
| Web | `ui/public/legal/THIRD_PARTY_NOTICES.txt` (served at `/legal/THIRD_PARTY_NOTICES.txt`) | 117 |
| Desktop (Windows, Signal build) | `desktop/app/legal/THIRD_PARTY_NOTICES.txt` (Qt resource + installer `licenses/`) | 516 |
| Android (Signal build) | `android/app/src/main/assets/legal/THIRD_PARTY_NOTICES.txt` | 586 |

The AGPLv3 text ships next to each one as `LICENSE.txt`.

RedVault Messages is not affiliated with, sponsored or endorsed by Signal
Messenger LLC. It uses libsignal under AGPL-3.0-only; Signal's copyright
notices and licence are kept unchanged in libsignal's source and reproduced in
the desktop and Android notices.

## Regenerate for every release

```
python tools/compliance/third_party_notices.py web
python tools/compliance/third_party_notices.py desktop --msys2-list desktop/packaging/msys2-shipped-packages.txt
python tools/compliance/third_party_notices.py android --maven-list android/maven-runtime-dependencies.txt --fetch-poms
python tools/compliance/check.py
```

When the installer's DLL set or the Android dependencies change, refresh
`desktop/packaging/msys2-shipped-packages.txt` and
`android/maven-runtime-dependencies.txt` first (each file's header says how).
The Linux AppImage's system libraries are listed per build in
`dist/linux-sources/SOURCES.tsv` by `desktop/packaging/collect-linux-sources.sh`.
