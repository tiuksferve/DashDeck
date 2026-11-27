"""Providers próprios do DashDeck³ (Config, estado do aparelho). Os do DashDeck vêm do providers.py de lá.

A interface é a mesma do DashDeck (summary, badge, pages, render, on_tap, on_button), para os módulos
de lá (Meshtastic, ADS-B, AIS, GPS, Stocks...) se portarem quase sem mudanças. Diferenças:
  - o ecrã é 480×480 (quadrado): listas e kv têm mais espaço, as boxes ficam em 3 colunas;
  - o ícone de cada box é uma imagem que o dashd desenha e guarda no cartão (icons/<id>.bin); sem ela, o
    cubo mostra uma placa de 8 bits com a inicial;
  - `self.session` dá acesso ao aparelho (hello, último imu/mic, send()).
"""
from __future__ import annotations

import threading
import json
import os
import socket
import subprocess
import time
from pathlib import Path

import paths


class Provider:
    def __init__(self, node: dict, app):
        self.node = node
        self.app = app
        self.session = None        # Session do aparelho que tem a tela aberta (definido pelo deckd)

    def summary(self) -> str:
        return ""

    def badge(self) -> str:
        return ""

    def pages(self) -> int:
        return 1

    def render(self, page: int = 0) -> dict:
        return {"tpl": "text", "text": "—"}

    def on_tap(self, item_id: str):
        return None

    def on_button(self, btn_id: str):
        return None

    def on_event(self, msg: dict) -> None:
        """Eventos do aparelho que não são toques (imu, mic, ...)."""


# ------------------------------------------------------------------------------------- sistema

class SystemProvider(Provider):
    def __init__(self, node, app):
        super().__init__(node, app)
        self._last_cpu = self._cpu_times()

    @staticmethod
    def _cpu_times() -> tuple[int, int]:
        with open("/proc/stat") as f:
            v = [int(x) for x in f.readline().split()[1:]]
        return sum(v), v[3] + v[4]

    def _cpu_pct(self) -> float:
        # medida no máximo a cada 2 s: várias leituras seguidas davam deltas minúsculos (0% ↔ 25%) e
        # cada oscilação gerava um "update" inútil pela rede
        now = time.monotonic()
        if now - getattr(self, "_cpu_t", 0) >= 2:
            tot, idle = self._cpu_times()
            dt, di = tot - self._last_cpu[0], idle - self._last_cpu[1]
            self._last_cpu = (tot, idle)
            self._cpu_t = now
            self._cpu_v = 100.0 * (1 - di / dt) if dt else 0.0
        return getattr(self, "_cpu_v", 0.0)

    @staticmethod
    def _temp() -> float | None:
        try:
            with open("/sys/class/thermal/thermal_zone0/temp") as f:
                return int(f.read()) / 1000
        except OSError:
            return None

    @staticmethod
    def _mem() -> tuple[int, int]:
        info = {}
        with open("/proc/meminfo") as f:
            for ln in f:
                k, v = ln.split(":")
                info[k] = int(v.split()[0])
        return info["MemTotal"] // 1024, info["MemAvailable"] // 1024

    @staticmethod
    def battery() -> tuple[int | None, bool]:
        base = "/sys/class/power_supply"
        try:
            for d in os.listdir(base):
                p = os.path.join(base, d)
                if open(os.path.join(p, "type")).read().strip() == "Battery":
                    cap = int(open(os.path.join(p, "capacity")).read())
                    st = open(os.path.join(p, "status")).read().strip()
                    return cap, st == "Charging"
        except OSError:
            pass
        return None, False

    def summary(self) -> str:
        t = self._temp()
        return f"CPU {self._cpu_pct():.0f}%" + (f" · {t:.0f}°C" if t else "")

    def render(self, page: int = 0) -> dict:
        cpu = self._cpu_pct()
        t = self._temp()
        tot, avail = self._mem()
        st = os.statvfs("/")
        free_gb = st.f_bavail * st.f_frsize / 1e9
        up = time.monotonic()
        bat, chg = self.battery()
        load = os.getloadavg()[0]
        rows = [
            ["Máquina", socket.gethostname()],
            ["Ligado há", f"{int(up // 3600)} h {int(up % 3600 // 60)} min"],
            ["CPU", f"{cpu:.0f}% · load {load:.2f}", "warn" if cpu > 80 else None],
            ["Temperatura", f"{t:.1f} °C" if t else "—", "err" if t and t > 75 else "warn" if t and t > 65 else None],
            ["RAM", f"{tot - avail} / {tot} MB"],
            ["Disco livre", f"{free_gb:.1f} GB", "warn" if free_gb < 1 else None],
        ]
        if bat is not None:
            rows.append(["Bateria", f"{bat}%" + (" a carregar" if chg else ""), "err" if bat < 15 and not chg else None])
        return {"tpl": "kv", "rows": [[r[0], r[1]] + ([r[2]] if len(r) > 2 and r[2] else []) for r in rows]}


# --------------------------------------------------------------------------------- o próprio DashDeck³

class DeckProvider(Provider):
    """Estado do aparelho e testes do hardware (som, microfone, IMU, botões)."""

    def __init__(self, node, app):
        super().__init__(node, app)
        self.mic_on = False
        self.imu_on = False
        self.mic = None
        self.imu = None
        self.keys: list[str] = []

    def summary(self) -> str:
        s = self.session or self.app.any_session()
        if not s:
            return ""
        bat = s.hello.get("bat", -1)
        return f"{bat}%" if bat is not None and bat >= 0 else s.hello.get("fw", "")

    def pages(self) -> int:
        return 2

    def on_event(self, msg: dict) -> None:
        t = msg.get("t")
        if t == "mic" and "db" in msg:
            self.mic = msg
        elif t == "imu" and "ax" in msg:
            self.imu = msg
        elif t == "key":
            self.keys = ([f"{msg.get('k')} {msg.get('ev')}"] + self.keys)[:4]

    def render(self, page: int = 0) -> dict:
        s = self.session
        h = s.hello if s else {}
        if page == 0:
            rssi = h.get("rssi", 0)
            rows = [
                ["Firmware", h.get("fw", "?")],
                ["Ligação", f"{h.get('via', '?')} · {h.get('ip', '')}"],
                ["Wi-Fi", f"{rssi} dBm" if rssi else "—", "ok" if rssi > -60 else "warn" if rssi > -72 else "err"],
                ["Bateria", f"{h.get('bat')}%" + (" a carregar" if h.get("chg") else "") if h.get("bat", -1) >= 0 else "sem bateria"],
                ["PSRAM livre", f"{h.get('psram', 0) // 1024} KB"],
                ["RAM livre", f"{h.get('heap', 0) // 1024} KB"],
                ["Botões", " · ".join(self.keys) or "carregue num botão"],
            ]
            return {"tpl": "kv", "rows": rows,
                    "buttons": [{"id": "beep", "label": "Bip"}, {"id": "chime", "label": "Aviso"},
                                {"id": "stats", "label": "Atualizar", "color": "#2A323D"},
                                {"id": "reboot", "label": "Reiniciar", "color": "#2A323D", "confirm": "Reiniciar o DashDeck³?"}]}
        rows = []
        if self.mic_on:
            m = self.mic or {}
            rows.append(["Microfone", f"{m.get('db', '…')} dB (pico {m.get('peak', '…')})"])
        if self.imu_on:
            i = self.imu or {}
            rows += [["Aceleração", f"{i.get('ax', 0):.1f} {i.get('ay', 0):.1f} {i.get('az', 0):.1f} m/s²"],
                     ["Giroscópio", f"{i.get('gx', 0):.0f} {i.get('gy', 0):.0f} {i.get('gz', 0):.0f} °/s"]]
        if not rows:
            rows = [["Sensores", "ligue o microfone ou o IMU"]]
        return {"tpl": "kv", "rows": rows,
                "buttons": [{"id": "mic", "label": "Mic " + ("off" if self.mic_on else "on")},
                            {"id": "imu", "label": "IMU " + ("off" if self.imu_on else "on")}]}

    def on_button(self, btn_id: str):
        s = self.session
        if not s:
            return None
        if btn_id == "beep":
            s.send({"t": "beep", "f": 880, "ms": 150})
        elif btn_id == "chime":
            s.send({"t": "notify", "from": "deckd", "text": "Teste de aviso em pixel art"})
        elif btn_id == "stats":
            s.send({"t": "hello?"})
        elif btn_id == "reboot":
            s.send({"t": "reboot"})
        elif btn_id == "mic":
            self.mic_on = not self.mic_on
            s.send({"t": "mic", "hz": 5 if self.mic_on else 0})
        elif btn_id == "imu":
            self.imu_on = not self.imu_on
            s.send({"t": "imu", "stream": 5 if self.imu_on else 0})
        return None

    def on_leave(self) -> None:
        if self.session and (self.mic_on or self.imu_on):
            self.session.send({"t": "mic", "hz": 0})
            self.session.send({"t": "imu", "stream": 0})
        self.mic_on = self.imu_on = False


# ------------------------------------------------------------------------------- Config do DashDeck³

STATE_FILE = paths.DECK3_STATE


def load_state() -> dict:
    try:
        return json.loads(STATE_FILE.read_text())
    except (OSError, ValueError):
        return {}


def save_state(st: dict) -> None:
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(st, indent=1, ensure_ascii=False))
    tmp.replace(STATE_FILE)


def nmcli(*args: str) -> str:
    try:
        return subprocess.run(["nmcli", *args], capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def nm_wifi() -> dict[str, str]:
    """Redes Wi-Fi guardadas no NetworkManager do uConsole: {ssid: nome da ligação}, da mais recente."""
    rows = []
    for ln in nmcli("-t", "-f", "NAME,TYPE,TIMESTAMP", "connection", "show").splitlines():
        parts = ln.rsplit(":", 2)
        if len(parts) == 3 and parts[1] == "802-11-wireless":
            rows.append((int(parts[2] or 0), parts[0].replace("\\:", ":")))
    out = {}
    for _, name in sorted(rows, reverse=True):
        ssid = nmcli("-g", "802-11-wireless.ssid", "connection", "show", name)
        if ssid and ssid not in out:
            out[ssid] = name
    return out


def nm_psk(conn: str) -> str:
    return nmcli("-s", "-g", "802-11-wireless-security.psk", "connection", "show", conn)


def fmt_dur(h: float) -> str:
    if h < 1:
        return f"{h * 60:.0f} min"
    return f"{int(h)} h {int(h % 1 * 60):02d} min"


IDLE_CHOICES = [(15, "15 s"), (30, "30 s"), (60, "1 min"), (120, "2 min"), (300, "5 min"), (600, "10 min"), (0, "nunca")]


class ConfigProvider(DeckProvider):
    """Box Config: ecrã e som, bateria, Wi-Fi, Bluetooth e o aparelho (estado + testes do hardware).

    As definições do cubo (brilho, volume, Bluetooth, redes) ficam guardadas no próprio aparelho;
    escurecer/apagar ficam no dashd (~/.config/dashd/deck3.json), por aparelho.
    """

    PAGES = ["Ecrã e som", "Bateria", "Wi-Fi", "Bluetooth", "Aparelho", "Sensores"]

    def __init__(self, node, app):
        super().__init__(node, app)
        self.capacity = int(node.get("battery_mah", 1000))
        self.scan: list[dict] | None = None
        self.scanning = False
        self.nm: dict[str, str] | None = None
        self._asked: dict[str, float] = {}

    # --- utilitários
    def ask(self, what: dict, every: float) -> None:
        """Pede dados ao aparelho, no máximo a cada `every` segundos (render corre a cada refresh)."""
        k = what["t"]
        if self.session and time.monotonic() - self._asked.get(k, 0) >= every:
            self._asked[k] = time.monotonic()
            self.session.send(what)

    def last(self, t: str) -> dict:
        return self.session.last.get(t, {}) if self.session else {}

    def dev_state(self) -> dict:
        st = load_state()
        return st.setdefault("devices", {}).setdefault(self.session.id if self.session else "?", {})

    def set_dev_state(self, **kw) -> None:
        st = load_state()
        st.setdefault("devices", {}).setdefault(self.session.id, {}).update(kw)
        save_state(st)

    def idle(self) -> tuple[int, int]:
        d = self.dev_state()
        cfg = self.app.cfg.get("deckd", {})
        return int(d.get("dim", cfg.get("dim", 60))), int(d.get("off", cfg.get("off", 300)))

    def refresh_nm(self) -> None:
        """Lê as redes do NetworkManager numa thread (o nmcli demora) e redesenha no fim."""
        def work():
            self.nm = nm_wifi()
            if self.session:
                self.session.show()
        threading.Thread(target=work, daemon=True).start()

    # --- interface
    def summary(self) -> str:
        s = self.session or self.app.any_session()
        if not s:
            return "desligado"
        b = s.last.get("bat") or {}
        pct = b.get("pct", s.hello.get("bat", -1))
        chg = b.get("chg", s.hello.get("chg"))
        if pct is None or pct < 0:
            return s.hello.get("fw", "")
        return f"{pct}%" + (" · a carregar" if chg else "")

    def pages(self) -> int:
        return len(self.PAGES)

    def on_event(self, msg: dict) -> None:
        super().on_event(msg)
        if msg.get("t") == "wifi_scan":
            self.scanning = False
            self.scan = sorted(msg.get("aps", []), key=lambda a: -a.get("rssi", -100))
            if "err" in msg and self.session:
                self.session.send({"t": "toast", "text": f"Procura falhou: {msg['err']}", "level": "err"})
        elif msg.get("t") == "cfg" and msg.get("err") and self.session:
            self.session.send({"t": "toast", "text": msg["err"], "level": "err"})
        elif msg.get("t") == "bt" and msg.get("err") and self.session:
            self.session.send({"t": "toast", "text": f"Bluetooth: {msg['err']}", "level": "err"})

    def render(self, page: int = 0) -> dict:
        title = f"Config · {self.PAGES[page]}"
        body = [self.page_display, self.page_battery, self.page_wifi, self.page_bt,
                self.page_device, lambda: super(ConfigProvider, self).render(1)][page]()
        return {"title": title, **body}

    # --- páginas
    def page_display(self) -> dict:
        h = self.session.hello if self.session else {}
        dim, off = self.idle()
        name = dict(IDLE_CHOICES)
        return {
            "tpl": "kv",
            "sliders": [
                {"id": "bl", "label": "Brilho", "v": h.get("bl", self.app.bl), "min": 5, "max": 100, "step": 5,
                 "unit": "%", "live": "bl"},
                {"id": "vol", "label": "Volume", "v": h.get("vol", 60), "min": 0, "max": 100, "step": 5,
                 "unit": "%", "color": "#B48CFF"},
            ],
            "rows": [["Escurecer após", name.get(dim, f"{dim} s")], ["Apagar após", name.get(off, f"{off} s")]],
            "buttons": [{"id": "dim", "label": "Escurecer", "color": "#2A323D"},
                        {"id": "off", "label": "Apagar", "color": "#2A323D"},
                        {"id": "beep", "label": "Testar som"}],
        }

    def estimate(self, b: dict) -> str:
        """Tempo até vazia/cheia pela inclinação da última meia hora (mesmo estado de carga)."""
        hist = b.get("hist") or []
        if not hist:
            return "a medir…"
        chg = bool(hist[-1][3] & 1)
        pts = [(age, pct) for age, pct, _mv, fl in hist if age <= 3600 and bool(fl & 1) == chg]
        if len(pts) >= 2 and pts[0][0] - pts[-1][0] >= 900:
            dt_h = (pts[0][0] - pts[-1][0]) / 3600
            rate = (pts[-1][1] - pts[0][1]) / dt_h   # %/h
            pct = b.get("pct", 0)
            if chg and rate > 0.5:
                return f"cheia em {fmt_dur((100 - pct) / rate)}"
            if not chg and rate < -0.5:
                return f"{fmt_dur(pct / -rate)} restantes ({-rate * self.capacity / 100:.0f} mA)"
        if chg and b.get("icc"):
            return f"cheia em ~{fmt_dur((100 - b.get('pct', 0)) / 100 * self.capacity / b['icc'])}"
        return "a medir… (precisa de 15 min)"

    def page_battery(self) -> dict:
        self.ask({"t": "bat?", "n": 120}, 10)
        b = self.last("bat")
        if not b:
            return {"tpl": "text", "text": "A ler a bateria…"}
        if not b.get("bat", True) or b.get("pct", -1) < 0:
            return {"tpl": "kv", "rows": [["Bateria", "não detetada", "warn"], ["USB", f"{b.get('vbus', 0) / 1000:.2f} V"]]}
        hist = b.get("hist") or []
        pct, mv = b["pct"], b["mv"]
        span = hist[0][0] if hist else 0
        left = f"-{fmt_dur(span / 3600)}" if span >= 120 else "início"
        pts_pct = [r[1] for r in hist] + [pct]
        pts_mv = [r[2] for r in hist] + [mv]
        col = "#4BE38A" if pct > 40 else "#FFB23E" if pct > 15 else "#FF5470"
        rows = [
            ["Estado", b.get("state", ""), "ok" if b.get("chg") else None],
            ["Estimativa", self.estimate(b)],
            ["Tensão", f"{mv / 1000:.2f} V"],
            ["Capacidade", f"{self.capacity} mAh · {pct * self.capacity // 100} mAh agora"],
        ]
        if b.get("usb"):
            rows.append(["USB", f"{b.get('vbus', 0) / 1000:.2f} V"])
        if b.get("icc"):
            rows.append(["Carregador", f"{b['icc']} mA até {b.get('cv', 4200) / 1000:.2f} V"])
        if b.get("temp"):
            rows.append(["Temp. do PMU", f"{b['temp']:.0f} °C", "warn" if b["temp"] > 60 else None])
        rows.append(["Histórico", f"{len(hist)} pontos · {fmt_dur(span / 3600)} (guardado no cartão)" if span
                     else "a começar (1 ponto por minuto)"])
        return {
            "tpl": "kv",
            "chart": [
                {"title": "Carga", "value": f"{pct}%" + (" ⚡" if b.get("chg") else ""), "value_color": col,
                 "min": 0, "max": 100, "h": 140, "labels": [left, "agora"],
                 "series": [{"color": col, "points": pts_pct}]},
                {"title": "Tensão (mV)", "value": f"{mv} mV", "min": 3300, "max": 4300, "h": 100,
                 "labels": [left, "agora"], "series": [{"color": "#3EC6FF", "points": pts_mv}]},
            ],
            "rows": [[r[0], r[1]] + ([r[2]] if len(r) > 2 and r[2] else []) for r in rows],
            "buttons": [{"id": "icc", "label": f"Carga: {b.get('icc', '?')} mA", "color": "#2A323D",
                         "confirm": f"Mudar a corrente de carga para {self.next_icc(b.get('icc', 0))} mA?"},
                        {"id": "bat_refresh", "label": "Atualizar"}],
        }

    @staticmethod
    def next_icc(cur: int) -> int:
        """Corrente de carga: 100..500 mA (até 0,5C numa bateria de 1000 mAh)."""
        opts = [100, 200, 300, 400, 500]
        return next((x for x in opts if x > cur), opts[0])

    def page_wifi(self) -> dict:
        self.ask({"t": "cfg?"}, 5)
        if self.nm is None and not self._asked.get("nm"):
            self._asked["nm"] = time.monotonic()
            self.refresh_nm()
        c = self.last("cfg")
        h = self.session.hello if self.session else {}
        saved = c.get("nets", [])
        items = [{"id": "cur", "label": c.get("ssid") or "sem rede",
                  "sub": f"{c.get('ip', '')} · {c.get('state', '')}",
                  "right": f"{h.get('rssi', 0)} dBm" if h.get("rssi") else "", "color": "#4BE38A", "bg": "#0E1A14"}]
        for n in saved:
            if n == c.get("ssid"):
                continue
            items.append({"id": "del:" + n, "label": n, "sub": "guardada no DashDeck³ · tocar para apagar",
                          "confirm": f"Apagar a rede {n} do DashDeck³?"})
        if self.scanning:
            items.append({"id": "-", "label": "A procurar redes…", "color": "#8A97A8"})
        elif self.scan is not None:
            nm = self.nm or {}
            for a in self.scan:
                ss = a["ssid"]
                if ss in saved:
                    continue
                if ss in nm:
                    items.append({"id": "add:" + ss, "label": ss, "sub": "conhecida do uConsole · tocar para enviar",
                                  "right": f"{a['rssi']} dBm", "color": "#3EC6FF",
                                  "confirm": f"Enviar {ss} e a palavra-passe do uConsole para o DashDeck³?"})
                else:
                    items.append({"id": "unk:" + ss, "label": ss,
                                  "sub": ("protegida" if a.get("lock") else "aberta") + " · desconhecida do uConsole",
                                  "right": f"{a['rssi']} dBm", "color": "#8A97A8"})
        return {"tpl": "list", "items": items,
                "buttons": [{"id": "scan", "label": "Procurar"},
                            {"id": "share", "label": "Copiar do uConsole", "color": "#2A323D",
                             "confirm": "Enviar para o DashDeck³ as redes Wi-Fi guardadas no uConsole (até 8)?"}]}

    def page_bt(self) -> dict:
        self.ask({"t": "bt?"}, 3)
        b = self.last("bt")
        if not b:
            return {"tpl": "text", "text": "A ler o Bluetooth…"}
        on = b.get("on")
        rows = [["Estado", "ligado" if on else "desligado", "ok" if on else "sub"],
                ["Nome", b.get("name", "")]]
        if on:
            rows += [["Endereço", b.get("addr", "") or "…"],
                     ["Ligação", "telemóvel/PC ligado" if b.get("conn") else "à espera", "ok" if b.get("conn") else None],
                     ["Serviços", "bateria, informação do aparelho"]]
        if b.get("err"):
            rows.append(["Erro", b["err"], "err"])
        rows.append(["", "Ligado, o telemóvel ou o PC encontram o DashDeck³ e mostram a bateria dele. Gasta mais bateria."])
        return {"tpl": "kv", "rows": rows,
                "buttons": [{"id": "bt_off" if on else "bt_on", "label": "Desligar" if on else "Ligar",
                             "color": "#2A323D" if on else None}]}

    def page_device(self) -> dict:
        body = super().render(0)
        self.ask({"t": "sd?"}, 30)
        sd = self.last("sd")
        if sd.get("ok"):
            used = sd.get("total_mb", 0) - sd.get("free_mb", 0)
            row = ["Cartão SD", f"{sd.get('name', '')} · {used / 1024:.1f} / {sd.get('total_mb', 0) / 1024:.1f} GB"]
        else:
            row = ["Cartão SD", "sem cartão" if sd else "…", "warn" if sd else None]
        body["rows"].insert(5, [x for x in row if x is not None])
        return body

    # --- ações
    def on_slider(self, item: str, v: int):
        s = self.session
        if item == "bl":
            s.set_bl(v)
        elif item == "vol":
            s.hello["vol"] = v
            s.send({"t": "vol", "v": v})
            s.send({"t": "beep", "f": 880, "ms": 90})
        return None

    def on_tap(self, item: str):
        s = self.session
        if item.startswith("del:"):
            s.send({"t": "wifi", "del": item[4:]})
            self._asked.pop("cfg?", None)
            return f"Rede {item[4:]} apagada"
        if item.startswith("add:"):
            ss = item[4:]
            psk = nm_psk((self.nm or {}).get(ss, ss))
            s.send({"t": "wifi", "ssid": ss, "pass": psk})
            self._asked.pop("cfg?", None)
            return f"{ss} enviada"
        if item.startswith("unk:"):
            return "Ligue o uConsole a essa rede primeiro (fica com a palavra-passe) e volte a tocar"
        return None

    def on_button(self, btn_id: str):
        s = self.session
        if not s:
            return None
        if btn_id in ("dim", "off"):
            dim, off = self.idle()
            vals = [v for v, _ in IDLE_CHOICES]
            cur = dim if btn_id == "dim" else off
            nxt = vals[(vals.index(cur) + 1) % len(vals)] if cur in vals else vals[0]
            if btn_id == "dim":
                dim = nxt
                if off and dim and off < dim:
                    off = dim
            else:
                off = nxt
                if off and dim and off < dim:
                    dim = off
            self.set_dev_state(dim=dim, off=off)
            s.send({"t": "idle", "dim": dim, "off": off})
            return None
        if btn_id == "icc":
            cur = self.last("bat").get("icc", 0)
            s.send({"t": "bat_cfg", "icc": self.next_icc(cur)})
            return None
        if btn_id == "bat_refresh":
            self._asked.pop("bat?", None)
            return None
        if btn_id == "scan":
            self.scanning = True
            self.refresh_nm()
            s.send({"t": "wifi", "scan": True})
            return None
        if btn_id == "share":
            nm = nm_wifi()
            n = 0
            for ss, conn in list(nm.items())[:8]:
                s.send({"t": "wifi", "ssid": ss, "pass": nm_psk(conn)})
                n += 1
            self._asked.pop("cfg?", None)
            return f"{n} redes enviadas"
        if btn_id in ("bt_on", "bt_off"):
            s.send({"t": "bt", "on": btn_id == "bt_on"})
            return "A ligar o Bluetooth…" if btn_id == "bt_on" else None
        if btn_id == "reboot":
            s.send({"t": "reboot"})
            return None
        return super().on_button(btn_id)

    def on_leave(self) -> None:
        super().on_leave()
        self.scan = None
        self.scanning = False
        self.nm = None
        self._asked.pop("nm", None)


REGISTRY = {"system": SystemProvider, "deck": DeckProvider, "config": ConfigProvider}
