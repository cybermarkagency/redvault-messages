# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

"""Generates desktop/app/icons/ui/*.svg from the web app's design icons
(ui/src/components/icons24.tsx), so both apps draw the same icons.

    python desktop/tools/gen_icons.py

Re-run after changing icons24.tsx, then list any new file in
desktop/app/CMakeLists.txt's RESOURCES.
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ui" / "src" / "components" / "icons24.tsx"
OUT = ROOT / "desktop" / "app" / "icons" / "ui"

HEADER = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" '
    'stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">\n'
)


def kebab(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "-", name).lower()


def jsx_to_svg(body: str) -> str:
    body = body.replace("<>", "").replace("</>", "")
    body = re.sub(r"\{/\*.*?\*/\}", "", body, flags=re.S)
    # camelCase attributes to SVG's kebab-case
    body = re.sub(r"\b(stroke|fill|clip|font)([A-Z]\w*)=", lambda m: f"{m.group(1)}-{kebab(m.group(2))}=", body)
    body = re.sub(r"=\{(-?[\d.]+)\}", r'="\1"', body)
    lines = [line.strip() for line in body.strip().splitlines() if line.strip()]
    return "".join(f"  {line}\n" for line in lines)


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    OUT.mkdir(parents=True, exist_ok=True)
    written = []
    for match in re.finditer(r"export const (\w+)Icon = make\(", text):
        start = match.end()
        depth = 1
        i = start
        while depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        body = text[start : i - 1]
        name = kebab(match.group(1))
        (OUT / f"{name}.svg").write_text(HEADER + jsx_to_svg(body) + "</svg>\n", encoding="utf-8", newline="\n")
        written.append(name)
    # The end-call icon is the phone rotated 135 degrees.
    phone = (OUT / "phone.svg").read_text(encoding="utf-8")
    inner = phone[len(HEADER) : -len("</svg>\n")]
    (OUT / "phone-off.svg").write_text(
        HEADER + '  <g transform="rotate(135 12 12)">\n' + inner + "  </g>\n</svg>\n", encoding="utf-8", newline="\n"
    )
    written.append("phone-off")
    print("\n".join(f"        icons/ui/{n}.svg" for n in sorted(written)))


if __name__ == "__main__":
    main()
