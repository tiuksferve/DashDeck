"""Pastas do serviço: estado (~/.config/dashd), cache (~/.cache/dashd) e registos (~/.local/state/dashd).

O DashDeck e o Deck³ partilham estas pastas desde a união (docs/deck3/unificacao.md, fase 5); o que era do
Deck³ em ~/.config/deck3 e ~/.local/state/deck3 é copiado para cá no arranque (migrate_deck3), sem apagar
o original. A variável DASHDECK_STATE muda o nome das pastas (testes).
"""
from __future__ import annotations

import logging
import os
import shutil
from pathlib import Path

log = logging.getLogger("dashd")

NAME = os.environ.get("DASHDECK_STATE", "dashd")
CONFIG = Path.home() / ".config" / NAME
CACHE = Path.home() / ".cache" / NAME
STATE = Path.home() / ".local" / "state" / NAME   # registos: bateria e eventos de fora de casa do Deck³

DECK3_TOKEN = CONFIG / "deck3-token"   # token partilhado com o cubo (gravado nele pelo setup/deck3/provision.py)
DECK3_STATE = CONFIG / "deck3.json"    # escurecer/apagar de cada cubo (box Config)

OLD_DECK3_CONFIG = Path.home() / ".config" / "deck3"
OLD_DECK3_STATE = Path.home() / ".local" / "state" / "deck3"


def migrate_deck3() -> list[str]:
    """Copia o estado do Deck³ das pastas antigas para as do serviço, só o que ainda não existe.
    Devolve o que copiou. Pode chamar-se sempre (no arranque e no setup/install.sh)."""
    done = []
    pairs = [(OLD_DECK3_CONFIG / "token", DECK3_TOKEN), (OLD_DECK3_CONFIG / "state.json", DECK3_STATE)]
    if OLD_DECK3_STATE.is_dir():
        pairs += [(f, STATE / f.name) for f in OLD_DECK3_STATE.iterdir() if f.is_file()]
    for src, dst in pairs:
        if not src.is_file() or dst.exists():
            continue
        try:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            if src.name == "token":
                dst.chmod(0o600)
            done.append(f"{src} -> {dst}")
        except OSError as e:
            log.warning("migração do Deck³: %s: %s", src, e)
    for d in done:
        log.info("migração do Deck³: %s", d)
    return done
