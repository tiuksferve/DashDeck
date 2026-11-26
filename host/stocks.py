"""Cotações de ações (box Stocks) pelo Yahoo Finance, sem chave, e os logotipos das empresas.

O endpoint de gráfico (/v8/finance/chart) dá a cotação atual, o fecho anterior e os valores do
dia (de 5 em 5 min). Só o símbolo sai do uConsole. Os logotipos são marcas das empresas: não vão
para o repositório; são descarregados uma vez para ~/.cache/deck3/logos e adaptados ao ecrã
(margens cortadas, logotipos escuros clareados para se verem no fundo escuro).
"""
from __future__ import annotations

import io
import json
import logging
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

log = logging.getLogger("deck3.stocks")

CHART = "https://query1.finance.yahoo.com/v8/finance/chart/{}?range=1d&interval=5m"
LOGO = "https://companiesmarketcap.com/img/company-logos/64/{}.png"
LOGOS = Path.home() / ".cache" / "deck3" / "logos"
UA = {"User-Agent": "Mozilla/5.0 (X11; Linux aarch64)"}   # o Yahoo recusa pedidos sem browser


def _get(url: str, timeout: float = 10) -> bytes:
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=timeout) as r:
        return r.read()


def fetch(sym: str) -> dict:
    """{"price","prev","cur","name","t","open","closes":[...],"times":[...],"start","end"}."""
    d = json.loads(_get(CHART.format(urllib.parse.quote(sym))))
    res = (d.get("chart") or {}).get("result")
    if not res:
        raise ValueError(((d.get("chart") or {}).get("error") or {}).get("description") or "sem dados")
    r = res[0]
    m = r["meta"]
    q = (r.get("indicators", {}).get("quote") or [{}])[0]
    pts = [(t, c) for t, c in zip(r.get("timestamp") or [], q.get("close") or []) if c is not None]
    reg = (m.get("currentTradingPeriod") or {}).get("regular") or {}
    return {"price": m.get("regularMarketPrice"), "prev": m.get("chartPreviousClose") or m.get("previousClose"),
            "cur": m.get("currency", ""), "name": m.get("longName") or m.get("shortName") or sym,
            "t": m.get("regularMarketTime", 0), "high": m.get("regularMarketDayHigh"),
            "low": m.get("regularMarketDayLow"), "exch": m.get("fullExchangeName") or m.get("exchangeName", ""),
            "times": [p[0] for p in pts], "closes": [p[1] for p in pts],
            "start": reg.get("start", 0), "end": reg.get("end", 0)}


def _trim(im):
    box = im.getchannel("A").point(lambda a: 255 if a > 24 else 0).getbbox()
    return im.crop(box) if box else im


def logo(sym: str, crop: float = 1.0) -> str | None:
    """Caminho do logotipo (32 px) na cache; descarrega-o se faltar. None se não houver.
    crop < 1 guarda só essa fração da largura, a contar da esquerda (ex.: SpaceX 0,65: o X com a
    curva encurtada, porque o logotipo inteiro é muito largo e ficava minúsculo)."""
    path = LOGOS / (f"{sym.replace('/', '_')}" + (f"-c{round(crop * 100)}" if crop < 1 else "") + ".png")
    if path.exists():
        return str(path)
    from PIL import Image
    try:
        im = Image.open(io.BytesIO(_get(LOGO.format(urllib.parse.quote(sym)), timeout=8))).convert("RGBA")
    except Exception as e:
        log.debug("logotipo %s: %s", sym, e)
        return None
    im = _trim(im)
    if crop < 1:
        im = _trim(im.crop((0, 0, max(1, int(im.width * crop)), im.height)))
    # logotipo escuro (ex.: Apple, SpaceX, pretos): fica quase branco, com a mesma forma
    px = [(r, g, b) for r, g, b, a in im.getdata() if a > 128]
    if px and sum(0.3 * r + 0.59 * g + 0.11 * b for r, g, b in px) / len(px) < 70:
        im = Image.merge("RGBA", (*[Image.new("L", im.size, 235)] * 3, im.getchannel("A")))
    # largos usam a largura toda; os outros ficam com 2 px de margem
    im.thumbnail((32, 32) if im.width > im.height * 1.4 else (28, 28), Image.LANCZOS)
    out = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    out.paste(im, ((32 - im.width) // 2, (32 - im.height) // 2), im)
    LOGOS.mkdir(parents=True, exist_ok=True)
    out.save(path)
    return str(path)


class Quotes:
    """Cotações em segundo plano: a cada `fast` s enquanto a tela está aberta, a cada `slow` s se
    só o box do menu as mostra, e paradas quando ninguém as pede há 10 min."""

    def __init__(self, symbols: list[str], fast: float = 60, slow: float = 300, crops: dict | None = None):
        self.symbols = symbols
        self.crops = crops or {}
        self.fast, self.slow = fast, slow
        self.data: dict[str, dict] = {}
        self.errors: dict[str, str] = {}
        self.logos: dict[str, str] = {}
        self.updated = 0.0
        self._seen = 0.0       # última vez que alguém leu
        self._open = 0.0       # última vez que a tela esteve aberta
        self._wake = threading.Event()
        threading.Thread(target=self._run, daemon=True, name="stocks").start()

    def touch(self, screen: bool = False) -> None:
        now = time.monotonic()
        self._seen = now
        if screen:
            stale = now - self.updated > self.fast
            self._open = now
            if stale:
                self._wake.set()
        elif not self.updated:
            self._wake.set()

    def _run(self) -> None:
        while True:
            now = time.monotonic()
            if now - self._seen < 600:
                for s in self.symbols:
                    try:
                        self.data[s] = fetch(s)
                        self.errors.pop(s, None)
                    except Exception as e:
                        self.errors[s] = str(e)[:60]
                        log.debug("cotação %s: %s", s, e)
                    if s not in self.logos:
                        p = logo(s, float(self.crops.get(s, 1.0)))
                        if p:
                            self.logos[s] = p
                self.updated = time.monotonic()
            wait = self.fast if time.monotonic() - self._open < 120 else self.slow
            self._wake.wait(wait)
            self._wake.clear()
