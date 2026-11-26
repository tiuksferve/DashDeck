"""Pastas de estado (~/.config/<nome>) e de cache (~/.cache/<nome>) do serviço.

O nome é "dashd"; a variável DASHDECK_STATE muda-o. Durante a união com o Deck³ (docs/deck3/unificacao.md)
o deckd corre com "deck3", para manter o estado do cubo (token, brilho, relógio) e não mexer no do dashd
que está a correr. A fase 5 junta as duas pastas.
"""
from __future__ import annotations

import os
from pathlib import Path

NAME = os.environ.get("DASHDECK_STATE", "dashd")
CONFIG = Path.home() / ".config" / NAME
CACHE = Path.home() / ".cache" / NAME
