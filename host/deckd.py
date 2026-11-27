#!/usr/bin/env python3
"""deckd — o antigo serviço do Deck³. Agora o dashd serve o DashDeck e o Deck³ ao mesmo tempo (deck.py).

Este atalho corre o dashd só com o Deck³ (sem a USB), para testes noutra porta, ex.:
    /usr/bin/python3 host/deckd.py --deck-port 18765    e    host/fakedeck.py --port 18765 --run "tap config"
"""
import sys

import dashd

if __name__ == "__main__":
    dashd.main(["--no-serial", *sys.argv[1:]])
