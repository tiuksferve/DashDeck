#!/usr/bin/env bash
# Compila e grava o Deck³ (ESP32-S3).
#   firmware/deck3-s3/flash.sh            compila e grava pela USB (porta /dev/deck3, ou DECK3_PORT=/dev/ttyACM0)
#   firmware/deck3-s3/flash.sh build      só compila
#   firmware/deck3-s3/flash.sh ota        compila e envia pelo Wi-Fi (o serviço tem de estar a correr e o Deck³ ligado)
#   firmware/deck3-s3/flash.sh <args>     passa os argumentos ao idf.py (ex.: monitor, menuconfig)
set -e
cd "$(dirname "$0")"
. ../../setup/deck3/idf-env.sh
BUILD=${DECK3_BUILD:-/dev/shm/deck3-s3-build}   # build na RAM (refeito após reboot)
PORT=${DECK3_PORT:-/dev/deck3}
case "${1:-flash}" in
    build) idf.py -B "$BUILD" build ;;
    ota)
        idf.py -B "$BUILD" build
        /usr/bin/python3 ../../host/deckctl.py ota "$BUILD/deck3.bin" ;;
    flash)
        idf.py -B "$BUILD" build
        idf.py -B "$BUILD" -p "$PORT" flash ;;
    *) idf.py -B "$BUILD" -p "$PORT" "$@" ;;
esac
