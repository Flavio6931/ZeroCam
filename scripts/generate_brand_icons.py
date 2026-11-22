"""Genera los iconos de ZeroCam (marca v4) para Windows y Android.

Fuente única de la geometría (igual que en Figma, "ZeroCam — Brand Sheet v4"):
el 0 son dos superelipses (n = 2.5) en un espacio de 128 unidades y el punto
es la cámara/luz REC. Hay dos cortes ópticos: REGULAR (>= 48 px) y SMALL
(<= 32 px, trazo más grueso y punto mayor).

Salidas:
  windows/app/res/zerocam.ico                         (16-256 px, entradas PNG)
  android/app/src/main/res/drawable/ic_launcher_*.xml (adaptive: fondo, frente, monocromo)
  android/app/src/main/res/drawable/ic_stat_zerocam.xml (notificación, blanco)
  android/app/src/main/res/mipmap-anydpi-v26/ic_launcher{,_round}.xml
  android/app/src/main/res/values/brand_colors.xml

Uso:  python scripts/generate_brand_icons.py [--preview <png>]
"""

from __future__ import annotations

import argparse
import io
import math
import struct
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

INK = "#141518"
PAPER = "#F3F1EC"
SIGNAL = "#FF4F1F"

CENTER = 64.0
SUPERELLIPSE_N = 2.5
CONTOUR_POINTS = 144
MARK_TOP, MARK_HEIGHT = 19.0, 90.0  # caja ajustada de la marca en unidades de 128

ICO_SIZES = (256, 64, 48, 40, 32, 24, 20, 16)
SMALL_CUT_MAX_PX = 32
SUPERSAMPLE = 16
TILE_RADIUS = 28.0  # de 128


@dataclass(frozen=True)
class Cut:
    outer: tuple[float, float]
    inner: tuple[float, float]
    dot: tuple[float, float, float]  # cx, cy, r
    tile_scale: float  # escala de la marca dentro del icono cuadrado


REGULAR = Cut(outer=(31, 45), inner=(17.5, 34), dot=(64, 44, 7.5), tile_scale=0.80)
SMALL = Cut(outer=(31, 45), inner=(15, 31.5), dot=(64, 46, 8.5), tile_scale=0.86)
# 16-20 px: el contraformo se cierra y el punto casi lo llena para seguir viéndose naranja.
MICRO = Cut(outer=(32, 46), inner=(14.5, 30), dot=(64, 50, 11), tile_scale=0.92)
MICRO_CUT_MAX_PX = 20


def superellipse(a: float, b: float) -> list[tuple[float, float]]:
    """Contorno de superelipse centrado en (64, 64), en unidades de 128."""
    pts = []
    for i in range(CONTOUR_POINTS):
        t = 2 * math.pi * i / CONTOUR_POINTS
        c, s = math.cos(t), math.sin(t)
        x = CENTER + a * math.copysign(abs(c) ** (2 / SUPERELLIPSE_N), c)
        y = CENTER + b * math.copysign(abs(s) ** (2 / SUPERELLIPSE_N), s)
        pts.append((x, y))
    return pts


def transform(pts, scale: float, origin: float, factor: float):
    """Escala alrededor del centro 64 y lleva al sistema destino."""
    return [((origin + (x - CENTER) * scale) * factor, (origin + (y - CENTER) * scale) * factor) for x, y in pts]


# --------------------------------------------------------------------------- Windows (.ico)

def render_tile(size: int) -> Image.Image:
    if size <= MICRO_CUT_MAX_PX:
        cut = MICRO
    elif size <= SMALL_CUT_MAX_PX:
        cut = SMALL
    else:
        cut = REGULAR
    big = size * SUPERSAMPLE
    f = big / 128.0
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, big - 1, big - 1), radius=TILE_RADIUS * f, fill=INK)
    d.polygon(transform(superellipse(*cut.outer), cut.tile_scale, CENTER, f), fill=PAPER)
    d.polygon(transform(superellipse(*cut.inner), cut.tile_scale, CENTER, f), fill=INK)
    cx, cy, r = cut.dot
    (dx, dy), = transform([(cx, cy)], cut.tile_scale, CENTER, f)
    rr = r * cut.tile_scale * f
    d.ellipse((dx - rr, dy - rr, dx + rr, dy + rr), fill=SIGNAL)
    return img.resize((size, size), Image.Resampling.LANCZOS)


def write_ico(path: Path, images: list[Image.Image]) -> None:
    """ICO con entradas PNG (soportado desde Windows Vista y por rc.exe)."""
    blobs = []
    for im in images:
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        blobs.append(buf.getvalue())
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for im, blob in zip(images, blobs):
        w, h = im.size
        entries += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + entries + b"".join(blobs))


# --------------------------------------------------------------------------- Android (vector)

def path_data(pts) -> str:
    head, *rest = pts
    return f"M{head[0]:.2f},{head[1]:.2f}" + "".join(f"L{x:.2f},{y:.2f}" for x, y in rest) + "Z"


def circle_data(cx: float, cy: float, r: float) -> str:
    return f"M{cx - r:.2f},{cy:.2f}a{r:.2f},{r:.2f} 0 1,0 {2 * r:.2f},0a{r:.2f},{r:.2f} 0 1,0 {-2 * r:.2f},0Z"


def mark_paths(cut: Cut, viewport: float, mark_height: float) -> tuple[str, str]:
    """Devuelve (path del cero con hueco evenOdd, path del punto) centrados en el viewport."""
    scale = mark_height / MARK_HEIGHT
    origin = viewport / 2
    zero = path_data(transform(superellipse(*cut.outer), scale, origin, 1)) + path_data(
        transform(superellipse(*cut.inner), scale, origin, 1))
    cx, cy, r = cut.dot
    (dx, dy), = transform([(cx, cy)], scale, origin, 1)
    return zero, circle_data(dx, dy, r * scale)


def vector(viewport: float, size_dp: int, body: str) -> str:
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- Generado por scripts/generate_brand_icons.py (marca ZeroCam v4). No editar a mano. -->\n"
        '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
        f'    android:width="{size_dp}dp"\n    android:height="{size_dp}dp"\n'
        f'    android:viewportWidth="{viewport:g}"\n    android:viewportHeight="{viewport:g}">\n'
        f"{body}</vector>\n"
    )


def vpath(data: str, color: str, even_odd: bool = False) -> str:
    rule = '\n        android:fillType="evenOdd"' if even_odd else ""
    return f'    <path\n        android:fillColor="{color}"{rule}\n        android:pathData="{data}" />\n'


def write_android(res: Path) -> list[Path]:
    # Adaptive icon: viewport 108, zona visible ~72dp; la marca mide 50dp de alto (zona segura 66dp).
    zero, dot = mark_paths(REGULAR, 108, 50)
    zero_sm, dot_sm = mark_paths(SMALL, 108, 50)
    stat_zero, stat_dot = mark_paths(SMALL, 24, 20)
    files = {
        "values/brand_colors.xml": (
            '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
            f'    <color name="brand_ink">{INK}</color>\n'
            f'    <color name="brand_paper">{PAPER}</color>\n'
            f'    <color name="brand_signal">{SIGNAL}</color>\n</resources>\n'
        ),
        "drawable/ic_launcher_background.xml": vector(108, 108, vpath("M0,0h108v108h-108z", "@color/brand_ink")),
        "drawable/ic_launcher_foreground.xml": vector(
            108, 108, vpath(zero, "@color/brand_paper", True) + vpath(dot, "@color/brand_signal")),
        # Android 13+ "Themed icons": una sola tinta, el sistema la colorea.
        "drawable/ic_launcher_monochrome.xml": vector(
            108, 108, vpath(zero_sm, "#FFFFFFFF", True) + vpath(dot_sm, "#FFFFFFFF")),
        "drawable/ic_stat_zerocam.xml": vector(
            24, 24, vpath(stat_zero, "#FFFFFFFF", True) + vpath(stat_dot, "#FFFFFFFF")),
    }
    adaptive = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@drawable/ic_launcher_background" />\n'
        '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
        '    <monochrome android:drawable="@drawable/ic_launcher_monochrome" />\n'
        "</adaptive-icon>\n"
    )
    files["mipmap-anydpi-v26/ic_launcher.xml"] = adaptive
    files["mipmap-anydpi-v26/ic_launcher_round.xml"] = adaptive
    written = []
    for rel, content in files.items():
        p = res / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8", newline="\n")
        written.append(p)
    return written


def build_preview(path: Path, tiles: list[Image.Image]) -> None:
    pad = 24
    width = sum(t.width for t in tiles) + pad * (len(tiles) + 1)
    height = max(t.height for t in tiles) + 2 * pad
    sheet = Image.new("RGBA", (width, height), "#E6E2D9")
    x = pad
    for t in tiles:
        sheet.alpha_composite(t, (x, height - pad - t.height))
        x += t.width + pad
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", type=Path, help="PNG de previsualización de los tamaños del .ico")
    args = parser.parse_args()

    tiles = [render_tile(s) for s in ICO_SIZES]
    ico = ROOT / "windows" / "app" / "res" / "zerocam.ico"
    write_ico(ico, tiles)
    print(f"ICO   {ico.relative_to(ROOT)}  ({ico.stat().st_size:,} B, {len(tiles)} tamaños)")

    for p in write_android(ROOT / "android" / "app" / "src" / "main" / "res"):
        print(f"XML   {p.relative_to(ROOT)}")

    if args.preview:
        build_preview(args.preview, tiles)
        print(f"PNG   {args.preview}")


if __name__ == "__main__":
    main()
