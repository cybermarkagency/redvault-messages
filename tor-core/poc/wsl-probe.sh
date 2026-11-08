#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Runs the Linux probe inside WSL: rv-tor-probe with the given arguments, fresh
# state/cache dirs under ~/rv-tor-run/<name> unless KEEP=1.
set -e
umask 077
NAME="$1"; shift
RUN="$HOME/rv-tor-run/$NAME"
[ "${KEEP:-0}" = "1" ] || rm -rf "$RUN"
mkdir -p "$RUN/state" "$RUN/cache"
chmod 700 "$HOME/rv-tor-run" "$RUN" "$RUN/state" "$RUN/cache"
exec "$HOME/rv-tor-target/debug/rv-tor-probe" --state "$RUN/state" --cache "$RUN/cache" "$@"
