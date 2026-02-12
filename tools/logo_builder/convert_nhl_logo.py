"""Convert the local NHL shield logo to 30x30 RGB565 for the scoreboard."""

import os
import sys

from PIL import Image, ImageEnhance


SIZE = 30


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
    img_p = image_rgb.quantize(
        colors=colors, method=Image.MEDIANCUT,
        dither=Image.FLOYDSTEINBERG if dither else Image.NONE,
    )
    img_quantized = img_p.convert("RGB")
    width, height = img_quantized.size
    out = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            r, g, b = img_quantized.getpixel((x, y))
            out.putpixel((x, y), (
                quantize_channel(r, bits),
                quantize_channel(g, bits),
                quantize_channel(b, bits),
            ))
    return out


def rgb_to_rgb565_le(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def write_rgb565(path, image_rgb):
    with open(path, "wb") as f:
        for y in range(image_rgb.height):
            for x in range(image_rgb.width):
                r, g, b = image_rgb.getpixel((x, y))
                f.write(rgb_to_rgb565_le(r, g, b))


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    input_path = os.path.join(script_dir, "nhlLogo.png")

    if not os.path.exists(input_path):
        print(f"[error] {input_path} not found")
        return 1

    print(f"[load] {input_path}")
    img = Image.open(input_path).convert("RGBA")
    # Composite onto black background
    background = Image.new("RGB", img.size, (0, 0, 0))
    background.paste(img, mask=img.split()[3])
    img_rgb = background

    img_rgb = img_rgb.resize((SIZE, SIZE), Image.LANCZOS)
    print(f"[resize] -> {SIZE}x{SIZE}")

    img_rgb = enhance_image_for_low_depth(img_rgb)
    img_rgb = quantize_image_rgb_adaptive(img_rgb, 4)
    print("[quantize] 4-bit per channel")

    data_dir = os.path.normpath(os.path.join(script_dir, "..", "..", "data", "logos"))
    os.makedirs(data_dir, exist_ok=True)

    rgb565_path = os.path.join(data_dir, "nhl_logo.rgb565")
    write_rgb565(rgb565_path, img_rgb)
    print(f"[save] {rgb565_path}")

    png_path = os.path.join(data_dir, "nhl_logo.png")
    img_rgb.save(png_path, format="PNG")
    print(f"[save] {png_path} (preview)")

    print(f"[done] {SIZE}x{SIZE} -> {rgb565_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
