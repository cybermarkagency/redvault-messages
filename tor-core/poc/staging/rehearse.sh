#!/usr/bin/env bash
# A rehearsal of the staging onion setup (deploy/install_tor_onion.sh + deploy/check_internal_binds.sh) on this PC, because
# the staging server isn't reachable from here. It does NOT run the installer (root, apt, systemd); it renders the torrc and
# the nginx site from the installer's own text (render_deploy.py) and runs them for real, in user space inside WSL (Ubuntu):
#
#   C tor 0.4.9 (the Tor Project's daemon, as on the server) -> nginx 1.28 (the rendered onion site) -> an echo upstream
#
# then reaches the new onion address through a second, client-only tor. Differences from the server, nothing else:
# DataDirectory, log destination (a file instead of syslog), and the echo upstream standing in for the API.
# Needs aptroot.sh to have installed: nginx tor.   Usage: rehearse.sh <web-dist-dir>
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WEB_ROOT="${1:?web dist dir}"
ROOT="${ROOT:-$HOME/aptroot}"; SR="$ROOT/sysroot"
export LD_LIBRARY_PATH="$SR/usr/lib/x86_64-linux-gnu:$SR/lib/x86_64-linux-gnu"
TOR="$SR/usr/bin/tor"; NGINX="$SR/usr/sbin/nginx"
W="$HOME/rv-staging"; rm -rf "$W"; mkdir -p "$W"/{hs,tor-data,client-data,nginx/tmp}
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT
pass=0; fail=0
check() { # check <name> <exit-code> [detail]
  if [ "$2" -eq 0 ]; then echo "PASS  $1"; pass=$((pass + 1)); else echo "FAIL  $1  ${3:-}"; fail=$((fail + 1)); fi
}

echo "== render the torrc from deploy/install_tor_onion.sh"
python3 "$HERE/render_deploy.py" "$REPO/deploy/install_tor_onion.sh" torrc ONION_API_PORT=3001 ONION_NGINX_PORT=8081 HS_DIR="$W/hs" > "$W/torrc.rendered"
sed -e 's#^Log notice syslog#Log notice file '"$W"'/tor.log#' "$W/torrc.rendered" > "$W/torrc"
echo "DataDirectory $W/tor-data" >> "$W/torrc"
sed 's/^/    /' "$W/torrc"
grep -q '^SocksPort 0$' "$W/torrc"; check "torrc has no client SOCKS port (SocksPort 0)" $?
grep -q '^HiddenServicePort 80 127.0.0.1:8081$' "$W/torrc"; check "the onion forwards only to nginx on loopback 8081" $?

echo "== tor --verify-config"
chmod 700 "$W/hs"
"$TOR" -f "$W/torrc" --verify-config > "$W/verify.out" 2>&1; rc=$?
tail -3 "$W/verify.out"; check "tor accepts the rendered torrc (including the DoS-defence and PoW options)" $rc
grep -i -E "unknown option|not recognized|ignoring|not supported|warn" "$W/verify.out" | head -5

echo "== start the onion service (tor), then render the nginx site with its address"
chmod 700 "$W/hs"
"$TOR" -f "$W/torrc" > "$W/tor.stdout" 2>&1 & PIDS+=($!)
for _ in $(seq 1 60); do [ -s "$W/hs/hostname" ] && break; sleep 1; done
ONION="$(tr -d '\r\n ' < "$W/hs/hostname" 2>/dev/null || true)"
[ -n "$ONION" ]; check "tor created the onion address" $? "(see $W/tor.stdout)"
echo "   onion: $ONION"
python3 "$HERE/render_deploy.py" "$REPO/deploy/install_tor_onion.sh" site ONION_API_PORT=3001 ONION_NGINX_PORT=8081 ONION="$ONION" WEB_ROOT="$WEB_ROOT" > "$W/site.conf"
cat > "$W/nginx/nginx.conf" <<CONF
worker_processes 1;
daemon off;
pid $W/nginx/nginx.pid;
error_log $W/nginx/error.log notice;
events { worker_connections 256; }
http {
    include $SR/etc/nginx/mime.types;
    default_type application/octet-stream;
    client_body_temp_path $W/nginx/tmp/body;
    proxy_temp_path $W/nginx/tmp/proxy;
    fastcgi_temp_path $W/nginx/tmp/fastcgi;
    uwsgi_temp_path $W/nginx/tmp/uwsgi;
    scgi_temp_path $W/nginx/tmp/scgi;
    include $W/site.conf;
}
CONF
"$NGINX" -p "$W/nginx" -c "$W/nginx/nginx.conf" -t > "$W/nginx-t.out" 2>&1; rc=$?
tail -2 "$W/nginx-t.out"; check "nginx -t accepts the rendered onion site" $rc

echo "== run nginx and the echo upstream"
python3 "$HERE/echo_upstream.py" & PIDS+=($!)
"$NGINX" -p "$W/nginx" -c "$W/nginx/nginx.conf" > "$W/nginx.stdout" 2>&1 & PIDS+=($!)
for _ in $(seq 1 30); do curl -s -o /dev/null -m 2 http://127.0.0.1:3001/ && curl -s -o /dev/null -m 2 http://127.0.0.1:8081/ && break; sleep 0.5; done

H=(-H "Host: $ONION")
code=$(curl -s -o /dev/null -D - -m 10 "${H[@]}" http://127.0.0.1:8081/ | tr -d '' | awk 'NR==1{c=$2} tolower($1)=="location:"{l=$2} END{print c, l}')
[ "$code" = "302 /redvaultmessages/" ]; check "/ redirects to /redvaultmessages/" $? "got: $code"
body=$(curl -s -m 10 "${H[@]}" http://127.0.0.1:8081/redvaultmessages/)
echo "$body" | grep -qi "<html\|<!doctype"; check "the web app is served from the alias" $?
echo "$body" | grep -qi "redvault"; check "...and it is the Redvault page" $?

echo "== the API location strips what identifies a visitor"
j=$(curl -s -m 10 "${H[@]}" -A "SecretBrowser/9.9 (my machine)" -e "https://tracker.example/page" -H "Accept-Language: de-CH" -H "X-Forwarded-For: 203.0.113.9" -H "X-Real-IP: 203.0.113.9" -H "Forwarded: for=203.0.113.9" http://127.0.0.1:8081/redvaultmessages/api/ping)
printf '%s' "$j" > "$W/ping.json"
python3 - "$W/ping.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); h = d["headers"]
print("   upstream saw path:", d["path"], "| headers:", sorted(h))
bad = [k for k in ("x-forwarded-for", "x-real-ip", "forwarded", "user-agent", "referer", "accept-language") if h.get(k)]
sys.exit(0 if d["path"] == "/api/ping" and not bad else 1)
PY
check "path rewritten to /api/ping; X-Forwarded-For, X-Real-IP, Forwarded, User-Agent, Referer, Accept-Language arrive empty" $?
python3 - "$W/ping.json" "$ONION" <<'PY'
import json, sys
sys.exit(0 if json.load(open(sys.argv[1]))["headers"].get("host") == sys.argv[2] else 1)
PY
check "the Host header (the onion name) is kept" $?

echo "== streaming: a 120 MB encrypted-file upload passes through unbuffered"
head -c 125829120 /dev/urandom > "$W/blob.bin"
want=$(sha256sum "$W/blob.bin" | cut -d' ' -f1)
curl -s -m 300 "${H[@]}" -X PUT --data-binary @"$W/blob.bin" -H "Content-Type: application/octet-stream" http://127.0.0.1:8081/redvaultmessages/api/v1/files/x > "$W/upload.json"
python3 - "$W/upload.json" "$want" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); print("   upstream got", d["bodyBytes"], "bytes, path", d["path"])
sys.exit(0 if d["bodyBytes"] == 125829120 and d["bodySha256"] == sys.argv[2] and d["path"] == "/api/v1/files/x" else 1)
PY
check "the bytes arrive intact (same SHA-256) at /api/v1/files/x" $?
buffered=$(ls "$W/nginx/tmp/body" 2>/dev/null | wc -l)
[ "$buffered" -eq 0 ]; check "nginx wrote no request-body temp files (proxy_request_buffering off)" $? "($buffered files)"
curl -s -m 20 "${H[@]}" http://127.0.0.1:8081/redvaultmessages/api/backup/abc123 > "$W/backup.json"
python3 - "$W/backup.json" <<'PY'
import json, sys
sys.exit(0 if json.load(open(sys.argv[1]))["path"] == "/api/backup/abc123" else 1)
PY
check "backup paths are rewritten to /api/backup/..." $?
rm -f "$W/blob.bin"

echo "== check_internal_binds.sh"
INTERNAL_PORTS="3000 3001 8081 5432 9050 9051" bash "$REPO/deploy/check_internal_binds.sh"; check "internal ports are on loopback only (this rehearsal's 3001 and 8081)" $?
python3 -m http.server 3000 --bind 0.0.0.0 > /dev/null 2>&1 & decoy=$!
sleep 3
bash "$REPO/deploy/check_internal_binds.sh" > "$W/binds-negative.out" 2>&1; rc=$?
kill "$decoy" 2>/dev/null
head -2 "$W/binds-negative.out"
[ "$rc" -ne 0 ] && grep -q "EXPOSED: port 3000" "$W/binds-negative.out"; check "the checker FAILS when an internal port (3000) listens on all addresses" $?

echo "== reach it through the real Tor network (a second, client-only tor)"
for _ in $(seq 1 120); do grep -q "Bootstrapped 100%" "$W/tor.log" 2>/dev/null && break; sleep 2; done
grep -E "Bootstrapped 100%|escriptor|ploaded" "$W/tor.log" | tail -3
"$TOR" --DataDirectory "$W/client-data" --SocksPort 19050 --Log "notice file $W/client.log" > "$W/client.stdout" 2>&1 & PIDS+=($!)
for _ in $(seq 1 120); do grep -q "Bootstrapped 100%" "$W/client.log" 2>/dev/null && break; sleep 2; done
reached=1
for attempt in 1 2 3 4 5 6; do
  if curl -s -m 90 --socks5-hostname 127.0.0.1:19050 "http://$ONION/redvaultmessages/api/ping" > "$W/tor-ping.json" 2>/dev/null && grep -q '"path": "/api/ping"' "$W/tor-ping.json"; then reached=0; break; fi
  echo "   attempt $attempt: not reachable yet"; sleep 20
done
check "the onion address answers through Tor: /redvaultmessages/api/ping reaches the upstream as /api/ping" $reached
if [ $reached -eq 0 ]; then
  python3 - "$W/tor-ping.json" <<'PY'
import json, sys
print("   headers the upstream saw via Tor:", sorted(json.load(open(sys.argv[1]))["headers"]))
PY
fi
curl -s -m 90 --socks5-hostname 127.0.0.1:19050 "http://$ONION/redvaultmessages/" > "$W/tor-page.html" 2>/dev/null
grep -qi "<html\|<!doctype" "$W/tor-page.html"; check "the web app loads from the onion address through Tor" $?

echo
echo "rehearsal: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
