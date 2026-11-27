"""Varanda do Romeu no DashDeck³ (cubo em pé girado à esquerda): as imagens para o cartão do cubo e os dados da vista.

Imagens (host/assets/varanda): varanda.png (cena 480x480) e os sprites do Romeu (<nome>.png, sprites.json com o
tamanho, a âncora e as poses). Convertem-se para formatos simples que o firmware lê sem descodificar PNG:
  varanda/bg.bin      "VBG1", u16 w, u16 h, paleta 256 x RGB888, w*h índices
  varanda/<nome>.bin  "VSP1", u16 w, h, ax, ay, w*h RGB565 (LE), w*h alfa (0/1)
  varanda/poses.json  {"pose": ["sprite", ...]}
  varanda/v_<versão>.ok  marca de que o conjunto está completo (o dashd vê-a com um "ls" e não volta a enviar)

Dados da vista ({"t":"varanda", ...}, a cada VIEW_S enquanto a varanda está aberta): tempo ("wx": rain, storm,
snow ou ""), nascer e pôr do sol ("sun": [min, min]), o avião e o barco mais próximos ("planes", "boats").
"""
from __future__ import annotations

import functools
import hashlib
import json
import logging
import math
import struct
from pathlib import Path

from PIL import Image

log = logging.getLogger("dashd.varanda")
ASSETS = Path(__file__).resolve().parent / "assets" / "varanda"
VIEW_S = 8


@functools.lru_cache(maxsize=1)
def build() -> tuple[str, dict[str, bytes]]:
    """(versão, {caminho no cartão: bytes}). A marca de versão não vai na lista (envia-se no fim)."""
    man = json.loads((ASSETS / "sprites.json").read_text())
    files: dict[str, bytes] = {}
    bg = Image.open(ASSETS / "varanda.png").convert("RGB").resize((480, 480), Image.LANCZOS)
    q = bg.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = (q.getpalette() or [])[:768]
    pal += [0] * (768 - len(pal))
    files["varanda/bg.bin"] = b"VBG1" + struct.pack("<HH", 480, 480) + bytes(pal) + q.tobytes()
    for name, f in man["frames"].items():
        im = Image.open(ASSETS / f"{name}.png").convert("RGBA")
        w, h = im.size
        px = bytearray()
        alpha = bytearray()
        for r, g, b, a in im.getdata():
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            px += struct.pack("<H", v)
            alpha.append(1 if a >= 128 else 0)
        files[f"varanda/{name}.bin"] = b"VSP1" + struct.pack("<HHHH", w, h, f["ax"], f["ay"]) + bytes(px) + bytes(alpha)
    files["varanda/poses.json"] = json.dumps(man["poses"], separators=(",", ":")).encode()
    hsh = hashlib.sha1()
    for k in sorted(files):
        hsh.update(k.encode())
        hsh.update(files[k])
    return hsh.hexdigest()[:10], files


def marker() -> str:
    return f"v_{build()[0]}.ok"


def _dist_km(a: tuple[float, float], b: tuple[float, float]) -> float:
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 6371 * 2 * math.asin(math.sqrt(h))


def _nearest(prov) -> tuple[dict, float] | None:
    """O alvo com posição mais perto da estação do provider (ADS-B/AIS), e a distância em km."""
    try:
        prov.load()
        st = prov.station()
    except Exception as e:  # o provider pode estar sem serviço
        log.debug("varanda: %s", e)
        return None
    best = None
    for t in getattr(prov, "targets", []):
        if t.get("lat") is None or t.get("lon") is None or not st:
            continue
        d = _dist_km(st, (t["lat"], t["lon"]))
        if best is None or d < best[1]:
            best = (t, d)
    return best


def view(app) -> dict:
    """A mensagem com os dados da vista, a partir dos providers do menu (relógio, ADS-B, AIS)."""
    msg: dict = {"t": "varanda"}
    provs = {}
    for node in _nodes(app.cfg.get("menu", []) + app.cfg.get("dock", [])):
        kind = node.get("provider")
        if kind in ("clock", "adsb", "ais") and kind not in provs:
            try:
                provs[kind] = app.provider(node)
            except Exception as e:
                log.debug("varanda: provider %s: %s", kind, e)
    clk = provs.get("clock")
    wx = clk._wx() if clk and hasattr(clk, "_wx") else None
    if wx:
        msg["wx"] = {"chuva": "rain", "trovoada": "storm", "neve": "snow"}.get(wx.get("kind", ""), "")
        if wx.get("sunrise") and wx.get("sunset"):
            msg["sun"] = [int(wx["sunrise"]), int(wx["sunset"])]
    if "adsb" in provs:
        n = _nearest(provs["adsb"])
        if n:
            t, d = n
            alt = t["raw"].get("alt_baro") or t["raw"].get("alt_geom") or 0
            alt_m = round(alt * 0.3048) if isinstance(alt, (int, float)) else 0
            msg["planes"] = [{"id": t["name"][:15], "alt": alt_m, "dist": round(d, 1)}]
    if "ais" in provs:
        n = _nearest(provs["ais"])
        if n:
            t, d = n
            msg["boats"] = [{"name": t["name"][:23], "dist": round(d / 1.852, 1)}]
    return msg


def _nodes(items):
    for it in items:
        yield it
        yield from _nodes(it.get("items", []))
