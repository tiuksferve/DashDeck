"""Rotas dos voos (origem e destino) pelo indicativo, no hexdb.io.

O ADS-B não transmite a rota: só o indicativo (ex.: TAP1352). O hexdb.io devolve a rota registada
para esse indicativo ("LPPT-EGLL") e os dados de cada aeroporto. Só o indicativo sai do uConsole
(nunca a posição). As respostas ficam em ~/.cache/dashd/routes.json: rotas 24 h, "sem rota" 6 h,
aeroportos para sempre. As consultas correm numa thread, uma de cada vez, sem atrasar a tela.
"""
from __future__ import annotations

import json
import logging
import queue
import re
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

log = logging.getLogger("dashd.routes")

API = "https://hexdb.io/api/v1"
CACHE = Path.home() / ".cache" / "dashd" / "routes.json"
ROUTE_TTL, MISS_TTL = 24 * 3600, 6 * 3600
OLD = 2 * 365 * 86400          # rota registada há mais de 2 anos: mostrada a cinza
VALID = re.compile(r"^[A-Z]{3}[0-9][0-9A-Z]{0,4}$")   # voo comercial: 3 letras ICAO + número


def short_name(name: str) -> str:
    """Nome curto de um aeroporto (sem "International Airport" e afins)."""
    for w in (" International Airport", " Intl Airport", " Airport", " International", " Airfield", " Aerodrome"):
        name = name.replace(w, "")
    return name.strip()


class Routes:
    def __init__(self, enabled: bool = True, interval: float = 1.0):
        self.enabled = enabled
        self.interval = interval
        self.routes: dict[str, dict] = {}     # indicativo -> {"route": "LPPT-EGLL" | "", "upd": int, "t": float}
        self.airports: dict[str, dict] = {}   # ICAO -> {"iata","name","city","cc"} ou {} se desconhecido
        self._q: queue.Queue = queue.Queue()
        self._pending: set[str] = set()
        self._dirty = False
        self._lock = threading.Lock()
        self._load()
        if enabled:
            threading.Thread(target=self._worker, daemon=True, name="rotas").start()

    # ---- cache em disco
    def _load(self) -> None:
        try:
            d = json.loads(CACHE.read_text())
            self.routes, self.airports = d.get("routes", {}), d.get("airports", {})
        except (OSError, ValueError):
            pass

    def _save(self) -> None:
        with self._lock:
            now = time.time()
            routes = {k: v for k, v in self.routes.items() if now - v.get("t", 0) < ROUTE_TTL}
            data = json.dumps({"routes": routes, "airports": self.airports})
        try:
            CACHE.parent.mkdir(parents=True, exist_ok=True)
            tmp = CACHE.with_suffix(".tmp")
            tmp.write_text(data)
            tmp.replace(CACHE)
        except OSError as e:
            log.debug("rotas: não gravou a cache: %s", e)

    # ---- consultas
    def _get(self, path: str):
        req = urllib.request.Request(API + path, headers={"User-Agent": "dashd"})
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                body = r.read().decode("utf-8", "replace").strip()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise
        if not body or body.lower() in ("n/a", "null"):
            return None
        return json.loads(body)

    def _fetch(self, cs: str) -> None:
        d = self._get(f"/route/icao/{cs}")
        route = (d or {}).get("route") or ""
        with self._lock:
            self.routes[cs] = {"route": route if re.fullmatch(r"[A-Z0-9]{4}(-[A-Z0-9]{4})+", route) else "",
                               "upd": int((d or {}).get("updatetime") or 0), "t": time.time()}
        for icao in self.routes[cs]["route"].split("-") if self.routes[cs]["route"] else []:
            if icao not in self.airports:
                time.sleep(self.interval)
                a = self._get(f"/airport/icao/{icao}") or {}
                with self._lock:
                    self.airports[icao] = {"iata": a.get("iata") or "", "name": short_name(a.get("airport") or ""),
                                           "city": a.get("region_name") or "", "cc": a.get("country_code") or ""}
        self._dirty = True

    def _worker(self) -> None:
        last_save = time.monotonic()
        while True:
            try:
                cs = self._q.get(timeout=30)
            except queue.Empty:
                cs = None
            if cs:
                try:
                    self._fetch(cs)
                except (OSError, ValueError) as e:   # sem rede: tenta outra vez mais tarde
                    log.debug("rotas: %s: %s", cs, e)
                    with self._lock:
                        self.routes[cs] = {"route": "", "upd": 0, "t": time.time() - MISS_TTL + 600}
                self._pending.discard(cs)
                time.sleep(self.interval)
            if self._dirty and time.monotonic() - last_save > 20:
                self._dirty = False
                last_save = time.monotonic()
                self._save()

    # ---- uso pelo provider
    def want(self, callsign: str) -> None:
        """Pede a rota (em segundo plano) se ainda não estiver na cache."""
        cs = (callsign or "").strip().upper()
        if not self.enabled or not VALID.match(cs) or cs in self._pending:
            return
        r = self.routes.get(cs)
        if r and time.time() - r["t"] < (ROUTE_TTL if r["route"] else MISS_TTL):
            return
        self._pending.add(cs)
        self._q.put(cs)

    def get(self, callsign: str) -> dict | None:
        """{"from": aeroporto, "to": aeroporto, "via": [..], "old": bool} ou None (sem rota conhecida)."""
        r = self.routes.get((callsign or "").strip().upper())
        if not r or not r["route"]:
            return None
        legs = r["route"].split("-")
        ap = [dict(self.airports.get(i) or {}, icao=i) for i in legs]
        return {"from": ap[0], "to": ap[-1], "via": ap[1:-1],
                "old": bool(r["upd"]) and time.time() - r["upd"] > OLD}

    @staticmethod
    def code(ap: dict) -> str:
        return ap.get("iata") or ap["icao"]

    def short(self, callsign: str) -> str:
        """Ex.: "LIS » LHR" (o » existe na fonte do ESP; a seta não)."""
        r = self.get(callsign)
        return f"{self.code(r['from'])} » {self.code(r['to'])}" if r else ""

    @staticmethod
    def place(ap: dict) -> str:
        """Ex.: "LIS · Lisbon Portela, PT"."""
        code = ap.get("iata") or ap["icao"]
        name = ap.get("name") or ap.get("city") or ""
        return f"{code} · {name}" + (f", {ap['cc']}" if ap.get("cc") else "") if name else code
