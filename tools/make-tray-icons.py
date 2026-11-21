"""Regenerate bitmap icons using Pillow and Windows' Bahnschrift typeface.

The font is used only to generate icons, never at runtime, and is not bundled.
"""
import os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 20, 24, 28, 32, 36, 40, 48, 56, 64]
FONT_PATH = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts/bahnschrift.ttf"


def render(label, size, foreground, font):
    # Render the actual font at high resolution, then downsample its alpha mask.
    # Condensed width preserves recognizable bowls and the R's diagonal leg.
    bounds = font.getbbox(label)
    mask = Image.new("L", (bounds[2] - bounds[0], bounds[3] - bounds[1]))
    ImageDraw.Draw(mask).text((-bounds[0], -bounds[1]), label, font=font, fill=255)
    mask = mask.crop(mask.getbbox())
    height = round(size * 10 / 16)
    width = size if len(label) > 1 else round(height * 0.7)
    mask = mask.resize((width, height), Image.Resampling.LANCZOS)
    alpha = Image.new("L", (size, size))
    alpha.paste(mask, ((size - width) // 2, (size - height) // 2))
    image = Image.new("RGBA", (size, size), foreground)
    image.putalpha(alpha)
    return image


def main():
    if not FONT_PATH.is_file():
        raise FileNotFoundError(f"Windows Bahnschrift font is required: {FONT_PATH}")
    font = ImageFont.truetype(str(FONT_PATH), 160)
    font.set_variation_by_axes([650, 75])  # Weight; condensed width.
    destination = Path(__file__).resolve().parents[1] / "toggleHDR"
    for name, label in [("HDR", "HDR"), ("SDR", "SDR"), ("Unknown", "?")]:
        for suffix, color in [("", (255, 255, 255, 255)), ("-dark", (0, 0, 0, 255))]:
            images = [render(label, size, color, font) for size in SIZES]
            target = destination / f"{name}{suffix}.ico"
            images[-1].save(target, format="ICO", sizes=[(size, size) for size in SIZES],
                            append_images=images[:-1])
            with Image.open(target) as icon:
                assert icon.ico.sizes() == {(size, size) for size in SIZES}
                for size in SIZES:
                    frame = icon.ico.getimage((size, size)).convert("RGBA")
                    assert frame.getpixel((0, 0))[3] == 0
                    assert frame.getchannel("A").getbbox() is not None
                    # Smooth edges vary only in opacity, never hue.
                    assert all(pixel[:3] == color[:3] for _, pixel in frame.getcolors(size * size)
                               if pixel[3] != 0)
    print(f"Created six transparent monochrome font icons, each with {len(SIZES)} sizes.")


if __name__ == "__main__":
    main()
