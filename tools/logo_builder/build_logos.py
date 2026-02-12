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
LOGO_WIDTH = 25
LOGO_HEIGHT = 20


def quantize_channel(value, bits):
    if bits >= 8:
        return int(value)
    levels = (1 << bits) - 1
    return int(round(value * levels / 255.0) * (255.0 / levels))


def enhance_image_for_low_depth(image_rgb, contrast=1.3, saturation=1.4, sharpness=1.2):
    """Améliore l'image avant quantization pour de meilleurs résultats."""
    # Augmenter le contraste
    enhancer = ImageEnhance.Contrast(image_rgb)
    image_rgb = enhancer.enhance(contrast)
    
    # Augmenter la saturation
    enhancer = ImageEnhance.Color(image_rgb)
    image_rgb = enhancer.enhance(saturation)
    
    # Augmenter la netteté
    enhancer = ImageEnhance.Sharpness(image_rgb)
    image_rgb = enhancer.enhance(sharpness)
    
    return image_rgb


def quantize_image_rgb_adaptive(image_rgb, bits, dither=False):
    """Quantification adaptative qui préserve mieux les couleurs."""
    if bits >= 8:
        return image_rgb

    # Approche 1: Utiliser PIL pour générer une palette optimale
    # Convertir en palette avec plus de couleurs d'abord pour analyse
    colors = 2 ** (bits * 3)  # Nombre total de couleurs possibles
    if colors > 256:
        colors = 256
    
    # Quantifier avec palette adaptative
    img_p = image_rgb.quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG if dither else Image.NONE)
    img_quantized = img_p.convert('RGB')
    
    # Maintenant appliquer la quantification par canal pour correspondre au hardware
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


def quantize_image_rgb(image_rgb, bits, dither=False):
    if bits >= 8:
        return image_rgb

    width, height = image_rgb.size
    if not dither:
        out = Image.new("RGB", (width, height))
        for y in range(height):
            for x in range(width):
                r, g, b = image_rgb.getpixel((x, y))
                out.putpixel(
                    (x, y),
                    (
                        quantize_channel(r, bits),
                        quantize_channel(g, bits),
                        quantize_channel(b, bits),
                    ),
                )
        return out

    # Floyd-Steinberg dithering per channel (small images, pure Python is ok).
    buf = [
        [list(image_rgb.getpixel((x, y))) for x in range(width)]
        for y in range(height)
    ]
    for y in range(height):
        for x in range(width):
            r, g, b = buf[y][x]
            qr = quantize_channel(r, bits)
            qg = quantize_channel(g, bits)
            qb = quantize_channel(b, bits)
            err = (r - qr, g - qg, b - qb)
            buf[y][x] = [qr, qg, qb]

            def add_error(nx, ny, factor):
                if 0 <= nx < width and 0 <= ny < height:
                    for c in range(3):
                        buf[ny][nx][c] = max(
                            0,
                            min(255, buf[ny][nx][c] + err[c] * factor),
                        )

            add_error(x + 1, y, 7 / 16)
            add_error(x - 1, y + 1, 3 / 16)
            add_error(x, y + 1, 5 / 16)
            add_error(x + 1, y + 1, 1 / 16)

    out = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            r, g, b = buf[y][x]
            out.putpixel((x, y), (int(r), int(g), int(b)))
    return out


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


def svg_to_png_bytes(svg_bytes, size):
    try:
        import resvg_py

        svg_string = svg_bytes.decode("utf-8", errors="replace")
        return resvg_py.svg_to_bytes(svg_string=svg_string)
    except Exception as exc:
        raise RuntimeError(f"SVG render failed: {exc}") from exc


def fetch_logo_image(logo_url):
    clean_url = normalize_logo_url(logo_url)
    svg_bytes = fetch_bytes(clean_url)
    png_bytes = svg_to_png_bytes(svg_bytes, LOGO_WIDTH)
    return Image.open(BytesIO(png_bytes)).convert("RGBA")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--date", default=None, help="YYYY-MM-DD")
    parser.add_argument("--out", default="out", help="Output folder")
    parser.add_argument(
        "--depth",
        type=int,
        default=0,
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
        default=1.3,
        help="Contrast enhancement factor (default: 1.3)",
    )
    parser.add_argument(
        "--saturation",
        type=float,
        default=1.4,
        help="Saturation enhancement factor (default: 1.4)",
    )
    parser.add_argument(
        "--sharpness",
        type=float,
        default=1.2,
        help="Sharpness enhancement factor (default: 1.2)",
    )
    args = parser.parse_args()

    date = args.date
    if not date:
        date = dt.date.today().isoformat()

    out_dir = args.out
    os.makedirs(out_dir, exist_ok=True)

    standings_url = STANDINGS_URL_FMT.format(date=date)
    data = fetch_json(standings_url)
    standings = data.get("standings", [])

    manifest = []

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
            img_rgba = img_rgba.resize((LOGO_WIDTH, LOGO_HEIGHT), Image.LANCZOS)
            img_rgb = rgba_to_rgb(img_rgba)
            
            # Améliorer l'image AVANT quantization
            img_rgb = enhance_image_for_low_depth(
                img_rgb, 
                args.contrast, 
                args.saturation, 
                args.sharpness
            )
            
            if args.depth and args.depth > 0:
                img_rgb = quantize_image_rgb_adaptive(img_rgb, args.depth, args.dither)
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
                "width": LOGO_WIDTH,
                "height": LOGO_HEIGHT,
            }
        )

        print(f"[ok] {abbrev} -> {rgb565_path}")

    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(f"[done] {len(manifest)} logos")

    # Copier automatiquement vers data/logos
    data_logos_dir = os.path.join("..", "..", "data", "logos")
    if os.path.isabs(out_dir):
        # Si out_dir est absolu, calculer le chemin absolu vers data/logos
        script_dir = os.path.dirname(os.path.abspath(__file__))
        data_logos_dir = os.path.join(script_dir, "..", "..", "data", "logos")
    
    data_logos_dir = os.path.normpath(data_logos_dir)
    
    # Supprimer et recréer le dossier data/logos
    if os.path.exists(data_logos_dir):
        shutil.rmtree(data_logos_dir)
        print(f"[clean] Removed {data_logos_dir}")
    
    os.makedirs(data_logos_dir, exist_ok=True)
    print(f"[deploy] Created {data_logos_dir}")
    
    # Copier tous les fichiers .rgb565
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