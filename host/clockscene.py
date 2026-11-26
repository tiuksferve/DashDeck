"""Modo relógio: previsão do tempo (Open-Meteo), cena da paisagem em pixel art e ícones 16x16.

A cena (céu, sol/lua, nuvens, montanhas, casa, lago, nevoeiro) é desenhada aqui e enviada ao ESP como
fundo de 8 bpp. A hora, a previsão, a chuva, a neve e os relâmpagos são desenhados pelo ESP
(firmware/main/clockface.c). O desenho segue a pré-visualização aprovada (mesmas cores e formas).
"""
from __future__ import annotations

import json
import logging
import math
import random
import threading
import time
import urllib.request
from pathlib import Path

from PIL import Image

log = logging.getLogger("deck3.clock")

CACHE = Path.home() / ".cache" / "deck3" / "weather.json"
API = ("https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}"
       "&current=temperature_2m,weather_code,wind_speed_10m,is_day"
       "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
       "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
       "&timezone=auto&forecast_days=1&forecast_hours=12")

KINDS = {"limpo": "Limpo", "parcial": "Pouco nublado", "nublado": "Nublado", "chuva": "Chuva",
         "trovoada": "Trovoada", "neve": "Neve", "nevoeiro": "Nevoeiro"}
CLOUD_FACTOR = {"limpo": 0, "parcial": .25, "nublado": .6, "chuva": .72, "trovoada": .85, "neve": .55,
                "nevoeiro": .5}
ANIM = {"chuva": "rain", "trovoada": "storm", "neve": "snow"}


def wmo_kind(code: int) -> str:
    """Código WMO do Open-Meteo -> tipo de céu do mostrador."""
    if code in (0, 1):
        return "limpo"
    if code == 2:
        return "parcial"
    if code == 3:
        return "nublado"
    if code in (45, 48):
        return "nevoeiro"
    if 71 <= code <= 77 or code in (85, 86):
        return "neve"
    if code >= 95:
        return "trovoada"
    if 51 <= code <= 67 or 80 <= code <= 82:
        return "chuva"
    return "nublado"


def _hm(iso: str) -> int | None:
    try:
        hh, mm = iso.split("T")[1].split(":")[:2]
        return int(hh) * 60 + int(mm)
    except (IndexError, ValueError):
        return None


class Weather:
    """Previsão do dia, atualizada a cada 30 min numa thread e guardada em cache (funciona offline).

    position() devolve (lat, lon) ou None; as coordenadas saem do uConsole arredondadas a 0,01°
    (cerca de 1 km).
    """

    def __init__(self, position, every: float = 1800, on_update=None):
        self.position = position
        self.every = every
        self.on_update = on_update
        self.data: dict | None = None
        try:
            self.data = json.loads(CACHE.read_text())
        except (OSError, ValueError):
            pass
        threading.Thread(target=self._run, daemon=True, name="weather").start()

    def _pos(self) -> tuple[float, float] | None:
        try:
            p = self.position()
        except Exception as e:
            log.debug("posição: %s", e)
            p = None
        if p:
            return round(p[0], 2), round(p[1], 2)
        if self.data and "lat" in self.data:
            return self.data["lat"], self.data["lon"]
        return None

    def fetch(self) -> bool:
        pos = self._pos()
        if not pos:
            log.info("previsão: sem posição (GPS sem fix e sem lat/lon no config)")
            return False
        url = API.format(lat=pos[0], lon=pos[1])
        req = urllib.request.Request(url, headers={"User-Agent": "deck3-clock/1.0"})
        with urllib.request.urlopen(req, timeout=15) as r:
            j = json.load(r)
        cur, day = j["current"], j["daily"]
        self.data = {
            "lat": pos[0], "lon": pos[1], "fetched": time.time(), "date": day["time"][0],
            "kind": wmo_kind(int(cur.get("weather_code", 3))),
            "t": round(cur.get("temperature_2m", 0)), "wind": round(cur.get("wind_speed_10m", 0)),
            "is_day": bool(cur.get("is_day", 1)),
            "max": round(day["temperature_2m_max"][0]), "min": round(day["temperature_2m_min"][0]),
            "rain": int(day.get("precipitation_probability_max", [0])[0] or 0),
            "sunrise": _hm(day["sunrise"][0]), "sunset": _hm(day["sunset"][0]),
            # próximas horas: [hora local "AAAA-MM-DDTHH:00", tipo, temperatura, % chuva, é dia]
            "hours": [[tm, wmo_kind(int(code if code is not None else 3)), round(tc if tc is not None else 0),
                       int(pp or 0), bool(dy)]
                      for tm, code, tc, pp, dy in zip(j.get("hourly", {}).get("time", []),
                                                      j.get("hourly", {}).get("weather_code", []),
                                                      j.get("hourly", {}).get("temperature_2m", []),
                                                      j.get("hourly", {}).get("precipitation_probability", []),
                                                      j.get("hourly", {}).get("is_day", []))],
        }
        try:
            CACHE.parent.mkdir(parents=True, exist_ok=True)
            CACHE.write_text(json.dumps(self.data))
        except OSError:
            pass
        log.info("previsão: %s %s° (%s/%s), chuva %s%%", self.data["kind"], self.data["t"],
                 self.data["max"], self.data["min"], self.data["rain"])
        return True

    def _run(self) -> None:
        while True:
            try:
                ok = self.fetch()
            except Exception as e:
                log.info("previsão indisponível: %s", e)
                ok = False
            if ok and self.on_update:
                self.on_update()
            time.sleep(self.every if ok else 300)

    def next_hours(self, n: int = 6) -> list[list]:
        """As n horas a partir da atual (a hora em curso primeiro), da previsão guardada."""
        d = self.current()
        if not d:
            return []
        now = time.strftime("%Y-%m-%dT%H:00")
        return [h for h in d.get("hours", []) if h[0] >= now][:n]

    def current(self) -> dict | None:
        """A previsão se tiver menos de 12 h, senão None."""
        d = self.data
        if d and time.time() - d.get("fetched", 0) < 12 * 3600:
            return d
        return None


# ------------------------------------------------------------------ desenho

def _hex(s: str) -> tuple[int, int, int]:
    return int(s[1:3], 16), int(s[3:5], 16), int(s[5:7], 16)


def _mix(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def _mul(c, k):
    return (c[0] * k, c[1] * k, c[2] * k)


def _clamp(v, a=0.0, b=1.0):
    return a if v < a else b if v > b else v


def sun_elev(m: int, sr: int, ss: int) -> float:
    """~ elevação do sol: 0 no nascer/pôr, 1 ao meio do dia, até -1 duas horas depois do pôr."""
    if sr <= m <= ss:
        return math.sin(math.pi * (m - sr) / (ss - sr))
    dist = min(sr - m, m + 1440 - ss) if m < sr else min(m - ss, sr + 1440 - m)
    return -min(1.0, dist / 120)


def moon_phase(epoch: float) -> float:
    ref = 947182440 / 86400   # lua nova de 2000-01-06 18:14 UTC
    return ((epoch / 86400 - ref) / 29.530588853) % 1


class _Canvas:
    def __init__(self, w: int, h: int):
        self.w, self.h = w, h
        self.p = [(0.0, 0.0, 0.0)] * (w * h)

    def px(self, x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            self.p[y * self.w + x] = c

    def bl(self, x, y, c, a):
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            i = y * self.w + x
            self.p[i] = _mix(self.p[i], c, a)

    def get(self, x, y):
        return self.p[int(_clamp(y, 0, self.h - 1)) * self.w + int(_clamp(x, 0, self.w - 1))]

    def rect(self, x, y, w, h, c):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.px(i, j, c)

    def disc(self, cx, cy, r, c, a=None):
        rr = r * r + r * 0.6
        for y in range(math.floor(cy - r - 1), math.ceil(cy + r + 2)):
            for x in range(math.floor(cx - r - 1), math.ceil(cx + r + 2)):
                if (x - cx) ** 2 + (y - cy) ** 2 <= rr:
                    self.px(x, y, c) if a is None else self.bl(x, y, c, a)


def _coast(cv: "_Canvas", w: int, h: int, HZ: int, L: float, G: float, cf: float, snowy: bool, bot, sun_x: float,
           e: float, epoch: float) -> None:
    """Costa com farol: mar com reflexo do céu, um cabo ao longe, falésia com farol (aceso à noite) e um veleiro."""
    shift = int(epoch / 300)
    # cabo ao longe, à esquerda, quase da cor do céu
    head = _mix(_mix(_hex("#1c2244"), _hex("#5d74a0"), L), bot, .45)
    for x in range(0, int(w * 0.42)):
        top = HZ - round(9 * math.sin(math.pi * x / (w * 0.42)) ** 0.7 + 2 * math.sin(x * .3))
        for y in range(max(0, top), HZ):
            cv.p[y * w + x] = head
    # mar: reflexo do céu, mais escuro e azulado, ondulado, com brilhos e o reflexo do sol
    deep = _mix(_hex("#0a1630"), _hex("#1f5c8a"), L)
    glint = _hex("#ffd890")
    for y in range(HZ, h):
        k = (y - HZ) / max(1, h - HZ)
        for x in range(w):
            off = round(math.sin(y * .7 + x * .05 + shift))
            src = cv.get(x + off, HZ - 1 - (y - HZ) * 2)
            c0 = _mix(_mix(_mul(src, .6), deep, .35 + .4 * k), _hex("#14284a"), .1)
            if (x * 5 + y * 11 + shift * 7) % 37 == 0:
                c0 = _mix(c0, (255, 255, 255), .3 * (1 - k))
            if abs(x - sun_x) < 4 - (y - HZ) / 12 and (y + shift) % 3 and e > -.05:
                c0 = _mix(c0, glint, .5 * (1 - cf))
            cv.p[y * w + x] = c0
        if y == HZ:
            for x in range(w):
                cv.p[y * w + x] = _mul(cv.p[y * w + x], 1.15)
    # falésia à direita
    x0 = int(w * 0.66)
    rock = _mix(_hex("#1d1a22"), _hex("#7b6a5c"), L)
    rock_l = _mix(_hex("#2a2530"), _hex("#a08a72"), L)
    grass = _mix(_hex("#66709a"), _hex("#e8ecf3"), L) if snowy else _mix(_hex("#15301f"), _hex("#5a9a48"), L)
    foam = _mix(_hex("#5a6488"), _hex("#f2f6fa"), L)
    tops = []
    for x in range(x0, w):
        u = (x - x0) / max(1, w - x0)
        top = HZ - 30 + round(4 * math.sin(x * .09) + 2 * math.sin(x * .27)) if u > .12 else HZ - 30 + round((.12 - u) * 160)
        tops.append(top)
        for y in range(top, h):
            if y < top + 3:
                c0 = grass
            elif (y - top) % 9 == 0:                       # camadas da rocha
                c0 = _mul(rock, .82)
            elif (x * 7919 + y * 104729) % 23 == 0:         # pintas soltas
                c0 = rock_l
            else:
                c0 = rock
            cv.p[y * w + x] = c0
    # espuma onde o mar bate na falésia
    for y in range(HZ, h):
        for k in range(1, 4):
            if (y * 3 + k + shift) % 4 and k <= 1 + (y + shift) % 3:
                cv.p[y * w + x0 - k] = foam
    # farol
    lx = int(w * 0.84)
    base = tops[lx - x0] if 0 <= lx - x0 < len(tops) else HZ - 30
    white = _mix(_hex("#5a5f70"), _hex("#f4f4f0"), L)
    red = _mix(_hex("#3a1418"), _hex("#c8382e"), L)
    dark = _mix(_hex("#101018"), _hex("#30323c"), L)
    tw, th = 8, 30
    for j in range(th):
        y = base - j
        half = tw // 2 - (1 if j > th * .6 else 0)
        band = red if (j // 6) % 2 else white
        cv.rect(lx - half, y, half * 2, 1, band)
    cv.rect(lx - 6, base - th, 12, 1, dark)                       # varanda
    lit = L < .55
    lamp = _hex("#ffe27a") if lit else _hex("#9fc3dd")
    cv.rect(lx - 3, base - th - 5, 6, 5, lamp)                    # lanterna
    cv.rect(lx - 4, base - th - 7, 8, 2, dark)                    # cúpula
    if lit:   # feixe de luz, suave, para a esquerda
        cv.disc(lx, base - th - 3, 6, _hex("#ffe27a"), .2)
        for k in range(1, 60):
            for dy in range(-k // 8, k // 8 + 1):
                cv.bl(lx - 3 - k, base - th - 3 + dy, _hex("#ffe9a0"), .14 * (1 - k / 60))
    # veleiro a atravessar devagar
    bx = int((epoch / 40) % (w * 0.6)) + 6
    by = HZ + 7
    hull = _mix(_hex("#2a1a14"), _hex("#6a4028"), L)
    sail = _mix(_hex("#7a8098"), _hex("#fbfbf6"), L)
    cv.rect(bx - 6, by, 12, 2, hull)
    for j in range(10):
        cv.rect(bx - j // 2, by - 2 - j, max(1, j // 2), 1, sail)
    cv.rect(bx, by - 12, 1, 12, hull)


def _city(cv: "_Canvas", w: int, h: int, HZ: int, L: float, G: float, cf: float, snowy: bool, bot, sun_x: float,
          e: float, epoch: float) -> None:
    """Cidade à beira-rio: prédios ao longe, prédios perto com janelas (acesas à noite), cais com candeeiros e
    o reflexo de tudo na água."""
    shift = int(epoch / 300)
    lit = L < .55
    far = _mix(_mix(_hex("#1a2042"), _hex("#6a7fa8"), L), bot, .4)
    near = _mix(_hex("#0c0f1e"), _hex("#3c4458"), L)
    near_l = _mix(_hex("#151a2e"), _hex("#58627a"), L)
    roof_snow = _mix(_hex("#6a7496"), _hex("#eef2fa"), L)
    glass = _mix(_hex("#1a2238"), _hex("#9cc0dc"), L)
    warm = _hex("#ffd36a")
    rb = random.Random(11)
    # prédios ao longe (silhueta clara, com algumas antenas)
    x = 0
    while x < w:
        bw, bh = rb.randint(10, 22), rb.randint(18, 44)
        cv.rect(x, HZ - 4 - bh, bw, bh, far)
        if rb.random() < .25:
            cv.rect(x + bw // 2, HZ - 4 - bh - 6, 1, 6, far)
        x += bw + rb.randint(-2, 1)
    # prédios perto: alturas que deixam livre o canto da hora (esquerda) e o do tempo (direita, em cima)
    x = -4
    while x < w:
        bw = rb.randint(14, 30)
        bh = rb.randint(26, 62 if w * .25 < x < w * .6 else 48)
        top = HZ - 4 - bh
        cv.rect(x, top, bw, bh, near)
        cv.rect(x, top, 1, bh, near_l)                               # aresta iluminada
        if snowy:
            cv.rect(x, top - 1, bw, 2, roof_snow)
        kind = rb.random()
        for wy in range(top + 4, HZ - 8, 5):                          # janelas 2x2 em grelha
            for wx in range(x + 3, x + bw - 3, 4):
                on = ((wx * 31 + wy * 17 + int(kind * 97)) * 2654435761 >> 7) % 100 < (38 if lit else 0)
                if on:
                    cv.rect(wx, wy, 2, 2, warm)
                elif kind < .5:
                    cv.rect(wx, wy, 2, 2, _mul(glass, .55 if lit else .9))
        if rb.random() < .2 and bh > 40:                             # luz vermelha no topo
            cv.px(x + bw // 2, top - 2, _hex("#ff4a3a") if lit else _hex("#a03030"))
        x += bw + rb.randint(1, 4)
    # cais e candeeiros
    quay = _mix(_hex("#141822"), _hex("#5a5a60"), L)
    cv.rect(0, HZ - 4, w, 4, quay)
    cv.rect(0, HZ - 4, w, 1, _mul(quay, 1.3))
    for lx in range(12, w, 38):
        cv.rect(lx, HZ - 12, 1, 8, _mul(quay, .7))
        cv.rect(lx - 1, HZ - 13, 3, 1, warm if lit else _hex("#9aa4b4"))
        if lit:
            cv.disc(lx, HZ - 12, 4, warm, .16)
    # água com o reflexo da cidade (as janelas acesas desenham riscos)
    water_tint = _hex("#101a34")
    glint = _hex("#ffd890")
    for y in range(HZ, h):
        k = (y - HZ) / max(1, h - HZ)
        for xx in range(w):
            off = round(math.sin(y * .8 + shift + xx * .02))
            c0 = _mix(_mul(cv.get(xx + off, HZ - 1 - (y - HZ) * 2), .6), water_tint, .25 + .3 * k)
            if (xx * 5 + y * 11 + shift * 7) % 41 == 0:
                c0 = _mix(c0, (255, 255, 255), .2 * (1 - k))
            if abs(xx - sun_x) < 3 - (y - HZ) / 9 and (y + shift) % 3 and e > -.05:
                c0 = _mix(c0, glint, .5 * (1 - cf))
            cv.p[y * w + xx] = c0
        if y == HZ:
            for xx in range(w):
                cv.p[y * w + xx] = _mul(cv.p[y * w + xx], .8)


def house_spot(w: int, h: int) -> tuple[int, int]:
    """Canto esquerdo e base da casa da paisagem do lago (o ESP desenha o cão ao lado)."""
    return w - 94, h - 22 - 5


def light(minute: int, sr: int, ss: int) -> float:
    """Luz do dia 0..1 (0 = noite), a mesma que escurece a paisagem."""
    return _clamp((sun_elev(minute, sr, ss) + .12) / .4)


def render_scene(w: int, h: int, minute: int, kind: str, sr: int, ss: int, epoch: float,
                 scene: str = "lago", winter: bool = False) -> Image.Image:
    """Paisagem sem a hora nem as partículas (essas são do ESP). Devolve uma imagem RGB w x h.
    scene: "lago", "costa" ou "cidade". winter: terra com neve mesmo sem nevar (dezembro a fevereiro)."""
    cv = _Canvas(w, h)
    shift = int(epoch / 300)
    # linha da água: o lago fica em baixo; na costa o mar começa a meio (a faixa da previsão fica por cima)
    HZ = h - 22 if scene in ("lago", "cidade") else int(h * 0.56)
    cf = CLOUD_FACTOR.get(kind, 0)
    e = sun_elev(minute, sr, ss)
    L = light(minute, sr, ss)
    G = _clamp(1 - abs(e) / .3) * (1 - cf * .6)
    snowy = kind == "neve" or winter
    top = _mix(_hex("#070b24"), _hex("#2f6fd0"), L)
    bot = _mix(_hex("#1c2350"), _hex("#a3d6f7"), L)
    top = _mix(top, _hex("#4a3a7a"), G * .5)
    bot = _mix(bot, _hex("#f2884a"), G * .85)

    def grey(c):
        lum = (c[0] * .3 + c[1] * .59 + c[2] * .11) * .82
        return (lum, lum, lum * 1.06)
    top = _mix(top, grey(top), cf)
    bot = _mix(bot, grey(bot), cf)

    # céu em faixas com dithering ordenado
    NB = 12
    bands = [_mix(top, bot, i / (NB - 1)) for i in range(NB)]
    for y in range(HZ):
        f = y / (HZ - 1) * (NB - 1)
        b = int(f)
        fr = f - b
        row = y * w
        for x in range(w):
            bi = b
            if bi < NB - 1 and ((fr > .75 and (x + y) % 2 == 0) or (fr > .5 and x % 2 == 0 and y % 2 == 0)):
                bi += 1
            cv.p[row + x] = bands[bi]

    rnd = random.Random(42)
    if L < .35 and cf < .65:
        for _ in range(int(46 * w / 320)):
            sx, sy, ph = rnd.random() * w, rnd.random() * (HZ - 54), rnd.random() * 6
            cv.bl(sx, sy, (255, 255, 240), (1 - L / .35) * (1 - cf) * (.55 + .45 * math.sin(ph)))

    amp = HZ - 22 - (26 if w > h else 140)   # o arco do sol/lua não sobe para a zona da previsão
    sun_x = -99.0
    if e > -.08:
        p = _clamp((minute - sr) / (ss - sr))
        sun_x = 16 + (w - 32) * p
        sy = HZ - 22 - amp * max(e, 0)
        col = _mix(_hex("#ff6a1c"), _hex("#ffb040"), _clamp(e * 3))   # laranja (mais forte junto ao horizonte)
        vis = 1 - cf * .85
        cv.disc(sun_x, sy, 11, col, .18 * vis)
        cv.disc(sun_x, sy, 7, col, vis)
    if e < .05:
        nl = 1440 - (ss - sr)
        p = ((minute - ss + 1440) % 1440) / nl
        if p <= 1:
            mx, my = 16 + (w - 32) * p, HZ - 22 - amp * .96 * math.sin(math.pi * p)
            ph = moon_phase(epoch)
            k = math.cos(2 * math.pi * ph)
            r, vis = 6, 1 - cf * .8
            for yy in range(-r, r + 1):
                for xx in range(-r, r + 1):
                    if xx * xx + yy * yy > r * r + r * .6:
                        continue
                    nx, ny = xx / r, yy / r
                    ww = math.sqrt(max(0, 1 - ny * ny))
                    lit = nx > k * ww if ph < .5 else nx < -k * ww
                    cv.bl(mx + xx, my + yy, _hex("#f6dc84") if lit else _hex("#5a6080"), (1 if lit else .35) * vis)

    n_cl = {"limpo": 0, "parcial": 3, "nublado": 7, "chuva": 9, "trovoada": 9, "neve": 6, "nevoeiro": 2}.get(kind, 0)
    cl = _mix(_hex("#3c4260"), _hex("#f5f7fc"), L)
    cl = _mix(cl, _hex("#ffb89a"), G * .55)
    if kind in ("chuva", "trovoada"):
        cl = _mul(cl, .62)
    cs = _mul(cl, .82)
    rc = random.Random(9)
    drift = epoch / 60
    for _ in range(n_cl):
        x0, y0 = rc.random() * (w + 80), (12 + rc.random() * 62) * HZ / 150
        cw, sp = 18 + rc.random() * 30, 2 + rc.random() * 4
        blobs = [(rc.random(), rc.random() * .5, .5 + rc.random() * .5) for _ in range(4)]
        cx = (x0 + drift * sp) % (w + 80) - 40
        for bx, by, br in blobs:
            r = cw * .32 * br
            bx0, by0 = cx + (bx - .5) * cw, y0 + by * 8
            for y in range(math.floor(by0 - r), math.ceil(by0 + r) + 1):
                for x in range(math.floor(bx0 - r), math.ceil(bx0 + r) + 1):
                    dx, dy = x - bx0, y - by0
                    if dx * dx + dy * dy <= r * r and y < HZ:
                        cv.px(x, y, cs if dy > r * .35 else cl)

    if scene == "lago":
        far = _mix(_mix(_hex("#1c2244"), _hex("#6178a6"), L), bot, .3)
        far_l = _mul(far, 1.12)
        snow_c = _mix(_hex("#6a7496"), _hex("#eef2fa"), L)
        hill = _mix(_hex("#5a6488"), _hex("#dfe5ee"), L) if snowy else _mix(_hex("#101e1c"), _hex("#3a6e40"), L)
        hill_l = _mul(hill, 1.1)
        mead = _mix(_hex("#66709a"), _hex("#e8ecf3"), L) if snowy else _mix(_hex("#16301f"), _hex("#5a9a48"), L)
        mead_d = _mul(mead, .85)
        snow_line = HZ - (42 if snowy else 58)
        far_raw = [HZ - 44 - (17 * math.sin(x * .022 + 1.3) + 9 * math.sin(x * .061 + .4) + 4 * math.sin(x * .14 + 2))
                   for x in range(w)]
        mid_raw = [HZ - 18 - (6 * math.sin(x * .035 + 2.1) + 4 * math.sin(x * .09) + 2 * math.sin(x * .21 + 1))
                   for x in range(w)]
        far_h = [round(v / 2) * 2 for v in far_raw]
        mid_h = [round(v) for v in mid_raw]
        rt = random.Random(7)
        tex = [rt.random() for _ in range(w * 6)]
        water_tint = _hex("#14284a")
        glint = _hex("#ffd890")
        shift = int(epoch / 300)
        for x in range(w):
            fh, mh = far_h[x], mid_h[x]
            rising = far_raw[x] < far_raw[max(0, x - 1)]   # encosta virada à luz (declive real, sem riscas)
            mrise = mid_raw[x] < mid_raw[max(0, x - 1)]
            for y in range(fh, h):
                if y >= HZ:
                    off = round(math.sin(y * .9 + shift))
                    c0 = _mix(_mul(cv.get(x + off, HZ - 1 - (y - HZ) * 2), .62), water_tint, .18)
                    if (x * 7 + y * 13 + shift * 3) % 29 == 0:
                        c0 = _mix(c0, (255, 255, 255), .25)
                    if abs(x - sun_x) < 3 - (y - HZ) / 9 and (y + shift) % 3 and e > -.05:
                        c0 = _mix(c0, glint, .55 * (1 - cf))
                    if y == HZ:
                        c0 = _mul(c0, .75)
                    cv.p[y * w + x] = c0
                elif y >= HZ - 6:
                    cv.p[y * w + x] = mead_d if tex[(y - HZ + 6) * w + x] > .8 else mead
                elif y >= mh:
                    cv.p[y * w + x] = hill_l if mrise and y < mh + 3 else hill
                else:
                    cap = y < snow_line + (x * 7) % 5 and y < fh + 14
                    cv.p[y * w + x] = snow_c if cap else (far_l if rising else far)

        tree = _mix(_hex("#0a1a14"), _hex("#24563a"), L)
        trunk = _mix(_hex("#20140c"), _hex("#5a3a22"), L)
        base = HZ - 5
        for tx in (26, 38, 54, w - 52, w - 36, w - 20):
            hgt = 10 if tx % 3 else 13
            cv.rect(tx, base - 2, 1, 3, trunk)
            for r in range(hgt - 2):
                wd = 1 + 2 * int(r / 2.2)
                cv.rect(tx - wd // 2, base - hgt + r, wd, 1, snow_c if snowy and r % 3 == 0 else tree)
        hx, _ = house_spot(w, h)
        wall = _mix(_hex("#3c3236"), _hex("#d6b892"), L)
        roof = snow_c if snowy else _mix(_hex("#3a1a20"), _hex("#b0473a"), L)
        cv.rect(hx, base - 8, 13, 8, wall)
        for r in range(7):
            cv.rect(hx - 1 + r, base - 9 - r, 15 - 2 * r, 1, roof)
        cv.rect(hx + 10, base - 15, 2, 4, _mul(wall, .7))
        cv.rect(hx + 2, base - 4, 2, 4, _mul(wall, .45))
        lit = L < .55
        cv.rect(hx + 7, base - 6, 3, 2, _hex("#ffd36a") if lit else _hex("#8fb6d4"))
        if lit:
            cv.disc(hx + 8, base - 5, 4, _hex("#ffd36a"), .12)
    elif scene == "cidade":
        _city(cv, w, h, HZ, L, G, cf, snowy, bot, sun_x, e, epoch)
    else:
        _coast(cv, w, h, HZ, L, G, cf, snowy, bot, sun_x, e, epoch)

    if kind == "nevoeiro":
        fc = _mix(_hex("#2c3044"), _hex("#cdd2dc"), L)
        y0 = max(0, HZ - 90)
        for y in range(y0, h):
            for x in range(w):
                a = _clamp(.18 + .42 * (y - y0) / 110 + .1 * math.sin(y * .35 + shift * .6 + x * .015))
                cv.bl(x, y, fc, a)

    img = Image.new("RGB", (w, h))
    img.putdata([(int(c[0]), int(c[1]), int(c[2])) for c in cv.p])
    return img


def scene_bg(key: str, img: Image.Image):
    """Imagem RGB -> (key, w, h, paleta de 256 "#rrggbb", bytes 8 bpp) para App.send_bg."""
    q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:768]
    pal += [0] * (768 - len(pal))
    colors = [f"#{pal[i]:02x}{pal[i + 1]:02x}{pal[i + 2]:02x}" for i in range(0, 768, 3)]
    return key, img.width, img.height, colors, q.tobytes()


# ------------------------------------------------------------------ ícones 16x16

IC = {k: _hex(v) for k, v in {
    "sun": "#ffd23f", "sunO": "#f49a00", "cw": "#eef1f8", "cs": "#a9b3c8", "cd": "#6f7790", "cds": "#4d5368",
    "drop": "#5aaaff", "bolt": "#ffe14d", "snow": "#ffffff", "fog": "#c3c8d4", "moon": "#f2dc8a",
    "moonS": "#b8a060", "hl": "#ffffff", "ol": "#1c2236", "cb": "#8a93ad", "cbs": "#5f6782"}.items()}


def weather_icon(kind: str, night: bool) -> dict:
    """Ícone 16x16 como {"pal": [...], "role": [...], "d": 256 dígitos} (0 = transparente)."""
    g: list = [None] * 256

    def p(x, y, c, r=1):
        if 0 <= x < 16 and 0 <= y < 16:
            g[y * 16 + x] = (c, r)

    def circ(cx, cy, r, c=None, role=1, erase=False):
        for y in range(16):
            for x in range(16):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r * .6:
                    if erase:
                        g[y * 16 + x] = None
                    else:
                        p(x, y, c, role)

    def sun(cx, cy, r):
        circ(cx, cy, r, "sun")
        for y in range(16):
            for x in range(16):
                dx, dy = x - cx, y - cy
                dd = dx * dx + dy * dy
                if r * r + r * .6 >= dd > (r - 1) ** 2 + (r - 1) * .6 and dx + dy > 0:
                    p(x, y, "sunO", 2)
        for k in range(8):
            a = k * math.pi / 4
            for rr in (r + 2, r + 3):
                p(round(cx + math.cos(a) * rr), round(cy + math.sin(a) * rr), "sunO" if k % 2 else "sun",
                  2 if k % 2 else 1)

    def moon(cx, cy, r):
        circ(cx, cy, r, "moon")
        circ(cx + 3, cy - 2, r - 1, erase=True)
        p(cx - 2, cy + 2, "moonS", 2)
        p(cx - 3, cy, "moonS", 2)
        p(13, 3, "moon")
        p(14, 9, "moonS", 2)

    def mask(circles, box):
        """Píxeis de uma nuvem: círculos (cx, cy, r) mais um retângulo (x0, x1, y0, y1)."""
        m = set()
        for cx, cy, r in circles:
            for y in range(16):
                for x in range(16):
                    if (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r * .6:
                        m.add((x, y))
        x0, x1, y0, y1 = box
        m |= {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}
        return {(x, y) for x, y in m if 0 <= x < 16 and 0 <= y < 16}

    def puff(m, body, shade, hl=True):
        """Nuvem com volume: brilho no topo à esquerda, sombra nas 2 linhas de baixo e, por cima do que já
        está desenhado (lua, sol, outra nuvem), um contorno escuro que separa os dois."""
        for x, y in m:
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, -1)):
                q = (x + dx, y + dy)
                if q not in m and 0 <= q[0] < 16 and 0 <= q[1] < 16 and g[q[1] * 16 + q[0]] is not None \
                        and g[q[1] * 16 + q[0]][0] != "ol":
                    p(q[0], q[1], "ol", 2)
        bottom = {}
        for x, y in m:
            bottom[x] = max(bottom.get(x, -1), y)
        cx = sum(x for x, _ in m) / max(1, len(m))
        for x, y in m:
            c = body
            if y >= bottom[x] - 1:
                c = shade
            elif hl and (x, y - 1) not in m and x <= cx + 1:
                c = "hl"
            p(x, y, c, 2 if c == shade else 1)

    def cloud(oy, c, s):
        puff(mask([(5, 10 + oy, 3), (9, 7 + oy, 4), (12, 10 + oy, 3)], (3, 14, 10 + oy, 13 + oy)), c, s,
             hl=c == "cw")

    if kind == "limpo":
        moon(7, 8, 5) if night else sun(8, 8, 4)
    elif kind == "parcial":
        if night:
            circ(10, 4, 4, "moon")       # quarto crescente mais à vista por cima da nuvem
            circ(13, 3, 3, erase=True)
            p(8, 5, "moonS", 2)
        else:
            sun(10, 5, 3)
        puff(mask([(4, 11, 2.4), (7, 9, 3), (11, 11, 2.4)], (2, 13, 11, 14)), "cw", "cs")
    elif kind == "nublado":   # nuvem de trás mais escura, a da frente branca com contorno
        puff(mask([(9, 4, 3), (12, 3, 3.2), (14, 6, 2)], (7, 15, 5, 7)), "cb", "cbs", hl=False)
        puff(mask([(4, 11, 2.6), (7, 9, 3.2), (11, 11, 2.6)], (2, 13, 11, 14)), "cw", "cs")
    elif kind == "chuva":
        cloud(-3, "cs", "cd")
        for x, y in ((4, 12), (8, 13), (12, 12), (6, 14), (10, 15)):
            p(x, y, "drop")
            p(x - 1, y + 1, "drop")
    elif kind == "trovoada":
        cloud(-3, "cd", "cds")
        for x, y in ((9, 10), (8, 11), (7, 12), (8, 12), (9, 12), (8, 13), (7, 14), (6, 15)):
            p(x, y, "bolt")
        for x, y in ((4, 12), (3, 13), (13, 12), (12, 13)):
            p(x, y, "drop")
    elif kind == "neve":
        cloud(-3, "cw", "cs")
        for x, y in ((4, 12), (8, 13), (12, 12), (6, 15), (11, 15)):
            p(x, y, "snow")
        for x, y in ((8, 12), (8, 14), (7, 13), (9, 13)):
            p(x, y, "cs", 2)
    elif kind == "nevoeiro":
        for y, a, b in ((3, 2, 13), (6, 0, 9), (6, 11, 15), (9, 3, 14), (12, 1, 7), (12, 9, 13)):
            for x in range(a, b + 1):
                p(x, y, "fog")
                p(x, y + 1, "cs", 2)

    names: list[str] = []
    roles: list[int] = []
    out = []
    for v in g:
        if v is None:
            out.append("0")
            continue
        c, r = v
        key = f"{c}:{r}"
        if key not in names:
            names.append(key)
            roles.append(r)
        out.append(str(names.index(key) + 1))
    pal = ["#%02x%02x%02x" % IC[n.split(":")[0]] for n in names]
    return {"pal": pal[:7], "role": roles[:7], "d": "".join(out)}
