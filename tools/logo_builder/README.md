# NHL Logo Builder (Hub75 20x20)

This tool downloads NHL team logos (dark variants), rasterizes them to 20x20,
and exports RGB565 binary files for a 32x64 Hub75 panel.

## Output format
- `*.rgb565`: raw RGB565, little-endian, row-major, 20x20 pixels.
- `*.png`: preview image for quick inspection.
- `manifest.json`: mapping of team abbrev to files.

## Setup
Create a virtual env and install dependencies:

```
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

## Usage
```
python build_logos.py --date 2026-01-26 --out out
```

If `--date` is omitted, it defaults to today's date.

### Defaults
By default, the script uses:
- `--depth 0` (no quantization)
- `--dither` disabled

If you want to override, pass flags explicitly.

## Notes
- Logos are fetched from the standings endpoint:
  `https://api-web.nhle.com/v1/standings/{date}`
- `teamLogo` URLs are converted from `_light.svg` to `_dark.svg`.
- Logos are fetched as SVG and rasterized with resvg-py.
