"""O Deck³ no motor do DashDeck: transporte WebSocket, servidor e o que é só do cubo.

O Deck³ (ESP32-S3 AMOLED em cubo) liga-se ao uConsole por Wi-Fi. É mais uma sessão do mesmo motor
(dashd.App), ao lado do DashDeck pela USB; os providers são os mesmos. Aqui está:

  - WsLink: o transporte da sessão do cubo (WebSocket com token, a partir da thread do asyncio);
  - Server: GET /deck?id=..&token=.. (WebSocket do aparelho), GET /fw/deck3.bin (OTA, só durante uma
    atualização pedida pelo deckctl) e POST /ctl (controlo local, só 127.0.0.1: ver deckctl.py);
  - DeckMixin: o que muda quando a sessão atual é o cubo (tamanho da tela, hora com fuso, botões físicos,
    bateria, eventos de fora de casa, sliders, cartão SD, OTA, box Config).

Protocolo: docs/deck3/protocolo.md. Configuração: secções [deckd], [keys] e [about] do config.toml.
"""
from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import queue
import secrets
import threading
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import ws
from session import Session, SessionEvents

log = logging.getLogger("deckd")
# o token e o estado do cubo continuam nas pastas do Deck³ até à fase 5 da união (docs/deck3/unificacao.md)
CONF_DIR = Path.home() / ".config" / "deck3"
STATE_DIR = Path.home() / ".local" / "state" / "deck3"
TOKEN_FILE = CONF_DIR / "token"


def load_token() -> str:
    """Token partilhado com o aparelho (gravado nele pelo setup/provision.py)."""
    CONF_DIR.mkdir(parents=True, exist_ok=True)
    if not TOKEN_FILE.exists():
        TOKEN_FILE.write_text(secrets.token_hex(16) + "\n")
        TOKEN_FILE.chmod(0o600)
    return TOKEN_FILE.read_text().strip()


def posix_tz() -> str:
    """A regra POSIX do fuso (última linha do /etc/localtime, formato TZif v2+): o ESP calcula a hora local
    e a mudança de hora sozinho."""
    try:
        tail = Path("/etc/localtime").read_bytes().rstrip(b"\n").rsplit(b"\n", 1)[-1].decode()
        return tail or "UTC0"
    except OSError:
        return "UTC0"


# --------------------------------------------------------------------------- transporte

class WsLink:
    """Transporte do motor (send, acks, jpg_acks) por WebSocket, a partir da thread do asyncio."""

    def __init__(self, events):
        self.events = events   # session.SessionEvents: os eventos saem marcados com a sessão do Deck³
        self.acks: queue.Queue = queue.Queue()
        self.jpg_acks: queue.Queue = queue.Queue()
        self.loop: asyncio.AbstractEventLoop | None = None
        self.sock: ws.WebSocket | None = None
        self.dev_id = ""
        self.local_ip = ""
        self.safe = None   # engine.lcd_safe (texto adaptado às fontes do aparelho)

    def send(self, msg: dict, raw: bool = False) -> None:
        if not self.sock or not self.loop:
            return
        data = json.dumps(msg if raw or not self.safe else self.safe(msg), ensure_ascii=False, separators=(",", ":"))
        sock = self.sock
        asyncio.run_coroutine_threadsafe(self._send(sock, data), self.loop)

    async def _send(self, sock, data: str) -> None:
        try:
            await sock.send(data)
        except Exception as e:
            log.debug("envio falhou: %s", e)


class Dev:
    """O aparelho ligado, visto pelos providers do Deck³ (self.session nos providers do deckproviders.py)."""

    def __init__(self, app, sess: Session):
        self.app = app
        self.sess = sess
        self.id = ""
        self.hello: dict = {}
        self.last: dict[str, dict] = {}

    def send(self, msg: dict) -> None:
        self.sess.link.send(msg)

    def set_bl(self, v: int) -> None:
        self.hello["bl"] = v
        self.sess.backlight = v
        self.send({"t": "bl", "v": v})

    def show(self) -> None:
        self.sess.link.events.put(("refresh", None))


class DeckMixin:
    """O que é do Deck³ no motor. Cada método só muda o comportamento quando a sessão atual é a do cubo
    (self.session.kind == "deck3"); para o DashDeck passa ao motor (super())."""
    DECK_MSGS = {"key", "imu", "mic", "bat", "bt", "wifi_scan", "bat_low", "offline_log", "cfg", "sd", "ls",
                 "file", "rm", "slider", "ota", "pong"}

    def __init__(self, cfg: dict):
        super().__init__(cfg)
        self.deck_session: Session | None = None
        d = cfg.get("deckd")
        if not d or not d.get("enabled", True):
            return
        import deckproviders
        import providers
        from dashd import lcd_safe
        # menu do cubo: o mesmo (os itens com devices = [...] escolhem o ecrã), mais os do dock do DashDeck,
        # que o cubo não tem (menos o Início e o brilho, que estão nos botões físicos)
        menu = cfg.get("menu", [])
        ids = {i.get("id") for i in menu}
        extra = [i for i in cfg.get("dock", []) if i.get("type") not in ("home", "backlight") and i.get("id") not in ids]
        sess = Session(kind="deck3", emojis=False, layout="grid", dock_screens=set(), screen_off=0,
                       refresh=float(d.get("refresh", 2.0)), backlight=int(d.get("backlight", 80)), rotation=0)
        sess.root = {"id": "home", "type": "submenu", "label": d.get("title", "Deck³"), "items": menu + extra}
        sess.stack = [sess.root]
        sess.link = WsLink(SessionEvents(self.events, sess))
        sess.link.safe = lcd_safe
        sess.dev = Dev(self, sess)
        self.sessions.append(sess)
        self.deck_session = sess
        self.cfg_deck = d
        self.tz = posix_tz()
        self.keys = {"boot_short": "back", "boot_long": "home", "user_short": "open:config", "pwr_short": "bl",
                     **cfg.get("keys", {})}
        self.bl_levels = d.get("bl_levels", [20, 50, 80, 100])
        self.fw_path: Path | None = None
        self._offline_n = 0
        self._bat_logged = 0.0
        for name, cls in deckproviders.REGISTRY.items():
            if name != "system":   # o Sistema é o do providers.py (mais completo)
                providers.REGISTRY[name] = cls

    def is_deck(self) -> bool:
        return self.session.kind == "deck3"

    def start_deck(self) -> None:
        """Servidor WebSocket (thread do asyncio) e o pedido da bateria a cada minuto."""
        if not self.deck_session:
            return
        srv = Server(self, int(self.cfg_deck.get("port", 8765)), load_token())
        threading.Thread(target=lambda: asyncio.run(srv.main()), daemon=True, name="ws").start()

        def ticker():
            while True:
                time.sleep(60)
                self.deck_session.link.events.put(("tick60", None))
        threading.Thread(target=ticker, daemon=True, name="tick60").start()

    def run(self) -> None:
        self.start_deck()
        super().run()

    # --- compatibilidade com os providers do Deck³
    @property
    def bl(self) -> int:
        return self.deck_session.backlight if self.deck_session else self.backlight

    def any_session(self):
        ds = self.deck_session
        return ds.dev if ds and ds.esp_ready else None

    def provider(self, node: dict):
        import deckproviders
        p = super().provider(node)
        if isinstance(p, deckproviders.Provider) and self.deck_session:
            p.session = self.deck_session.dev
        return p

    def handle_result(self, res) -> None:
        if isinstance(res, str):   # os providers do Deck³ devolvem o aviso como texto
            self.toast(res, "info")
            self.show(full=False)
            return
        super().handle_result(res)

    # --- o ecrã
    @property
    def has_rotation(self) -> bool:
        return not self.is_deck()   # o cubo não roda (o Sistema não mostra a página do ecrã)

    @property
    def SHOT_PREFIX(self) -> str:
        return "deck3" if self.is_deck() else "dashdeck"

    def canvas_area(self) -> tuple[int, int]:
        """No Deck³: corpo da tela (480x480, cabeçalho de 56 px, margens), para os plots e mapas."""
        return (444, 372) if self.is_deck() else super().canvas_area()

    def send_time(self) -> None:
        if not self.is_deck():
            return super().send_time()
        self.link.send({"t": "time", "epoch": int(time.time()), "tz": self.tz})
        self.last_time = time.monotonic()

    # o que o firmware do Deck³ ainda não tem (ícones, emojis, fundos, dock, cabeçalho)
    def send_icons(self, scr: dict) -> None:
        if not self.is_deck():
            super().send_icons(scr)

    def send_emojis(self, scr: dict) -> None:
        if not self.is_deck():
            super().send_emojis(scr)

    def send_bg(self, bg) -> bool:
        return False if self.is_deck() else super().send_bg(bg)

    def send_dock(self) -> None:
        if not self.is_deck():
            super().send_dock()

    def send_header(self) -> None:
        if not self.is_deck():
            super().send_header()

    # --- botões físicos
    def key_action(self, key: str, ev: str) -> None:
        act = self.keys.get(f"{key}_{ev}", "")
        if not act or act == "none":
            return
        if act == "back":
            self.on_back()
        elif act == "home":
            self.stack = [self.root]
            self.show()
        elif act == "bl":
            dev = self.session.dev
            cur = dev.hello.get("bl", self.backlight)
            nxt = next((x for x in self.bl_levels if x > cur), self.bl_levels[0])
            dev.set_bl(nxt)
            self.toast(f"Brilho {nxt}%", "info")
        elif act.startswith("open:"):
            child = next((i for i in self.root.get("items", []) if i.get("id") == act[5:]), None)
            if child:
                self.stack = [self.root]
                self.activate(child)
        elif act.startswith("shell:"):
            self.run_bg(lambda: self._shell({"cmd": act[6:]}))
        elif act.startswith("launch:"):
            from dashd import launch
            launch(act[7:])

    def current_provider(self):
        cur = self.node()
        owner = cur["id"] if cur.get("type") == "provider" else cur.get("owner") if cur.get("type") == "detail" else None
        return self.providers.get(owner) if owner else None

    # --- mensagens do aparelho
    def handle(self, kind: str, data) -> None:
        if not self.is_deck():
            return super().handle(kind, data)
        import deckproviders
        dev = self.session.dev
        if kind == "tick60":
            if self.esp_ready:
                self.link.send({"t": "bat?", "n": 2})
            return
        if kind != "msg":
            return super().handle(kind, data)
        t = data.get("t")
        if t:
            dev.last[t] = data
        if t == "hello":
            dev.hello = data
            dev.id = self.link.dev_id
            if "bl" in data:   # o brilho fica guardado no aparelho
                self.backlight = int(data["bl"])
            super().handle(kind, data)
            if self.esp_ready:
                st = deckproviders.load_state().get("devices", {}).get(dev.id, {})
                self.link.send({"t": "idle", "dim": int(st.get("dim", self.cfg_deck.get("dim", 60))),
                                "off": int(st.get("off", self.cfg_deck.get("off", 300)))})
                ab = self.cfg.get("about", {})
                self.link.send({"t": "about", "lines": ab.get("lines", ["O DashDeck elevado ao cubo"])})
                self.link.send({"t": "bat?", "n": 2})
            return
        if t not in self.DECK_MSGS:
            return super().handle(kind, data)
        if not self.esp_ready:
            return
        prov = self.current_provider()
        if t == "key":
            if prov and hasattr(prov, "on_event") and isinstance(prov, deckproviders.Provider):
                prov.on_event(data)
            self.key_action(data.get("k", ""), data.get("ev", ""))
        elif t == "slider":
            if prov and hasattr(prov, "on_slider"):
                self.handle_result(prov.on_slider(data.get("item", ""), int(data.get("v", 0))))
        elif t == "offline_log":
            self.offline_log(data)
        else:
            if t == "bat":
                self.log_battery(data)
            elif t == "bat_low":
                log.warning("bateria fraca no Deck³: %s%%", data.get("pct"))
                os.spawnvp(os.P_NOWAIT, "notify-send", ["notify-send", "-u", "critical", "Deck³",
                                                        f"Bateria fraca: {data.get('pct')}%"])
            elif t in ("ota", "cfg"):
                log.info("%s", data)
            if prov and hasattr(prov, "on_event") and isinstance(prov, deckproviders.Provider):
                prov.on_event(data)
        if t != "pong":
            self.show(full=False)

    def offline_log(self, msg: dict) -> None:
        dev = self.session.dev
        evs = msg.get("events", [])
        f = STATE_DIR / f"eventos-{dev.id}.jsonl"
        f.parent.mkdir(parents=True, exist_ok=True)
        with f.open("a") as fh:
            for e in evs:
                fh.write(json.dumps(e, ensure_ascii=False) + "\n")
        self._offline_n += len(evs)
        low = [e for e in evs if e.get("t") == "bat_low"]
        if low:
            os.spawnvp(os.P_NOWAIT, "notify-send", ["notify-send", "Deck³",
                                                    f"Enquanto esteve fora a bateria desceu a {low[-1].get('pct')}%"])
        if msg.get("last"):
            n, self._offline_n = self._offline_n, 0
            log.info("%d eventos de fora de casa (%s)", n, f)
            self.toast(f"{n} evento{'s' if n != 1 else ''} de quando esteve fora", "info")

    def log_battery(self, b: dict) -> None:
        """~/.local/state/deck3/bateria-<id>.csv, uma linha por minuto."""
        dev = self.session.dev
        if b.get("pct", -1) < 0:
            return
        dev.hello.update(bat=b["pct"], chg=b.get("chg"))
        now = time.time()
        if now - self._bat_logged < 55:
            return
        self._bat_logged = now
        f = STATE_DIR / f"bateria-{dev.id}.csv"
        f.parent.mkdir(parents=True, exist_ok=True)
        new = not f.exists()
        with f.open("a") as fh:
            if new:
                fh.write("hora,pct,mv,a_carregar,usb\n")
            fh.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')},{b['pct']},{b.get('mv', 0)},"
                     f"{int(bool(b.get('chg')))},{int(bool(b.get('usb')))}\n")

    # --- controlo local (deckctl), chamado da thread do asyncio: só lê a sessão do cubo
    def control(self, req: dict) -> dict:
        ds = self.deck_session
        dev, link = ds.dev, ds.link
        cmd = req.get("cmd")
        if cmd == "status":
            devs = [{"id": dev.id, "screen": ds.stack[-1].get("id"), **dev.hello}] if link.sock else []
            return {"ok": True, "devices": devs, "port": int(self.cfg_deck.get("port", 8765))}
        if not link.sock:
            return {"ok": False, "err": "nenhum Deck³ ligado"}
        if cmd == "last":
            return {"ok": True, "last": {dev.id: dev.last.get(req["t"]) if req.get("t") else dev.last}}
        if cmd == "inject":   # simula uma mensagem do aparelho (testes: deckctl.py tap/swipe/back)
            link.events.put(("msg", req["msg"]))
        elif cmd == "send":
            link.send(req["msg"], raw=True)
        elif cmd == "ota":
            self.fw_path = Path(req["path"]).resolve()
            port = int(self.cfg_deck.get("port", 8765))
            link.send({"t": "ota", "url": f"http://{link.local_ip}:{port}/fw/deck3.bin"})
        else:
            return {"ok": False, "err": f"comando desconhecido: {cmd}"}
        return {"ok": True, "devices": 1}


# --------------------------------------------------------------------------- servidor

class Server:
    def __init__(self, app, port: int, token: str):
        self.app = app
        self.port = port
        self.token = token

    async def serve_ws(self, reader, writer, path: str, headers: dict) -> None:
        q = parse_qs(urlsplit(path).query)
        dev_id = q.get("id", ["?"])[0][:16]
        if not hmac.compare_digest(q.get("token", [""])[0], self.token):
            log.warning("token errado de %s (%s)", writer.get_extra_info("peername"), dev_id)
            writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            await writer.drain()
            writer.close()
            return
        writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Accept: {ws.accept_key(headers.get('sec-websocket-key', ''))}\r\n\r\n").encode())
        await writer.drain()
        sock = ws.WebSocket(reader, writer)
        link = self.app.deck_session.link
        if link.sock:
            try:
                await link.sock.close()
            except Exception:
                pass
        link.sock, link.dev_id, link.local_ip = sock, dev_id, writer.get_extra_info("sockname")[0]
        log.info("ligado: %s de %s", dev_id, writer.get_extra_info("peername")[0])
        link.events.put(("connected", None))
        try:
            while True:
                raw = await sock.recv()
                try:
                    m = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                if m.get("t") in ("bgack", "iconack", "emojiack"):
                    link.acks.put(m)
                elif m.get("t") == "jpgack":
                    link.jpg_acks.put(m)
                else:
                    link.events.put(("msg", m))
        except (ws.Closed, asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            if link.sock is sock:
                link.sock = None
                link.events.put(("disconnected", None))
            log.info("desligado: %s", dev_id)

    async def serve_fw(self, writer, path: str) -> None:
        fw = self.app.fw_path
        if not fw or not path.startswith("/fw/") or not fw.exists():
            writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            return
        data = fw.read_bytes()
        writer.write(f"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: {len(data)}\r\n"
                     f"Connection: close\r\n\r\n".encode())
        writer.write(data)
        log.info("firmware enviado (%d bytes)", len(data))

    async def serve_ctl(self, reader, writer, headers: dict) -> None:
        if writer.get_extra_info("peername")[0] not in ("127.0.0.1", "::1"):
            writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        body = await reader.readexactly(int(headers.get("content-length", "0")))
        try:
            res = self.app.control(json.loads(body or b"{}"))
        except Exception as e:
            res = {"ok": False, "err": str(e)}
        out = json.dumps(res, ensure_ascii=False).encode()
        writer.write(f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {len(out)}\r\n\r\n".encode()
                     + out)

    async def handle_conn(self, reader, writer) -> None:
        try:
            method, path, headers = await asyncio.wait_for(ws.read_http_head(reader), 10)
            if path.startswith("/deck") and headers.get("upgrade", "").lower() == "websocket":
                await self.serve_ws(reader, writer, path, headers)
                return
            if method == "GET" and path.startswith("/fw/"):
                await self.serve_fw(writer, path)
            elif method == "POST" and path == "/ctl":
                await self.serve_ctl(reader, writer, headers)
            else:
                writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            await writer.drain()
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            pass
        finally:
            if not writer.is_closing():
                writer.close()

    async def main(self) -> None:
        self.app.deck_session.link.loop = asyncio.get_running_loop()
        server = await asyncio.start_server(self.handle_conn, host=None, port=self.port, limit=1 << 20)
        log.info("deckd a ouvir na porta %d (token em %s)", self.port, TOKEN_FILE)
        async with server:
            await server.serve_forever()
