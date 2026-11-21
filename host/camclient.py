"""Cliente do CamDeck (camd, câmara XIAO ESP32-S3 Sense no uConsole) para a box Câmara.

O camd pede palavra-passe aos browsers; os programas do próprio uConsole entram com o token local
de ~/.config/camdeck/local-token (cabeçalho X-CamDeck-Token, só aceite a partir de 127.0.0.1).
"""
from __future__ import annotations

import json
import urllib.error
import urllib.request
from pathlib import Path

TOKEN = Path.home() / ".config" / "camdeck" / "local-token"


class CamError(Exception):
    pass


class CamClient:
    def __init__(self, url: str = "http://127.0.0.1:8090", token_file: str | Path = TOKEN):
        self.url = url.rstrip("/")
        self.token_file = Path(token_file).expanduser()

    def _token(self) -> str:
        try:
            return self.token_file.read_text().strip()
        except OSError:
            raise CamError("sem token do CamDeck (o camd já arrancou?)") from None

    def _req(self, path: str, data: dict | None = None, timeout: float = 5.0, hdrs: dict | None = None) -> bytes:
        body = json.dumps(data).encode() if data is not None else None
        req = urllib.request.Request(self.url + path, data=body, method="POST" if data is not None else "GET",
                                     headers={"X-CamDeck-Token": self._token(),
                                              "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                if hdrs is not None:
                    hdrs.update(r.headers.items())
                return r.read()
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read()).get("error") or e.reason
            except (ValueError, OSError):
                msg = e.reason
            if e.code == 401:
                msg = "o camd recusou o token (versão antiga do CamDeck?)"
            raise CamError(str(msg)) from None
        except (urllib.error.URLError, OSError) as e:
            raise CamError(f"CamDeck sem resposta ({getattr(e, 'reason', e)})") from None

    def _json(self, path: str, data: dict | None = None, timeout: float = 5.0) -> dict:
        return json.loads(self._req(path, data, timeout))

    def state(self) -> dict:
        return self._json("/api/state", timeout=3)

    def frame(self, after: int = -1) -> tuple[int, bytes]:
        """(número, JPEG) do quadro seguinte a `after` (o camd espera por ele), na resolução do stream."""
        h: dict = {}
        data = self._req(f"/api/frame.jpg?after={after}", timeout=7, hdrs=h)
        return int(h.get("X-Frame") or -1), data

    def thumb(self, kind: str, name: str) -> bytes:
        return self._req(f"/thumb/{kind}/{name}", timeout=6)

    def snap(self) -> dict:
        return self._json("/api/snap", {}, timeout=15)

    def record(self, on: bool) -> dict:
        return self._json("/api/record", {"on": on}, timeout=10)

    def set(self, key: str, val) -> dict:
        return self._json("/api/set", {"key": key, "val": val})

    def prefs(self, **kw) -> dict:
        return self._json("/api/prefs", kw)
