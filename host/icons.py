"""Ícones das aplicações para o ESP: resolve pelo tema GTK (como o menu do uConsole) e converte para
RGB565A8 (plano RGB565 little-endian seguido do plano alfa), o formato que o LVGL desenha direto."""
from __future__ import annotations

import configparser
import functools
import hashlib
import logging
import os
from pathlib import Path

from PIL import Image

log = logging.getLogger("dashd.icons")
SIZE = 32


@functools.lru_cache(maxsize=1)
def _theme():
    os.environ.setdefault("NO_AT_BRIDGE", "1")   # evita o aviso AT-SPI do GTK num daemon
    import gi
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk
    theme = Gtk.IconTheme.new()          # não precisa de display
    name = "PiXflat"
    ini = Path.home() / ".config/gtk-3.0/settings.ini"
    cp = configparser.ConfigParser()
    try:
        cp.read(ini)
        name = cp.get("Settings", "gtk-icon-theme-name", fallback=name)
    except configparser.Error:
        pass
    theme.set_custom_theme(name)
    return theme, Gtk


def _pixbuf_to_pil(pb) -> Image.Image:
    mode = "RGBA" if pb.get_has_alpha() else "RGB"
    img = Image.frombytes(mode, (pb.get_width(), pb.get_height()), pb.get_pixels(), "raw", mode,
                          pb.get_rowstride())
    return img.convert("RGBA")


EMOJI_FONT = "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"


def _emoji(ch: str, size: int) -> Image.Image | None:
    """Ícone a partir de um emoji colorido (ex.: "emoji:✈"). A fonte só existe a 109 px; reduz-se."""
    from PIL import ImageDraw, ImageFont
    try:
        font = ImageFont.truetype(EMOJI_FONT, 109)
    except OSError:
        return None
    im = Image.new("RGBA", (160, 136), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((0, 0), ch, font=font, embedded_color=True)
    bbox = im.getbbox()
    if not bbox:
        return None
    im = im.crop(bbox)
    inner = round(size * 0.8)       # os ícones do tema têm margem; o emoji recortado ocupava tudo
    im.thumbnail((inner, inner), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(im, ((size - im.width) // 2, (size - im.height) // 2), im)
    return out


# Ícones em pixel art de 8 bits ("pix:<nome>:<#cor>"): no cubo, no lugar dos símbolos do LVGL do DashDeck,
# na cor da box (ex.: o envelope verde do Meshtastic). o = contorno, w = claro, p = cor.
PIX = {
    "mail": ["oooooooooooooooo",
             "owwppppppppppwwo",
             "opwwppppppppwwpo",
             "oppwwppppppwwppo",
             "opppwwppppwwpppo",
             "oppppwwppwwppppo",
             "opppppwwwwpppppo",
             "oppppppwwppppppo",
             "oppppppppppppppo",
             "oppppppppppppppo",
             "oppppppppppppppo",
             "oooooooooooooooo"],
}


def _pix(spec: str, size: int) -> Image.Image | None:
    name, _, col = spec.partition(":")
    rows = PIX.get(name)
    if not rows:
        return None
    c = col.lstrip("#")
    try:
        r, g, b = (int(c[i:i + 2], 16) for i in (0, 2, 4))
    except ValueError:
        r, g, b = 0x67, 0xEA, 0x94

    def mix(t: float, to: tuple[int, int, int]) -> tuple[int, int, int, int]:
        return (round(r + (to[0] - r) * t), round(g + (to[1] - g) * t), round(b + (to[2] - b) * t), 255)
    pal = {"o": mix(0.7, (0, 0, 0)), "w": mix(0.55, (255, 255, 255)), "p": (r, g, b, 255)}
    h, w = len(rows), len(rows[0])
    ps = max(1, int(size * 0.9) // w)   # píxel do desenho: inteiro, para ficar nítido
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ox, oy = (size - w * ps) // 2, (size - h * ps) // 2
    px = im.load()
    for j, row in enumerate(rows):
        for i, k in enumerate(row):
            if k in pal:
                for y in range(oy + j * ps, oy + (j + 1) * ps):
                    for x in range(ox + i * ps, ox + (i + 1) * ps):
                        px[x, y] = pal[k]
    return im


def _load(src: str, size: int) -> Image.Image | None:
    if src.startswith("emoji:"):
        return _emoji(src[6:], size)
    if src.startswith("pix:"):
        return _pix(src[4:], size)
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf
    try:
        own = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons", src)
        if not os.path.isabs(src) and src.endswith((".png", ".svg")) and os.path.exists(own):
            src = own   # ícone próprio do DashDeck (host/icons), ex.: board.svg
        if os.path.isabs(src):
            if not os.path.exists(src):
                return None
            pb = GdkPixbuf.Pixbuf.new_from_file_at_scale(src, size, size, True)
        else:
            theme, Gtk = _theme()
            name = src.rsplit(".", 1)[0] if src.endswith((".png", ".svg", ".xpm")) else src
            info = theme.lookup_icon(name, size, Gtk.IconLookupFlags.FORCE_SIZE)
            if info is None:
                for d in ("/usr/share/pixmaps",):
                    for ext in (".png", ".svg", ".xpm"):
                        if os.path.exists(f"{d}/{name}{ext}"):
                            return _load(f"{d}/{name}{ext}", size)
                return None
            pb = info.load_icon()
        img = _pixbuf_to_pil(pb)
    except Exception as e:
        log.debug("ícone %s: %s", src, e)
        return None
    if img.size != (size, size):
        img.thumbnail((size, size), Image.LANCZOS)
        canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        canvas.paste(img, ((size - img.width) // 2, (size - img.height) // 2))
        img = canvas
    return img


def icon_id(src: str) -> str:
    return "i" + hashlib.sha1(src.encode()).hexdigest()[:10]


@functools.lru_cache(maxsize=128)
def rgb565a8(src: str, size: int = SIZE) -> bytes | None:
    img = _load(src, size)
    if img is None:
        return None
    rgb = bytearray()
    alpha = bytearray()
    for r, g, b, a in img.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        rgb += bytes((v & 0xFF, v >> 8))
        alpha.append(a)
    return bytes(rgb + alpha)


@functools.lru_cache(maxsize=64)
def lvgl_bin(src: str, size: int) -> bytes | None:
    """Imagem binária do LVGL 9 (cabeçalho de 12 bytes + ARGB8888, B G R A), para o DashDeck³ ler do cartão
    sem descodificar PNG (lv_image "S:icons/<id>.bin")."""
    img = _load(src, size)
    if img is None:
        return None
    import struct
    w, h = img.size
    head = struct.pack("<BBHHHHH", 0x19, 0x10, 0, w, h, w * 4, 0)   # magia, LV_COLOR_FORMAT_ARGB8888
    r, g, b, a = img.split()
    return head + Image.merge("RGBA", (b, g, r, a)).tobytes()
