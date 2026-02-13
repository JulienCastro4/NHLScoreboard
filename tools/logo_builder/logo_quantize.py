"""
Perceptual logo quantization for HUB75 4-bit display.

Simulates the full hardware chain: RGB888 -> RGB565 -> CIE 1931 LUT -> 4-bit output.
Matches colors in CIELAB perceptual space for optimal visual results.
"""

import numpy as np
from PIL import Image, ImageEnhance
from scipy.spatial import KDTree
from skimage.color import rgb2lab


# ============================================================================
# CIE 1931 LUT (same formula as ESP32 hardware: generate_cie_luts.py)
# ============================================================================

def _generate_cie_lut(max_out):
    """Generate CIE 1931 lightness LUT mapping 8-bit input to N-bit output."""
    lut = []
    for i in range(256):
        L = i * 100.0 / 255.0
        if L <= 8:
            Y = L / 902.3
        else:
            Y = ((L + 16.0) / 116.0) ** 3
        lut.append(min(max_out, int(round(Y * max_out))))
    return lut


CIE_LUT_4BIT = _generate_cie_lut(15)  # 4-bit: 0-15


# ============================================================================
# Hardware palette builder
# ============================================================================

def _linear_to_srgb(linear):
    """Convert linear light values to sRGB gamma-corrected values."""
    return np.where(
        linear <= 0.0031308,
        linear * 12.92,
        1.055 * np.power(np.maximum(linear, 1e-10), 1.0 / 2.4) - 0.055
    )


def _build_hardware_palette():
    """Build palette of all achievable colors through the RGB565 -> CIE chain.

    Enumerates all 65536 RGB565 values, simulates the ESP32 color565to888
    reconstruction and CIE LUT application, then deduplicates by CIE output.

    Returns:
        palette_input: (N, 3) uint8 - RGB888 input values to write to file
        palette_lab:   (N, 3) float - CIELAB coordinates of perceived output
        tree:          KDTree in CIELAB space for nearest-neighbor queries
    """
    seen = {}  # (cie_r, cie_g, cie_b) -> (r8, g8, b8) representative input

    for r5 in range(32):
        for g6 in range(64):
            for b5 in range(32):
                # Simulate ESP32 color565to888 reconstruction
                r8 = (r5 << 3) | (r5 >> 2)
                g8 = (g6 << 2) | (g6 >> 4)
                b8 = (b5 << 3) | (b5 >> 2)

                # Apply CIE LUT per channel
                key = (CIE_LUT_4BIT[r8], CIE_LUT_4BIT[g8], CIE_LUT_4BIT[b8])

                if key not in seen:
                    seen[key] = (r8, g8, b8)

    # Sort by key for deterministic order
    keys = sorted(seen.keys())
    palette_input = np.array([seen[k] for k in keys], dtype=np.uint8)

    # Convert CIE output levels to perceived color in CIELAB
    # CIE output is LINEAR brightness (duty cycle): level / 15
    cie_linear = np.array(keys, dtype=np.float64) / 15.0

    # Linear -> sRGB (for rgb2lab which expects sRGB input)
    srgb = np.clip(_linear_to_srgb(cie_linear), 0, 1)

    # Convert to CIELAB (rgb2lab expects (H, W, 3) sRGB image in [0, 1])
    lab = rgb2lab(srgb.reshape(1, -1, 3)).reshape(-1, 3)

    tree = KDTree(lab)
    return palette_input, lab, tree


# Lazy-loaded module-level palette
_palette_cache = None


def _get_palette():
    global _palette_cache
    if _palette_cache is None:
        _palette_cache = _build_hardware_palette()
    return _palette_cache


# ============================================================================
# Quantization
# ============================================================================

def quantize_perceptual(img_rgb):
    """Quantize an RGB image to the hardware palette using CIELAB matching.

    For each pixel, finds the closest achievable hardware color in CIELAB
    perceptual space. Near-black pixels are forced to pure black, near-white
    to pure white.

    Args:
        img_rgb: PIL Image in RGB mode

    Returns:
        PIL Image in RGB mode, quantized to hardware palette
    """
    palette_input, palette_lab, tree = _get_palette()

    arr = np.array(img_rgb, dtype=np.float64) / 255.0
    h, w = arr.shape[:2]

    # Convert to CIELAB
    lab = rgb2lab(arr)
    pixels_lab = lab.reshape(-1, 3)

    # Find nearest palette color for all pixels (vectorized)
    _, indices = tree.query(pixels_lab)
    result = palette_input[indices].reshape(h, w, 3).copy()

    # Force near-black to pure black
    L = pixels_lab[:, 0]
    near_black = L < 5
    result_flat = result.reshape(-1, 3)
    result_flat[near_black] = [0, 0, 0]

    # Force near-white to pure white
    a = pixels_lab[:, 1]
    b = pixels_lab[:, 2]
    near_white = (L > 95) & (np.abs(a) < 5) & (np.abs(b) < 5)
    result_flat[near_white] = [255, 255, 255]

    return Image.fromarray(result.astype(np.uint8), "RGB")


def quantize_perceptual_dithered(img_rgb, strength=0.4):
    """Floyd-Steinberg dithering in CIELAB space with reduced strength.

    Not recommended at 20x20 as dithering patterns are very visible.
    May help logos with subtle gradients.

    Args:
        img_rgb: PIL Image in RGB mode
        strength: Error diffusion strength (0-1). Default 0.4.

    Returns:
        PIL Image in RGB mode, dithered and quantized
    """
    palette_input, palette_lab, tree = _get_palette()

    arr = np.array(img_rgb, dtype=np.float64) / 255.0
    lab = rgb2lab(arr)
    h, w = lab.shape[:2]

    result = np.zeros((h, w, 3), dtype=np.uint8)

    for y in range(h):
        for x in range(w):
            pixel_lab = lab[y, x].copy()
            L = pixel_lab[0]

            # Near-black
            if L < 5:
                result[y, x] = [0, 0, 0]
                continue
            # Near-white
            if L > 95 and abs(pixel_lab[1]) < 5 and abs(pixel_lab[2]) < 5:
                result[y, x] = [255, 255, 255]
                continue

            # Find nearest palette color
            _, idx = tree.query(pixel_lab)
            result[y, x] = palette_input[idx]

            # Compute and diffuse error in CIELAB space
            error = (pixel_lab - palette_lab[idx]) * strength
            if x + 1 < w:
                lab[y, x + 1] += error * (7.0 / 16.0)
            if y + 1 < h:
                if x > 0:
                    lab[y + 1, x - 1] += error * (3.0 / 16.0)
                lab[y + 1, x] += error * (5.0 / 16.0)
                if x + 1 < w:
                    lab[y + 1, x + 1] += error * (1.0 / 16.0)

    return Image.fromarray(result, "RGB")


# ============================================================================
# Main entry point
# ============================================================================

def rgba_to_rgb(image):
    """Convert RGBA to RGB with black background."""
    background = Image.new("RGB", image.size, (0, 0, 0))
    if image.mode == "RGBA":
        background.paste(image, mask=image.split()[3])
    else:
        background.paste(image)
    return background


# ============================================================================
# Per-team color corrections
# ============================================================================

# "red"         = cast any pink/orange artifact to pure red
# "orange"      = cast pink to orange (kill B only, keep G)
# "green_boost" = boost dark green pixels so they don't vanish to black
TEAM_COLOR_FIXES = {
    # International
    "SVK": "red", "LAT": "red", "ITA": "red", "DEN": "red",
    "SUI": "red", "FRA": "red", "CAN": "red", "USA": "red", "CZE": "red",
    # NHL - red
    "NJD": "red", "WSH": "red", "SEA": "red", "WPG": "red",
    "CBJ": "red", "MTL": "red",
    "FLA": "red", "CAR": "red", "NYR": "red", "MIN": "red",
    # NHL - orange
    "PHI": "orange", "NYI": "orange", "EDM": "orange",
    # NHL - green boost
    "DAL": "green_boost",
}


def _apply_pre_quant_fix(img_rgb, team_abbrev):
    """Pre-quantization fix applied on the downscaled image BEFORE palette matching."""
    rule = TEAM_COLOR_FIXES.get(team_abbrev)
    if not rule:
        return img_rgb
    # No pre-quant fixes currently active
    return img_rgb


def _apply_post_quant_fix(img_rgb, team_abbrev):
    """Post-quantization fix applied on the quantized image."""
    rule = TEAM_COLOR_FIXES.get(team_abbrev)
    if not rule:
        return img_rgb

    arr = np.array(img_rgb, dtype=np.uint8)
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)

    # White boundary: if min(R,G,B) >= 150, the pixel is close to white.
    # Force these to pure white instead of casting to team color.
    min_ch = np.minimum(np.minimum(r, g), b)
    near_white = min_ch >= 150

    if rule == "red":
        # R-dominant warm pixels -> pure red. Protects white and black.
        warm = (r >= 70) & ((r - g) > 15) & ((r - b) > 15)
        # Cast to red only if NOT near-white
        mask = warm & ~near_white
        arr[:, :, 1][mask] = 0  # kill G
        arr[:, :, 2][mask] = 0  # kill B
        # Force near-white warm pixels to pure white
        white_mask = warm & near_white
        arr[:, :, 0][white_mask] = 255
        arr[:, :, 1][white_mask] = 255
        arr[:, :, 2][white_mask] = 255

    elif rule == "orange":
        # Only kill blue (pink -> orange/red), keep green for orange tones
        pink = (r >= 80) & (b >= 40) & (r > b)
        # Cast to orange only if NOT near-white
        mask = pink & ~near_white
        arr[:, :, 2][mask] = 0  # kill B only
        # Force near-white pink pixels to pure white
        white_mask = pink & near_white
        arr[:, :, 0][white_mask] = 255
        arr[:, :, 1][white_mask] = 255
        arr[:, :, 2][white_mask] = 255

    elif rule == "green_boost":
        # DAL: green pixels are too dim after CIE LUT on hardware.
        # Use the exact same palette green that ITA (Italy flag) gets — that one works.
        # Quantize ITA flag green (0,146,70) through the hardware palette to find it.
        palette_input, palette_lab, tree = _get_palette()
        ref_green = np.array([[[0, 146, 70]]], dtype=np.float64) / 255.0
        ref_lab = rgb2lab(ref_green).reshape(3)
        _, idx = tree.query(ref_lab)
        ita_green = palette_input[idx]  # exact RGB that ITA's green maps to

        not_black = (r + g + b) > 15
        is_green = (g >= r) & (g >= b) & not_black & ~near_white
        arr[:, :, 0][is_green] = ita_green[0]
        arr[:, :, 1][is_green] = ita_green[1]
        arr[:, :, 2][is_green] = ita_green[2]

    return Image.fromarray(arr, "RGB")


# ============================================================================
# Main pipeline
# ============================================================================

def process_logo_perceptual(img_rgba, target_size=20,
                            contrast=1.2, saturation=1.3, sharpness=1.1,
                            dither=False, team_abbrev=None):
    """Full pipeline: RGBA logo -> quantized RGB for 4-bit HUB75 display.

    Args:
        img_rgba: RGBA PIL Image (high resolution from SVG rasterization)
        target_size: Output size in pixels (default 20)
        contrast: Contrast enhancement (default 1.2)
        saturation: Color saturation enhancement (default 1.3)
        sharpness: Sharpness enhancement (default 1.1)
        dither: Enable soft Floyd-Steinberg dithering (default False)
        team_abbrev: Team abbreviation for per-team color fixes (optional)

    Returns:
        RGB PIL Image, target_size x target_size, quantized to hardware palette
    """
    # 1. Convert to RGB and enhance at high resolution
    img_rgb = rgba_to_rgb(img_rgba)

    img_rgb = ImageEnhance.Contrast(img_rgb).enhance(contrast)
    img_rgb = ImageEnhance.Color(img_rgb).enhance(saturation)
    img_rgb = ImageEnhance.Sharpness(img_rgb).enhance(sharpness)

    # 2. Downscale with BOX filter (area-averaging, no color bleeding)
    img_small = img_rgb.resize((target_size, target_size), Image.BOX)

    # 3. Pre-quantization team fix (e.g. DAL green boost)
    if team_abbrev:
        img_small = _apply_pre_quant_fix(img_small, team_abbrev)

    # 4. Perceptual quantization to hardware palette
    if dither:
        img_result = quantize_perceptual_dithered(img_small)
    else:
        img_result = quantize_perceptual(img_small)

    # 5. Post-quantization team fix (e.g. pink->red)
    if team_abbrev:
        img_result = _apply_post_quant_fix(img_result, team_abbrev)

    return img_result
