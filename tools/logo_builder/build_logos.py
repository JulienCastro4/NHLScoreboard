import argparse
import datetime as dt
import json
import os
import shutil
import sys
from io import BytesIO
from urllib.parse import urlparse, urlunparse

import requests
from PIL import Image, ImageEnhance


STANDINGS_URL_FMT = "https://api-web.nhle.com/v1/standings/{date}"
SIZE = 20

# International teams that appear in NHL international games
INTERNATIONAL_TEAMS = [
    "AUT",  # Austria
    "CAN",  # Canada
    "CZE",  # Czech Republic / Czechia
    "DEN",  # Denmark
    "FIN",  # Finland
    "FRA",  # France
    "GER",  # Germany
    "ITA",  # Italy
    "LAT",  # Latvia
    "NOR",  # Norway
    "RUS",  # Russia
    "SVK",  # Slovakia
    "SUI",  # Switzerland
    "SWE",  # Sweden
    "USA",  # United States
]


# ============================================================================
# Legacy quantization (kept for --legacy flag comparison)
# ============================================================================

def quantize_channel(value, bits):
    if bits >= 8:
        return int(value)
    levels = (1 << bits) - 1
    return int(round(value * levels / 255.0) * (255.0 / levels))


def enhance_image_for_low_depth(image_rgb, contrast=1.3, saturation=1.4, sharpness=1.2):
    enhancer = ImageEnhance.Contrast(image_rgb)
    image_rgb = enhancer.enhance(contrast)
    enhancer = ImageEnhance.Color(image_rgb)
    image_rgb = enhancer.enhance(saturation)
    enhancer = ImageEnhance.Sharpness(image_rgb)
    image_rgb = enhancer.enhance(sharpness)
    return image_rgb


def quantize_image_rgb_adaptive(image_rgb, bits, dither=False):
    if bits >= 8:
        return image_rgb
    colors = 2 ** (bits * 3)
    if colors > 256:
        colors = 256
    img_p = image_rgb.quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG if dither else Image.NONE)
    img_quantized = img_p.convert('RGB')
    width, height = img_quantized.size
    out = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            r, g, b = img_quantized.getpixel((x, y))
            out.putpixel(
                (x, y),
                (
                    quantize_channel(r, bits),
                    quantize_channel(g, bits),
                    quantize_channel(b, bits),
                ),
            )
    return out


# ============================================================================
# Common helpers
# ============================================================================

def rgb_to_rgb565_le(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def normalize_logo_url(url):
    parts = urlparse(url)
    return urlunparse(parts._replace(query="", fragment=""))


def dark_logo_url(url):
    return normalize_logo_url(url).replace("_light.svg", "_dark.svg")


def fetch_json(url):
    resp = requests.get(url, timeout=30)
    resp.raise_for_status()
    return resp.json()


def fetch_bytes(url):
    resp = requests.get(url, timeout=30)
    resp.raise_for_status()
    return resp.content


def rgba_to_rgb(image):
    background = Image.new("RGB", image.size, (0, 0, 0))
    background.paste(image, mask=image.split()[3])
    return background


def write_rgb565(path, image_rgb):
    with open(path, "wb") as f:
        for y in range(image_rgb.height):
            for x in range(image_rgb.width):
                r, g, b = image_rgb.getpixel((x, y))
                f.write(rgb_to_rgb565_le(r, g, b))


def svg_to_png_bytes(svg_bytes):
    try:
        import resvg_py

        svg_string = svg_bytes.decode("utf-8", errors="replace")
        return resvg_py.svg_to_bytes(svg_string=svg_string)
    except Exception as exc:
        raise RuntimeError(f"SVG render failed: {exc}") from exc


def fetch_logo_image(logo_url):
    clean_url = normalize_logo_url(logo_url)
    svg_bytes = fetch_bytes(clean_url)
    png_bytes = svg_to_png_bytes(svg_bytes)
    return Image.open(BytesIO(png_bytes)).convert("RGBA")


# ============================================================================
# Image processing pipeline
# ============================================================================

def process_logo(img_rgba, args, team_abbrev=None):
    """Process a logo image through the selected pipeline."""
    if args.legacy:
        img_rgba = img_rgba.resize((SIZE, SIZE), Image.LANCZOS)
        img_rgb = rgba_to_rgb(img_rgba)
        img_rgb = enhance_image_for_low_depth(
            img_rgb, args.contrast, args.saturation, args.sharpness
        )
        if args.depth and args.depth > 0:
            img_rgb = quantize_image_rgb_adaptive(img_rgb, args.depth, args.dither)
        return img_rgb
    else:
        from logo_quantize import process_logo_perceptual
        return process_logo_perceptual(
            img_rgba,
            target_size=SIZE,
            contrast=args.contrast,
            saturation=args.saturation,
            sharpness=args.sharpness,
            dither=args.dither,
            team_abbrev=team_abbrev,
        )


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="Build NHL and international team logos for scoreboard")
    parser.add_argument("--date", default=None, help="YYYY-MM-DD for NHL standings")
    parser.add_argument("--out", default="out", help="Output folder")
    parser.add_argument(
        "--depth",
        type=int,
        default=4,
        help="Quantize per-channel bit depth (e.g., 4 for 4bpc). 0 = no quantize.",
    )
    parser.add_argument(
        "--dither",
        action="store_true",
        help="Enable Floyd-Steinberg dithering when quantizing.",
    )
    parser.add_argument(
        "--contrast",
        type=float,
        default=1.2,
        help="Contrast enhancement factor (default: 1.2)",
    )
    parser.add_argument(
        "--saturation",
        type=float,
        default=1.3,
        help="Saturation enhancement factor (default: 1.3)",
    )
    parser.add_argument(
        "--sharpness",
        type=float,
        default=1.1,
        help="Sharpness enhancement factor (default: 1.1)",
    )
    parser.add_argument(
        "--legacy",
        action="store_true",
        help="Use legacy quantization (uniform channel rounding) instead of perceptual.",
    )
    parser.add_argument(
        "--no-nhl",
        action="store_true",
        help="Skip NHL team logos (only build international).",
    )
    parser.add_argument(
        "--no-international",
        action="store_true",
        help="Skip international team logos (only build NHL).",
    )
    args = parser.parse_args()

    date = args.date
    if not date:
        date = dt.date.today().isoformat()

    out_dir = args.out
    os.makedirs(out_dir, exist_ok=True)

    manifest = []
    seen_abbrevs = set()

    # ---- NHL team logos from standings API ----
    if not args.no_nhl:
        print(f"[nhl] Fetching standings for {date}...")
        standings_url = STANDINGS_URL_FMT.format(date=date)
        data = fetch_json(standings_url)
        standings = data.get("standings", [])

        for entry in standings:
            abbrev = (
                entry.get("teamAbbrev", {}).get("default")
                or entry.get("teamAbbrev")
                or entry.get("teamCommonName", {}).get("default")
            )
            logo_url = entry.get("teamLogo", "")
            if not abbrev or not logo_url:
                continue

            try:
                logo_url = dark_logo_url(logo_url)
                img_rgba = fetch_logo_image(logo_url)
                img_rgb = process_logo(img_rgba, args, team_abbrev=abbrev)
            except Exception as exc:
                print(f"[skip] {abbrev}: {exc}")
                continue

            rgb565_path = os.path.join(out_dir, f"{abbrev}.rgb565")
            png_path = os.path.join(out_dir, f"{abbrev}.png")

            write_rgb565(rgb565_path, img_rgb)
            img_rgb.save(png_path, format="PNG")

            manifest.append(
                {
                    "team": abbrev,
                    "logo_url": logo_url,
                    "rgb565": os.path.basename(rgb565_path),
                    "preview_png": os.path.basename(png_path),
                    "width": SIZE,
                    "height": SIZE,
                }
            )
            seen_abbrevs.add(abbrev)
            print(f"[ok] {abbrev} -> {rgb565_path}")

        print(f"[nhl] {len(seen_abbrevs)} NHL logos")

    # ---- International team logos ----
    if not args.no_international:
        intl_count = 0
        for team_code in INTERNATIONAL_TEAMS:
            if team_code in seen_abbrevs:
                continue
            logo_url = f"https://assets.nhle.com/logos/ntl/svg/{team_code}_dark.svg"
            try:
                img_rgba = fetch_logo_image(logo_url)
                img_rgb = process_logo(img_rgba, args, team_abbrev=team_code)
            except Exception as exc:
                print(f"[skip] {team_code} (intl): {exc}")
                continue

            rgb565_path = os.path.join(out_dir, f"{team_code}.rgb565")
            png_path = os.path.join(out_dir, f"{team_code}.png")

            write_rgb565(rgb565_path, img_rgb)
            img_rgb.save(png_path, format="PNG")

            manifest.append(
                {
                    "team": team_code,
                    "type": "international",
                    "logo_url": logo_url,
                    "rgb565": os.path.basename(rgb565_path),
                    "preview_png": os.path.basename(png_path),
                    "width": SIZE,
                    "height": SIZE,
                }
            )
            seen_abbrevs.add(team_code)
            intl_count += 1
            print(f"[ok] {team_code} (intl) -> {rgb565_path}")

        print(f"[intl] {intl_count} international logos")

    # ---- Write manifest ----
    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(f"[done] {len(manifest)} total logos")

    # ---- Deploy to data/logos ----
    script_dir = os.path.dirname(os.path.abspath(__file__))
    data_logos_dir = os.path.normpath(os.path.join(script_dir, "..", "..", "data", "logos"))

    os.makedirs(data_logos_dir, exist_ok=True)

    copied = 0
    for entry in manifest:
        src = os.path.join(out_dir, entry["rgb565"])
        dst = os.path.join(data_logos_dir, entry["rgb565"])
        if os.path.exists(src):
            shutil.copy2(src, dst)
            copied += 1

    print(f"[deploy] Copied {copied} logos to {data_logos_dir}")


if __name__ == "__main__":
    sys.exit(main())
