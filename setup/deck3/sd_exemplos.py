#!/usr/bin/python3
"""Gera cenas de exemplo em voxels para o cartão SD do Deck³ e envia-as (deckctl put).

    setup/deck3/sd_exemplos.py            gera e envia para deck3/vox/ (o deckd tem de estar ligado ao Deck³)
    setup/deck3/sd_exemplos.py --local D  só grava os .json na pasta D

Formato (o mesmo do {"t":"vox"}): size [nx, ny, nz] até 24, pal (até 15 cores), layers de baixo para cima,
cada linha com nx carateres ('.' vazio, '1'-'9' 'a'-'f' = cor). Usar numa tela: {"tpl": "vox", "model": "ilha"}.
"""
import argparse
import json
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def ilha(n=20, h=12) -> dict:
    """Ilha com praia, relva, rochas, uma palmeira e água à volta."""
    random.seed(7)
    pal = ["#2F7FD8", "#5FB0FF", "#F2D38A", "#4BAE4F", "#2E7D32", "#8D6E63", "#9E9E9E", "#6D4C41", "#66BB6A"]
    # 1 água funda, 2 água, 3 areia, 4 relva, 5 relva escura, 6 tronco, 7 rocha, 8 terra, 9 folhas
    grid = [[[0] * n for _ in range(n)] for _ in range(h)]
    c = (n - 1) / 2
    for y in range(n):
        for x in range(n):
            d = math.hypot(x - c, y - c) + random.uniform(-0.8, 0.8)
            top = max(0, int(6.5 - d * 0.75))
            grid[0][y][x] = 1 if d > 8.5 else 2
            for z in range(1, top + 1):
                grid[z][y][x] = 8 if z < top else (3 if top <= 2 else 4 if (x + y) % 5 else 5)
            if top >= 4 and random.random() < 0.06:
                grid[min(top + 1, h - 1)][y][x] = 7
    px, py = int(c) + 2, int(c) - 1          # palmeira
    base = next(z for z in range(h - 1, -1, -1) if grid[z][py][px]) + 1
    for z in range(base, min(base + 5, h)):
        grid[z][py][px] = 6
    top = min(base + 5, h - 1)
    for dx, dy in [(0, 0), (1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2)]:
        if 0 <= px + dx < n and 0 <= py + dy < n:
            grid[top][py + dy][px + dx] = 9
    layers = [["".join(".123456789"[v] for v in row) for row in layer] for layer in grid]
    return {"size": [n, n, h], "pal": pal, "layers": layers}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--local", help="só gravar nesta pasta")
    a = ap.parse_args()
    cenas = {"ilha": ilha()}
    out = Path(a.local) if a.local else Path(tempfile.mkdtemp())
    out.mkdir(parents=True, exist_ok=True)
    for nome, m in cenas.items():
        f = out / f"{nome}.json"
        f.write_text(json.dumps(m, separators=(",", ":")))
        print(f"{f} ({f.stat().st_size} bytes)")
        if not a.local:
            r = subprocess.run([sys.executable, str(HERE.parent.parent / "host" / "deckctl.py"), "put", str(f), f"vox/{nome}.json"])
            if r.returncode:
                return r.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
