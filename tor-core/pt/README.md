# Bridge transport programs

Bridges (see `src/bridges.rs`) need two programs from the Tor Project, which
the apps ship next to themselves and Arti starts as child processes:

| Program | Transports | Source (exact version) | Licence |
|---|---|---|---|
| lyrebird | obfs4, WebTunnel | `gitlab.torproject.org/tpo/anti-censorship/pluggable-transports/lyrebird` @ `v0.0.0-20261104192811-59ac7e400c38` (commit 59ac7e400c384773f5584dbe366516ad04a33d9f) | BSD-2-Clause, with GPL-3.0-or-later parts (`internal/x25519ell2`, `transports/meeklite`), so the built program is GPL-3.0-or-later |
| snowflake-client | Snowflake | `gitlab.torproject.org/tpo/anti-censorship/pluggable-transports/snowflake/v2` @ `v2.15.3` (commit 3bd77dc0f574255d072f70a840e18c0af3d43866) | BSD-3-Clause |

`build-transports.sh TARGET OUT_DIR` builds both from that source through
Go's module mirror (checksums checked against sum.golang.org), each with the
dependency versions its own `go.mod`/`go.sum` pins. The licence texts are in
`licenses/` and are installed with the apps.

lyrebird is GPL-3.0-or-later as built: whoever receives the program can ask
for its source. It is unmodified upstream source at the commit above. Before
a release that ships it, put that source (`go mod download` of the module, plus
its dependencies with `go mod vendor`) in the public source release next to the
app's own source, so the offer can be met from there.

The built-in bridge lists (`src/builtin_bridges.json`) are Tor Browser's,
from https://bridges.torproject.org/moat/circumvention/builtin (fetched
2026-11-17). Refresh them before each release.
