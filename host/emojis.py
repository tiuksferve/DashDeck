"""Emojis no texto do ESP: separa as sequências de emoji e desenha-as como imagens a cores.

Uma "sequência" é o que se vê como um só emoji: um símbolo com variação (☀️), tom de pele (👍🏽),
uma bandeira (dois indicadores regionais, 🇵🇹), um keycap (1️⃣) ou uma junção ZWJ (👨‍👩‍👧).
O dashd troca cada sequência por um código privado (U+100000+i) e envia a imagem ao ESP
(firmware: imgfont no fallback das fontes). Sem fonte de emoji, as sequências são removidas.
"""
from __future__ import annotations

import functools
import logging

from PIL import Image, ImageDraw, ImageFont

log = logging.getLogger("dashd.emoji")

FONT = "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"
CP0 = 0x100000        # códigos privados usados no texto enviado ao ESP
MAX = 48              # imagens guardadas no ESP
SIZE = 14             # altura da imagem (px); a largura tem +1 px de espaço à direita

VS16, ZWJ, KEYCAP = 0xFE0F, 0x200D, 0x20E3


def _pict(cp: int) -> bool:
    """Pictograma que pode ser emoji (Extended_Pictographic, aproximado por blocos)."""
    return (0x1F000 <= cp <= 0x1FAFF or 0x2600 <= cp <= 0x27BF or 0x2300 <= cp <= 0x23FF
            or 0x2B00 <= cp <= 0x2BFF or 0x2190 <= cp <= 0x21FF or 0x25A0 <= cp <= 0x25FF
            or 0x2934 <= cp <= 0x2935 or 0x3030 == cp or 0x303D == cp or 0x3297 == cp or 0x3299 == cp
            or cp in (0x00A9, 0x00AE, 0x203C, 0x2049, 0x2122, 0x2139, 0x24C2))


def _ri(cp: int) -> bool:
    return 0x1F1E6 <= cp <= 0x1F1FF


def _modifier(cp: int) -> bool:
    return 0x1F3FB <= cp <= 0x1F3FF or 0xE0020 <= cp <= 0xE007F or cp in (VS16, KEYCAP)


def split(text: str, in_font) -> list[tuple[bool, str]]:
    """Divide o texto em pedaços (é_emoji, texto). in_font(cp) diz se a fonte normal tem o carácter:
    esses (← → © …) só contam como emoji quando vêm com o seletor de variação."""
    out: list[tuple[bool, str]] = []
    plain = []
    i, n = 0, len(text)

    def flush():
        if plain:
            out.append((False, "".join(plain)))
            plain.clear()

    while i < n:
        cp = ord(text[i])
        nxt = ord(text[i + 1]) if i + 1 < n else 0
        j = i
        if _ri(cp):
            j = i + 2 if _ri(nxt) else i + 1          # bandeira (um indicador sozinho é removido)
        elif text[i] in "0123456789#*" and (nxt == KEYCAP or (nxt == VS16 and i + 2 < n and ord(text[i + 2]) == KEYCAP)):
            j = i + (3 if nxt == VS16 else 2)
        elif _pict(cp) and (not in_font(cp) or nxt == VS16):
            j = i + 1
            while j < n:
                c = ord(text[j])
                if _modifier(c):
                    j += 1
                elif c == ZWJ and j + 1 < n and _pict(ord(text[j + 1])):
                    j += 2
                else:
                    break
        if j > i:
            flush()
            seq = text[i:j]
            if not (len(seq) == 1 and _ri(cp)):
                out.append((True, seq))
            i = j
        else:
            plain.append(text[i])
            i += 1
    flush()
    return out


@functools.lru_cache(maxsize=1)
def _font():
    try:
        return ImageFont.truetype(FONT, 109, layout_engine=ImageFont.Layout.RAQM)
    except (OSError, KeyError):
        try:
            return ImageFont.truetype(FONT, 109)
        except OSError:
            log.info("sem fonte de emoji (%s): os emojis são removidos", FONT)
            return None


@functools.lru_cache(maxsize=256)
def rgb565a8(seq: str) -> tuple[int, int, bytes] | None:
    """Imagem do emoji em RGB565A8 (w, h, dados). A fonte só existe a 109 px; reduz-se."""
    font = _font()
    if font is None:
        return None
    im = Image.new("RGBA", (420, 160), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((4, 4), seq, font=font, embedded_color=True)
    bbox = im.getbbox()
    if not bbox:
        return None
    im = im.crop(bbox)
    if im.width > im.height * 1.6:     # a fonte não juntou a sequência (ex.: ZWJ sem glifo): só o 1.º
        im = im.crop((0, 0, im.height, im.height))
    im.thumbnail((SIZE, SIZE), Image.LANCZOS)
    w, h = SIZE + 1, SIZE
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(im, ((SIZE - im.width) // 2, (h - im.height) // 2), im)
    rgb, alpha = bytearray(), bytearray()
    for r, g, b, a in out.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        rgb += bytes((v & 0xFF, v >> 8))
        alpha.append(a)
    return w, h, bytes(rgb + alpha)


class Registry:
    """Que sequência está em que código no ESP. Os mais antigos são substituídos (até MAX)."""

    def __init__(self):
        self.cp: dict[str, int] = {}       # sequência -> código, só as confirmadas pelo ESP
        self.order: list[str] = []         # da mais antiga para a mais recente

    def clear(self) -> None:
        self.cp.clear()
        self.order.clear()

    def touch(self, seq: str) -> None:
        if seq in self.order:
            self.order.remove(seq)
            self.order.append(seq)

    def slot_for(self, seq: str, keep: set[str]) -> int:
        """Código livre para seq; substitui a mais antiga que não esteja na tela atual."""
        used = set(self.cp.values())
        for i in range(MAX):
            if CP0 + i not in used:
                return CP0 + i
        for old in self.order:
            if old not in keep:
                cp = self.cp.pop(old)
                self.order.remove(old)
                return cp
        return -1

    def set(self, seq: str, cp: int) -> None:
        self.cp[seq] = cp
        self.touch(seq)
        if seq not in self.order:
            self.order.append(seq)
