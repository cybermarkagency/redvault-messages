#!/usr/bin/env bash
# Runs the real Android app's Tor probe (androidTest .../tor/TorAppProbeTest.kt)
# on an emulator, with the leak evidence alongside:
#   - the device's socket tables sampled every 0.5 s (socket-sampler.sh), for
#     procnet_to_connlog.py + check_relays.py against the app's own Tor consensus;
#   - a DNS capture (tcpdump, needs a userdebug image: adb root);
#   - the scratch server's request log (which listener every request used).
#
#   run-app-probe.sh <adb> <serial> <app apk> <test apk> <output dir>
#
# Git Bash: export MSYS_NO_PATHCONV=1 first and pass Windows-style paths.
set -u
ADB="$1"; D="$2"; APP="$3"; TEST="$4"; OUT="$5"
PKG=com.redvaultsystems.redvaultmessage
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"

"$ADB" -s "$D" root > /dev/null; sleep 3; "$ADB" -s "$D" wait-for-device
"$ADB" -s "$D" install -r -t "$APP" | tail -1
"$ADB" -s "$D" install -r -t "$TEST" | tail -1
"$ADB" -s "$D" shell pm list packages -U "$PKG" | tr -d '\r' | grep "^package:$PKG uid:" | sed 's/.*uid://' > "$OUT/uid.txt"
"$ADB" -s "$D" shell pm clear "$PKG" > /dev/null

bash "$HERE/socket-sampler.sh" "$ADB" "$D" "$OUT/procnet.txt" &
SAMPLER=$!
"$ADB" -s "$D" shell "rm -f /data/local/tmp/rv-dns.pcap; tcpdump -i any -n -s 0 -w /data/local/tmp/rv-dns.pcap 'port 53 or port 853 or port 5353'" > "$OUT/tcpdump.txt" 2>&1 &
DUMP=$!
curl -s -X POST http://127.0.0.1:18082/log/clear > /dev/null
"$ADB" -s "$D" logcat -c

"$ADB" -s "$D" shell am instrument -w -e torProbe true -e class "$PKG.tor.TorAppProbeTest" \
  "$PKG.test/androidx.test.runner.AndroidJUnitRunner" > "$OUT/instrument.txt" 2>&1

"$ADB" -s "$D" logcat -d -s TorProbe:I > "$OUT/logcat.txt"
kill "$SAMPLER" 2> /dev/null
"$ADB" -s "$D" shell pkill tcpdump > /dev/null 2>&1; sleep 2; kill "$DUMP" 2> /dev/null
"$ADB" -s "$D" shell "tcpdump -r /data/local/tmp/rv-dns.pcap -n -tt 2>/dev/null" > "$OUT/dns.txt"
"$ADB" -s "$D" shell "cat /data/data/$PKG/no_backup/tor/cache/dir_blobs/con_microdesc*" > "$OUT/consensus.txt"
curl -s http://127.0.0.1:18082/log > "$OUT/server-log.json"
grep -o '{.*' "$OUT/logcat.txt"
