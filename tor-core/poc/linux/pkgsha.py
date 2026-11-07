"""Reads `apt-cache show <name>` from stdin; prints the SHA-256 of the stanza whose Filename ends with <file>."""
import re
import sys

# The archive's file names have no epoch ("2:2.7-2" is stored as "2.7-2"); apt's local names do ("2%3a2.7-2").
wanted = re.sub(r"_\d+(%3a|:)", "_", sys.argv[1])
for stanza in sys.stdin.read().split("\n\n"):
    fields = {}
    for line in stanza.splitlines():
        if ": " in line and not line.startswith(" "):
            key, value = line.split(": ", 1)
            fields[key] = value
    name = fields.get("Filename", "").rsplit("/", 1)[-1]
    if name == wanted and "SHA256" in fields:
        print(fields["SHA256"])
        break
