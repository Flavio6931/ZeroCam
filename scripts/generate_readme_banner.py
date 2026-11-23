"""Genera el banner oficial de ZeroCam para README.md (`docs/banner.png`).

Reutiliza la geometría de Marca v4 (`generate_brand_icons.py`) y los tokens
oscuros de la interfaz (`#0F1117`, `#171A21`, `#1F242E`, `#262C38`, `#2E3545`)
con renderizado 2x y reducción LANCZOS a 1600x460 px.

Uso:  python scripts/generate_readme_banner.py
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from generate_brand_icons import SIGNAL, TILE_RADIUS, render_tile  # noqa: E402

ROOT = SCRIPTS_DIR.parent
OUTPUT_PATH = ROOT / "docs" / "banner.png"
FONTS_DIR = Path(r"C:\Windows\Fonts")

SCALE = 2
WIDTH = 1600 * SCALE
HEIGHT = 460 * SCALE

BG_OUTER = (15, 17, 23, 255)      # #0F1117
BG_CARD = (23, 26, 33, 255)       # #171A21
BG_PILL = (31, 36, 46, 255)       # #1F242E
BORDER_CARD = (38, 44, 56, 255)   # #262C38
BORDER_SUBTLE = (46, 53, 69, 255) # #2E3545
TEXT_TITLE = (243, 241, 236, 255) # #F3F1EC
TEXT_SUB = (160, 170, 191, 255)   # #A0AABF
TEXT_TAG = (226, 232, 240, 255)   # #E2E8F0
TEXT_MONO = (148, 163, 184, 255)  # #94A3B8
TEXT_DIM = (100, 116, 139, 255)   # #64748B


def load_font(filename: str, size: int) -> ImageFont.FreeTypeFont:
    """Carga una fuente TTF de Windows y falla rápido si no existe."""
    font_path = FONTS_DIR / filename
    if not font_path.is_file():
        raise FileNotFoundError(f"Fuente requerida no encontrada: {font_path}")
    return ImageFont.truetype(str(font_path), size)


def make_radial_glow(
    size: tuple[int, int],
    center: tuple[int, int],
    radius: int,
    rgb: tuple[int, int, int],
    max_alpha: int,
) -> Image.Image:
    """Crea un resplandor radial continuo usando gradiente nativo sin escalones."""
    # radial_gradient("L") va de 0 en el centro a 255 en los bordes (256x256).
    ramp = Image.radial_gradient("L").point(
        lambda v: int(round(max_alpha * ((1.0 - min(v, 180) / 180.0) ** 2.2)))
    )
    diameter = radius * 2
    alpha_patch = ramp.resize((diameter, diameter), Image.Resampling.BICUBIC)
    glow_patch = Image.new("RGBA", (diameter, diameter), (rgb[0], rgb[1], rgb[2], 255))
    glow_patch.putalpha(alpha_patch)

    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    layer.paste(glow_patch, (center[0] - radius, center[1] - radius))
    return layer.filter(ImageFilter.GaussianBlur(radius=16))


def draw_dot_grid(card_mask: Image.Image, card_box: tuple[int, int, int, int]) -> Image.Image:
    """Dibuja una cuadrícula discreta de puntos dentro de los límites de la tarjeta."""
    grid = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(grid)
    x0, y0, x1, y1 = card_box
    step = 64
    dot_r = 2
    color = (BORDER_SUBTLE[0], BORDER_SUBTLE[1], BORDER_SUBTLE[2], 95)
    for gx in range(x0 + 56, x1 - 48, step):
        for gy in range(y0 + 56, y1 - 48, step):
            draw.ellipse((gx - dot_r, gy - dot_r, gx + dot_r, gy + dot_r), fill=color)
    grid.putalpha(Image.composite(grid.getchannel("A"), Image.new("L", (WIDTH, HEIGHT), 0), card_mask))
    return grid


def hex_to_rgb(hex_color: str) -> tuple[int, int, int]:
    """Convierte un color hexadecimal #RRGGBB a tupla RGB."""
    raw = hex_color.lstrip("#")
    return (int(raw[0:2], 16), int(raw[2:4], 16), int(raw[4:6], 16))


def render_banner() -> Image.Image:
    """Renderiza el banner a resolución 2x y lo reduce a 1600x460 px con LANCZOS."""
    canvas = Image.new("RGBA", (WIDTH, HEIGHT), BG_OUTER)

    card_box = (40, 40, WIDTH - 40, HEIGHT - 40)
    card_radius = 40

    card_mask = Image.new("L", (WIDTH, HEIGHT), 0)
    ImageDraw.Draw(card_mask).rounded_rectangle(card_box, radius=card_radius, fill=255)

    card_layer = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    ImageDraw.Draw(card_layer).rounded_rectangle(card_box, radius=card_radius, fill=BG_CARD)

    # Resplandores ambientales dentro de la tarjeta (izquierda señal naranja, derecha azul)
    glow_signal = make_radial_glow((WIDTH, HEIGHT), (560, HEIGHT // 2), 620, hex_to_rgb(SIGNAL), 36)
    glow_blue = make_radial_glow((WIDTH, HEIGHT), (2740, 720), 680, (45, 125, 255), 34)
    glows = Image.alpha_composite(glow_signal, glow_blue)
    glows.putalpha(Image.composite(glows.getchannel("A"), Image.new("L", (WIDTH, HEIGHT), 0), card_mask))

    dots = draw_dot_grid(card_mask, card_box)

    canvas = Image.alpha_composite(canvas, card_layer)
    canvas = Image.alpha_composite(canvas, glows)
    canvas = Image.alpha_composite(canvas, dots)

    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle(card_box, radius=card_radius, outline=BORDER_CARD, width=3)

    # Ícono oficial Marca v4 a la izquierda (220 px finales -> 440 px en 2x)
    icon_size = 440
    icon_x = 320
    icon_y = (HEIGHT - icon_size) // 2
    icon_radius = TILE_RADIUS * (icon_size / 128.0)

    shadow = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (icon_x - 4, icon_y + 12, icon_x + icon_size + 4, icon_y + icon_size + 20),
        radius=icon_radius + 4,
        fill=(0, 0, 0, 125),
    )
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius=24))
    canvas = Image.alpha_composite(canvas, shadow)

    tile = render_tile(icon_size)
    canvas.paste(tile, (icon_x, icon_y), tile)

    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle(
        (icon_x, icon_y, icon_x + icon_size - 1, icon_y + icon_size - 1),
        radius=icon_radius,
        outline=BORDER_SUBTLE,
        width=3,
    )

    # Tipografías Windows
    font_title = load_font("segoeuib.ttf", 174)
    font_sub = load_font("segoeui.ttf", 46)
    font_pill = load_font("seguisb.ttf", 31)
    font_mono = load_font("CascadiaMono.ttf", 30)
    font_corner = load_font("CascadiaMono.ttf", 27)

    content_x = 880
    right_limit = WIDTH - 130

    # Detalle técnico en esquina superior derecha
    corner_text = "1080p \u00b7 60 FPS \u00b7 C++20 / Kotlin"
    c_box = draw.textbbox((0, 0), corner_text, font=font_corner)
    c_w = c_box[2] - c_box[0]
    draw.text((right_limit - c_w, 112), corner_text, font=font_corner, fill=TEXT_DIM)

    # Etiqueta superior mono con indicador naranja Signal
    kicker_y = 202
    kicker_text = "ZERO-COPY VIRTUAL CAMERA"
    k_box = draw.textbbox((0, 0), kicker_text, font=font_mono)
    k_h = k_box[3] - k_box[1]
    dot_r = 9
    dot_cy = kicker_y + k_box[1] + k_h // 2
    draw.ellipse((content_x, dot_cy - dot_r, content_x + dot_r * 2, dot_cy + dot_r), fill=SIGNAL)
    draw.text((content_x + dot_r * 2 + 18, kicker_y), kicker_text, font=font_mono, fill=TEXT_MONO)

    # Wordmark principal
    title_y = 246
    draw.text((content_x - 6, title_y), "ZeroCam", font=font_title, fill=TEXT_TITLE)

    # Subtítulo
    sub_y = 470
    sub_text = "C\u00e1mara virtual H.264 de ultra baja latencia para Windows 10/11 y Android"
    s_box = draw.textbbox((content_x, sub_y), sub_text, font=font_sub)
    if s_box[2] > right_limit:
        raise RuntimeError(f"Subtítulo excede el límite derecho: {s_box[2]} > {right_limit}")
    draw.text((content_x, sub_y), sub_text, font=font_sub, fill=TEXT_SUB)

    # 4 cápsulas inferiores con los pilares de arquitectura
    pills = [
        ("H.264 Zero-Copy", (45, 125, 255, 255)),
        ("Wire v2 AES-256-GCM", (34, 197, 94, 255)),
        ("USB ADB + Wi-Fi", hex_to_rgb(SIGNAL) + (255,)),
        ("DirectShow + Win11 MF", (96, 165, 250, 255)),
    ]

    pill_x = content_x
    pill_y = 588
    pill_h = 74
    pill_radius = pill_h // 2
    pill_gap = 22

    for label, dot_color in pills:
        t_box = draw.textbbox((0, 0), label, font=font_pill)
        t_w = t_box[2] - t_box[0]
        t_h = t_box[3] - t_box[1]
        pill_w = 28 + 14 + 16 + t_w + 30
        if pill_x + pill_w > right_limit:
            raise RuntimeError(f"Cápsula '{label}' excede el límite derecho: {pill_x + pill_w} > {right_limit}")

        draw.rounded_rectangle(
            (pill_x, pill_y, pill_x + pill_w, pill_y + pill_h),
            radius=pill_radius,
            fill=BG_PILL,
            outline=BORDER_SUBTLE,
            width=2,
        )
        p_dot_r = 7
        p_dot_cx = pill_x + 28 + p_dot_r
        p_dot_cy = pill_y + pill_h // 2
        draw.ellipse(
            (p_dot_cx - p_dot_r, p_dot_cy - p_dot_r, p_dot_cx + p_dot_r, p_dot_cy + p_dot_r),
            fill=dot_color,
        )
        text_x = p_dot_cx + p_dot_r + 16 - t_box[0]
        text_y = pill_y + (pill_h - t_h) // 2 - t_box[1]
        draw.text((text_x, text_y), label, font=font_pill, fill=TEXT_TAG)
        pill_x += pill_w + pill_gap

    return canvas.resize((WIDTH // SCALE, HEIGHT // SCALE), Image.Resampling.LANCZOS)


def main() -> int:
    """Genera `docs/banner.png` y reporta dimensiones finales."""
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    img = render_banner()
    img.convert("RGB").save(OUTPUT_PATH, format="PNG", optimize=True)
    size_bytes = OUTPUT_PATH.stat().st_size
    print(f"[ZeroCam] Banner generado: {OUTPUT_PATH} ({img.width}x{img.height}, {size_bytes} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
