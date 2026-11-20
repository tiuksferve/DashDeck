"""Janelas abertas no uConsole (labwc/wlroots) pelo protocolo zwlr_foreign_toplevel_manager_v1.

Cliente Wayland mínimo em Python puro (só o socket e o formato das mensagens), sem bibliotecas
extra. Cada chamada abre uma ligação curta: os identificadores das janelas não sobrevivem entre
ligações, por isso uma janela é identificada pela posição na lista mais o app_id e o título.
O protocolo é o mesmo que a barra de tarefas (wf-panel-pi) usa.
"""
from __future__ import annotations

import os
import socket
import struct
import time

MANAGER = "zwlr_foreign_toplevel_manager_v1"
# pedidos do zwlr_foreign_toplevel_handle_v1
REQ_SET_MIN, REQ_UNSET_MIN, REQ_ACTIVATE, REQ_CLOSE = 2, 3, 4, 5
# valores do evento state
ST_MAXIMIZED, ST_MINIMIZED, ST_ACTIVATED, ST_FULLSCREEN = 0, 1, 2, 3


class WaylandError(Exception):
    pass


def _socket_path() -> str:
    run = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    disp = os.environ.get("WAYLAND_DISPLAY") or "wayland-0"
    return disp if disp.startswith("/") else os.path.join(run, disp)


def _str(text: str) -> bytes:
    b = text.encode() + b"\0"
    return struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)


def _get_str(body: bytes, off: int = 0) -> tuple[str, int]:
    n, = struct.unpack_from("<I", body, off)
    s = body[off + 4:off + 4 + max(0, n - 1)].decode("utf-8", "replace")
    return s, off + 4 + ((n + 3) & ~3)


class _Conn:
    """Uma ligação: liga, junta as janelas e (opcionalmente) envia pedidos antes de fechar."""

    def __init__(self, timeout: float = 2.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        try:
            self.sock.connect(_socket_path())
        except OSError as e:
            self.sock.close()
            raise WaylandError(f"sem compositor Wayland ({e})") from e
        self.next_id = 2
        self.buf = b""
        self.globals: dict[str, tuple[int, int]] = {}
        self.wins: dict[int, dict] = {}     # id do handle -> janela
        self.order: list[int] = []
        self.seat = 0

    def close(self):
        self.sock.close()

    def new_id(self) -> int:
        i = self.next_id
        self.next_id += 1
        return i

    def send(self, obj: int, op: int, payload: bytes = b"") -> None:
        self.sock.sendall(struct.pack("<II", obj, ((8 + len(payload)) << 16) | op) + payload)

    def roundtrip(self) -> None:
        cb = self.new_id()
        self.send(1, 0, struct.pack("<I", cb))   # wl_display.sync
        deadline = time.monotonic() + 2.0
        while True:
            if time.monotonic() > deadline:
                raise WaylandError("o compositor não respondeu")
            try:
                data = self.sock.recv(65536)
            except socket.timeout as e:
                raise WaylandError("o compositor não respondeu") from e
            if not data:
                raise WaylandError("ligação fechada pelo compositor")
            self.buf += data
            while len(self.buf) >= 8:
                obj, so = struct.unpack_from("<II", self.buf)
                size, op = so >> 16, so & 0xFFFF
                if size < 8 or len(self.buf) < size:
                    break
                body, self.buf = self.buf[8:size], self.buf[size:]
                if obj == cb:
                    return
                self._event(obj, op, body)

    def _event(self, obj: int, op: int, body: bytes) -> None:
        if obj == 1 and op == 0:   # wl_display.error
            _, code = struct.unpack_from("<II", body)
            msg, _ = _get_str(body, 8)
            raise WaylandError(f"erro do compositor {code}: {msg}")
        if obj == self.registry and op == 0:   # global
            name, = struct.unpack_from("<I", body)
            iface, off = _get_str(body, 4)
            ver, = struct.unpack_from("<I", body, off)
            self.globals[iface] = (name, ver)
            return
        if obj == getattr(self, "manager", -1):
            if op == 0:   # toplevel(new_id)
                h, = struct.unpack_from("<I", body)
                self.wins[h] = {"title": "", "app_id": "", "state": set(), "parent": 0}
                self.order.append(h)
            return
        w = self.wins.get(obj)
        if w is None:
            return
        if op == 0:
            w["title"] = _get_str(body)[0]
        elif op == 1:
            w["app_id"] = _get_str(body)[0]
        elif op == 4:
            n, = struct.unpack_from("<I", body)
            w["state"] = set(struct.unpack_from(f"<{n // 4}I", body, 4))
        elif op == 6:   # closed
            self.wins.pop(obj, None)
            if obj in self.order:
                self.order.remove(obj)
        elif op == 7:
            w["parent"], = struct.unpack_from("<I", body)

    def bind(self, iface: str, maxver: int) -> int:
        if iface not in self.globals:
            raise WaylandError(f"o compositor não tem {iface}")
        name, ver = self.globals[iface]
        oid = self.new_id()
        self.send(self.registry, 0, struct.pack("<I", name) + _str(iface) +
                  struct.pack("<II", min(ver, maxver), oid))
        return oid

    def collect(self, with_seat: bool = False) -> list[tuple[int, dict]]:
        self.registry = self.new_id()
        self.send(1, 1, struct.pack("<I", self.registry))   # wl_display.get_registry
        self.roundtrip()
        self.manager = self.bind(MANAGER, 3)
        if with_seat:
            self.seat = self.bind("wl_seat", 1)
        self.roundtrip()
        self.roundtrip()   # os eventos de cada janela chegam depois de o handle ser criado
        return [(h, self.wins[h]) for h in self.order if h in self.wins]


def _view(i: int, w: dict) -> dict:
    st = w["state"]
    return {"idx": i, "title": w["title"], "app_id": w["app_id"], "child": bool(w["parent"]),
            "active": ST_ACTIVATED in st, "minimized": ST_MINIMIZED in st,
            "maximized": ST_MAXIMIZED in st, "fullscreen": ST_FULLSCREEN in st}


def windows() -> list[dict]:
    """Janelas abertas, pela ordem do compositor (a mais antiga primeiro)."""
    c = _Conn()
    try:
        return [_view(i, w) for i, (_, w) in enumerate(c.collect())]
    finally:
        c.close()


def act(idx: int, app_id: str, title: str, action: str) -> bool:
    """action: activate | minimize | close. Devolve False se a janela já não existir."""
    c = _Conn()
    try:
        wins = c.collect(with_seat=(action == "activate"))
        cand = [h for h, w in wins if w["app_id"] == app_id and w["title"] == title]
        if not cand:   # o título pode ter mudado (ex.: terminal): tenta pelo app_id e posição
            cand = [h for i, (h, w) in enumerate(wins) if w["app_id"] == app_id and i == idx] or \
                   [h for h, w in wins if w["app_id"] == app_id][:1]
        if not cand:
            return False
        h = cand[0] if len(cand) == 1 else next((x for i, (x, _) in enumerate(wins) if x in cand and i == idx),
                                                cand[0])
        if action == "activate":
            if ST_MINIMIZED in c.wins[h]["state"]:
                c.send(h, REQ_UNSET_MIN)
            c.send(h, REQ_ACTIVATE, struct.pack("<I", c.seat))
        elif action == "minimize":
            c.send(h, REQ_SET_MIN)
        elif action == "close":
            c.send(h, REQ_CLOSE)
        else:
            raise ValueError(action)
        c.roundtrip()
        return True
    finally:
        c.close()


if __name__ == "__main__":
    for w in windows():
        print(w)
