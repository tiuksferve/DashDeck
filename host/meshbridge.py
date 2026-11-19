"""Cliente da API local do MeshDeckWeb (/api/local/*), a ponte para o Meshtastic.

O meshtasticd só aceita um cliente na API TCP 4403 e esse cliente é o MeshDeckWeb; o dashd pede-lhe
os dados por HTTP em 127.0.0.1 em vez de abrir outra ligação ao nó.
"""
from __future__ import annotations

import json
import logging
import threading
import time
import urllib.error
import urllib.request

log = logging.getLogger("dashd.mesh")

BASE = "http://127.0.0.1:8080"


class MeshBridge:
    def __init__(self, base: str = BASE, ttl: float = 2.0):
        self.base = base.rstrip("/")
        self.ttl = ttl
        self._data: dict | None = None
        self._t = 0.0
        self._gps: dict | None = None
        self._gps_t = 0.0
        self.error = ""
        self._lock = threading.Lock()

    def _req(self, path: str, body: dict | None = None, timeout: float = 3.0) -> dict:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data,
                                     headers={"Content-Type": "application/json"} if data else {})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())

    def dash(self, force: bool = False) -> dict | None:
        """Resumo do MeshDeckWeb (cache de ttl s). None se o MeshDeckWeb não responder."""
        with self._lock:
            if not force and self._data is not None and time.monotonic() - self._t < self.ttl:
                return self._data
            try:
                self._data = self._req("/api/local/dash")
                self.error = ""
            except urllib.error.HTTPError as e:
                self._data = None
                self.error = ("API local desligada no MeshDeckWeb (--local-api on)" if e.code == 404
                              else f"HTTP {e.code}")
            except (OSError, ValueError) as e:
                self._data = None
                self.error = "MeshDeckWeb parado"
                log.debug("ponte: %s", e)
            self._t = time.monotonic()
            return self._data

    def gps(self, ttl: float = 1.0) -> dict | None:
        """GPS do uConsole ao vivo no MeshDeckWeb (/api/local/gps, cache de ttl s).

        None se o MeshDeckWeb não responder ou não tiver essa rota (versão antiga).
        """
        with self._lock:
            if time.monotonic() - self._gps_t < ttl:
                return self._gps
            try:
                self._gps = self._req("/api/local/gps", timeout=1.5)
            except (OSError, ValueError, urllib.error.HTTPError):
                self._gps = None
            self._gps_t = time.monotonic()
            return self._gps

    def send(self, text: str, channel: int = 0, dest: str | None = None) -> tuple[bool, str]:
        body = {"text": text, "dest": dest} if dest else {"text": text, "channel": channel}
        try:
            self._req("/api/local/send", body)
            self._t = 0.0
            return True, ""
        except urllib.error.HTTPError as e:
            try:
                return False, json.loads(e.read()).get("error", f"HTTP {e.code}")
            except ValueError:
                return False, f"HTTP {e.code}"
        except OSError as e:
            return False, "MeshDeckWeb parado"

    def mark_read(self, key: str | None = None) -> None:
        try:
            self._req("/api/local/read", {"key": key} if key else {"all": True})
            self._t = 0.0
        except (OSError, ValueError):
            pass

    def position(self) -> tuple[float, float, float | None] | None:
        d = self.dash()
        p = (d or {}).get("local_position") or {}
        if p.get("found") and p.get("lat") is not None and p.get("lon") is not None:
            return float(p["lat"]), float(p["lon"]), p.get("alt")
        return None


_shared: MeshBridge | None = None


def shared() -> MeshBridge:
    global _shared
    if _shared is None:
        _shared = MeshBridge()
    return _shared
