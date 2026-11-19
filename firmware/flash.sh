#!/usr/bin/env bash
# Compila e grava o DashDeck. Pára o dashd enquanto grava (ele ocupa a porta).
set -e
cd "$(dirname "$0")"
. ~/esp/idf-env.sh   # cópia em setup/idf-env.sh
BUILD=/dev/shm/dashdeck-build          # build na RAM: poupa o cartão (é refeito após reboot)
systemctl --user stop dashd 2>/dev/null || true
trap 'systemctl --user start dashd 2>/dev/null || true' EXIT
idf.py -B "$BUILD" -p /dev/esp-dash "${@:-flash}"
