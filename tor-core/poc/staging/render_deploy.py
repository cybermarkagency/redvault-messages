#!/usr/bin/env python3
"""Renders the files deploy/install_tor_onion.sh would write, without running it (it needs root, apt and systemd).

The script writes its torrc and nginx site with unquoted heredocs; this lifts those heredocs out of the real script and
expands them the way bash would ($NAME, ${NAME}, and a backslash before a dollar for a literal dollar), so what is
tested is the script's own text.

  render_deploy.py <script> <target> KEY=VALUE...      target: "torrc" or "site"   (prints the file)
"""
import re
import sys

script, target, *pairs = sys.argv[1:]
env = dict(p.split("=", 1) for p in pairs)
markers = {"torrc": "cat > /etc/tor/torrc <<EOF", "site": 'cat > "$SITE" <<EOF'}

lines = open(script, encoding="utf-8").read().replace("\r\n", "\n").split("\n")
start = next(i for i, l in enumerate(lines) if l.strip() == markers[target])
end = next(i for i in range(start + 1, len(lines)) if lines[i] == "EOF")
body = "\n".join(lines[start + 1 : end]) + "\n"

token = re.compile(r"\\([\\$`\n])|\$\{(\w+)\}|\$(\w+)")


def expand(m):
    if m.group(1) is not None:
        return "" if m.group(1) == "\n" else m.group(1)
    name = m.group(2) or m.group(3)
    if name not in env:
        raise SystemExit(f"{target}: the script uses ${name}; pass {name}=...")
    return env[name]


sys.stdout.write(token.sub(expand, body))
