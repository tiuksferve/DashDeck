#!/usr/bin/python3
"""fakedeck — um DashDeck³ falso no terminal, para testar o deckd e os providers sem a placa.

Liga-se ao deckd como o aparelho (WebSocket + token), envia o hello e mostra cada tela em texto.
Comandos (interativo, ou em --run "tap deck; btn beep; back"):
  tap <item>   btn <botão>   back   left | right (swipe)   key <boot|user|pwr> <short|long|double>
  wait <s>     quit
"""
import argparse
import asyncio
import json
import sys
from pathlib import Path

import ws

TOKEN = Path.home() / ".config" / "dashd" / "deck3-token"


def show(msg: dict) -> None:
    t = msg["t"]
    if t == "screen":
        pg = f"  [{msg.get('page', 0) + 1}/{msg['pages']}]" if msg.get("pages", 1) > 1 else ""
        print(f"\n┏━ {msg.get('title') or msg['id']} ({msg['id']}, {msg.get('tpl')}){pg}")
    elif t == "update":
        print(f"┃ (update {', '.join(k for k in msg if k not in ('t', 'id'))})")
    else:
        if t not in ("ping",):
            print(f"  ← {json.dumps(msg, ensure_ascii=False)}")
        return
    for it in msg.get("items", []):
        extra = " ".join(f"{k}={it[k]}" for k in ("sub", "img", "badge", "right") if k in it)
        print(f"┃ [{it.get('id')}] {it.get('label')}  {extra}")
    for r in msg.get("rows", []):
        print(f"┃ {r[0]:<14} {r[1]}" + (f"  ({r[2]})" if len(r) > 2 else ""))
    if "text" in msg:
        print("┃ " + msg["text"].replace("\n", "\n┃ "))
    if msg.get("buttons"):
        print("┃ botões: " + "  ".join(f"<{b['id']}:{b['label']}>" for b in msg["buttons"]))


async def run(host: str, port: int, script: str | None) -> None:
    token = TOKEN.read_text().strip()
    sock = await ws.connect(host, port, f"/deck?id=fakedeck0001&token={token}&fw=fake")
    hello = {"t": "hello", "dev": "deck3", "fw": "fake", "id": "fakedeck0001", "w": 480, "h": 480, "via": "wifi",
             "heap": 200000, "psram": 7000000, "bat": 87, "chg": False, "rssi": -55, "ip": "127.0.0.1",
             "caps": ["notify", "audio", "mic", "imu", "keys", "ota", "confirm"]}
    await sock.send(json.dumps(hello))
    screen = {"id": ""}

    async def reader():
        while True:
            msg = json.loads(await sock.recv())
            if msg["t"] == "screen":
                screen.update(msg)
            show(msg)

    rtask = asyncio.create_task(reader())

    async def do(line: str) -> bool:
        p = line.split()
        if not p:
            return True
        c, args = p[0], p[1:]
        send = None
        if c == "quit":
            return False
        if c == "wait":
            await asyncio.sleep(float(args[0]) if args else 1)
        elif c == "tap":
            send = {"t": "tap", "id": screen["id"], "item": args[0]}
        elif c == "btn":
            send = {"t": "btn", "id": screen["id"], "btn": args[0]}
        elif c == "back":
            send = {"t": "back", "id": screen["id"]}
        elif c in ("left", "right"):
            send = {"t": "swipe", "id": screen["id"], "dir": c}
        elif c == "key":
            send = {"t": "key", "k": args[0], "ev": args[1] if len(args) > 1 else "short"}
        else:
            print(f"? {c}")
        if send:
            await sock.send(json.dumps(send))
            await asyncio.sleep(0.3)
        return True

    try:
        if script:
            await asyncio.sleep(0.5)
            for line in script.split(";"):
                if not await do(line.strip()):
                    break
            await asyncio.sleep(0.5)
        else:
            loop = asyncio.get_running_loop()
            while True:
                line = await loop.run_in_executor(None, sys.stdin.readline)
                if not line or not await do(line.strip()):
                    break
    finally:
        rtask.cancel()
        await sock.close()


def main() -> None:
    ap = argparse.ArgumentParser(description="DashDeck³ falso")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--run", help='comandos separados por ";"')
    a = ap.parse_args()
    asyncio.run(run(a.host, a.port, a.run))


if __name__ == "__main__":
    main()
