#!/usr/bin/env bash
# Compila e grava o Deck³.
#   firmware/flash.sh            compila e grava pela USB (porta /dev/deck3, ou DECK3_PORT=/dev/ttyACM0)
#   firmware/flash.sh build      só compila
#   firmware/flash.sh ota        compila e envia pelo Wi-Fi (o deckd tem de estar a correr e o Deck³ ligado)
#   firmware/flash.sh <args>     passa os argumentos ao idf.py (ex.: monitor, menuconfig)
set -e
cd "$(dirname "$0")"
# funciona no repositório do Deck³ (firmware/, setup/, host/) e no projeto unido
# (firmware/deck3-s3/, setup/deck3/, host/)
if [ -f ../setup/idf-env.sh ]; then . ../setup/idf-env.sh; else . ../../setup/deck3/idf-env.sh; fi
HOSTDIR=$([ -d ../host ] && echo ../host || echo ../../host)
BUILD=${DECK3_BUILD:-/dev/shm/deck3-build}   # build na RAM (refeito após reboot)
PORT=${DECK3_PORT:-/dev/deck3}
case "${1:-flash}" in
    build) idf.py -B "$BUILD" build ;;
    ota)
        idf.py -B "$BUILD" build
        /usr/bin/python3 "$HOSTDIR/deckctl.py" ota "$BUILD/deck3.bin" ;;
    flash)
        idf.py -B "$BUILD" build
        idf.py -B "$BUILD" -p "$PORT" flash ;;
    *) idf.py -B "$BUILD" -p "$PORT" "$@" ;;
esac
