"""Fontes de dados das telas do DashDeck.

Cada provider devolve em render(page) os campos de uma tela (tpl, rows/items/lines/text/plot, buttons).
pages() diz quantas páginas tem (mudadas com swipe lateral). summary() é o subtítulo do card no menu.
"""
from __future__ import annotations

import json
import logging
import math
import re
import os
import shutil
import socket
import subprocess
import threading
import time
from pathlib import Path

import maptiles
import meshbridge

log = logging.getLogger("dashd.providers")


def battery() -> tuple[int, str] | None:
    for d in Path("/sys/class/power_supply").glob("*battery*"):
        try:
            return int((d / "capacity").read_text()), (d / "status").read_text().strip()
        except (OSError, ValueError):
            pass
    return None


def fmt_dur(sec: float) -> str:
    sec = int(sec)
    d, sec = divmod(sec, 86400)
    h, sec = divmod(sec, 3600)
    m = sec // 60
    return f"{d}d {h}h{m:02d}" if d else f"{h}h{m:02d}"


class Provider:
    def __init__(self, node: dict, app):
        self.node = node
        self.app = app

    def summary(self) -> str:
        return ""

    def badge(self) -> str:
        """Número no selo vermelho do box (ex.: mensagens não lidas); "" = sem selo."""
        return ""

    def pages(self) -> int:
        return 1

    def render(self, page: int = 0) -> dict:
        return {"tpl": "text", "text": "—"}

    def on_tap(self, item_id: str):
        return None

    def on_button(self, btn_id: str):
        return None


# ------------------------------------------------------------------ sistema

class SystemProvider(Provider):
    def __init__(self, node, app):
        super().__init__(node, app)
        self._cpu_prev = self._cpu_times()

    @staticmethod
    def _cpu_times() -> tuple[int, int]:
        f = Path("/proc/stat").read_text().split("\n", 1)[0].split()[1:]
        vals = list(map(int, f))
        idle = vals[3] + vals[4]
        return idle, sum(vals)

    def _cpu_pct(self) -> float:
        idle, total = self._cpu_times()
        pi, pt = self._cpu_prev
        self._cpu_prev = (idle, total)
        dt = total - pt
        return 100.0 * (1 - (idle - pi) / dt) if dt > 0 else 0.0

    @staticmethod
    def _temp() -> float | None:
        try:
            return int(Path("/sys/class/thermal/thermal_zone0/temp").read_text()) / 1000
        except (OSError, ValueError):
            return None

    @staticmethod
    def _mem() -> tuple[int, int]:
        info = {}
        for line in Path("/proc/meminfo").read_text().splitlines():
            k, v = line.split(":", 1)
            info[k] = int(v.split()[0])
        return info["MemTotal"] - info["MemAvailable"], info["MemTotal"]

    @staticmethod
    def _ips() -> list[tuple[str, str]]:
        try:
            out = subprocess.run(["ip", "-4", "-o", "addr", "show"], capture_output=True, text=True, timeout=3).stdout
        except Exception:
            return []
        res = []
        for line in out.splitlines():
            p = line.split()
            if p[1] != "lo":
                res.append((p[1], p[3].split("/")[0]))
        return res

    def summary(self) -> str:
        t = self._temp()
        used, total = self._mem()
        return f"{t:.0f}°C · RAM {100 * used / total:.0f}%" if t else ""

    def render(self, page: int = 0) -> dict:
        used, total = self._mem()
        du = shutil.disk_usage("/")
        t = self._temp()
        cpu = self._cpu_pct()
        up = float(Path("/proc/uptime").read_text().split()[0])
        rows = [
            ["Host", socket.gethostname()],
            ["Uptime", fmt_dur(up)],
            ["CPU", f"{cpu:.0f}% · {os.getloadavg()[0]:.2f}", "warn" if cpu > 80 else None],
            ["Temp", f"{t:.1f} °C" if t else "?", "err" if t and t > 75 else ("warn" if t and t > 65 else None)],
            ["RAM", f"{used / 1048576:.1f}/{total / 1048576:.1f} GB"],
            ["Disco /", f"{du.free / 1e9:.1f} GB livres", "err" if du.free < 1e9 else None],
        ]
        bat = battery()
        if bat:
            rows.append(["Bateria", f"{bat[0]}% {'carregando' if bat[1] == 'Charging' else ''}".strip(),
                         "err" if bat[0] < 15 else None])
        for ifname, ip in self._ips():
            rows.append([ifname, ip])
        return {"tpl": "kv", "rows": [[r[0], r[1]] + ([r[2]] if len(r) > 2 and r[2] else []) for r in rows]}


# ------------------------------------------------------------------ GPS
#
# Regra: se o GPS estiver ativo no Meshtastic (meshtasticd ligado e com GPS.SerialPath no config.yaml),
# o tty pertence ao meshtasticd e a posição vem do Meshtastic. Caso contrário, lemos NMEA direto do tty.
# Nunca abrimos o tty enquanto o meshtasticd o usa: dois leitores na mesma porta corrompem os dados.

def meshtastic_gps_path(cfg="/etc/meshtasticd/config.yaml") -> str | None:
    """SerialPath do GPS configurado no meshtasticd (None se comentado/ausente)."""
    try:
        lines = Path(cfg).read_text().splitlines()
    except OSError:
        return None
    in_gps = False
    for line in lines:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if not line.startswith((" ", "\t")):
            in_gps = line.split(":", 1)[0].strip() == "GPS"
            continue
        if in_gps and line.strip().startswith("SerialPath:"):
            return line.split(":", 1)[1].split("#", 1)[0].strip() or None
    return None


def unit_active(unit: str) -> bool:
    try:
        return subprocess.run(["systemctl", "is-active", "--quiet", unit], timeout=3).returncode == 0
    except subprocess.TimeoutExpired:
        return False


# Talkers NMEA -> nome; letras RINEX para o rótulo do satélite
SYSTEMS = {"GP": ("GPS", "G"), "GL": ("GLONASS", "R"), "GA": ("Galileo", "E"), "GB": ("BeiDou", "C"),
           "GQ": ("QZSS", "J"), "GI": ("NavIC", "I")}
GSA_SYSID = {"1": "GP", "2": "GL", "3": "GA", "4": "GB", "5": "GQ", "6": "GI"}


def norm_talker(t: str, prn: int | None = None) -> str:
    if t == "BD":
        return "GB"
    if t == "GN" and prn is not None:  # GSA combinado sem system id: adivinha pela faixa do PRN
        return "GL" if 65 <= prn <= 96 else "GP"
    return t


def _nmea_coord(val: str, hemi: str) -> float | None:
    if not val:
        return None
    dot = val.index(".")
    deg = float(val[:dot - 2]) + float(val[dot - 2:]) / 60
    return -deg if hemi in ("S", "W") else deg


class NmeaReader:
    """Lê NMEA de um tty numa thread. Pára (e fecha a porta) quando should_run() fica falso."""

    def __init__(self, port: str, baud: int, should_run):
        self.port, self.baud, self.should_run = port, baud, should_run
        self.fix: dict = {}
        self.sats_view: dict[str, int] = {}
        self.sats: dict[tuple[str, int], dict] = {}   # (sistema, prn) -> el, az, snr, t
        self.used: dict[tuple[str, int], float] = {}  # (sistema, prn) -> último GSA
        self.last_rx = 0.0
        self.error = ""
        threading.Thread(target=self._run, daemon=True, name="nmea").start()

    @staticmethod
    def _valid(line: str) -> bool:
        if not line.startswith("$") or "*" not in line:
            return False
        body, cks = line[1:].rsplit("*", 1)
        x = 0
        for ch in body:
            x ^= ord(ch)
        try:
            return x == int(cks[:2], 16)
        except ValueError:
            return False

    def _parse(self, line: str) -> None:
        f = line[1:].split("*", 1)[0].split(",")
        kind = f[0][2:]
        try:
            if kind == "GGA" and len(f) > 9:
                self.fix.update(time=f[1], quality=int(f[6] or 0), used=int(f[7] or 0),
                                hdop=float(f[8]) if f[8] else None,
                                alt=float(f[9]) if f[9] else None,
                                lat=_nmea_coord(f[2], f[3]), lon=_nmea_coord(f[4], f[5]))
            elif kind == "RMC" and len(f) > 9:
                self.fix.update(valid=f[2] == "A", date=f[9],
                                speed=float(f[7]) * 1.852 if f[7] else None,
                                track=float(f[8]) if f[8] else None)
            elif kind == "GSA" and len(f) > 2:
                self.fix["mode"] = int(f[2] or 1)
                sysid = f[18] if len(f) > 18 else ""
                talker = GSA_SYSID.get(sysid, f[0][:2])
                now = time.time()
                for prn in f[3:15]:
                    if prn.isdigit():
                        self.used[(norm_talker(talker, int(prn)), int(prn))] = now
            elif kind == "GSV" and len(f) > 3:
                talker = norm_talker(f[0][:2])
                self.sats_view[talker] = int(f[3] or 0)
                now = time.time()
                for i in range(4, len(f) - 3, 4):
                    if f[i].isdigit():
                        self.sats[(talker, int(f[i]))] = {
                            "el": int(f[i + 1]) if f[i + 1] else None,
                            "az": int(f[i + 2]) if f[i + 2] else None,
                            "snr": int(f[i + 3]) if f[i + 3] else None, "t": now}
        except (ValueError, IndexError):
            pass

    def _run(self) -> None:
        import serial  # pyserial
        while True:
            if not self.should_run():
                time.sleep(2)
                continue
            try:
                with serial.Serial(self.port, self.baud, timeout=1) as s:
                    self.error = ""
                    log.info("GPS: lendo %s a %d baud", self.port, self.baud)
                    while self.should_run():
                        raw = s.readline()
                        if not raw:
                            continue
                        line = raw.decode("ascii", errors="ignore").strip()
                        if self._valid(line):
                            self.last_rx = time.time()
                            self._parse(line)
                    log.info("GPS: libertando %s", self.port)
            except Exception as e:  # porta inexistente, sem permissão, etc.
                self.error = str(e)
                log.debug("GPS tty: %s", e)
                time.sleep(5)

    def alive(self) -> bool:
        return time.time() - self.last_rx < 5

    def sky(self) -> list[tuple[str, int, dict, bool]]:
        """Satélites vistos nos últimos 10 s: (sistema, prn, dados, em uso)."""
        now = time.time()
        return [(k[0], k[1], v, now - self.used.get(k, 0) < 5)
                for k, v in list(self.sats.items()) if now - v["t"] < 10]


# Nome do sistema (MeshDeckWeb) -> talker, para reaproveitar o desenho do modo tty.
SYSTEM_TALKER = {"GPS": "GP", "GLONASS": "GL", "Galileo": "GA", "BeiDou": "GB", "QZSS": "GQ", "NavIC": "GI"}


class MeshDeckGps:
    """GPS ao vivo vindo do MeshDeckWeb (/api/local/gps), com a interface do NmeaReader.

    Com o gpsmux instalado, o MeshDeckWeb lê o mesmo GPS que o meshtasticd usa e
    dá fix, DOP e céu. O dashd não abre o tty.
    """

    def __init__(self, data: dict):
        f = data.get("fix") or {}
        self.error = data.get("error", "")
        self.source = data.get("source", "")
        utc = time.gmtime(f["time"]) if f.get("time") else None
        self.fix = {
            "quality": f.get("fix_quality") or (1 if f.get("fix") else 0),
            "valid": bool(f.get("fix")), "used": f.get("sats_used") or 0,
            "hdop": f.get("hdop"), "alt": f.get("alt"), "lat": f.get("lat"), "lon": f.get("lon"),
            "mode": f.get("fix_type") or 1,
            "speed": f["speed_mps"] * 3.6 if f.get("speed_mps") is not None else None,
            "track": f.get("course"),
            "time": time.strftime("%H%M%S", utc) if utc else "",
        }
        self.sats_view = {"GN": f.get("sats_in_view") or 0}
        self._sky = data.get("sky") or []

    def alive(self) -> bool:
        return True

    def sky(self) -> list[tuple[str, int, dict, bool]]:
        return [(SYSTEM_TALKER.get(s.get("system"), s.get("system", "?")), s["prn"],
                 {"el": s.get("el"), "az": s.get("az"), "snr": s.get("snr")}, bool(s.get("used")))
                for s in self._sky]


class GpsProvider(Provider):
    def __init__(self, node, app):
        super().__init__(node, app)
        self.tty = node.get("tty", "/dev/ttyAMA0")
        self.baud = int(node.get("baud", 9600))
        self._src = "?"
        self._src_t = 0.0
        self.reader = NmeaReader(self.tty, self.baud, lambda: self.source() == "tty")

    def _meshdeck(self) -> MeshDeckGps | None:
        """GPS ao vivo do MeshDeckWeb, se ele o estiver a ler (gpsmux ou tty dele)."""
        d = meshbridge.shared().gps()
        if d and d.get("state") in ("fix", "searching"):
            return MeshDeckGps(d)
        return None

    def source(self) -> str:
        """'meshdeck' se o MeshDeckWeb tem o GPS ao vivo; senão 'meshtastic' se o meshtasticd
        o está a usar; senão 'tty'. O 'meshdeck' vê-se a cada pedido (cache de 1 s); o resto
        reavalia-se a cada 10 s."""
        if self._meshdeck() is not None:
            return "meshdeck"
        now = time.monotonic()
        if now - self._src_t > 10:
            path = meshtastic_gps_path()
            self._src = "meshtastic" if path and unit_active("meshtasticd") else "tty"
            self._src_t = now
        return self._src

    def _live(self):
        """Leitor do modo actual com fix/céu: MeshDeckWeb ou o tty."""
        md = self._meshdeck()
        return md if md is not None else self.reader

    def _source_label(self) -> str:
        if self.source() == "meshdeck":
            md = self._meshdeck()
            return "MeshDeckWeb" + (" · gpsmux" if md and md.source == "gpsmux" else "")
        return self.tty

    def _fix_label(self) -> tuple[str, str]:
        f = self._live().fix
        if not f.get("quality") and not f.get("valid"):
            return "Sem fix", "err"
        return ("3D", "ok") if f.get("mode") == 3 else ("2D", "warn")

    def summary(self) -> str:
        if self.source() == "meshtastic":
            return "Meshtastic · " + ("com posição" if meshbridge.shared().position() else "sem posição")
        r = self._live()
        if not r.alive():
            return "sem dados do GPS"
        return f"{self._fix_label()[0]} · {r.fix.get('used', 0)} sat"

    def pages(self) -> int:
        return 2

    def position(self) -> tuple[float, float] | None:
        if self.source() == "meshtastic":
            p = meshbridge.shared().position()
            return (p[0], p[1]) if p else None
        r = self._live()
        f = r.fix
        if self.source() in ("tty", "meshdeck") and r.alive() and f.get("lat") is not None and \
                (f.get("quality") or f.get("valid")):
            return f["lat"], f["lon"]
        return None

    @staticmethod
    def _snr_color(snr) -> str:
        return "#3FB950" if snr >= 30 else ("#D29922" if snr >= 20 else "#F85149")

    def render_sky(self) -> dict:
        W, H = self.app.canvas_area()
        S = min(W, H)
        c, R = S // 2, S // 2 - 3
        plot = {"w": S, "h": S, "R": R, "rings": [R // 3, 2 * R // 3, R], "rlabels": ["60°", "30°", ""],
                "cardinal": True, "pts": []}
        out = {"tpl": "plot", "title": "GPS · Céu", "plot": plot}
        if self.source() == "meshtastic":
            plot["msg"] = "GPS em uso pelo meshtasticd\nsem dados de satélites"
            out["rows"] = [["Fonte", "Meshtastic"]]
            return out
        r = self._live()
        if not r.alive():
            plot["msg"] = f"Sem NMEA em {self.tty}"
            out["rows"] = [["Fonte", self.tty]]
            return out
        sky = sorted(r.sky(), key=lambda s: (s[3], s[2]["snr"] or 0))  # usados por cima
        per: dict[str, list[int]] = {}
        for talker, prn, d, used in sky:
            cnt = per.setdefault(talker, [0, 0])
            cnt[1] += 1
            cnt[0] += used
            if d["el"] is None or d["az"] is None or d["snr"] is None:
                continue   # sem sinal (cinza): não vai para o gráfico, mas conta nos totais
            r = (90 - max(0, min(90, d["el"]))) / 90 * R
            a = math.radians(d["az"])
            letter = SYSTEMS.get(talker, (talker, "?"))[1]
            plot["pts"].append([round(c + r * math.sin(a)), round(c - r * math.cos(a)), self._snr_color(d["snr"]),
                                f"{letter}{prn}", None, 1 if used else 0])
        if not plot["pts"]:
            plot["msg"] = "Nenhum satélite com sinal" if sky else "Nenhum satélite visível"
        out["rows"] = [[SYSTEMS.get(t, (t,))[0], f"{u}/{v} em uso"] for t, (u, v) in sorted(per.items())]
        return out

    def render(self, page: int = 0) -> dict:
        if page == 1:
            return self.render_sky()
        if self.source() == "meshtastic":
            # O tty é do meshtasticd; a posição do nó local vem pela ponte do MeshDeckWeb.
            br = meshbridge.shared()
            d = br.dash()
            rows = [["Fonte", "Meshtastic"], ["tty", meshtastic_gps_path() or "?"]]
            if d is None:
                rows.append(["Posição", br.error or "sem ponte", "err"])
                return {"tpl": "kv", "rows": rows}
            pos = br.position()
            ln = d.get("local_node") or {}
            rows.append(["GPS do nó", "ligado" if ln.get("gps_enabled") else "desligado",
                         None if ln.get("gps_enabled") else "warn"])
            if pos:
                rows += [["Fix", "sim", "ok"], ["Lat", f"{pos[0]:.6f}"], ["Lon", f"{pos[1]:.6f}"]]
                if pos[2] is not None:
                    rows.append(["Alt", f"{pos[2]:.0f} m"])
            else:
                rows.append(["Fix", "sem posição", "err"])
            return {"tpl": "kv", "rows": [r if len(r) < 3 or r[2] else r[:2] for r in rows]}
        r = self._live()
        if not r.alive():
            return {"tpl": "text", "text": f"Sem NMEA em {self.tty} ({self.baud} baud).\n{r.error}"}
        f = r.fix
        fix, col = self._fix_label()
        vis = sum(r.sats_view.values())
        rows = [["Fonte", self._source_label()], ["Fix", fix, col],
                ["Satélites", f"{f.get('used', 0)} usados / {vis}"]]
        if f.get("lat") is not None and fix != "Sem fix":
            rows += [["Lat", f"{f['lat']:.6f}"], ["Lon", f"{f['lon']:.6f}"]]
            if f.get("alt") is not None:
                rows.append(["Alt", f"{f['alt']:.0f} m"])
            if f.get("speed") is not None:
                rows.append(["Veloc.", f"{f['speed']:.1f} km/h"])
            if f.get("track") is not None:
                rows.append(["Rumo", f"{f['track']:.0f}°"])
        if f.get("hdop") is not None:
            rows.append(["HDOP", f"{f['hdop']:.1f}"])
        t = f.get("time", "")
        if len(t) >= 6:
            rows.append(["UTC", f"{t[0:2]}:{t[2:4]}:{t[4:6]}"])
        return {"tpl": "kv", "rows": rows}


# ------------------------------------------------------------------ ADS-B (readsb/dump1090 JSON)

# ------------------------------------------------------------------ Meshtastic (pela ponte do MeshDeckWeb)

def fmt_ago(sec: float | None) -> str:
    if sec is None:
        return "?"
    sec = max(0, sec)
    if sec < 60:
        return "agora"
    if sec < 3600:
        return f"{int(sec // 60)} min"
    if sec < 86400:
        return f"{int(sec // 3600)} h"
    return f"{int(sec // 86400)} d"


def haversine_km(a: tuple, b: tuple) -> float:
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 2 * 6371 * math.asin(math.sqrt(h))


def bat_text(level) -> str:
    if level is None:
        return ""
    return "externa" if level > 100 else f"{level}%"


class MeshtasticProvider(Provider):
    PAGES = ["Painel", "Nós", "Conversas"]
    # Fundo dos cards (sutil, sobre o C_CARD #18202A): verde = recebido por RF, lilás = via MQTT
    BG_RF, BG_MQTT, BG_DEST = "#16271F", "#241D33", "#172638"
    STATUS = {"sending": "a enviar", "ack": "entregue", "ack_implicit": "retransmitida",
              "nak": "falhou", "unconfirmed": "sem confirmação"}

    def __init__(self, node, app):
        super().__init__(node, app)
        self.br = meshbridge.shared()
        self.quick = list(node.get("quick", ["OK", "A caminho", "Cheguei", "Preciso de ajuda"]))[:12]
        self.dest: tuple[str, int | str] = ("ch", int(node.get("channel", 0)))   # ("ch", idx) ou ("dm", "!id")
        self.start_cmd = node.get("start_cmd", "")
        self._nodes: dict[str, dict] = {}
        self._msgs: dict[str, dict] = {}
        self._sent: dict[int, tuple[float, str]] = {}   # frase -> (hora, texto enviado)
        self._detail_msg: dict | None = None    # mensagem aberta (para o Responder)
        self._detail_node: str | None = None    # nó aberto (para a Mensagem direta)
        self._dm_seen: set | None = None         # diretas recebidas já conhecidas (para o aviso)
        self._conv_key: str = ""                 # conversa aberta ("ch:0" ou "dm:!id")

    def pages(self) -> int:
        return len(self.PAGES)

    def badge(self) -> str:
        d = self.br.dash()
        n = (d or {}).get("unread_total", 0)
        return str(n) if n else ""

    def summary(self) -> str:
        d = self.br.dash()
        if d is None:
            return self.br.error
        if not d.get("connected"):
            return "MeshDeckWeb sem nó"
        c = d.get("counts", {})
        return f"{c.get('online', 0)} ativos · {c.get('total', 0)} nós"

    def _chan_name(self, d: dict, idx: int) -> str:
        return next((c["name"] for c in d.get("channels", []) if c["index"] == idx), f"Canal {idx}")

    def _node_name(self, d: dict, nid: str) -> str:
        n = next((n for n in d.get("nodes", []) if n.get("id_string") == nid), {})
        return n.get("short_name") or n.get("long_name") or nid

    def _dest_name(self, d: dict) -> str:
        kind, v = self.dest
        return f"#{self._chan_name(d, v)}" if kind == "ch" else self._node_name(d, v)

    def _offline(self, d) -> dict:
        btns = [{"id": "start", "label": "Iniciar MeshDeckWeb", "color": "ok"}] if self.start_cmd else []
        if d is None:
            return {"tpl": "text", "text": f"{self.br.error}.\nO módulo Meshtastic usa o MeshDeckWeb como ponte "
                                           "(a API do meshtasticd só aceita um cliente).", "buttons": btns}
        return {"tpl": "text", "text": "O MeshDeckWeb não está ligado ao nó.\nLigue-o no MeshDeckWeb.",
                "buttons": btns}

    def render(self, page: int = 0) -> dict:
        d = self.br.dash()
        title = f"Meshtastic · {self.PAGES[page]}"
        if d is None or not d.get("connected"):
            return {"title": title, **self._offline(d)}
        out = [self._painel, self._nos, self._mensagens][page](d)
        out["title"] = title
        return out

    def _msg_mqtt(self, d: dict, m: dict) -> bool:
        t = m.get("transport")
        if t in ("rf", "mqtt"):
            return t == "mqtt"
        # pki/psk (ou MeshDeckWeb antigo): usa o último pacote do remetente
        n = next((n for n in d.get("nodes", []) if n.get("id_string") == m.get("from_id")), {})
        return bool(n.get("via_mqtt"))

    # ---- páginas
    def _painel(self, d: dict) -> dict:
        now = time.time()
        ln, loc, c = d.get("local_node") or {}, d.get("local") or {}, d.get("counts", {})
        unread = d.get("unread_total", 0)
        rows = [["Nó local", f"{ln.get('short_name', '?')} · {ln.get('node_id', '')}"],
                ["Ativos (2 h)", f"{c.get('online', 0)} de {c.get('total', 0)}", "ok" if c.get("online") else None],
                ["Não lidas", str(unread), "err" if unread else "sub"]]
        nodes = d.get("nodes", [])
        if nodes and nodes[0].get("last_heard"):
            n = nodes[0]
            rows.append(["Último ouvido", f"{n.get('short_name') or n['id_string']} · {fmt_ago(now - n['last_heard'])}"])
        if loc.get("battery_level") is not None:
            rows.append(["Bateria", bat_text(loc["battery_level"])])
        if loc.get("channel_utilization") is not None:
            rows.append(["Uso do canal", f"{loc['channel_utilization']:.1f}%"])
        if loc.get("air_util_tx") is not None:
            rows.append(["Air TX", f"{loc['air_util_tx']:.1f}%"])
        pos = self.br.position()
        rows.append(["Posição", f"{pos[0]:.4f}, {pos[1]:.4f}" if pos else "sem fix", None if pos else "warn"])
        return {"tpl": "kv", "rows": [r if len(r) < 3 or r[2] else r[:2] for r in rows]}

    def _nos(self, d: dict) -> dict:
        now = time.time()
        pos = self.br.position()
        items = []
        self._nodes = {}
        for n in d.get("nodes", [])[:30]:   # cada linha JSON para o ESP tem de caber em 8 KB
            nid = n["id_string"]
            self._nodes[nid] = n
            ago = now - n["last_heard"] if n.get("last_heard") else None
            parts = [fmt_ago(ago)]
            if n.get("via_mqtt"):
                parts.append("MQTT")
            else:
                if n.get("snr") is not None:
                    parts.append(f"SNR {n['snr']:.0f}")
                if n.get("hops_away") is not None:
                    parts.append({0: "direto", 1: "1 salto"}.get(n["hops_away"], f"{n['hops_away']} saltos"))
            if pos and n.get("latitude") is not None:
                parts.append(f"{haversine_km(pos, (n['latitude'], n['longitude'])):.1f} km")
            active = ago is not None and ago < 7200
            items.append({"id": nid, "label": f"{n.get('short_name') or '?'} · {n.get('long_name') or nid}",
                          "sub": " · ".join(parts), "right": bat_text(n.get("battery_level")),
                          "color": "#E6EDF3" if active else "#8B98A5",
                          "bg": self.BG_MQTT if n.get("via_mqtt") else self.BG_RF})
        return {"tpl": "list", "items": items, "empty": "Nenhum nó"}

    def _conv_name(self, d: dict, key: str) -> str:
        if key.startswith("ch:") and key[3:].isdigit():
            return f"#{self._chan_name(d, int(key[3:]))}"
        return self._node_name(d, key[3:]) if key.startswith("dm:") else key

    def _mensagens(self, d: dict) -> dict:
        """Conversas: cada canal e cada nó com mensagens diretas. As com não lidas vêm primeiro."""
        now = time.time()
        unread = d.get("unread", {}) or {}
        last: dict[str, dict] = {}
        for m in d.get("messages", []):          # vêm da mais recente para a mais antiga
            last.setdefault(m["key"], m)
        keys = [f"ch:{c['index']}" for c in d.get("channels", [])]
        keys += [k for k in list(last) + list(unread) if k.startswith("dm:") and k not in keys]
        seen = []
        for k in keys:
            if k not in seen:
                seen.append(k)
        seen.sort(key=lambda k: (-(unread.get(k, 0) > 0), -(last[k]["time"] if k in last else 0)))
        items = []
        for k in seen:
            m, n = last.get(k), unread.get(k, 0)
            direct = k.startswith("dm:")
            if m:
                who = "eu" if m["outgoing"] else m["from"]
                sub = f"{who}: {m['text']}" if not direct or m["outgoing"] else m["text"]
            else:
                sub = "sem mensagens · toque para escrever"
            items.append({"id": f"conv:{k}", "label": self._conv_name(d, k) + (" · direta" if direct else ""),
                          "sub": sub, "right": f"{n} nova{'s' if n > 1 else ''}" if n else (fmt_ago(now - m["time"]) if m else ""),
                          "color": "#F0883E" if n else ("#E6EDF3" if m else "#8B98A5"),
                          **({"bg": self.BG_DEST} if direct else {})})
        return {"tpl": "list", "items": items, "empty": "Sem canais"}

    def _conversation(self) -> dict:
        """Mensagens de uma conversa (da mais recente), com o card para escrever no topo."""
        d = self.br.dash() or {}
        now = time.time()
        k = self._conv_key
        items = [{"id": "write", "label": f"Escrever para {self._conv_name(d, k)}", "sub": "Respostas rápidas",
                  "right": "Escrever", "color": "#58A6FF", "bg": self.BG_DEST}]
        self._msgs = {}
        for i, m in enumerate([m for m in d.get("messages", []) if m["key"] == k]):
            mid = f"m{i}"
            self._msgs[mid] = m
            it = {"id": mid, "label": "eu" if m["outgoing"] else m["from"], "sub": m["text"],
                  "right": fmt_ago(now - m["time"]), "color": "#58A6FF" if m["outgoing"] else "#E6EDF3"}
            if not m["outgoing"]:
                it["bg"] = self.BG_MQTT if self._msg_mqtt(d, m) else self.BG_RF
            items.append(it)
        if len(items) == 1:
            items.append({"id": "none", "label": "Sem mensagens", "sub": "desde que o MeshDeckWeb arrancou",
                          "color": "#8B98A5"})
        return {"tpl": "list", "items": items}

    def open_conversation(self, key: str) -> dict:
        self._conv_key = key
        d = self.br.dash() or {}
        if (d.get("unread") or {}).get(key):
            self.br.mark_read(key)            # abrir a conversa = ler (também no MeshDeckWeb)
        return {"id": "mesh_conv", "label": self._conv_name(d, key), "dynamic": "conv", "owner": self.node["id"],
                **self._conversation()}

    def _expand(self, text: str) -> str:
        if "{pos}" in text:
            pos = self.br.position()
            text = text.replace("{pos}", f"{pos[0]:.5f},{pos[1]:.5f}" if pos else "sem posição")
        return text

    def _reply_screen(self, dest: tuple[str, int | str], about: str = "") -> dict:
        """Respostas rápidas para um destino (canal ou nó). Cada frase pede confirmação e é enviada."""
        self.dest = dest
        self._reply_about = about
        return {"id": "mesh_reply", "label": "Responder" if about else "Nova mensagem", "dynamic": "reply",
                "owner": self.node["id"], **self.render_detail("reply")}

    def render_detail(self, kind: str) -> dict:
        """Telas de detalhe refeitas a cada atualização (conversa aberta; resposta, que muda com o destino)."""
        if kind == "conv":
            return self._conversation()
        d = self.br.dash() or {}
        name = self._dest_name(d)
        kind_, v = self.dest
        about = getattr(self, "_reply_about", "")
        items = [{"id": "dest", "label": f"Para: {name}",
                  "sub": (f"Canal {v}" if kind_ == "ch" else "Mensagem direta") + (f" · {about}" if about else "")
                  + " · toque para mudar", "right": "Mudar", "color": "#58A6FF", "bg": self.BG_DEST}]
        for i, q in enumerate(self.quick):
            text = self._expand(q)
            short = text if len(text) <= 28 else text[:27] + "…"
            items.append({"id": f"q{i}", "label": text, "sub": "Toque para enviar", "right": "Enviar",
                          "confirm": f"Enviar \"{short}\" para {name[:20]}?"})
        return {"tpl": "list", "items": items}

    @staticmethod
    def _reply_dest(m: dict) -> tuple[str, int | str]:
        """Para onde vai a resposta: a mesma conversa (o canal da mensagem ou o outro lado da direta)."""
        if m.get("direct"):
            return "dm", (m.get("to_id") if m.get("outgoing") else m.get("from_id"))
        key = m.get("key", "")
        return "ch", int(key[3:]) if key.startswith("ch:") and key[3:].isdigit() else 0

    def _dest_picker(self) -> dict:
        d = self.br.dash() or {}
        now = time.time()
        items = []
        for c in d.get("channels", []):
            cur = self.dest == ("ch", c["index"])
            items.append({"id": f"ch:{c['index']}", "label": f"#{c['name']}", "sub": f"Canal {c['index']}",
                          "right": "atual" if cur else "", "bg": self.BG_DEST if cur else None})
        # mensagem direta: quem já conversou comigo primeiro, depois os nós ativos
        dm = [m["from_id"] if not m["outgoing"] else m["to_id"] for m in d.get("messages", []) if m["direct"]]
        active = [n["id_string"] for n in d.get("nodes", [])
                  if n.get("last_heard") and now - n["last_heard"] < 7200]
        seen = []
        for nid in dm + active:
            if nid and nid not in seen:
                seen.append(nid)
        nodes = {n["id_string"]: n for n in d.get("nodes", [])}
        for nid in seen[:15]:
            n = nodes.get(nid, {})
            cur = self.dest == ("dm", nid)
            ago = fmt_ago(now - n["last_heard"]) if n.get("last_heard") else "?"
            items.append({"id": f"dm:{nid}", "label": f"{n.get('short_name') or '?'} · {n.get('long_name') or nid}",
                          "sub": f"Direta · {ago}" + (" · MQTT" if n.get("via_mqtt") else ""),
                          "right": "atual" if cur else "",
                          "bg": self.BG_DEST if cur else (self.BG_MQTT if n.get("via_mqtt") else self.BG_RF)})
        for it in items:
            if it.get("bg") is None:
                it.pop("bg")
        return {"id": "mesh_dest", "label": "Enviar para", "tpl": "list", "items": items,
                "owner": self.node["id"]}

    # ---- ações
    def _send(self, text: str):
        kind, v = self.dest
        ok, err = (self.br.send(text, channel=v) if kind == "ch" else self.br.send(text, dest=v))
        return ("Enviada", "ok") if ok else (f"Falhou: {err}", "err")

    def on_button(self, btn_id: str):
        if btn_id == "reply" and self._detail_msg:
            m = self._detail_msg
            return self._reply_screen(self._reply_dest(m), "resposta a " + ("mim" if m["outgoing"] else m["from"])[:12])
        if btn_id == "dm" and self._detail_node:
            return self._reply_screen(("dm", self._detail_node))
        if btn_id == "start" and self.start_cmd:
            subprocess.Popen(self.start_cmd, shell=True, start_new_session=True,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return ("A iniciar o MeshDeckWeb...", "info")
        return None

    def on_tap(self, item_id: str):
        now = time.time()
        if item_id.startswith("conv:"):
            return self.open_conversation(item_id[5:])
        if item_id == "write" and self._conv_key:
            k = self._conv_key
            return self._reply_screen(("ch", int(k[3:])) if k.startswith("ch:") else ("dm", k[3:]))
        if item_id == "dest":
            return self._dest_picker()
        if item_id.startswith(("ch:", "dm:")):          # escolhido no seletor de destino
            kind, v = item_id.split(":", 1)
            self.dest = ("ch", int(v)) if kind == "ch" else ("dm", v)
            d = self.br.dash() or {}
            return ("back", f"Para {self._dest_name(d)}")
        if item_id.startswith("q") and item_id[1:].isdigit() and int(item_id[1:]) < len(self.quick):
            i = int(item_id[1:])
            text = self._expand(self.quick[i])
            res = self._send(text)
            if res[1] == "ok":
                self._sent[i] = (now, text)
                return ("back", f"Enviada para {self._dest_name(self.br.dash() or {})}")   # volta à mensagem
            return res
        if item_id in self._nodes:
            n = self._nodes[item_id]
            rows = [["ID", item_id], ["Nome", n.get("long_name") or "?"], ["Curto", n.get("short_name") or "?"]]
            if n.get("hw_model"):
                rows.append(["Hardware", n["hw_model"]])
            rows.append(["Ouvido", fmt_ago(now - n["last_heard"]) if n.get("last_heard") else "?"])
            rows.append(["Via", "MQTT" if n.get("via_mqtt") else "RF", "#B392F0" if n.get("via_mqtt") else "ok"])
            if n.get("snr") is not None:
                rows.append(["SNR", f"{n['snr']:.1f} dB"])
            if n.get("hops_away") is not None:
                rows.append(["Saltos", str(n["hops_away"])])
            if n.get("battery_level") is not None:
                rows.append(["Bateria", bat_text(n["battery_level"])])
            if n.get("voltage"):
                rows.append(["Tensão", f"{n['voltage']:.2f} V"])
            if n.get("latitude") is not None:
                rows += [["Lat", f"{n['latitude']:.5f}"], ["Lon", f"{n['longitude']:.5f}"]]
                pos = self.br.position()
                if pos:
                    rows.append(["Distância", f"{haversine_km(pos, (n['latitude'], n['longitude'])):.1f} km"])
            self._detail_node = item_id
            return {"id": f"node_{item_id}", "label": n.get("short_name") or item_id, "tpl": "kv", "rows": rows,
                    "buttons": [{"id": "dm", "label": "Mensagem direta", "color": "ok"}]}
        if item_id in self._msgs:
            return self.message_detail(self._msgs[item_id])
        return None

    def new_direct(self) -> list[dict]:
        """Mensagens diretas recebidas desde a última vez (as que existiam no arranque não contam)."""
        d = self.br.dash()
        if not d or not d.get("connected"):
            return []
        dms = [m for m in d.get("messages", []) if m.get("direct") and not m.get("outgoing")]
        keys = {(m.get("from_id"), m.get("time"), m.get("text")) for m in dms}
        if self._dm_seen is None:
            self._dm_seen = keys
            return []
        new = [m for m in dms if (m.get("from_id"), m.get("time"), m.get("text")) not in self._dm_seen]
        self._dm_seen |= keys
        return new

    def sender_name(self, m: dict) -> str:
        return self._node_name(self.br.dash() or {}, m.get("from_id", "")) or m.get("from", "?")

    def message_detail(self, m: dict) -> dict:
        """Detalhe de uma mensagem, com o botão Responder."""
        now = time.time()
        who = "eu" if m["outgoing"] else f"{m['from']} ({m['from_id']})"
        where = m["channel"] or "mensagem direta"
        head = f"{who}\n{where} · {fmt_ago(now - m['time'])}"
        if m["outgoing"]:
            st = self.STATUS.get(m.get("status", ""), m.get("status", ""))
            if st:
                head += f" · {st}"
        else:
            d = self.br.dash() or {}
            head += " · MQTT" if self._msg_mqtt(d, m) else " · RF"
            if m.get("hops") is not None and not self._msg_mqtt(d, m):
                head += {0: " direto", 1: " 1 salto"}.get(m["hops"], f" {m['hops']} saltos")
        self._detail_msg = m
        return {"id": f"msg_{int(m.get('time', 0))}", "label": "Mensagem", "tpl": "text", "owner": self.node["id"],
                "text": f"{head}\n\n{m['text']}", "buttons": [{"id": "reply", "label": "Responder", "color": "ok"}]}


# ------------------------------------------------------------------ SDR: quem está a usar o RTL-SDR

SDR_USB_IDS = {("0bda", "2832"), ("0bda", "2838")}     # RTL2832U


def sdr_devices() -> list[str]:
    """Caminhos /dev/bus/usb/BBB/DDD dos RTL-SDR ligados."""
    out = []
    for dev in Path("/sys/bus/usb/devices").glob("*"):
        try:
            ids = ((dev / "idVendor").read_text().strip(), (dev / "idProduct").read_text().strip())
            if ids in SDR_USB_IDS:
                bus, num = int((dev / "busnum").read_text()), int((dev / "devnum").read_text())
                out.append(f"/dev/bus/usb/{bus:03d}/{num:03d}")
        except (OSError, ValueError):
            continue
    return out


def proc_name(pid: int) -> str:
    """Nome legível de um processo (para Python, o script: intercept.py em vez de python)."""
    try:
        args = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        args = [a.decode(errors="replace") for a in args if a]
        comm = Path(f"/proc/{pid}/comm").read_text().strip()
    except OSError:
        return str(pid)
    if comm.startswith("python") or comm in ("sudo",):
        script = next((os.path.basename(a) for a in args[1:] if not a.startswith("-")), comm)
        return script
    return comm


def sdr_users() -> dict[int, str]:
    """PID -> nome dos processos com o RTL-SDR aberto (sudo fuser, porque são de outros utilizadores)."""
    users: dict[int, str] = {}
    for dev in sdr_devices():
        try:
            r = subprocess.run(["sudo", "-n", "fuser", dev], capture_output=True, text=True, timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            continue
        for tok in r.stdout.split():
            if tok.isdigit():
                users[int(tok)] = proc_name(int(tok))
    return users


def unit_pid(unit: str) -> int:
    try:
        out = subprocess.run(["systemctl", "show", unit, "-p", "MainPID", "--value"],
                             capture_output=True, text=True, timeout=3).stdout.strip()
        return int(out or 0)
    except (OSError, subprocess.TimeoutExpired, ValueError):
        return 0


def sdr_unit(pid: int) -> str:
    """Serviço systemd do processo (system.slice/x.service), ou "" se não for de um serviço do sistema."""
    try:
        cg = Path(f"/proc/{pid}/cgroup").read_text()
    except OSError:
        return ""
    for part in cg.strip().split("/"):
        if part.endswith(".service") and "system.slice" in cg and not part.startswith("user@"):
            return part
    return ""


def proc_age(pid: int) -> float | None:
    """Segundos desde que o processo arrancou."""
    try:
        ticks = int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19])
        up = float(Path("/proc/uptime").read_text().split()[0])
        return up - ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError):
        return None


def sdr_freq(pid: int, name: str) -> str:
    """Frequência em uso, quando se consegue saber."""
    low = name.lower()
    if low.startswith(("readsb", "dump1090")):
        return "1090 MHz (ADS-B)"
    if "ais" in low:
        return "161,975 / 162,025 MHz (AIS)"
    if low.startswith("gqrx"):
        try:   # controlo remoto do Gqrx (Ferramentas > Remote control), porta 7356
            with socket.create_connection(("127.0.0.1", 7356), timeout=0.5) as c:
                c.sendall(b"f\n")
                hz = int(c.recv(64).split()[0])
                return f"{hz / 1e6:.4f} MHz".replace(".", ",")
        except (OSError, ValueError, IndexError):
            return "? (ligue o Remote control do Gqrx)"
    try:
        args = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        args = [a.decode(errors="replace") for a in args if a]
    except OSError:
        args = []
    if "-f" in args[:-1]:
        v = args[args.index("-f") + 1]
        try:
            f = float(v.rstrip("MmKk")) * (1e6 if v[-1:] in "Mm" else 1e3 if v[-1:] in "Kk" else 1)
            return f"{f / 1e6:.3f} MHz".replace(".", ",")
        except ValueError:
            return v
    if low.startswith("rtl_tcp"):
        return "escolhida pelo cliente (rtl_tcp)"
    if low.startswith("rtl_433"):
        return "433,92 MHz (rtl_433)"
    return "?"


def sdr_info() -> dict:
    """Dongle ligado (nome, série) e a alimentação do SDR na placa AIO v2 (GPIO7)."""
    info = {"present": False, "name": "", "serial": "", "rail": None}
    for dev in Path("/sys/bus/usb/devices").glob("*"):
        try:
            ids = ((dev / "idVendor").read_text().strip(), (dev / "idProduct").read_text().strip())
        except OSError:
            continue
        if ids in SDR_USB_IDS:
            rd = lambda f: (dev / f).read_text().strip() if (dev / f).exists() else ""
            info.update(present=True, name=rd("product") or "RTL2832U", serial=rd("serial"))
            break
    try:
        out = subprocess.run(["pinctrl", "get", "7"], capture_output=True, text=True, timeout=2).stdout
        info["rail"] = True if " hi" in out else False if " lo" in out else None
    except (OSError, subprocess.TimeoutExpired):
        pass
    return info


class SdrProvider(Provider):
    """Monitor do RTL-SDR: se o dongle está ligado e alimentado, quem o usa, em que frequência e há quanto
    tempo, e o histórico recente. "Libertar SDR" pára o serviço (ou fecha o programa) que o tem aberto."""

    def __init__(self, node, app):
        super().__init__(node, app)
        self.users: dict[int, str] = {}
        self.info: dict = sdr_info()
        self.history: list[tuple[float, str]] = []
        self._prev: set[str] = set()
        threading.Thread(target=self._poll, daemon=True, name="sdr").start()

    def _poll(self) -> None:
        while True:
            try:
                self.info = sdr_info()
                self.users = sdr_users() if self.info["present"] else {}
                now = {n for n in self.users.values()}
                for n in sorted(now - self._prev):
                    self.history.insert(0, (time.time(), f"{n} abriu"))
                for n in sorted(self._prev - now):
                    self.history.insert(0, (time.time(), f"{n} libertou"))
                del self.history[8:]
                if now != self._prev:
                    self.app.events.put(("refresh", None))
                self._prev = now
            except Exception as e:
                log.debug("sdr: %s", e)
            time.sleep(4)

    def summary(self) -> str:
        if not self.info.get("present"):
            return "sem dongle" if self.info.get("rail") is not False else "desligado (AIO)"
        if not self.users:
            return "livre"
        pid, name = next(iter(self.users.items()))
        f = sdr_freq(pid, name).split(" (")[0]
        return f"{name} · {f}" if f != "?" else name

    def render(self, page: int = 0) -> dict:
        info, rows = self.info, []
        rail = {True: ("ligada", "ok"), False: ("desligada", "err"), None: ("?", "sub")}[info.get("rail")]
        rows.append(["Alimentação (AIO)", rail[0], rail[1]])
        if info.get("present"):
            rows.append(["Dongle", f"{info['name']}" + (f" · SN {info['serial'][-6:]}" if info.get("serial") else "")])
        else:
            rows.append(["Dongle", "não encontrado", "err"])
        btns = []
        if info.get("present") and not self.users:
            rows.append(["Estado", "livre para outros programas", "ok"])
        for pid, name in self.users.items():
            unit = sdr_unit(pid)
            rows.append(["Em uso por", f"{name} (PID {pid})" + (f" · {unit}" if unit else ""), "warn"])
            rows.append(["Frequência", sdr_freq(pid, name)])
            age = proc_age(pid)
            if age is not None:
                rows.append(["Há", f"{age:.0f} s" if age < 60 else fmt_dur(age)])
        for t, txt in self.history[:5]:
            rows.append([time.strftime("%H:%M", time.localtime(t)), txt, "sub"])
        if self.users:
            names = ", ".join(sorted(set(self.users.values())))[:40]
            btns.append({"id": "free", "label": "Libertar SDR", "color": "err",
                         "confirm": f"Fechar {names} e libertar o SDR?"})
        return {"tpl": "kv", "rows": rows, "buttons": btns}

    def _free(self) -> None:
        stopped = set()
        for pid, name in list(self.users.items()):
            unit = sdr_unit(pid)
            if unit:
                if unit not in stopped:
                    subprocess.run(["sudo", "-n", "systemctl", "stop", unit], capture_output=True, timeout=30)
                    stopped.add(unit)
            else:
                subprocess.run(["sudo", "-n", "kill", str(pid)], capture_output=True, timeout=5)
        # quem ligou o ADS-B/AIS pelo DashDeck já não tem de o desligar ao sair
        for prov in self.app.providers.values():
            if isinstance(prov, TargetMapProvider) and prov.unit in stopped:
                prov.started_here = False
        time.sleep(1.5)
        self.users = sdr_users()
        ok = not self.users
        self.app.events.put(("toast", ("SDR livre" if ok else "O SDR continua em uso", "ok" if ok else "err")))
        self.app.events.put(("refresh", None))

    def on_button(self, btn_id: str):
        if btn_id == "free" and self.users:
            threading.Thread(target=self._free, daemon=True).start()
            return ("A libertar o SDR...", "info")
        return None


# ------------------------------------------------------------------ trajetos

class Tracker:
    """Posições por alvo para desenhar o trajeto: id -> [(t, lat, lon)], do mais antigo para o mais novo."""

    def __init__(self, max_age: float = 3600, max_pts: int = 600):
        self.max_age, self.max_pts = max_age, max_pts
        self.tracks: dict[str, list[tuple[float, float, float]]] = {}
        self.lock = threading.Lock()

    def add(self, tid: str, lat: float, lon: float, t: float | None = None) -> None:
        t = time.time() if t is None else t
        with self.lock:
            tr = self.tracks.setdefault(tid, [])
            if tr:
                lt, la, lo = tr[-1]
                if abs(la - lat) < 2e-4 and abs(lo - lon) < 2e-4 and t - lt < 60:
                    return          # parado (menos de ~20 m): não acumula pontos repetidos
            tr.append((t, lat, lon))
            if len(tr) > self.max_pts:
                del tr[: len(tr) - self.max_pts]

    def prune(self) -> None:
        cut = time.time() - self.max_age
        with self.lock:
            for tid in list(self.tracks):
                tr = [p for p in self.tracks[tid] if p[0] >= cut]
                if tr:
                    self.tracks[tid] = tr
                else:
                    del self.tracks[tid]

    def get(self, tid: str) -> list[tuple[float, float]]:
        with self.lock:
            return [(la, lo) for _, la, lo in self.tracks.get(tid, ())]

    def last(self, tid: str) -> tuple[float, float, float] | None:
        with self.lock:
            tr = self.tracks.get(tid)
            return tr[-1] if tr else None


B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"


def enc_px(x: float, y: float) -> str:
    """Um ponto do trajeto para o firmware: 2 caracteres por coordenada, valor px+1024."""
    out = ""
    for v in (x, y):
        v = max(0, min(4095, round(v) + 1024))
        out += B64URL[v >> 6] + B64URL[v & 63]
    return out


def dim(color: str, k: float = 0.55, bg=(0x0B, 0x0F, 0x14)) -> str:
    """Mistura a cor com o fundo (trajetos dos alvos não seguidos ficam mais discretos)."""
    c = color.lstrip("#")
    r, g, b = int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16)
    return "#%02X%02X%02X" % tuple(round(v * k + w * (1 - k)) for v, w in zip((r, g, b), bg))


# ------------------------------------------------------------------ mapa de alvos (ADS-B, AIS)

class TargetMapProvider(Provider):
    """Base do ADS-B e do AIS: serviço no SDR, lista, detalhe e mapa com trajetos e alvo seguido.

    A subclasse define load() (preenche self.targets e devolve True se há dados), station(),
    track(tid), target_row(t), target_detail(t), color(t), e os textos (NOUN, UNIT_DEFAULT...).
    Cada alvo é um dict com "id", "name", "lat", "lon", "heading" e os campos originais em "raw".
    """
    NOUN, NOUN_PL = "alvo", "alvos"
    UNIT_DEFAULT = ""
    ZMIN, ZMAX, FOLLOW_ZOOM, IDLE_ZOOM = 3, 15, 11, 9
    NICE = [0.25, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500]   # passos dos anéis (nm)
    TRACK_BUDGET = 2800        # caracteres de trajetos por tela (a linha JSON tem até 8 KB)
    LEGEND = ""

    def __init__(self, node, app):
        super().__init__(node, app)
        self.unit = node.get("unit", self.UNIT_DEFAULT)
        self.group = node.get("group", "sdr")
        self.targets: list[dict] = []
        self.zoom: int | None = None        # None = automático
        self.follow: str | None = None      # id do alvo seguido no mapa
        self._view: tuple[float, float, int] | None = None   # centro fixo enquanto segue
        self._pan: tuple[float, float] | None = None          # centro escolhido com as setas
        self._last_center: tuple[float, float, int, int, int] | None = None
        self._map = None
        self._prefetched = None
        self._eff_zoom = self.IDLE_ZOOM
        self._active = (0.0, False)
        self.started_here = False   # o DashDeck ligou o serviço (e desliga-o ao sair da tela)

    # ---- serviço
    def active(self) -> bool:
        t, v = self._active
        if time.monotonic() - t > 3:        # systemctl a cada refresh seria caro
            self._active = (time.monotonic(), unit_active(self.unit))
        return self._active[1]

    def _foreign_users(self) -> dict[int, str]:
        """Processos com o SDR que não são serviços do grupo (esses o service_action já pára)."""
        own = {unit_pid(u) for u in self.app.service_groups().get(self.group, [])} - {0}
        return {p: n for p, n in sdr_users().items() if p not in own}

    def _start_confirm(self) -> str:
        t, v = getattr(self, "_conf_cache", (0.0, ""))
        if time.monotonic() - t < 4:
            return v
        v = self._start_confirm_now()
        self._conf_cache = (time.monotonic(), v)
        return v

    def _start_confirm_now(self) -> str:
        users = self._foreign_users()
        busy = [u for u in self.app.service_groups().get(self.group, []) if u != self.unit and unit_active(u)]
        names = sorted(set(users.values()) | set(busy))
        return f"O SDR está em uso por {', '.join(names)[:40]}. Fechar e ligar {self.unit}?" if names else ""

    def _start(self) -> None:
        for pid, name in self._foreign_users().items():
            self.app.events.put(("toast", (f"Fechando {name}", "warn")))
            subprocess.run(["sudo", "-n", "kill", str(pid)], capture_output=True, timeout=5)
        if self._foreign_users():
            time.sleep(1.5)
        self.app.service_action("start", self.unit, self.group)
        self._active = (0.0, False)
        self.started_here = True

    def _stop(self) -> None:
        self.app.service_action("stop", self.unit)
        self._active = (0.0, False)
        self.started_here = False

    def on_leave(self) -> None:
        """Saiu-se da tela (e dos detalhes): se foi o DashDeck a ligar o serviço, desliga-o para o SDR
        ficar livre para outros programas. keep_running = true no config mantém-no ligado."""
        if not self.started_here or self.node.get("keep_running", False):
            return
        self.started_here = False

        def stop():
            self.app.service_action("stop", self.unit)
            self._active = (0.0, False)
            self.app.events.put(("toast", (f"{self.node.get('label', self.unit)} desligado: SDR livre", "ok")))
        threading.Thread(target=stop, daemon=True).start()

    def svc_item(self) -> dict:
        """Primeiro card da lista: estado do serviço; tocar liga ou desliga."""
        if self.active():
            return {"id": "svc", "label": f"{self.unit} ligado", "sub": "Toque para desligar e libertar o SDR",
                    "right": "Desligar", "color": "#3FB950", "bg": "#16271F",
                    "confirm": f"Desligar {self.unit}? O SDR fica livre."}
        it = {"id": "svc", "label": f"{self.unit} desligado", "sub": "Toque para ligar (usa o SDR)",
              "right": "Ligar", "color": "#8B98A5", "bg": "#2A1F16"}
        conf = self._start_confirm()
        if conf:
            it["confirm"] = conf
        return it

    # ---- a definir pelas subclasses
    def load(self) -> bool:
        return False

    def station(self) -> tuple[float, float] | None:
        return None

    def track(self, tid: str) -> list[tuple[float, float]]:
        return []

    def color(self, t: dict) -> str:
        return "#E6EDF3"

    def list_item(self, t: dict) -> dict:
        return {"id": t["id"], "label": t["name"]}

    def detail_rows(self, t: dict) -> list:
        return []

    def follow_rows(self, t: dict) -> list:
        return []

    # ---- comuns
    def find(self, tid: str | None) -> dict | None:
        return next((t for t in self.targets if t["id"] == tid), None) if tid else None

    def pages(self) -> int:
        return 2

    def summary(self) -> str:
        if not self.load():
            return f"{self.unit} parado" if not self.active() else "sem dados"
        pos = sum(1 for t in self.targets if t.get("lat") is not None)
        return f"{len(self.targets)} {self.NOUN_PL} · {pos} no mapa"

    def _tiles(self):
        if self.node.get("map", True) is False:
            return None
        if self._map is None:
            self._map = maptiles.TileMap(self.node.get("map_style", maptiles.DEFAULT_STYLE),
                                         url=self.node.get("map_url"),
                                         on_ready=lambda: self.app.events.put(("refresh", None)))
        return self._map

    def _center(self, with_pos: list[dict]) -> tuple[tuple[float, float] | None, str]:
        st = self.station()
        if st:
            return st, "receptor"
        for prov in self.app.providers.values():
            if isinstance(prov, GpsProvider) and prov.position():
                return prov.position(), "GPS"
        if with_pos:
            return (sum(t["lat"] for t in with_pos) / len(with_pos),
                    sum(t["lon"] for t in with_pos) / len(with_pos)), "média"
        return None, "?"

    def render(self, page: int = 0) -> dict:
        label = self.node.get("label", self.NOUN_PL)
        if page == 1:
            return {"title": f"{label} · Mapa", **self.render_map()}
        out = self.render_list()
        return {"title": f"{label} ({len(self.targets)})" if self.targets else label, **out}

    def render_list(self) -> dict:
        has = self.load()
        items = [self.svc_item()]
        if self.follow and self.find(self.follow):
            t = self.find(self.follow)
            items.append({"id": "unfollow", "label": f"Seguindo {t['name']}", "sub": "Toque para deixar de seguir",
                          "right": "Soltar", "color": "#58A6FF", "bg": "#172638"})
        items += [self.list_item(t) for t in self.targets[:40]]
        empty = "" if has else (f"Sem dados. A aguardar o {self.unit}..." if self.active() else "")
        out = {"tpl": "list", "items": items}
        if len(items) == 1 and empty:
            items[0]["sub"] = empty
        return out

    def render_map(self) -> dict:
        W, H = self.app.canvas_area()
        if W < H:
            H = min(H, 220)
        plot = {"w": W, "h": H, "center": True, "cross": False, "pts": []}
        out = {"tpl": "plot", "plot": plot, "buttons": []}
        if not self.load():
            plot["msg"] = f"Sem dados\n{self.unit} {'a arrancar...' if self.active() else 'desligado'}"
            if self.active():
                out["buttons"] = [{"id": "stop", "label": f"Desligar {self.unit}", "color": "err",
                                   "confirm": f"Desligar {self.unit}? O SDR fica livre."}]
            else:
                b = {"id": "start", "label": f"Ligar {self.unit}", "color": "ok"}
                conf = self._start_confirm()
                if conf:
                    b["confirm"] = conf
                out["buttons"] = [b]
            return out
        with_pos = [t for t in self.targets if t.get("lat") is not None and t.get("lon") is not None]
        ft = self.find(self.follow)
        fpos = None
        if self.follow:
            if ft and ft.get("lat") is not None:
                fpos = (ft["lat"], ft["lon"])
            else:
                last = self.track(self.follow)
                fpos = last[-1] if last else None

        if self._pan:                           # deslocado com as setas: centro e zoom fixos
            self._view = None
            lat0, lon0 = self._pan
            z = self.zoom if self.zoom is not None else self._eff_zoom
            src = "manual"
        elif fpos:
            z = self.zoom if self.zoom is not None else self.FOLLOW_ZOOM
            # o mapa só se move quando o alvo se afasta do centro: evita refazer o fundo a cada refresh
            if self._view and self._view[2] == z:
                vx, vy = maptiles.world_px(self._view[0], self._view[1], z)
                fx, fy = maptiles.world_px(fpos[0], fpos[1], z)
                if abs(fx - vx) > W * 0.3 or abs(fy - vy) > H * 0.3:
                    self._view = None
            if not self._view or self._view[2] != z:
                self._view = (round(fpos[0], 4), round(fpos[1], 4), z)
            lat0, lon0, src = self._view[0], self._view[1], "alvo"
        else:
            self._view = None
            center, src = self._center(with_pos)
            if not center:
                plot["msg"] = f"Nenhum {self.NOUN} com posição"
                out["rows"] = [[self.NOUN_PL.capitalize(), str(len(self.targets))]]
                return out
            nd = 2 if src == "média" else 3     # centro arredondado: não refaz o mapa a cada variação
            lat0, lon0 = round(center[0], nd), round(center[1], nd)
            z = self.zoom
            if z is None and not with_pos:
                z = self.IDLE_ZOOM
            if z is None:                       # maior zoom em que todos cabem
                z = self.ZMIN
                for zz in range(self.ZMAX, self.ZMIN - 1, -1):
                    cx, cy = maptiles.world_px(lat0, lon0, zz)
                    if all(abs(maptiles.world_px(t["lat"], t["lon"], zz)[0] - cx) < W / 2 - 6 and
                           abs(maptiles.world_px(t["lat"], t["lon"], zz)[1] - cy) < H / 2 - 6 for t in with_pos):
                        z = zz
                        break
        self._eff_zoom = z
        self._last_center = (lat0, lon0, z, W, H)
        # zoom nos cantos de cima, setas no meio de cada borda
        plot["ctrl"] = [["zout", "tl", "\u2212"], ["zin", "tr", "+"], ["pan_l", "l", ""], ["pan_r", "r", ""],
                        ["pan_u", "t", ""], ["pan_d", "b", ""]]
        cx, cy = maptiles.world_px(lat0, lon0, z)
        ppn = maptiles.px_per_nm(lat0, z)

        def to_px(lat, lon):
            x, y = maptiles.world_px(lat, lon, z)
            return x - cx + W / 2, y - cy + H / 2

        half_nm = min(W, H) / 2 / ppn
        step = max([n for n in self.NICE if 2 * n <= half_nm] or [self.NICE[0]])
        if not fpos or self._pan:
            plot.update(rings=[round(step * ppn), round(2 * step * ppn)], rlabels=[f"{step:g}", f"{2 * step:g}nm"])
        else:
            plot["center"] = False

        tiles = self._tiles()
        map_state = "desligado"
        if tiles:
            if self._prefetched != (lat0, lon0, z, W, H):
                self._prefetched = (lat0, lon0, z, W, H)
                tiles.prefetch(lat0, lon0, [zz for zz in (z - 1, z + 1) if self.ZMIN <= zz <= self.ZMAX], W, H)
            bg = tiles.render(lat0, lon0, z, W, H)
            if bg:
                plot["bg"] = bg[0]
                out["_bg"] = (bg[0], W, H, bg[1], bg[2])
                map_state = tiles.credit
            else:
                map_state = "a carregar..." if tiles.online else "offline, sem cache"

        # pontos: os mais próximos do centro por cima; o seguido por último
        dist = []
        for t in with_pos:
            px, py = to_px(t["lat"], t["lon"])
            dist.append((math.hypot(px - W / 2, py - H / 2) / ppn, t, px, py))
        dist.sort(key=lambda d: (d[1]["id"] == self.follow, -d[0]))
        hidden = 0
        shown = []
        for d, t, px, py in dist:
            if not (-4 <= px < W + 4 and -4 <= py < H + 4):
                hidden += 1
                continue
            shown.append(t)
            hd = t.get("heading")
            label = t["name"][:8].strip() if (t["id"] == self.follow or t.get("label", True)) else ""
            plot["pts"].append([round(px), round(py), self.color(t), label,
                                round(hd) if isinstance(hd, (int, float)) else None, 1])
        plot["pts"] = plot["pts"][-80:]

        # trajetos: o seguido primeiro (mais longo e grosso), depois os outros visíveis
        tracks, budget = [], self.TRACK_BUDGET
        order = ([self.follow] if self.follow else []) + [t["id"] for t in shown if t["id"] != self.follow]
        for tid in order:
            pts = self.track(tid)
            t = self.find(tid)
            if t and t.get("lat") is not None:
                pts = pts + [(t["lat"], t["lon"])]
            if len(pts) < 2:
                continue
            followed = tid == self.follow
            keep, lx, ly = [], None, None
            for la, lo in reversed(pts):            # do mais novo para o mais antigo
                x, y = to_px(la, lo)
                if lx is None or abs(x - lx) + abs(y - ly) >= 3:
                    keep.append(enc_px(x, y))
                    lx, ly = x, y
                if len(keep) >= (150 if followed else 30):
                    break
            if len(keep) < 2:
                continue
            s = "".join(reversed(keep))
            if len(s) > budget:
                if followed:
                    s = s[-(budget // 4) * 4:]
                else:
                    break
            budget -= len(s)
            col = self.color(t) if t else "#8B98A5"
            tracks.append([col if followed else dim(col), 2 if followed else 1, s])
        if tracks:
            plot["tracks"] = tracks

        rows = []
        if self.follow:
            name = ft["name"] if ft else self.follow
            rows.append(["Seguindo", name, "accent"])
            if ft:
                rows += self.follow_rows(ft)
            else:
                rows.append(["Estado", "sem sinal" if fpos else "sem posição", "warn"])
        rings = not fpos or self._pan
        rows.append(["Zoom", f"{z}" + (" (auto)" if self.zoom is None and not fpos and not self._pan else "")
                     + (f" · anel {step:g} nm" if rings else "")])
        if not fpos or self._pan:
            rows.append(["Centro", src, "warn" if src == "média" else None])
        rows.append(["No mapa", f"{len(plot['pts'])} de {len(self.targets)}"])
        if hidden:
            rows.append(["Fora do mapa", str(hidden), "sub"])
        if dist and not self.follow:
            d, t, *_ = min(dist, key=lambda x: x[0])
            rows.append(["Mais próximo", f"{t['name']} {d:.1f} nm"])
        if self.LEGEND:
            rows.append(["Cores", self.LEGEND, "sub"])
        rows.append(["Mapa", map_state, "sub"])
        out["rows"] = [r if r[-1] is not None else r[:-1] for r in rows]
        if self._pan:
            out["buttons"].append({"id": "recenter", "label": "Centrar", "color": "#30363D"})
        if self.follow:
            out["buttons"].append({"id": "unfollow", "label": "Soltar", "color": "#30363D"})
        return out

    def on_tap(self, item_id: str):
        if item_id == "svc":
            if self.active():
                self.app.run_bg(self._stop)
                return (f"Desligando {self.unit}...", "info")
            self.app.run_bg(self._start)
            return (f"Ligando {self.unit}...", "info")
        if item_id == "unfollow":
            return self.on_button("unfollow")
        t = self.find(item_id)
        if not t:
            return None
        following = self.follow == t["id"]
        return {"id": f"tg_{t['id']}", "label": t["name"], "tpl": "kv", "rows": self.detail_rows(t),
                "owner": self.node["id"],
                "buttons": [{"id": "unfollow", "label": "Deixar de seguir", "color": "#30363D"} if following else
                            {"id": f"follow:{t['id']}", "label": "Seguir no mapa", "color": "accent"}]}

    def on_button(self, btn_id: str):
        if btn_id == "start":
            self.app.run_bg(self._start)
            return (f"Ligando {self.unit}...", "info")
        if btn_id == "stop":
            self.app.run_bg(self._stop)
            return (f"Desligando {self.unit}...", "info")
        if btn_id.startswith("follow:"):
            self.follow = btn_id.split(":", 1)[1]
            self.zoom, self._view, self._pan = None, None, None
            self.app.page[self.node["id"]] = 1          # abre o mapa
            t = self.find(self.follow)
            return ("back", f"Seguindo {t['name'] if t else self.follow}")
        if btn_id == "recenter":
            self._pan, self.zoom = None, None
            return ("Mapa centrado", "info")
        if btn_id.startswith("pan_") and self._last_center:
            lat0, lon0, z, W, H = self._last_center
            x, y = maptiles.world_px(lat0, lon0, z)
            dx, dy = {"pan_l": (-W / 3, 0), "pan_r": (W / 3, 0), "pan_u": (0, -H / 3), "pan_d": (0, H / 3)}[btn_id]
            lat, lon = maptiles.latlon(x + dx, y + dy, z)
            self._pan = (round(lat, 5), round(lon, 5))
            if self.zoom is None:
                self.zoom = z
            return None
        if btn_id == "unfollow":
            self.follow, self._view, self._pan = None, None, None
            return ("back", "Mapa livre") if self.app.node().get("type") == "detail" else ("Mapa livre", "info")
        if btn_id in ("zin", "zout"):
            cur = self.zoom if self.zoom is not None else self._eff_zoom
            nxt = cur + (1 if btn_id == "zin" else -1)
            if not self.ZMIN <= nxt <= self.ZMAX:
                if self._pan:               # com o mapa deslocado não há zoom automático
                    return (f"Zoom {'máximo' if nxt > self.ZMAX else 'mínimo'}", "info")
                self.zoom = None
                return ("Zoom automático", "info")
            self.zoom = nxt
            return (f"Zoom {nxt}", "info")
        return None


def _fmt_rows(raw: dict, spec) -> list:
    rows = []
    for key, label, fmt in spec:
        v = raw.get(key)
        if v is None or v == "":
            continue
        try:
            rows.append([label, fmt.format(v).strip()])
        except (ValueError, TypeError):
            rows.append([label, str(v)])
    return rows


class AdsbProvider(TargetMapProvider):
    NOUN, NOUN_PL = "aeronave", "aeronaves"
    UNIT_DEFAULT = "readsb"
    FOLLOW_ZOOM = 10
    LEGEND = "<10k · <25k · acima ft"
    PATHS = ["/run/readsb/aircraft.json", "/run/dump1090-mutability/aircraft.json",
             "/run/dump1090-fa/aircraft.json", "/run/dump1090/aircraft.json"]

    def __init__(self, node, app):
        super().__init__(node, app)
        self.paths = [node["json"]] if node.get("json") else self.PATHS
        self.receiver: tuple[float, float] | None = None
        self.tracker = Tracker(max_age=float(node.get("track_minutes", 30)) * 60)
        self._loaded = (0.0, False)
        threading.Thread(target=self._poll, daemon=True).start()

    def _poll(self) -> None:
        """Acumula os trajetos mesmo com a tela fechada (o readsb só dá a posição atual)."""
        n = 0
        while True:
            try:
                self.load(force=True)
                n += 1
                if n % 30 == 0:
                    self.tracker.prune()
            except Exception as e:
                log.debug("adsb poll: %s", e)
            time.sleep(2)

    def _read_receiver(self, path: str) -> tuple[float, float] | None:
        try:
            with open(os.path.join(os.path.dirname(path), "receiver.json")) as f:
                r = json.load(f)
            if r.get("lat") is not None and r.get("lon") is not None and (r["lat"] or r["lon"]):
                return float(r["lat"]), float(r["lon"])
        except (OSError, json.JSONDecodeError, ValueError, TypeError):
            pass
        return None

    def load(self, force: bool = False) -> bool:
        t, ok = self._loaded
        if not force and time.monotonic() - t < 1:
            return ok
        for p in self.paths:
            try:
                st = os.stat(p)
                if time.time() - st.st_mtime > 60:
                    continue
                with open(p) as f:
                    data = json.load(f)
            except (OSError, json.JSONDecodeError):
                continue
            now = data.get("now", time.time())
            out = []
            for a in data.get("aircraft", []):
                if a.get("seen", 999) >= 60:
                    continue
                hexid = a.get("hex", "")
                name = (a.get("flight") or "").strip() or hexid.upper()
                trk = a.get("track")
                tg = {"id": hexid, "name": name, "lat": a.get("lat"), "lon": a.get("lon"),
                      "heading": trk, "label": bool((a.get("flight") or "").strip()), "raw": a}
                if tg["lat"] is not None and a.get("seen_pos", 0) < 30:
                    self.tracker.add(hexid, tg["lat"], tg["lon"], now - a.get("seen_pos", 0))
                out.append(tg)
            out.sort(key=lambda t: (t["lat"] is None, t["raw"].get("seen", 999)))
            self.targets = out
            self.receiver = self._read_receiver(p)
            self._loaded = (time.monotonic(), True)
            return True
        self.targets = []
        self._loaded = (time.monotonic(), False)
        return False

    def station(self):
        return self.receiver

    def track(self, tid):
        return self.tracker.get(tid)

    def color(self, t):
        a = t["raw"]
        if a.get("squawk") in ("7500", "7600", "7700"):
            return "#F85149"
        alt = a.get("alt_baro", a.get("altitude"))
        if alt == "ground":
            return "#8B98A5"
        if not isinstance(alt, (int, float)):
            return "#E6EDF3"
        return "#3FB950" if alt < 10000 else ("#58A6FF" if alt < 25000 else "#D2A8FF")

    @staticmethod
    def _alt(a: dict) -> str:
        alt = a.get("alt_baro", a.get("altitude"))
        return "solo" if alt == "ground" else (f"{alt} ft" if alt is not None else "")

    def list_item(self, t):
        a = t["raw"]
        gs = a.get("gs", a.get("speed"))
        sub = " · ".join(x for x in [self._alt(a), f"{gs:.0f} kt" if isinstance(gs, (int, float)) else ""] if x)
        emerg = a.get("squawk") in ("7500", "7600", "7700")
        return {"id": t["id"], "label": t["name"], "sub": sub, "right": a.get("squawk", ""),
                "color": "#F85149" if emerg else ("#58A6FF" if t["id"] == self.follow else
                                                 "#E6EDF3" if t["lat"] is not None else "#8B98A5")}

    def follow_rows(self, t):
        a = t["raw"]
        gs = a.get("gs")
        rows = [["Altitude", self._alt(a) or "?"]]
        if isinstance(gs, (int, float)):
            rows.append(["Veloc.", f"{gs:.0f} kt" + (f" · {a['track']:.0f}°" if a.get("track") is not None else "")])
        return rows

    def detail_rows(self, t):
        a = t["raw"]
        return [["Hex", t["id"].upper()]] + _fmt_rows(a, [
            ("flight", "Voo", "{}"), ("r", "Matrícula", "{}"), ("t", "Tipo", "{}"),
            ("squawk", "Squawk", "{}"), ("alt_baro", "Alt baro", "{} ft"), ("alt_geom", "Alt GNSS", "{} ft"),
            ("gs", "Veloc.", "{:.0f} kt"), ("track", "Rumo", "{:.0f}°"), ("baro_rate", "V/S", "{} ft/min"),
            ("lat", "Lat", "{:.4f}"), ("lon", "Lon", "{:.4f}"), ("r_dst", "Dist.", "{:.1f} nm"),
            ("rssi", "RSSI", "{:.1f} dBFS"), ("seen", "Visto", "{:.0f} s")]) + \
            [["Trajeto", f"{len(self.track(t['id']))} pontos", "sub"]]


AIS_TYPES = [  # (de, até, nome, cor)
    (20, 29, "WIG", "#8B98A5"), (30, 30, "Pesca", "#F0883E"), (31, 32, "Reboque", "#39C5CF"),
    (33, 35, "Serviço", "#39C5CF"), (36, 36, "Veleiro", "#D2A8FF"), (37, 37, "Recreio", "#D2A8FF"),
    (40, 49, "Alta velocidade", "#E3B341"), (50, 59, "Serviço", "#39C5CF"), (60, 69, "Passageiros", "#58A6FF"),
    (70, 79, "Carga", "#3FB950"), (80, 89, "Tanque", "#F85149"), (90, 99, "Outro", "#E6EDF3")]
AIS_STATUS = {0: "a navegar (motor)", 1: "fundeado", 2: "sem governo", 3: "manobra restrita",
              4: "condicionado pelo calado", 5: "atracado", 6: "encalhado", 7: "a pescar", 8: "a navegar (vela)",
              14: "AIS-SART", 15: ""}


class AisProvider(TargetMapProvider):
    NOUN, NOUN_PL = "navio", "navios"
    UNIT_DEFAULT = "ais-catcher"
    FOLLOW_ZOOM, IDLE_ZOOM, ZMAX = 13, 11, 16
    LEGEND = "carga · tanque · passag. · pesca · recreio"
    MAX_AGE = 900              # navios sem sinal há mais de 15 min saem da lista

    def __init__(self, node, app):
        super().__init__(node, app)
        self.url = node.get("url", "http://127.0.0.1:8119").rstrip("/")
        self._station = None
        self._loaded = (0.0, False)
        self._paths: tuple[float, dict] = (0.0, {})

    def _get(self, path: str):
        import urllib.request
        with urllib.request.urlopen(self.url + path, timeout=2) as r:
            return json.loads(r.read())

    def load(self, force: bool = False) -> bool:
        t, ok = self._loaded
        if not force and time.monotonic() - t < 1.5:
            return ok
        try:
            data = self._get("/api/ships.json")
        except (OSError, ValueError) as e:
            log.debug("ais: %s", e)
            self.targets = []
            self._loaded = (time.monotonic(), False)
            return False
        st = data.get("station") or {}
        self._station = (st["lat"], st["lon"]) if st.get("lat") or st.get("lon") else None
        out = []
        for s in data.get("ships", []):
            if (s.get("last_signal") or 0) > self.MAX_AGE:
                continue
            mmsi = str(s.get("mmsi", ""))
            spd = s.get("speed")
            hdg = s.get("cog") if isinstance(spd, (int, float)) and spd >= 0.5 else s.get("heading")
            if hdg is not None and not 0 <= hdg < 360:
                hdg = None
            out.append({"id": mmsi, "name": (s.get("shipname") or "").strip() or mmsi,
                        "lat": s.get("lat"), "lon": s.get("lon"), "heading": hdg,
                        "label": bool((s.get("shipname") or "").strip()) and self.type_info(s)[0] not in
                        ("Baliza (AtoN)", "Estação costeira"), "raw": s})
        out.sort(key=lambda t: (t["lat"] is None, t["raw"].get("last_signal") or 0))
        self.targets = out
        self._loaded = (time.monotonic(), True)
        return True

    def station(self):
        return self._station

    def track(self, tid):
        t, paths = self._paths
        if time.monotonic() - t > 5:
            try:
                paths = self._get("/api/allpath.json")
            except (OSError, ValueError):
                paths = {}
            self._paths = (time.monotonic(), paths)
        # [[lat, lon, t_primeiro, t_último], ...] do mais novo para o mais antigo
        return [(p[0], p[1]) for p in reversed(paths.get(tid, [])) if p[0] is not None]

    @staticmethod
    def type_info(s: dict) -> tuple[str, str]:
        if s.get("shipclass") == 12 or str(s.get("mmsi", "")).startswith("99"):
            return "Baliza (AtoN)", "#8B98A5"
        if str(s.get("mmsi", "")).startswith("00"):
            return "Estação costeira", "#8B98A5"
        st = s.get("shiptype") or 0
        for lo, hi, name, col in AIS_TYPES:
            if lo <= st <= hi:
                return name, col
        return "Desconhecido", "#E6EDF3"

    def color(self, t):
        return self.type_info(t["raw"])[1]

    def list_item(self, t):
        s = t["raw"]
        kind = self.type_info(s)[0]
        spd = s.get("speed")
        parts = [kind]
        if isinstance(spd, (int, float)):
            parts.append(f"{spd:.1f} kt")
        if s.get("distance") is not None:
            parts.append(f"{s['distance']:.1f} nm")
        parts.append(fmt_ago(s.get("last_signal")))
        return {"id": t["id"], "label": t["name"], "sub": " · ".join(parts), "right": s.get("country") or "",
                "color": "#58A6FF" if t["id"] == self.follow else
                ("#E6EDF3" if t["lat"] is not None else "#8B98A5")}

    def follow_rows(self, t):
        s = t["raw"]
        rows = [["Tipo", self.type_info(s)[0]]]
        if isinstance(s.get("speed"), (int, float)):
            rows.append(["Veloc.", f"{s['speed']:.1f} kt" + (f" · {s['cog']:.0f}°" if s.get("cog") is not None else "")])
        st = AIS_STATUS.get(s.get("status"))
        if st:
            rows.append(["Estado", st])
        return rows

    def detail_rows(self, t):
        s = t["raw"]
        rows = [["MMSI", t["id"]], ["Tipo", self.type_info(s)[0]]]
        rows += _fmt_rows(s, [("shipname", "Nome", "{}"), ("callsign", "Indicativo", "{}"), ("imo", "IMO", "{}"),
                              ("country", "País", "{}"), ("destination", "Destino", "{}")])
        st = AIS_STATUS.get(s.get("status"))
        if st:
            rows.append(["Estado", st])
        rows += _fmt_rows(s, [("speed", "Veloc.", "{:.1f} kt"), ("cog", "Rumo", "{:.0f}°"),
                              ("heading", "Proa", "{}°"), ("lat", "Lat", "{:.4f}"), ("lon", "Lon", "{:.4f}"),
                              ("distance", "Dist.", "{:.1f} nm")])
        if s.get("to_bow") is not None and s.get("to_stern") is not None:
            rows.append(["Comprimento", f"{s['to_bow'] + s['to_stern']} m"])
        rows += _fmt_rows(s, [("draught", "Calado", "{} m"), ("level", "Sinal", "{:.1f} dB")])
        rows.append(["Visto", fmt_ago(s.get("last_signal"))])
        rows.append(["Trajeto", f"{len(self.track(t['id']))} pontos", "sub"])
        return rows


# ------------------------------------------------------------------ AIO v2

class AioProvider(Provider):
    """Interruptores da placa AIO v2 (GPS, LoRa, SDR, USB) pelo aiov2_ctl, a aplicação "AIO v2 Controller".

    O estado vem de `aiov2_ctl --status` (o mesmo pinctrl que a aplicação lê) numa thread, e cada toque
    corre `aiov2_ctl <RECURSO> on|off`, que no LoRa também liga/pára o meshtasticd. Desligar pede
    confirmação (cortar o LoRa pára o Meshtastic; o SDR pode estar em uso).
    """
    FEATURES = [  # (recurso, rótulo, imagem, o que desligar corta)
        ("GPS", "GPS", "emoji:🛰", "o GPS deixa de dar posição"),
        ("LORA", "LoRa", "emoji:📶", "o meshtasticd é parado"),
        ("SDR", "SDR", "emoji:📡", "ADS-B e AIS deixam de receber"),
        ("USB", "USB", "emoji:🔌", "as portas USB da placa ficam sem energia"),
    ]
    ON_BG, OFF_BG = "#163A24", "#18202A"
    POWER_FILE = Path.home() / ".config" / "dashd" / "aio_power.json"
    BAT = Path("/sys/class/power_supply/axp20x-battery")
    AC = Path("/sys/class/power_supply/axp22x-ac/online")

    # ---- energia
    def battery(self) -> dict:
        """Bateria e potência total agora (o que se lê sem desligar nada)."""
        def rd(f):
            try:
                return (self.BAT / f).read_text().strip()
            except OSError:
                return None
        out = {"pct": rd("capacity"), "status": rd("status"), "ac": False, "w": None}
        try:
            out["ac"] = self.AC.read_text().strip() == "1"
        except OSError:
            pass
        v, i = rd("voltage_now"), rd("current_now")
        if v and i:
            out["w"] = abs(int(v) * int(i)) / 1e12
        return out

    def _measure(self, feat: str) -> None:
        """aiov2_ctl --measure: mede a bateria com o recurso ligado e desligado (uns segundos cada)."""
        label = next(f[1] for f in self.FEATURES if f[0] == feat)
        try:
            r = self._run("--measure", feat, "--seconds", "3", "--settle", "1", timeout=60)
            m = re.search(r"Δ\s*([+-]?\d+(?:\.\d+)?)\s*W", r.stdout)
        except (OSError, subprocess.TimeoutExpired) as e:
            m, r = None, None
            log.warning("aio medir %s: %s", feat, e)
        self.measuring = None
        if m:
            w = max(0.0, float(m.group(1)))
            self.measured[feat] = {"w": round(w, 2), "t": time.time()}
            try:
                self.POWER_FILE.parent.mkdir(parents=True, exist_ok=True)
                self.POWER_FILE.write_text(json.dumps(self.measured))
            except OSError:
                pass
            self.app.events.put(("toast", (f"{label}: {w:.2f} W".replace(".", ","), "ok")))
        else:
            self.app.events.put(("toast", (f"{label}: medição falhou", "err")))
        self._read()
        self.app.events.put(("refresh", None))

    def _watts(self, feat: str) -> str:
        m = self.measured.get(feat)
        return f"{m['w']:.1f} W".replace(".", ",") if m else ""

    def _info(self, feat: str) -> str:
        """Linha de informação ao vivo de cada recurso (só leituras)."""
        if feat == "GPS":
            gps = next((p for p in self.app.providers.values() if isinstance(p, GpsProvider)), None)
            if not gps:
                return ""
            if gps.source() == "meshtastic":
                return "pelo Meshtastic" if meshbridge.shared().position() else "sem posição"
            return gps.summary()
        if feat == "LORA":
            return "meshtasticd ativo" if unit_active("meshtasticd") else "meshtasticd parado"
        if feat == "SDR":
            sdr = next((p for p in self.app.providers.values() if isinstance(p, SdrProvider)), None)
            return sdr.summary() if sdr else ""
        return ""

    def render_detail(self, kind: str) -> dict:
        """Energia: bateria agora e o consumo de cada recurso (medido), com os botões Medir."""
        b = self.battery()
        st = {"Discharging": "a descarregar", "Charging": "a carregar", "Full": "cheia",
              "Not charging": "sem carregar"}.get(b.get("status") or "", (b.get("status") or "").lower())
        rows = [["Bateria", f"{b['pct']}%" + (f" · {st}" if st else "")]]
        if b["w"] is not None:
            rows.append(["Potência agora", f"{b['w']:.2f} W".replace(".", ","), "warn" if b["w"] > 6 else None])
        rows.append(["Fonte", "carregador" if b["ac"] else "bateria"])
        for feat, label, _, cut in self.FEATURES:
            m = self.measured.get(feat)
            when = time.strftime("%d/%m %H:%M", time.localtime(m["t"])) if m else ""
            rows.append([label, (f"{self._watts(feat)} · medido {when}" if m else "por medir")
                         + (" · a medir..." if self.measuring == feat else ""), None if m else "sub"])
        rows = [r if len(r) < 3 or r[2] else r[:2] for r in rows]
        btns = []
        if not self.measuring:
            for feat, label, _, cut in self.FEATURES:
                warn = f"Medir {label}? Fica {'desligado' if self.state.get(feat) else 'ligado'} uns 5 s"
                if feat == "LORA" and self.state.get(feat):
                    warn += " (o meshtasticd reinicia)"
                btns.append({"id": f"m:{feat}", "label": f"Medir {label}", "color": "warn", "confirm": warn + "."})
        return {"tpl": "kv", "rows": rows, "buttons": btns}

    def on_button(self, btn_id: str):
        if btn_id.startswith("m:") and not self.measuring:
            feat = btn_id[2:]
            if self.battery()["ac"]:
                return ("Desligue o carregador para medir (a medição usa a bateria)", "warn")
            self.measuring = feat
            threading.Thread(target=self._measure, args=(feat,), daemon=True).start()
            return ("A medir... (uns 10 s)", "info")
        return None

    def __init__(self, node, app):
        super().__init__(node, app)
        self.cmd = node.get("cmd", "/usr/local/bin/aiov2_ctl")
        self.state: dict[str, bool] = {}
        self.busy: dict[str, bool] = {}       # recurso a mudar (até a leitura seguinte confirmar)
        self.error = ""
        self.measured: dict[str, dict] = {}   # recurso -> {"w": Δ W, "t": hora} (medições guardadas)
        self.measuring: str | None = None
        try:
            self.measured = json.loads(self.POWER_FILE.read_text())
        except (OSError, ValueError):
            pass
        self._wake = threading.Event()
        threading.Thread(target=self._poll, daemon=True, name="aio").start()

    def _run(self, *args, timeout=20) -> subprocess.CompletedProcess:
        return subprocess.run(["/usr/bin/python3", self.cmd, *args], capture_output=True, text=True,
                              timeout=timeout)

    def _read(self) -> None:
        try:
            out = self._run("--status", timeout=10).stdout
        except (OSError, subprocess.TimeoutExpired) as e:
            self.error = f"aiov2_ctl: {e}"
            return
        st = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1].startswith("GPIO") and parts[0] in dict((f[0], 1) for f in self.FEATURES):
                st[parts[0]] = parts[-1].upper() == "ON"
        if st:
            self.state, self.error = st, ""
        else:
            self.error = "sem estado do aiov2_ctl"

    def _poll(self) -> None:
        while True:
            self._read()
            self.app.events.put(("refresh", None))
            self._wake.wait(5)
            self._wake.clear()

    def _toggle(self, feat: str, on: bool) -> None:
        label = next(f[1] for f in self.FEATURES if f[0] == feat)
        try:
            r = self._run(feat, "on" if on else "off", timeout=40)
            ok = r.returncode == 0
        except (OSError, subprocess.TimeoutExpired) as e:
            ok, r = False, None
            log.warning("aio %s: %s", feat, e)
        self._read()
        self.busy.pop(feat, None)
        if ok and self.state.get(feat) == on:
            self.app.events.put(("toast", (f"{label} {'ligado' if on else 'desligado'}", "ok")))
        else:
            err = (r.stderr or r.stdout).strip()[:60] if r else "sem resposta"
            self.app.events.put(("toast", (f"{label}: falhou ({err or 'estado não mudou'})", "err")))
        self.app.events.put(("refresh", None))

    def summary(self) -> str:
        if not self.state:
            return self.error or "a ler..."
        on = [f[1] for f in self.FEATURES if self.state.get(f[0])]
        return " · ".join(on) if on else "tudo desligado"

    def render(self, page: int = 0) -> dict:
        items = []
        for feat, label, img, cut in self.FEATURES:
            on = self.state.get(feat)
            info = self._info(feat)
            it = {"id": feat, "label": f"{label}\n{info}" if info else label, "img": img, "icon": "power"}
            w = self._watts(feat)
            if feat in self.busy:
                it.update(sub="a mudar...", sub_color="#D29922", bg=self.ON_BG if on else self.OFF_BG)
            elif on is None:
                it.update(sub="?", bg=self.OFF_BG)
            elif on:
                it.update(sub="Ligado" + (f" · {w}" if w else ""), sub_color="#3FB950", bg=self.ON_BG,
                          confirm=f"Desligar {label}? ({cut})")
            else:
                it.update(sub="Desligado" + (f" · {w}" if w else ""), sub_color="#8B98A5", bg=self.OFF_BG)
            items.append(it)
        b = self.battery()
        pw = f"{b['w']:.1f} W".replace(".", ",") if b["w"] is not None else "?"
        items.append({"id": "power", "label": f"Energia\n{b['pct']}% · {pw}", "img": "emoji:🔋", "icon": "charge",
                      "sub": "carregador" if b["ac"] else "bateria", "bg": self.OFF_BG,
                      "sub_color": "#D29922" if b["w"] and b["w"] > 6 else "#8B98A5"})
        scr = {"tpl": "menu", "layout": "grid", "items": items}
        if self.error and not self.state:
            scr["tpl"], scr["text"] = "text", f"AIO v2 indisponível: {self.error}"
            scr.pop("items")
        return scr

    def on_tap(self, item_id: str):
        if item_id == "power":
            return {"id": "aio_power", "label": "Energia", "dynamic": "power", "owner": self.node["id"],
                    **self.render_detail("power")}
        if item_id not in self.state or item_id in self.busy:
            return None
        on = not self.state[item_id]
        self.busy[item_id] = True
        threading.Thread(target=self._toggle, args=(item_id, on), daemon=True).start()
        return None


# ------------------------------------------------------------------ relógio

class ClockProvider(Provider):
    """Modo relógio em ecrã inteiro (tpl "clock"): paisagem ou digital retro, com a previsão do dia.

    O ESP conta a hora sozinho (acertada com {"t":"time"}); este provider envia a previsão, o ícone e,
    na paisagem, o fundo desenhado em clockscene.py (refeito a cada 5 min ou quando o tempo muda).
    O mostrador e o brilho escolhidos com gestos ficam em ~/.config/dashd/clock.json.
    """
    STATE = Path.home() / ".config" / "dashd" / "clock.json"
    LCD = {"ambar": 0, "âmbar": 0, "verde": 1, "ciano": 2}

    def __init__(self, node, app):
        super().__init__(node, app)
        import clockscene
        self.cs = clockscene
        self.face = 0 if node.get("face", "paisagem") == "paisagem" else 1
        self.bl = int(node.get("brightness", 50))
        try:
            st = json.loads(self.STATE.read_text())
            self.face = int(st.get("face", self.face)) & 1
            self.bl = int(st.get("bl", self.bl))
        except (OSError, ValueError):
            pass
        self.pal = self.LCD.get(str(node.get("lcd_color", "ambar")).lower(), 0)
        self._scene: tuple | None = None
        # a posição do GPS (modo tty) só existe com o provider GPS criado
        gps = next((n for n in app.cfg.get("menu", []) if n.get("provider") == "gps"), None)
        if gps:
            try:
                app.provider(gps)
            except Exception as e:
                log.debug("relógio: GPS indisponível: %s", e)
        self.weather =clockscene.Weather(self._position, on_update=lambda: app.events.put(("refresh", None))) \
            if node.get("weather", True) else None

    def _position(self) -> tuple[float, float] | None:
        if self.node.get("lat") is not None and self.node.get("lon") is not None:
            return float(self.node["lat"]), float(self.node["lon"])
        for prov in self.app.providers.values():
            if isinstance(prov, GpsProvider) and prov.position():
                return prov.position()
        p = meshbridge.shared().position()
        return (p[0], p[1]) if p else None

    def _save(self) -> None:
        try:
            self.STATE.parent.mkdir(parents=True, exist_ok=True)
            self.STATE.write_text(json.dumps({"face": self.face, "bl": self.bl}))
        except OSError as e:
            log.debug("relógio: %s", e)

    def on_event(self, data: dict) -> None:
        """{"t":"clock","face":"1"} ou {"t":"clock","bl":"60"} vindos dos gestos no ESP."""
        if "face" in data:
            self.face = int(data["face"]) & 1
        if "bl" in data:
            self.bl = max(10, min(100, int(data["bl"])))
        self._save()

    def _wx(self) -> dict | None:
        return self.weather.current() if self.weather else None

    def summary(self) -> str:
        wx = self._wx()
        return f"{wx['t']}° · {self.cs.KINDS.get(wx['kind'], '')}" if wx else "sem previsão"

    def render(self, page: int = 0) -> dict:
        now = time.localtime()
        minute = now.tm_hour * 60 + now.tm_min
        wx = self._wx()
        kind = wx["kind"] if wx else "limpo"
        sr = (wx or {}).get("sunrise") or 7 * 60
        ss = (wx or {}).get("sunset") or 18 * 60
        night = self.cs.sun_elev(minute, sr, ss) < -0.05
        clock = {"face": self.face, "pal": self.pal, "bl": self.bl, "night": night,
                 "dim": int(self.node.get("dim_after", 300)), "dim_bl": int(self.node.get("dim_brightness", 10)),
                 "anim": self.cs.ANIM.get(kind, "")}
        if wx:
            clock["wx"] = {k: wx[k] for k in ("t", "max", "min", "rain", "wind")}
            clock["icon"] = self.cs.weather_icon(kind, night)
        scr = {"tpl": "clock", "clock": clock}
        if self.face == 0:
            w, h = self.app.esp_w, self.app.esp_h
            key = f"clk{w}{kind[:3]}{time.strftime('%d%H')}{minute % 60 // 5}"
            if not self._scene or self._scene[0] != key:
                t0 = time.monotonic()
                img = self.cs.render_scene(w, h, minute, kind, sr, ss, time.time())
                self._scene = self.cs.scene_bg(key, img)
                log.debug("relógio: cena %s em %.2f s", key, time.monotonic() - t0)
            clock["bg"] = key
            scr["_bg"] = self._scene
        return scr


REGISTRY = {"system": SystemProvider, "gps": GpsProvider, "adsb": AdsbProvider, "ais": AisProvider,
            "mesh": MeshtasticProvider, "clock": ClockProvider, "aio": AioProvider, "sdr": SdrProvider}


def create(name: str, node: dict, app) -> Provider:
    cls = REGISTRY.get(name)
    if not cls:
        raise ValueError(f"provider desconhecido: {name}")
    return cls(node, app)
