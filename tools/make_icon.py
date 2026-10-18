#!/usr/bin/env python3
"""Make the PS5 app icon (sce_sys/icon0.png, 512x512) from a cover image.

The cover is usually portrait. It is drawn whole, centred at full height, over a blurred and
darkened copy of itself that fills the square, so nothing is cropped or stretched.

Usage: make_icon.py COVER OUT.png
"""
import sys

from PIL import Image, ImageEnhance, ImageFilter

SIZE = 512


def main():
    cover = Image.open(sys.argv[1]).convert('RGB')
    w, h = cover.size
    # Background: cover scaled to fill the square, blurred and dimmed.
    scale = max(SIZE / w, SIZE / h)
    back = cover.resize((max(SIZE, round(w * scale)), max(SIZE, round(h * scale))), Image.LANCZOS)
    left, top = (back.width - SIZE) // 2, (back.height - SIZE) // 2
    back = back.crop((left, top, left + SIZE, top + SIZE)).filter(ImageFilter.GaussianBlur(18))
    back = ImageEnhance.Brightness(back).enhance(0.45)
    # Foreground: the whole cover, fitted inside the square.
    fit = min(SIZE / w, SIZE / h)
    front = cover.resize((round(w * fit), round(h * fit)), Image.LANCZOS)
    back.paste(front, ((SIZE - front.width) // 2, (SIZE - front.height) // 2))
    back.save(sys.argv[2], optimize=True)
    print(f'{sys.argv[2]}: {SIZE}x{SIZE} from {w}x{h}')


if __name__ == '__main__':
    main()
