#!/usr/bin/python3
"""provision — configura o Deck³ pela USB: token do deckd e redes Wi-Fi (guardadas na NVS do aparelho).

  setup/deck3/provision.py --current                 a rede Wi-Fi a que o uConsole está ligado (do NetworkManager)
  setup/deck3/provision.py --nm "Casa" --nm "Hotspot uConsole"   redes guardadas no NetworkManager
  setup/deck3/provision.py --ssid Rede --pass segredo
  setup/deck3/provision.py --host 192.168.1.120:8765 endereço fixo do deckd ("" = automático, por mDNS)
  setup/deck3/provision.py --show                    só mostra o que o aparelho tem
  setup/deck3/provision.py --clear                   apaga as redes guardadas

O token vem de ~/.config/deck3/token (criado pelo deckd ou aqui). Só a USB aceita token e host.
O Deck³ é encontrado pelo hello ("dev":"deck3"); a porta do DashDeck (/dev/esp-dash, mesmo VID:PID) é
ignorada enquanto o dashd a estiver a usar.
"""
import argparse
import glob
import json
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path

import serial  # pyserial (python3-serial)

TOKEN = Path.home() / ".config" / "deck3" / "token"


def token() -> str:
    if not TOKEN.exists():
        TOKEN.parent.mkdir(parents=True, exist_ok=True)
        TOKEN.write_text(secrets.token_hex(16) + "\n")
        TOKEN.chmod(0o600)
    return TOKEN.read_text().strip()


def nmcli(*args: str) -> str:
    return subprocess.run(["nmcli", *args], capture_output=True, text=True).stdout.strip()


def nm_network(conn: str) -> tuple[str, str]:
    """(ssid, palavra-passe) de uma ligação Wi-Fi do NetworkManager."""
    ssid = nmcli("-g", "802-11-wireless.ssid", "connection", "show", conn)
    psk = nmcli("-s", "-g", "802-11-wireless-security.psk", "connection", "show", conn)
    if not ssid:
        sys.exit(f"ligação '{conn}' não encontrada no NetworkManager (nmcli connection show)")
    if not psk and nmcli("-g", "802-11-wireless-security.key-mgmt", "connection", "show", conn):
        sys.exit(f"sem acesso à palavra-passe de '{conn}': corra com sudo ou use --ssid/--pass")
    return ssid, psk


def current_connection() -> str:
    for ln in nmcli("-t", "-f", "NAME,TYPE", "connection", "show", "--active").splitlines():
        name, _, typ = ln.rpartition(":")
        if typ == "802-11-wireless":
            return name
    sys.exit("o uConsole não está ligado a nenhuma rede Wi-Fi")


def busy_ports() -> set[str]:
    """Portas a evitar: a do DashDeck enquanto o dashd corre."""
    out = set()
    if subprocess.run(["systemctl", "--user", "is-active", "--quiet", "dashd"]).returncode == 0 \
            and os.path.exists("/dev/esp-dash"):
        out.add(os.path.realpath("/dev/esp-dash"))
    return out


def read_json(port: serial.Serial, want: str, timeout: float = 3.0) -> dict | None:
    end = time.time() + timeout
    while time.time() < end:
        ln = port.readline().decode(errors="replace").strip()
        if not ln or ln.startswith("#"):
            continue
        try:
            msg = json.loads(ln)
        except json.JSONDecodeError:
            continue
        if msg.get("t") == want:
            return msg
    return None


def send(port: serial.Serial, msg: dict) -> None:
    port.write((json.dumps(msg, ensure_ascii=False) + "\n").encode())
    port.flush()


def find_deck(explicit: str | None) -> serial.Serial:
    cands = [explicit] if explicit else sorted(set(glob.glob("/dev/ttyACM*")) - busy_ports())
    for dev in cands:
        try:
            p = serial.Serial(dev, 115200, timeout=0.3)
        except serial.SerialException:
            continue
        p.reset_input_buffer()
        send(p, {"t": "hello?"})
        h = read_json(p, "hello", 2.0)
        if h and h.get("dev") == "deck3":
            print(f"Deck³ em {dev}: firmware {h.get('fw')}, id {h.get('id')}")
            return p
        p.close()
    sys.exit("Deck³ não encontrado pela USB (ligado? firmware gravado? use --port /dev/ttyACMx)")


def main() -> int:
    ap = argparse.ArgumentParser(description="Configura o Deck³ pela USB")
    ap.add_argument("--port")
    ap.add_argument("--current", action="store_true", help="a rede Wi-Fi atual do uConsole")
    ap.add_argument("--nm", action="append", default=[], help="ligação do NetworkManager (pode repetir)")
    ap.add_argument("--ssid")
    ap.add_argument("--pass", dest="password", default="")
    ap.add_argument("--host", help='ip:porta fixo do deckd, ou "" para automático')
    ap.add_argument("--clear", action="store_true")
    ap.add_argument("--show", action="store_true")
    a = ap.parse_args()

    p = find_deck(a.port)
    if not a.show:
        send(p, {"t": "token", "v": token()})
        read_json(p, "cfg")
        if a.clear:
            send(p, {"t": "wifi", "clear": True})
            read_json(p, "cfg")
        nets = [nm_network(c) for c in a.nm]
        if a.current:
            nets.append(nm_network(current_connection()))
        if a.ssid:
            nets.append((a.ssid, a.password))
        for ssid, psk in nets:
            send(p, {"t": "wifi", "ssid": ssid, "pass": psk})
            r = read_json(p, "cfg")
            print(f"  rede {ssid}: {'ok' if r and r.get('ok') else (r or {}).get('err', 'sem resposta')}")
        if a.host is not None:
            send(p, {"t": "host", "addr": a.host})
            read_json(p, "cfg")
    send(p, {"t": "cfg?"})
    time.sleep(0.2)
    r = read_json(p, "cfg")
    if r:
        print(f"  redes: {', '.join(r.get('nets', [])) or '(nenhuma)'}")
        print(f"  estado: {r.get('state')}  {r.get('ssid', '')} {r.get('ip', '')}")
        print(f"  deckd: {r.get('host')}  token: {'sim' if r.get('token') else 'não'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
