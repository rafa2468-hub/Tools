"""One-shot script to generate PWA icons. Re-run if you tweak the design."""
from PIL import Image, ImageDraw
import os

OUT_DIR = os.path.join(os.path.dirname(__file__), 'icons')
os.makedirs(OUT_DIR, exist_ok=True)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render_icon(size, maskable=False, filename=None):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Maskable icons need a safe area: content within central 80%.
    pad = int(size * 0.1) if maskable else 0
    inner = size - 2 * pad

    # Rounded-square background (full bleed when maskable so the OS can mask it)
    if maskable:
        bg_box = [0, 0, size, size]
        radius = 0
    else:
        bg_box = [pad, pad, size - pad, size - pad]
        radius = int(size * 0.22)

    # Diagonal gradient fill (purple -> cyan), drawn line by line
    top_color = (124, 92, 255)
    bot_color = (74, 214, 255)
    bg_layer = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    bg_draw = ImageDraw.Draw(bg_layer)
    for y in range(bg_box[1], bg_box[3]):
        t = (y - bg_box[1]) / max(1, bg_box[3] - bg_box[1])
        color = lerp(top_color, bot_color, t)
        bg_draw.line([(bg_box[0], y), (bg_box[2], y)], fill=color + (255,))

    # Apply rounded mask
    if radius > 0:
        mask = Image.new('L', (size, size), 0)
        ImageDraw.Draw(mask).rounded_rectangle(bg_box, radius=radius, fill=255)
        img.paste(bg_layer, (0, 0), mask)
    else:
        img.paste(bg_layer, (0, 0))

    # Music note glyph, white. Drawn as: vertical stem, flag bar, two filled note heads.
    cx, cy = size // 2, size // 2
    glyph_size = int(inner * 0.62)
    stem_h = int(glyph_size * 0.85)
    head_r = int(glyph_size * 0.18)
    head_offset = int(glyph_size * 0.32)

    # Stems
    stem_top_y = cy - stem_h // 2
    stem_bot_y = cy + stem_h // 2
    stem_w = max(2, int(glyph_size * 0.08))
    left_stem_x = cx - head_offset
    right_stem_x = cx + head_offset

    draw_mask = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(draw_mask)

    # Two parallel stems
    d.rectangle(
        [left_stem_x - stem_w // 2, stem_top_y, left_stem_x + stem_w // 2, stem_bot_y],
        fill=(255, 255, 255, 255),
    )
    d.rectangle(
        [right_stem_x - stem_w // 2, stem_top_y, right_stem_x + stem_w // 2, stem_bot_y],
        fill=(255, 255, 255, 255),
    )
    # Connecting flag (top bar) - slanted via two rectangles approximating
    flag_h = int(glyph_size * 0.16)
    d.polygon(
        [
            (left_stem_x - stem_w // 2, stem_top_y),
            (right_stem_x + stem_w // 2, stem_top_y - int(flag_h * 0.4)),
            (right_stem_x + stem_w // 2, stem_top_y - int(flag_h * 0.4) + flag_h),
            (left_stem_x - stem_w // 2, stem_top_y + flag_h),
        ],
        fill=(255, 255, 255, 255),
    )
    # Note heads (filled ellipses)
    d.ellipse(
        [left_stem_x - head_r - int(stem_w * 0.5), stem_bot_y - head_r,
         left_stem_x + head_r - int(stem_w * 0.5), stem_bot_y + head_r],
        fill=(255, 255, 255, 255),
    )
    d.ellipse(
        [right_stem_x - head_r - int(stem_w * 0.5), stem_bot_y - head_r,
         right_stem_x + head_r - int(stem_w * 0.5), stem_bot_y + head_r],
        fill=(255, 255, 255, 255),
    )

    img.alpha_composite(draw_mask)

    out_path = os.path.join(OUT_DIR, filename)
    img.save(out_path, 'PNG')
    print(f'wrote {out_path}')


if __name__ == '__main__':
    render_icon(192, maskable=False, filename='icon-192.png')
    render_icon(512, maskable=False, filename='icon-512.png')
    render_icon(192, maskable=True, filename='icon-192-maskable.png')
    render_icon(512, maskable=True, filename='icon-512-maskable.png')
    render_icon(180, maskable=False, filename='apple-touch-icon.png')
    render_icon(32, maskable=False, filename='favicon-32.png')
