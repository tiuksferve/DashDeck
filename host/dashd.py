#!/usr/bin/env python3
"""dashd — daemon do uConsole que controla a tela DashDeck (ESP32-C6) pela USB.

O ESP é só um renderizador: este daemon decide o menu, as telas e as ações.
Protocolo: uma mensagem JSON por linha (ver firmware/main/ui.c).
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import queue
import subprocess
import threading
import time
import tomllib
import unicodedata
from pathlib import Path

import serial  # pyserial

import emojis
import icons
import providers
import xdgapps

log = logging.getLogger("dashd")
HERE = Path(__file__).resolve().parent


# --------------------------------------------------------------------------- link

# Fontes do firmware (tools/gen_fonts.py): ASCII, Latin-1, alguns extras e os símbolos LVGL (U+F000..F8FF).
# O que não existir é trocado pelo equivalente mais próximo (sem isto aparecem quadrados).
# Exatamente o que está nas fontes (o • não está: vira ·; o hífen invisível U+00AD não tem desenho).
_EXTRA = set("–—…←→−≤≥")
_CHAR_MAP = str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"', "×": "x", "•": "·", "­": ""})
# Emojis: cada sequência que o ESP já tem como imagem vai como o código privado dela (ver emojis.py).
EMOJI = emojis.Registry()


def _glyph_ok(ch: str) -> bool:
    o = ord(ch)
    return (0x20 <= o <= 0x7E or (0xA0 <= o <= 0xFF and o != 0xAD) or ch in _EXTRA or 0xF000 <= o <= 0xF8FF
            or ch == "\n")


def _in_font(cp: int) -> bool:
    return _glyph_ok(chr(cp))


def _plain(s: str) -> str:
    """Só os caracteres em falta são trocados (decompostos ou removidos); os acentos dos outros ficam."""
    s = unicodedata.normalize("NFC", s)
    if all(_glyph_ok(c) for c in s):
        return s
    out = []
    for c in s:
        if _glyph_ok(c):
            out.append(c)
            continue
        c = c.translate(_CHAR_MAP)
        out.append("".join(x for x in unicodedata.normalize("NFKD", c) if _glyph_ok(x)))
    return "".join(out)


def lcd_text(s: str) -> str:
    if all(_glyph_ok(c) for c in s):
        return s
    out = []
    for is_emoji, part in emojis.split(s, _in_font):
        if is_emoji:
            cp = EMOJI.cp.get(part)
            if cp:
                out.append(chr(cp))   # sem imagem no ESP, o emoji é removido
        else:
            out.append(_plain(part))
    return "".join(out)


def emoji_seqs(v, acc: list[str]) -> list[str]:
    """Sequências de emoji (por ordem, sem repetir) em todo o texto de uma mensagem."""
    if isinstance(v, str):
        if not all(_glyph_ok(c) for c in v):
            for is_emoji, part in emojis.split(v, _in_font):
                if is_emoji and part not in acc:
                    acc.append(part)
    elif isinstance(v, list):
        for x in v:
            emoji_seqs(x, acc)
    elif isinstance(v, dict):
        for x in v.values():
            emoji_seqs(x, acc)
    return acc


def lcd_safe(v):
    if isinstance(v, str):
        return lcd_text(v)
    if isinstance(v, list):
        return [lcd_safe(x) for x in v]
    if isinstance(v, dict):
        return {k: lcd_safe(x) for k, x in v.items()}
    return v


class Link:
    """Ligação serial com reconexão automática. Entrega eventos numa fila."""

    def __init__(self, port: str, events: queue.Queue):
        self.port = port
        self.events = events
        self.acks: queue.Queue = queue.Queue()   # confirmações "bgack" do ESP (fluxo do mapa)
        self.ser: serial.Serial | None = None
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True, name="link").start()

    def send(self, msg: dict) -> None:
        data = (json.dumps(lcd_safe(msg), ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        with self.lock:
            if not self.ser:
                return
            try:
                self.ser.write(data)
            except (serial.SerialException, OSError) as e:
                log.warning("erro ao escrever: %s", e)
                self._close()

    def _close(self) -> None:
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None

    def _run(self) -> None:
        while True:
            if not self.ser:
                try:
                    s = serial.Serial(self.port, 115200, timeout=0.5)
                    time.sleep(0.2)
                    s.reset_input_buffer()  # descarta hellos/logs acumulados enquanto ninguém lia
                    with self.lock:
                        self.ser = s
                    log.info("ligado a %s", self.port)
                    self.events.put(("connected", None))
                except (serial.SerialException, OSError):
                    time.sleep(2)
                    continue
            try:
                line = self.ser.readline()
            except (serial.SerialException, OSError, TypeError) as e:
                log.warning("ligação perdida: %s", e)
                with self.lock:
                    self._close()
                self.events.put(("disconnected", None))
                time.sleep(1)
                continue
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            if text.startswith("{"):
                try:
                    m = json.loads(text)
                    if m.get("t") in ("bgack", "iconack", "emojiack"):
                        self.acks.put(m)
                    else:
                        self.events.put(("msg", m))
                except json.JSONDecodeError:
                    log.debug("JSON inválido do ESP: %r", text)
            elif text:
                log.debug("esp: %s", text.lstrip("# "))


# --------------------------------------------------------------------------- ações

def wayland_env() -> dict:
    env = dict(os.environ)
    uid = os.getuid()
    env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{uid}")
    env.setdefault("WAYLAND_DISPLAY", "wayland-0")
    env.setdefault("DISPLAY", ":0")
    return env


def launch_desktop(path: str) -> None:
    """Abre um .desktop como o menu do uConsole (trata Exec, %U e Terminal=true)."""
    log.info("launch: %s", path)
    subprocess.Popen(["gio", "launch", path], env=wayland_env(), start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def launch(cmd: str) -> None:
    log.info("launch: %s", cmd)
    subprocess.Popen(cmd, shell=True, env=wayland_env(), start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def service_state(unit: str) -> dict:
    props = "ActiveState,SubState,ActiveEnterTimestamp,MainPID,MemoryCurrent,Description"
    try:
        out = subprocess.run(["systemctl", "show", unit, "-p", props], capture_output=True,
                             text=True, timeout=5).stdout
    except subprocess.TimeoutExpired:
        return {}
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def systemctl(action: str, unit: str) -> tuple[bool, str]:
    r = subprocess.run(["sudo", "-n", "systemctl", action, unit], capture_output=True, text=True, timeout=30)
    return r.returncode == 0, (r.stderr or r.stdout).strip()


# --------------------------------------------------------------------------- app

STATE_PT = {"active": ("ativo", "ok"), "inactive": ("parado", "sub"), "failed": ("falhou", "err"),
            "activating": ("iniciando", "warn"), "deactivating": ("parando", "warn")}


class App:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        d = cfg.get("dashd", {})
        self.refresh = float(d.get("refresh", 2.0))
        self.backlight = int(d.get("backlight", 80))
        self.events: queue.Queue = queue.Queue()
        self.link = Link(d.get("port", "/dev/esp-dash"), self.events)
        self.root = {"id": "home", "type": "submenu", "label": d.get("title", "DashDeck"),
                     "items": cfg.get("menu", [])}
        self.stack: list[dict] = [self.root]
        self.providers: dict[str, providers.Provider] = {}
        self.last_sent = ""
        self.last_status = ""
        self.esp_ready = False
        self.last_hello = 0.0
        self.page: dict[str, int] = {}   # página atual de cada tela com várias páginas (swipe)
        self.rotation = int(d.get("rotation", 1)) & 3
        saved = self.load_state().get("rotation")   # escolhida no menu (Sistema > DashDeck)
        if isinstance(saved, int):
            self.rotation = saved & 3
        self.rot_sent = False
        self.esp_info: dict = {}   # último hello do ESP (versão, memória, cartão)
        self.esp_w, self.esp_h = 172, 320
        self.bg_sent: str | None = None
        self.last_time = 0.0
        self._owners: set = set()   # providers com tela na pilha
        self.notified: tuple | None = None   # (nó do Meshtastic, mensagem) do último aviso, para o toque
        self.last_dm_check = 0.0
        self.bg_hold = 0.0
        self.esp_sd = False   # não reenviar o fundo antes disto (o aviso de mensagem está com a memória)
        self.last_tpl = ""
        self.dock_lost = False   # o ESP libertou os ícones e o dock (para caber o relógio)
        self.layout = d.get("layout", "grid")      # menus em "grid" (boxes) ou "cards"
        self.dock = cfg.get("dock", [])
        self.dock_screens = set(d.get("dock_screens", ["home"]))   # telas que mostram o dock
        self.screen_off = int(d.get("screen_off", 0))                 # s sem toques na tela inicial
        self.icons_sent: set[str] = set()
        self._icon_src: dict[str, str] = {}

    # ---- estado guardado (escolhas feitas no próprio DashDeck)
    STATE = Path.home() / ".config" / "dashd" / "state.json"

    def load_state(self) -> dict:
        try:
            return json.loads(self.STATE.read_text())
        except (OSError, ValueError):
            return {}

    def save_state(self, **kv) -> None:
        st = self.load_state()
        st.update(kv)
        try:
            self.STATE.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.STATE.with_suffix(".tmp")
            tmp.write_text(json.dumps(st, indent=1))
            tmp.replace(self.STATE)
        except OSError as e:
            log.warning("não foi possível gravar %s: %s", self.STATE, e)

    def set_rotation(self, v: int) -> None:
        """Muda a rotação do ESP: grava-a (passa à frente do config.toml) e o ESP reinicia."""
        v &= 3
        self.rotation = v
        self.save_state(rotation=v)
        log.info("rotação %d pedida no menu (o ESP reinicia)", v)
        self.rot_sent = True
        self.link.send({"t": "rot", "v": v})
        self.esp_ready = False   # até ao hello seguinte, nada de telas para um ESP a reiniciar

    # ---- helpers
    def node(self) -> dict:
        return self.stack[-1]

    def provider(self, node: dict) -> providers.Provider:
        key = node["id"]
        if key not in self.providers:
            self.providers[key] = providers.create(node.get("provider", key), node, self)
        return self.providers[key]

    def toast(self, text: str, level: str = "info") -> None:
        self.link.send({"t": "toast", "text": text, "level": level})

    def service_groups(self) -> dict[str, list[str]]:
        groups: dict[str, list[str]] = {}

        def walk(items):
            for it in items:
                if it.get("type") == "service" and it.get("group"):
                    groups.setdefault(it["group"], []).append(it["unit"])
                walk(it.get("items", []))
        walk(self.cfg.get("menu", []))
        return groups

    def canvas_area(self) -> tuple[int, int]:
        """Área útil do conteúdo no ESP (sem barra e margens), para os plots."""
        return self.esp_w - 12, self.esp_h - 24 - 12

    def send_dock(self) -> None:
        views = [{k: v for k, v in self.item_view(i).items() if k != "sub"} for i in self.dock[:6]]
        self.send_icons({"items": views})
        self.link.send({"t": "dock", "items": views})

    def send_icons(self, scr: dict) -> None:
        """Envia (com confirmação) os ícones de app que o ESP ainda não tem."""
        import base64
        for it in scr.get("items", []):
            iid = it.get("img")
            if not iid or iid in self.icons_sent:
                continue
            if self.esp_sd and self._icon_on_card(iid):   # já está no cartão do ESP: não reenvia
                self.icons_sent.add(iid)
                continue
            data = icons.rgb565a8(self._icon_src[iid])
            while not self.link.acks.empty():
                self.link.acks.get_nowait()
            self.link.send({"t": "icon", "id": iid, "w": icons.SIZE, "h": icons.SIZE,
                            "d": base64.b64encode(data).decode()})
            try:
                ack = self.link.acks.get(timeout=1.5)
                if ack.get("id") == iid and ack.get("ok"):
                    self.icons_sent.add(iid)
                    continue
            except queue.Empty:
                pass
            log.warning("ícone %s não confirmado", iid)
            it.pop("img")   # mostra o símbolo; tenta de novo na próxima vez

    def _icon_on_card(self, iid: str) -> bool:
        """Pergunta ao ESP se o ícone já está no cartão SD (de um arranque anterior)."""
        while not self.link.acks.empty():
            self.link.acks.get_nowait()
        self.link.send({"t": "icon", "id": iid, "probe": True})
        try:
            ack = self.link.acks.get(timeout=1.0)
            return ack.get("id") == iid and bool(ack.get("ok"))
        except queue.Empty:
            return False

    def send_emojis(self, scr: dict) -> None:
        """Envia (com confirmação) as imagens dos emojis desta tela que o ESP ainda não tem."""
        import base64
        seqs = emoji_seqs(scr, [])[:emojis.MAX]
        keep = set(seqs)
        for seq in seqs:
            if seq in EMOJI.cp:
                EMOJI.touch(seq)
                continue
            img = emojis.rgb565a8(seq)
            if not img:
                continue
            cp = EMOJI.slot_for(seq, keep)
            if cp < 0:
                break
            w, h, data = img
            while not self.link.acks.empty():
                self.link.acks.get_nowait()
            self.link.send({"t": "emoji", "cp": cp, "w": w, "h": h, "d": base64.b64encode(data).decode()})
            try:
                ack = self.link.acks.get(timeout=1.5)
                if ack.get("cp") == cp and ack.get("ok"):
                    EMOJI.set(seq, cp)
                    continue
            except queue.Empty:
                pass
            log.warning("emoji %r não confirmado", seq)

    def send_bg(self, bg) -> bool:
        """Envia a imagem de fundo 4 bpp em pedaços base64 (uma linha JSON cada)."""
        import base64
        key, w, h, pal, data = bg
        if key == self.bg_sent:
            return False
        chunk = 2048
        while not self.link.acks.empty():
            self.link.acks.get_nowait()
        for attempt in range(3):
            ok = True
            for off in range(0, len(data), chunk):
                msg = {"t": "bg", "key": key, "off": off,
                       "d": base64.b64encode(data[off:off + chunk]).decode()}
                if off == 0:
                    msg.update(w=w, h=h, pal=pal)
                    if len(pal) > 16:
                        msg["bpp"] = 8   # fundo do relógio: 256 cores, 1 byte por pixel
                if off + chunk >= len(data):
                    msg["last"] = True
                self.link.send(msg)
                try:   # espera a confirmação deste pedaço antes de enviar o próximo
                    ack = self.link.acks.get(timeout=1.5)
                    ok = ack.get("off") == off and ack.get("ok")
                except queue.Empty:
                    ok = False
                if not ok:
                    break
            if ok:
                self.bg_sent = key
                return True
            log.warning("mapa: pedaço %s falhou, tentativa %d", off, attempt + 1)
            time.sleep(0.2)
            while not self.link.acks.empty():
                self.link.acks.get_nowait()
        return False

    def menu_items(self, node: dict) -> list[dict]:
        """Itens de um submenu: os do config + (se xdg_category) as aplicações dos .desktop."""
        items = list(node.get("items", []))
        if node.get("xdg_category"):
            xdg = xdgapps.apps(node["xdg_category"], node.get("xdg_exclude", ["System", "Accessibility"]))
            have = {i["exec"] for i in xdg}
            extra = [i for i in items if (i.get("cmd") or "").split(" ")[0] not in have]
            items = (extra + xdg) if node.get("xdg_after") else (xdg + extra)
            # telas com dados (ex.: Abertas) ficam à frente dos programas
            items = [i for i in items if i.get("type") == "provider"] + \
                    [i for i in items if i.get("type") != "provider"]
        return items

    # ---- render
    def item_view(self, it: dict) -> dict:
        v = {"id": it["id"], "label": it.get("label", it["id"]), "icon": it.get("icon", "right")}
        src = it.get("img")   # imagem explícita (ex.: "emoji:✈", nome de ícone do tema ou caminho)
        if not src and it.get("type") == "launch":
            src = it.get("app_icon") or (it.get("cmd") or "").split(" ")[0]
        if src:
            if icons.rgb565a8(src):
                v["img"] = icons.icon_id(src)
                self._icon_src[v["img"]] = src
        if it.get("color"):
            v["color"] = it["color"]
        sub = it.get("sub", "")
        if it.get("type") == "service":
            st = service_state(it["unit"]).get("ActiveState", "?")
            txt, col = STATE_PT.get(st, (st, "sub"))
            sub = f"{it['unit']}: {txt}"
            v["color"] = {"ok": "#3FB950", "err": "#F85149", "warn": "#D29922"}.get(col, it.get("color", "#8B98A5"))
        elif it.get("type") == "provider":
            try:
                prov = self.provider(it)
                sub = prov.summary() or sub
                badge = prov.badge()
                if badge:
                    v["badge"] = badge
            except Exception as e:  # um provider com erro não pode derrubar o menu
                log.debug("summary %s: %s", it["id"], e)
        v["sub"] = sub
        return v

    def _item_imgs(self, scr: dict) -> None:
        """Imagens dos itens de um provider ("emoji:…", ícone do tema, .png) -> ids de ícone."""
        for v in scr.get("items", []):
            src = v.get("img")
            if src and src not in self._icon_src:   # ainda não é um id de ícone
                if icons.rgb565a8(src):
                    v["img"] = icons.icon_id(src)
                    self._icon_src[v["img"]] = src
                else:
                    v.pop("img")

    def build(self, node: dict) -> dict:
        t = node.get("type")
        scr = {"id": node["id"], "title": node.get("label", node["id"]), "back": len(self.stack) > 1}
        if t == "submenu":
            layout = node.get("layout", self.layout)
            items = [self.item_view(i) for i in self.menu_items(node)]
            if layout == "grid":   # nos boxes, a descrição dos apps só ocupa espaço; o status fica
                raw = {i["id"]: i for i in self.menu_items(node)}
                for v in items:
                    if raw.get(v["id"], {}).get("type") == "launch":
                        v.pop("sub", None)
            scr.update(tpl="menu", items=items, layout=layout)
        elif t == "service":
            s = service_state(node["unit"])
            st = s.get("ActiveState", "?")
            txt, col = STATE_PT.get(st, (st, "sub"))
            rows = [["Unidade", node["unit"]], ["Estado", f"{txt} ({s.get('SubState', '')})", col]]
            if st == "active":
                rows.append(["Desde", (s.get("ActiveEnterTimestamp") or "").split(" ", 1)[-1][:14]])
                if s.get("MainPID", "0") != "0":
                    rows.append(["PID", s["MainPID"]])
                mem = s.get("MemoryCurrent", "")
                if mem.isdigit():
                    rows.append(["Memória", f"{int(mem) / 1048576:.0f} MB"])
            if node.get("group"):
                rows.append(["Grupo", node["group"]])
            btns = ([{"id": "stop", "label": "Parar", "color": "err", "confirm": f"Parar {node['unit']}?"},
                     {"id": "restart", "label": "Reiniciar", "color": "warn"}]
                    if st == "active" else [{"id": "start", "label": "Ligar", "color": "ok"}])
            scr.update(tpl="kv", rows=rows, buttons=btns)
        elif t == "provider":
            prov = self.provider(node)
            n = max(1, prov.pages())
            pg = min(self.page.get(node["id"], 0), n - 1)
            scr.update(prov.render(pg))
            self._item_imgs(scr)
            if n > 1:
                scr.update(pages=n, page=pg)
        elif t == "detail":
            prov = self.providers.get(node.get("owner"))
            if node.get("dynamic") and prov and hasattr(prov, "render_detail"):
                scr.update(prov.render_detail(node["dynamic"]))   # refeito a cada atualização
                self._item_imgs(scr)
            else:
                scr.update({k: v for k, v in node.items() if k in ("tpl", "rows", "items", "lines", "text", "buttons")})
        elif t == "text":
            scr.update(tpl="text", text=node.get("text", ""))
        else:
            scr.update(tpl="text", text=f"Tipo desconhecido: {t}")
        return scr

    def show(self, full: bool = True) -> None:
        self._check_left()
        if not self.esp_ready:
            return
        try:
            scr = self.build(self.node())
        except Exception as e:
            log.exception("erro a construir %s", self.node().get("id"))
            scr = {"id": self.node()["id"], "title": "Erro", "tpl": "text", "text": str(e),
                   "back": len(self.stack) > 1}
        if scr.get("tpl") != "clock":
            if self.last_tpl == "clock":
                self.bg_sent = None      # o ESP liberta o fundo do relógio ao sair
            if self.dock_lost:
                self.dock_lost = False
                self.send_dock()
        self.last_tpl = scr.get("tpl", "")
        self.send_icons(scr)
        self.send_emojis(scr)
        scr["dock"] = self.node().get("id") in self.dock_screens
        bg = scr.pop("_bg", None)
        if bg and time.monotonic() < self.bg_hold:
            bg = None
        if bg and self.send_bg(bg):
            self.last_sent = ""   # fundo novo: redesenha o plot mesmo que os dados não mudem
        payload = json.dumps(scr, sort_keys=True)
        if full:
            self.link.send({"t": "screen", **scr})
        elif payload != self.last_sent:
            self.link.send({"t": "update", **scr})
        self.last_sent = payload

    def _check_left(self) -> None:
        """Avisa os providers cujas telas (incluindo os detalhes) saíram da pilha (ex.: liberta o SDR)."""
        owners = {n["id"] for n in self.stack if n.get("type") == "provider"}
        owners |= {n.get("owner") for n in self.stack if n.get("type") == "detail"}
        for gone in self._owners - owners:
            prov = self.providers.get(gone)
            if prov and hasattr(prov, "on_leave"):
                try:
                    prov.on_leave()
                except Exception:
                    log.exception("on_leave %s", gone)
        self._owners = owners

    def check_direct(self) -> None:
        """Avisa o ESP de mensagens diretas novas ({"t":"notify"}), em qualquer tela."""
        node = next((n for n in self.cfg.get("menu", []) if n.get("provider") == "mesh"), None)
        if not node:
            return
        try:
            mesh = self.provider(node)
            new = mesh.new_direct()
        except Exception as e:
            log.debug("diretas: %s", e)
            return
        for m in new[-1:]:   # várias de uma vez: avisa a mais recente
            name = mesh.sender_name(m)
            text = m.get("text", "")
            self.send_emojis({"text": text, "from": name})
            log.info("mensagem direta de %s", name)
            self.link.send({"t": "notify", "from": name, "text": text})
            self.notified = (node, m)

    def clock_providers(self) -> list:
        nodes = [n for n in self.cfg.get("dock", []) + self.cfg.get("menu", []) if n.get("provider") == "clock"]
        return [self.provider(n) for n in nodes]

    def clock_tick(self) -> None:
        """Temporizador e alarme do relógio (a cada segundo, em qualquer tela)."""
        for prov in self.clock_providers():
            try:
                prov.tick()
            except Exception as e:
                log.warning("relógio: %s", e)

    def open_notified(self) -> None:
        """Toque no aviso: no alarme pára o som; na mensagem abre-a (com o Responder) por cima do Meshtastic."""
        ringing = [p for p in self.clock_providers() if p.ringing]
        if ringing:
            for p in ringing:
                p.stop_ring()
            self.show(full=False)
            return
        if not self.notified:
            return
        node, m = self.notified
        mesh = self.provider(node)
        self.page[node["id"]] = mesh.PAGES.index("Conversas")
        conv = {"type": "detail", **mesh.open_conversation(m["key"])}
        self.stack = [self.root, node, conv, {"type": "detail", **mesh.message_detail(m)}]
        self.show()

    def send_time(self) -> None:
        """Acerta o relógio do ESP (hora local em segundos desde 1970); o ESP conta sozinho a partir daí."""
        now = time.time()
        self.link.send({"t": "time", "epoch": round(now + time.localtime(now).tm_gmtoff, 3)})
        self.last_time = time.monotonic()

    def send_status(self, force: bool = False) -> None:
        parts = [time.strftime("%H:%M")]
        bat = providers.battery()
        if bat:
            parts.append(f"{bat[0]}%" + ("+" if bat[1] == "Charging" else ""))
        status = " ".join(parts)
        if force or status != self.last_status:
            self.link.send({"t": "status", "text": status})
            self.last_status = status

    # ---- eventos do ESP
    def on_tap(self, item_id: str) -> None:
        cur = self.node()
        owner = cur.get("owner") if cur.get("type") == "detail" else cur["id"] if cur.get("type") == "provider" else None
        if owner:
            # o provider responde com: dict = tela de detalhe, (texto, nível) = aviso,
            # ("back", texto) = fecha o detalhe (ex.: seletor) e avisa
            if owner in self.providers:
                self.handle_result(self.providers[owner].on_tap(item_id))
            return
        child = next((i for i in self.menu_items(cur) if i["id"] == item_id), None)
        if child:
            self.activate(child)

    def handle_result(self, res) -> None:
        """Resposta de um provider a um toque ou botão: dict = tela de detalhe, (texto, nível) = aviso,
        ("back", texto) = fecha o detalhe (ex.: seletor, "Seguir no mapa") e avisa."""
        if isinstance(res, dict):
            self.stack.append({"type": "detail", **res})
            self.show()
        elif isinstance(res, tuple) and res[0] == "back":
            if self.node().get("type") == "detail":
                self.on_back()
            else:
                self.show()
            self.toast(res[1], "info")
        elif isinstance(res, tuple):
            self.toast(*res)
            self.show(full=False)
        else:
            self.show(full=False)

    def activate(self, child: dict) -> None:
        """Executa um item (do menu ou do dock)."""
        item_id = child.get("id", "")
        t = child.get("type")
        if t == "home":
            self.stack = [self.root]
            self.show()
            return
        if t == "backlight":
            levels = child.get("levels", [25, 50, 75, 100])
            nxt = next((v for v in levels if v > self.backlight), levels[0])
            self.backlight = nxt
            self.link.send({"t": "bl", "v": nxt})
            self.toast(f"Brilho {nxt}%", "info")
            return
        if t == "launch":
            if child.get("desktop"):
                launch_desktop(child["desktop"])
            else:
                launch(child["cmd"])
            self.toast(f"Abrindo {child.get('label', item_id)}", "ok")
        elif t == "shell":
            self.run_bg(lambda: self._shell(child))
        else:
            self.stack.append(child)
            self.show()

    def _shell(self, node: dict) -> None:
        r = subprocess.run(node["cmd"], shell=True, capture_output=True, text=True, timeout=60, env=wayland_env())
        ok = r.returncode == 0
        self.events.put(("toast", (node.get("done", "Concluído") if ok else f"Erro ({r.returncode})",
                                   "ok" if ok else "err")))

    def on_button(self, btn: str) -> None:
        cur = self.node()
        if cur.get("type") == "service":
            unit = cur["unit"]
            self.run_bg(lambda: self.service_action(btn, unit, cur.get("group")))
        elif cur.get("type") in ("provider", "detail"):
            owner = cur["id"] if cur.get("type") == "provider" else cur.get("owner")
            if owner not in self.providers:
                return
            self.handle_result(self.providers[owner].on_button(btn))

    def service_action(self, action: str, unit: str, group: str | None = None) -> None:
        """Executa numa thread. Com grupo (ex.: 'sdr'), ao ligar pára os outros do grupo."""
        if action == "start" and group:
            for other in self.service_groups().get(group, []):
                if other != unit and service_state(other).get("ActiveState") == "active":
                    self.events.put(("toast", (f"Parando {other}", "warn")))
                    systemctl("stop", other)
        ok, err = systemctl(action, unit)
        verb = {"start": "ligado", "stop": "parado", "restart": "reiniciado"}.get(action, action)
        self.events.put(("toast", (f"{unit} {verb}" if ok else f"Falhou: {err[:60]}", "ok" if ok else "err")))
        self.events.put(("refresh", None))

    def run_bg(self, fn) -> None:
        threading.Thread(target=fn, daemon=True).start()

    def on_swipe(self, direction: str) -> None:
        """Esquerda = página seguinte, direita = anterior; direita na 1.ª página = voltar."""
        cur = self.node()
        n = self.provider(cur).pages() if cur.get("type") == "provider" else 1
        pg = min(self.page.get(cur["id"], 0), n - 1)
        if direction == "left" and pg < n - 1:
            self.page[cur["id"]] = pg + 1
            self.show()
        elif direction == "right" and pg > 0:
            self.page[cur["id"]] = pg - 1
            self.show()
        elif direction == "right":
            self.on_back()

    def on_back(self) -> None:
        if len(self.stack) > 1:
            self.stack.pop()
            self.show()

    def handle(self, kind: str, data) -> None:
        if kind == "connected":
            self.esp_ready = False
            self.link.send({"t": "hello?"})
        elif kind == "disconnected":
            self.esp_ready = False
        elif kind == "toast":
            self.toast(*data)
        elif kind == "refresh":
            self.show(full=False)
        elif kind == "notify":   # aviso pedido por um provider (alarme/temporizador do relógio)
            if self.esp_ready:
                self.link.send({"t": "notify", **data})
        elif kind == "msg":
            t = data.get("t")
            if t == "hello":
                now = time.monotonic()
                if self.esp_ready and now - self.last_hello < 2:
                    return  # hellos repetidos (buffer do ESP) não refazem a tela
                self.last_hello = now
                log.info("ESP DashDeck fw %s (%sx%s, rot %s, heap %s, maior bloco %s, cartão %s)", data.get("fw"),
                         data.get("w"), data.get("h"), data.get("rot"), data.get("heap"), data.get("largest", "?"),
                         f"{data['sd']} MB livres" if data.get("sd", -1) >= 0 else "ausente")
                if "rot" in data and int(data["rot"]) != self.rotation and not self.rot_sent:
                    self.rot_sent = True
                    log.info("a pedir rotação %d (o ESP reinicia)", self.rotation)
                    self.link.send({"t": "rot", "v": self.rotation})
                    return
                self.esp_info = dict(data)
                self.esp_w, self.esp_h = int(data.get("w", 172)), int(data.get("h", 320))
                self.esp_sd = data.get("sd", -1) >= 0   # cartão SD no ESP (ícones guardados entre arranques)
                self.bg_sent = None
                self.icons_sent.clear()
                EMOJI.clear()
                self.esp_ready = True
                self.link.send({"t": "bl", "v": self.backlight})
                self.send_time()
                self.link.send({"t": "idle", "home": self.screen_off})
                self.send_status(force=True)
                self.send_dock()
                self.show()
            elif not self.esp_ready:
                self.link.send({"t": "hello?"})
            elif t == "tap":
                self.on_tap(data.get("item", ""))
            elif t == "btn":
                self.on_button(data.get("btn", ""))
            elif t == "back":
                self.on_back()
            elif t == "dock":
                child = next((i for i in self.dock if i.get("id") == data.get("item")), None)
                if child:
                    self.activate(child)
            elif t == "swipe":
                self.on_swipe(data.get("dir", ""))
            elif t == "bgfreed":   # o aviso usou a memória do fundo (relógio): volta a enviá-lo depois
                self.bg_sent = None
                self.bg_hold = time.monotonic() + 6
            elif t == "notify" and data.get("act") == "open":
                self.open_notified()
            elif t in ("stats", "bench"):   # medições de desempenho (kill -USR1 <pid do dashd>)
                log.info("ESP %s: %s", t, {k: v for k, v in data.items() if k != "t"})
            elif t == "iconlost":   # o ESP precisou do lugar deste ícone: reenvia quando for preciso
                self.icons_sent.discard(data.get("id"))
            elif t == "iconsfreed":   # o relógio precisou da memória dos ícones
                self.icons_sent.clear()
                EMOJI.clear()
                self.dock_lost = True
            elif t == "clock":   # gesto no relógio (mostrador ou brilho), já aplicado pelo ESP
                cur = self.node()
                prov = self.providers.get(cur["id"]) if cur.get("type") == "provider" else None
                if prov and hasattr(prov, "on_event"):
                    prov.on_event(data)
                    self.show(full=False)

    def run(self) -> None:
        import signal   # kill -USR1: pede ao ESP as estatísticas do toque e do desenho (aparecem no log)
        if threading.current_thread() is threading.main_thread():
            signal.signal(signal.SIGUSR1, lambda *_: self.link.send({"t": "stats"}))
        last_ping = last_refresh = last_tick = 0.0
        while True:
            try:
                kind, data = self.events.get(timeout=0.25)
                self.handle(kind, data)
            except queue.Empty:
                pass
            now = time.monotonic()
            if self.esp_ready and now - last_ping >= 2:
                self.link.send({"t": "ping"})
                self.send_status()
                last_ping = now
                if now - self.last_time >= 600:   # o cristal do ESP desvia uns segundos por dia
                    self.send_time()
            if now - last_tick >= 1:
                last_tick = now
                self.clock_tick()
            if self.esp_ready and now - self.last_dm_check >= 3:
                self.last_dm_check = now
                self.check_direct()
            if self.esp_ready and now - last_refresh >= self.refresh:
                self.show(full=False)
                last_refresh = now


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-c", "--config", default=str(HERE / "config.toml"))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    with open(args.config, "rb") as f:
        cfg = tomllib.load(f)
    App(cfg).run()


if __name__ == "__main__":
    main()
