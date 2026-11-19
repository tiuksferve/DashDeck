#!/usr/bin/python3
"""Gera fontes LVGL (lv_font_fmt_txt, 4 bpp) com Latin-1 a partir de um TTF, usando o FreeType do PIL.

As fontes Montserrat embutidas no LVGL só têm ASCII + ° + •, por isso letras como ç/ã/é viravam quadrados.
As fontes geradas têm fallback para lv_font_montserrat_N, de onde vêm os símbolos (LV_SYMBOL_*).

Uso:  /usr/bin/python3 tools/gen_fonts.py   (escreve main/fonts/dash_font_N.c e dash_fonts.h)
"""
from pathlib import Path

from PIL import ImageFont

HERE = Path(__file__).resolve().parent
TTF = HERE / "fonts" / "Montserrat-Medium.ttf"
OUT = HERE.parent / "main" / "fonts"
SIZES = [12, 14, 16, 20]
EXTRA = [0x2013, 0x2014, 0x2026, 0x2190, 0x2192, 0x2212, 0x2264, 0x2265]   # – — … ← → − ≤ ≥
RANGES = [(0x20, 0x7E), (0xA0, 0xFF)]


def glyph(font, cp):
    ch = chr(cp)
    adv = round(font.getlength(ch) * 16)
    mask, (ox, oy) = font.getmask2(ch, mode="L")
    bbox = mask.getbbox()
    if not bbox:
        return adv, 0, 0, 0, 0, b""
    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0
    ascent, _ = font.getmetrics()
    ofs_x = ox + x0
    ofs_y = ascent - (oy + y1)          # base da caixa relativa à linha de base (para cima > 0)
    nibbles = [mask.getpixel((x, y)) >> 4 for y in range(y0, y1) for x in range(x0, x1)]
    if len(nibbles) % 2:
        nibbles.append(0)
    data = bytes((nibbles[i] << 4) | nibbles[i + 1] for i in range(0, len(nibbles), 2))
    return adv, w, h, ofs_x, ofs_y, data


def has_glyph(font, cp, notdef):
    m = font.getmask2(chr(cp), mode="L")[0]
    return cp == 0x20 or cp == 0xA0 or bytes(m) != notdef


def gen(size):
    font = ImageFont.truetype(str(TTF), size)
    notdef = bytes(font.getmask2("", mode="L")[0])
    ascent, descent = font.getmetrics()
    groups = [list(range(a, b + 1)) for a, b in RANGES]
    groups.append([cp for cp in EXTRA if has_glyph(font, cp, notdef)])

    bitmap = bytearray()
    dsc = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"]
    cmaps, lists = [], []
    gid = 1
    for gi, cps in enumerate(groups):
        if not cps:
            continue
        start = gid
        for cp in cps:
            adv, w, h, ox, oy, data = glyph(font, cp)
            dsc.append(f"    {{.bitmap_index = {len(bitmap)}, .adv_w = {adv}, .box_w = {w}, .box_h = {h}, "
                       f".ofs_x = {ox}, .ofs_y = {oy}}} /* U+{cp:04X} */")
            bitmap += data
            gid += 1
        if gi < len(RANGES):
            cmaps.append(f"    {{.range_start = {cps[0]}, .range_length = {len(cps)}, .glyph_id_start = {start}, "
                         f".unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
                         f".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}}")
        else:
            name = f"unicode_list_{gi}"
            lists.append(f"static const uint16_t {name}[] = {{{', '.join(hex(cp - cps[0]) for cp in cps)}}};")
            cmaps.append(f"    {{.range_start = {cps[0]}, .range_length = {cps[-1] - cps[0] + 1}, "
                         f".glyph_id_start = {start}, .unicode_list = {name}, .glyph_id_ofs_list = NULL, "
                         f".list_length = {len(cps)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}}")

    hexes = ", ".join(f"0x{b:02x}" for b in bitmap)
    src = f"""// Gerado por tools/gen_fonts.py — não editar. Montserrat Medium {size}px, 4 bpp, ASCII + Latin-1.
// Licença da fonte: SIL Open Font License (tools/fonts/OFL.txt).
#include "lvgl.h"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{{hexes}}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{(','+chr(10)).join(dsc)}
}};

{chr(10).join(lists)}

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
{(','+chr(10)).join(cmaps)}
}};

static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = {len(cmaps)},
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
}};

lv_font_t dash_font_{size} = {{   // não const: o fallback recebe a fonte de emojis no arranque
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {ascent + descent},
    .base_line = {descent},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .dsc = &font_dsc,
    .fallback = &lv_font_montserrat_{size},   // símbolos LV_SYMBOL_*
    .user_data = NULL,
}};
"""
    (OUT / f"dash_font_{size}.c").write_text(src)
    return gid - 1, len(bitmap)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    hdr = ["#pragma once", '#include "lvgl.h"', ""] + [f"extern lv_font_t dash_font_{s};" for s in SIZES]
    (OUT / "dash_fonts.h").write_text("\n".join(hdr) + "\n")
    for s in SIZES:
        n, b = gen(s)
        print(f"dash_font_{s}: {n} glifos, {b} bytes")


if __name__ == "__main__":
    main()
