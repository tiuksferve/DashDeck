"""Mapa de fundo para o mapa ADS-B: tiles raster (Web Mercator) com cache em disco.

Os tiles são baixados em segundo plano e guardados em ~/.cache/dashd/tiles, por isso, depois de vista
uma vez com internet, a região funciona offline. A imagem enviada ao ESP é reduzida a 16 cores (4 bpp).

Estilos (map_style no config.toml):
  simples  — Esri Dark Gray: terra cinza, mar escuro, poucos nomes. Pensado para fundo de dados.
  satelite — Esri World Imagery + nomes/fronteiras (o "satélite" do MeshDeckWeb).
  osm      — OpenStreetMap com filtro escuro (o "escuro" do MeshDeckWeb); muito detalhe para 1,47".
"""
from __future__ import annotations

import hashlib
import io
import logging
import math
import os
import queue
import threading
import urllib.request
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps

log = logging.getLogger("dashd.map")

ESRI = "https://server.arcgisonline.com/ArcGIS/rest/services"
STYLES = {
    "simples": {"layers": [f"{ESRI}/Canvas/World_Dark_Gray_Base/MapServer/tile/{{z}}/{{y}}/{{x}}",
                           f"{ESRI}/Canvas/World_Dark_Gray_Reference/MapServer/tile/{{z}}/{{y}}/{{x}}"],
                "credit": "© Esri"},
    "satelite": {"layers": [f"{ESRI}/World_Imagery/MapServer/tile/{{z}}/{{y}}/{{x}}",
                            f"{ESRI}/Reference/World_Boundaries_and_Places/MapServer/tile/{{z}}/{{y}}/{{x}}"],
                 "dim": 0.6, "credit": "© Esri"},
    # OSM: uso leve com User-Agent próprio e cache local (https://operations.osmfoundation.org/policies/tiles/).
    "osm": {"layers": ["https://tile.openstreetmap.org/{z}/{x}/{y}.png"], "dark": True, "dim": 0.8,
            "credit": "© OpenStreetMap"},
}
DEFAULT_STYLE = "simples"
USER_AGENT = "DashDeck-dashd/0.3 (uConsole aux display)"
TILE = 256
EARTH_C = 2 * math.pi * 6378137.0


def world_px(lat: float, lon: float, z: int) -> tuple[float, float]:
    """Coordenadas em pixels do mundo (Web Mercator) no zoom z."""
    lat = max(-85.05, min(85.05, lat))
    n = TILE * (2 ** z)
    x = (lon + 180.0) / 360.0 * n
    y = (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n
    return x, y


def latlon(x: float, y: float, z: int) -> tuple[float, float]:
    """Inverso de world_px."""
    n = TILE * (2 ** z)
    lon = x / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    return lat, lon


def px_per_nm(lat: float, z: int) -> float:
    m_per_px = EARTH_C * math.cos(math.radians(lat)) / (TILE * 2 ** z)
    return 1852.0 / m_per_px


def dark_style(img: Image.Image) -> Image.Image:
    """Mapa claro -> escuro: inverte e roda o matiz 180° (mar azul-escuro, terra escura, texto claro)."""
    h, s, v = ImageOps.invert(img).convert("HSV").split()
    h = h.point(lambda x: (x + 128) % 256)
    return Image.merge("HSV", (h, s, v)).convert("RGB")


class TileMap:
    def __init__(self, style: str = DEFAULT_STYLE, url: str | None = None, cache: str | None = None,
                 on_ready=None):
        """style: nome em STYLES; url: um servidor {z}/{x}/{y} próprio (substitui o estilo)."""
        self.style = dict(STYLES.get(style, STYLES[DEFAULT_STYLE]))
        if url:
            self.style = {"layers": [url], "credit": url.split("/")[2]}
        self.name = url or style
        self.credit = self.style.get("credit", "")
        self.root = Path(cache or os.path.expanduser("~/.cache/dashd/tiles"))
        self.on_ready = on_ready          # chamado (sem args) quando chegam tiles novos
        self.q: queue.Queue = queue.Queue()
        self.pending: set = set()
        self.failed: dict = {}
        self.lock = threading.Lock()
        self.online = True
        self._img_cache: dict[str, tuple] = {}
        threading.Thread(target=self._worker, daemon=True, name="tiles").start()

    # ---- tiles
    def _path(self, url, z, x, y) -> Path:
        return self.root / hashlib.sha1(url.encode()).hexdigest()[:8] / str(z) / str(x) / f"{y}.png"

    def _worker(self) -> None:
        while True:
            key = self.q.get()
            url, z, x, y = key
            p = self._path(url, z, x, y)
            try:
                req = urllib.request.Request(url.format(z=z, x=x, y=y), headers={"User-Agent": USER_AGENT})
                with urllib.request.urlopen(req, timeout=8) as r:
                    data = r.read()
                Image.open(io.BytesIO(data)).verify()
                p.parent.mkdir(parents=True, exist_ok=True)
                tmp = p.with_suffix(".tmp")
                tmp.write_bytes(data)
                tmp.replace(p)
                self.online = True
                ok = True
            except Exception as e:
                log.debug("tile %s: %s", key, e)
                self.failed[key] = self.failed.get(key, 0) + 1
                self.online = False
                ok = False
            with self.lock:
                self.pending.discard(key)
                done = not self.pending
            if ok and done and self.on_ready:
                self.on_ready()

    def _want(self, url, z, x, y) -> Image.Image | None:
        p = self._path(url, z, x, y)
        if p.exists():
            try:
                return Image.open(p).convert("RGBA")
            except Exception:
                p.unlink(missing_ok=True)
        key = (url, z, x, y)
        with self.lock:
            if key not in self.pending and self.failed.get(key, 0) < 3:
                self.pending.add(key)
                self.q.put(key)
        return None

    def _tile(self, z, x, y) -> Image.Image | None:
        """Tile composto: base + camadas transparentes por cima; None se faltar alguma."""
        imgs = [self._want(url, z, x, y) for url in self.style["layers"]]
        if any(i is None for i in imgs):
            return None
        out = imgs[0]
        for ov in imgs[1:]:
            out = Image.alpha_composite(out, ov)
        out = out.convert("RGB")
        if self.style.get("dark"):
            out = dark_style(out)
        return out

    # ---- composição
    def render(self, lat: float, lon: float, z: int, w: int, h: int):
        """Devolve (chave, paleta[16], bytes 4bpp) ou None se faltarem tiles (pedidos em 2.º plano)."""
        key = hashlib.sha1(f"{self.name}|{lat:.5f}|{lon:.5f}|{z}|{w}|{h}".encode()).hexdigest()[:12]
        if key in self._img_cache:
            return self._img_cache[key]
        cx, cy = world_px(lat, lon, z)
        left, top = int(cx - w / 2), int(cy - h / 2)
        n = 2 ** z
        img = Image.new("RGB", (w, h), (11, 15, 20))
        missing = False
        for ty in range(top // TILE, (top + h - 1) // TILE + 1):
            if ty < 0 or ty >= n:
                continue
            for tx in range(left // TILE, (left + w - 1) // TILE + 1):
                tile = self._tile(z, tx % n, ty)
                if tile is None:
                    missing = True
                    continue
                img.paste(tile, (tx * TILE - left, ty * TILE - top))
        if missing:
            return None
        if self.style.get("dim"):
            img = ImageEnhance.Brightness(img).enhance(self.style["dim"])
        q = img.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
        pal = q.getpalette()[:48]
        pal += [0] * (48 - len(pal))
        palette = [f"#{pal[i]:02X}{pal[i + 1]:02X}{pal[i + 2]:02X}" for i in range(0, 48, 3)]
        px = q.tobytes()
        if len(px) % 2:
            px += b"\0"
        packed = bytes((px[i] << 4) | (px[i + 1] & 0x0F) for i in range(0, len(px), 2))
        res = (key, palette, packed)
        if len(self._img_cache) > 8:
            self._img_cache.clear()
        self._img_cache[key] = res
        return res

    def prefetch(self, lat: float, lon: float, zooms, w: int, h: int) -> None:
        """Pede os tiles dos zooms vizinhos em volta do centro (Zoom +/- imediato e uso offline)."""
        for z in zooms:
            cx, cy = world_px(lat, lon, z)
            left, top = int(cx - w / 2), int(cy - h / 2)
            n = 2 ** z
            for ty in range(top // TILE, (top + h - 1) // TILE + 1):
                for tx in range(left // TILE, (left + w - 1) // TILE + 1):
                    if 0 <= ty < n:
                        self._tile(z, tx % n, ty)
