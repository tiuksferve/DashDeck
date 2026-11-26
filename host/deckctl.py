#!/usr/bin/python3
"""deckctl — controla o deckd a partir do uConsole (scripts, atalhos, outros programas).

  deckctl.py status                         aparelhos ligados
  deckctl.py notify "Texto" [--from Nome] [--kind alarm|squawk] [--vox modelo]
  deckctl.py toast "Texto" [--level ok|warn|err|info]
  deckctl.py beep [freq] [ms]
  deckctl.py send '{"t":"bl","v":30}'       qualquer mensagem do protocolo
  deckctl.py ota firmware/build/deck3.bin   atualiza o firmware por Wi-Fi
"""
import argparse
import base64
import json
import sys
import tomllib
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent


def ctl(req: dict, port: int) -> dict:
    r = urllib.request.Request(f"http://127.0.0.1:{port}/ctl", data=json.dumps(req).encode(), method="POST",
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=5) as resp:
        return json.load(resp)


def main() -> int:
    ap = argparse.ArgumentParser(description="Controlo do deckd")
    ap.add_argument("cmd", choices=["status", "last", "notify", "toast", "beep", "send", "ota", "put", "ls", "rm", "tap", "btn", "swipe", "back", "home"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--from", dest="frm", default="uConsole")
    ap.add_argument("--kind")
    ap.add_argument("--vox")
    ap.add_argument("--level", default="info")
    a = ap.parse_args()
    try:
        port = tomllib.load(open(HERE / "config.toml", "rb")).get("deckd", {}).get("port", 8765)
    except OSError:
        port = 8765
    if a.cmd == "status":
        req = {"cmd": "status"}
    elif a.cmd == "last":          # deckctl.py last [tipo]: últimas mensagens do aparelho
        req = {"cmd": "last", **({"t": a.args[0]} if a.args else {})}
    elif a.cmd == "put":           # deckctl.py put ficheiro.json vox/nome.json  (para o cartão SD do Deck³)
        if len(a.args) < 2 or not Path(a.args[0]).is_file():
            print("uso: deckctl.py put <ficheiro local> <caminho no cartão, em deck3/>", file=sys.stderr)
            return 2
        data = Path(a.args[0]).read_bytes()
        chunks = [data[i:i + 3072] for i in range(0, len(data), 3072)] or [b""]
        for i, ch in enumerate(chunks):
            msg = {"t": "file", "path": a.args[1], "data": base64.b64encode(ch).decode(), "append": i > 0}
            res = ctl({"cmd": "send", "msg": msg}, port)
            if not res.get("ok"):
                print(json.dumps(res, ensure_ascii=False), file=sys.stderr)
                return 1
        print(f"{len(data)} bytes enviados em {len(chunks)} pedaços para deck3/{a.args[1]} "
              "(confirme com: deckctl.py ls " + a.args[1].rsplit("/", 1)[0] + ")")
        return 0
    elif a.cmd in ("tap", "btn", "swipe", "back", "home"):   # simula o aparelho: deckctl.py tap gps · swipe left
        st = ctl({"cmd": "status"}, port)
        scr = (st.get("devices") or [{}])[0].get("screen", "home")
        if a.cmd == "home":
            msg = {"t": "key", "k": "boot", "ev": "long"}
        elif a.cmd == "back":
            msg = {"t": "back", "id": scr}
        else:
            key = {"tap": "item", "btn": "btn", "swipe": "dir"}[a.cmd]
            msg = {"t": a.cmd, "id": scr, key: a.args[0] if a.args else ""}
        req = {"cmd": "inject", "msg": msg}
    elif a.cmd in ("ls", "rm"):    # deckctl.py ls [pasta] · deckctl.py rm <caminho>; a resposta vem em: deckctl.py last ls
        req = {"cmd": "send", "msg": {"t": a.cmd, "path": a.args[0] if a.args else ""}}
    elif a.cmd == "ota":
        if not a.args or not Path(a.args[0]).exists():
            print("indique o .bin do firmware", file=sys.stderr)
            return 2
        req = {"cmd": "ota", "path": str(Path(a.args[0]).resolve())}
    else:
        if a.cmd == "notify":
            msg = {"t": "notify", "from": a.frm, "text": " ".join(a.args)}
            if a.kind:
                msg["kind"] = a.kind
            if a.vox:
                msg["vox"] = a.vox
        elif a.cmd == "toast":
            msg = {"t": "toast", "text": " ".join(a.args), "level": a.level}
        elif a.cmd == "beep":
            msg = {"t": "beep", "f": int(a.args[0]) if a.args else 880, "ms": int(a.args[1]) if len(a.args) > 1 else 150}
        else:
            msg = json.loads(a.args[0])
        req = {"cmd": "send", "msg": msg}
    try:
        res = ctl(req, port)
    except OSError as e:
        print(f"deckd não responde na porta {port}: {e}", file=sys.stderr)
        return 1
    print(json.dumps(res, ensure_ascii=False, indent=2))
    return 0 if res.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
