import argparse
import os
import sys
from PIL import Image, ImageEnhance


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

    # Approche: Utiliser PIL pour générer une palette optimale
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
    parser = argparse.ArgumentParser(description="Convertir une image en RGB565 pour le scoreboard")
    parser.add_argument("input", help="Fichier image d'entrée (PNG, JPG, etc.)")
    parser.add_argument("--output", "-o", help="Fichier de sortie .rgb565 (défaut: même nom que l'entrée)")
    parser.add_argument("--width", "-w", type=int, default=25, help="Largeur cible (défaut: 25)")
    parser.add_argument("--height", "-H", type=int, default=20, help="Hauteur cible (défaut: 20)")
    parser.add_argument("--no-resize", action="store_true", help="Ne pas redimensionner l'image")
    parser.add_argument("--depth", type=int, default=4, help="Profondeur de couleur en bits par canal (défaut: 4)")
    parser.add_argument("--dither", action="store_true", help="Activer le dithering")
    parser.add_argument("--contrast", type=float, default=1.3, help="Contraste (défaut: 1.3)")
    parser.add_argument("--saturation", type=float, default=1.4, help="Saturation (défaut: 1.4)")
    parser.add_argument("--sharpness", type=float, default=1.2, help="Netteté (défaut: 1.2)")
    parser.add_argument("--no-enhance", action="store_true", help="Désactiver l'amélioration d'image")
    
    args = parser.parse_args()
    
    if not os.path.exists(args.input):
        print(f"Erreur: fichier '{args.input}' introuvable")
        return 1
    
    # Déterminer le nom de sortie
    if args.output:
        output_rgb565 = args.output
    else:
        base = os.path.splitext(args.input)[0]
        output_rgb565 = f"{base}.rgb565"
    
    output_png = os.path.splitext(output_rgb565)[0] + "_preview.png"
    
    print(f"[load] {args.input}")
    img = Image.open(args.input)
    
    # Convertir en RGB (au cas où c'est RGBA, P, etc.)
    if img.mode == "RGBA":
        # Fond noir pour les transparences
        background = Image.new("RGB", img.size, (0, 0, 0))
        background.paste(img, mask=img.split()[3])
        img = background
    elif img.mode != "RGB":
        img = img.convert("RGB")
    
    # Redimensionner si demandé
    if not args.no_resize:
        original_size = img.size
        img = img.resize((args.width, args.height), Image.LANCZOS)
        print(f"[resize] {original_size[0]}x{original_size[1]} -> {args.width}x{args.height}")
    
    # Améliorer l'image
    if not args.no_enhance:
        img = enhance_image_for_low_depth(img, args.contrast, args.saturation, args.sharpness)
        print(f"[enhance] contrast={args.contrast}, saturation={args.saturation}, sharpness={args.sharpness}")
    
    # Quantizer
    if args.depth > 0 and args.depth < 8:
        img = quantize_image_rgb_adaptive(img, args.depth, args.dither)
        print(f"[quantize] {args.depth}-bit per channel, dither={'ON' if args.dither else 'OFF'}")
    
    # Écrire RGB565
    write_rgb565(output_rgb565, img)
    print(f"[save] {output_rgb565}")
    
    # Sauvegarder preview PNG
    img.save(output_png, format="PNG")
    print(f"[save] {output_png} (preview)")
    
    print(f"[done] {img.width}x{img.height} -> {output_rgb565}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
