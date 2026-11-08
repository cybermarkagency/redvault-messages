# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Draws the Microsoft Store logos from brand/make_icons.py, each at its final
# size so none is a blurry resize of another.
#
#   python packaging/msix/make-logos.py        (needs Pillow, rsvg-convert; Windows fonts)
#
# Assets/   goes inside the .msix (Start menu, taskbar, install prompt).
# listing/  is uploaded by hand in Partner Center: Store listing > Store logos.
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parents[2] / "brand"))
from make_icons import icon as logo, symbol  # noqa: E402

VIOLET = (111, 75, 216, 255)  # #6F4BD8, the logo's supporting colour
DARK = (28, 32, 40)  # #1C2028, the logo's tile
FONT = "C:/Windows/Fonts/segoeuib.ttf"


def padded(width, height, logo_size):
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    icon = logo(logo_size)
    canvas.paste(icon, ((width - logo_size) // 2, (height - logo_size) // 2), icon)
    return canvas


def main():
    assets = HERE / "Assets"
    listing = HERE / "listing"
    assets.mkdir(exist_ok=True)
    listing.mkdir(exist_ok=True)

    # Store listing and install prompt.
    logo(50).save(assets / "StoreLogo.png")
    logo(100).save(assets / "StoreLogo.scale-200.png")
    # Start menu tiles: a margin, as Windows' own tiles have.
    padded(150, 150, 102).save(assets / "Square150x150Logo.png")
    padded(300, 300, 204).save(assets / "Square150x150Logo.scale-200.png")
    padded(310, 150, 100).save(assets / "Wide310x150Logo.png")
    # App list, taskbar and title bar; "unplated" means no coloured plate behind it.
    logo(44).save(assets / "Square44x44Logo.png")
    logo(88).save(assets / "Square44x44Logo.scale-200.png")
    for size in (16, 24, 32, 48, 256):
        logo(size).save(assets / f"Square44x44Logo.targetsize-{size}.png")
        logo(size).save(assets / f"Square44x44Logo.targetsize-{size}_altform-unplated.png")

    # Partner Center: 1:1 box art (1080 and 2160) and the app tile icons (300, 150, 71).
    for size in (71, 150, 300, 1080, 2160):
        logo(size).save(listing / f"logo-{size}.png")
    # 2:3 poster art: the logo and the name on the app's green.
    for width in (720, 1440):
        poster(width).save(listing / f"poster-{width}x{width * 3 // 2}.png")
    # Xbox "branded key art": the same design, a little wider.
    poster(584, 800).save(listing / "key-art-584x800.png")
    # 16:9 super hero art: no title allowed, and the Store lays text over the left side.
    for width in (1920, 3840):
        hero(width).save(listing / f"hero-{width}x{width * 9 // 16}.png")


def hero(width):
    height = width * 9 // 16
    # A wash from the dark tile (left, under the Store's text) to the violet.
    wash = Image.linear_gradient("L").rotate(90).resize((width, height))
    im = Image.composite(Image.new("RGB", (width, height), VIOLET[:3]), Image.new("RGB", (width, height), DARK), wash)
    d = ImageDraw.Draw(im, "RGBA")
    # Faint rounded squares echoing the logo, spreading out from behind it.
    cx, cy = width * 0.72, height * 0.5
    for i in range(1, 5):
        half = height * (0.2 + 0.13 * i)
        d.rounded_rectangle([cx - half, cy - half, cx + half, cy + half], radius=half * 0.22,
                            outline=(255, 255, 255, 30 - 5 * i), width=max(2, width // 640))
    tile = height * 0.44
    d.rounded_rectangle([cx - tile / 2, cy - tile / 2, cx + tile / 2, cy + tile / 2], radius=tile * 0.22, fill="white")
    mark = symbol(round(tile * 0.7))
    im.paste(mark, (round(cx - mark.width / 2), round(cy - mark.height / 2)), mark)
    return im


def poster(width, height=None):
    height = height or width * 3 // 2
    im = Image.new("RGB", (width, height), VIOLET[:3])
    mark = symbol(round(width * 0.39))
    # The light tile: the violet arcs would vanish straight on the violet.
    d = ImageDraw.Draw(im)
    tile = width * 0.56
    left, top = (width - tile) / 2, height * 0.36 - tile / 2
    d.rounded_rectangle([left, top, left + tile, top + tile], radius=tile * 0.22, fill="white")
    im.paste(mark, (round((width - mark.width) / 2), round(height * 0.36 - mark.height / 2)), mark)
    font = ImageFont.truetype(FONT, round(width * 0.105))
    for text, y in (("RedVault", 0.68), ("Messages", 0.77)):
        b = font.getbbox(text)
        d.text(((width - (b[0] + b[2])) / 2, height * y - (b[1] + b[3]) / 2), text, font=font, fill="white")
    return im


if __name__ == "__main__":
    main()
