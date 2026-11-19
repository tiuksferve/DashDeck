# Instalação

Este guia monta o DashDeck do zero num uConsole com Pi OS (Bookworm, Wayland/labwc). No uConsole
atual tudo isto já está feito. Fica aqui para reinstalar ou para montar noutra máquina.

## 1. Hardware
- Waveshare **ESP32-C6-Touch-LCD-1.47**, ligada a uma porta USB do uConsole com um cabo USB-C de dados.
- Não usa nenhum GPIO do uConsole. A placa aparece como `303a:1001` (USB-Serial-JTAG nativo do ESP32-C6), normalmente em `/dev/ttyACM0`.
- A regra udev reconhece qualquer ESP32 em USB-Serial-JTAG. Com mais de um ligado, fixe o da tela pelo número de série (`udevadm info -a /dev/ttyACM0 | grep '{serial}'`) em `setup/99-esp-dash.rules`.

## 2. Lado do uConsole (dashd)

O daemon usa o Python do sistema (`/usr/bin/python3`, 3.11) e só bibliotecas dos pacotes Debian:

| Pacote | Para quê |
|---|---|
| `python3-serial` | ligação USB ao ESP |
| `python3-pil` | mapas, ícones emoji, conversão de imagens |
| `python3-gi`, `gir1.2-gtk-3.0` | encontrar os ícones das aplicações no tema GTK |
| `libglib2.0-bin` (`gio`) | abrir as aplicações a partir dos `.desktop` |
| `grim` | botão Captura do dock (opcional) |
| `fonts-noto-color-emoji` | ícones ✈ e 🚢 (opcional) |

Instalação automática (dependências, regra udev, grupo `dialout` e serviço):

```bash
cd ~/PycharmProjects/DashDeck
setup/install.sh
```

Ou à mão:

```bash
# 1. nome fixo /dev/esp-dash (com vários ESP32 ligados, fixe o serial em setup/99-esp-dash.rules)
sudo install -m 644 setup/99-esp-dash.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=tty
ls -l /dev/esp-dash

# 2. serviço do utilizador (arranca com a sessão gráfica)
mkdir -p ~/.config/systemd/user
ln -sf ~/PycharmProjects/DashDeck/host/dashd.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now dashd
```

Notas:
- O caminho `~/PycharmProjects/DashDeck` está fixo em `host/dashd.service` (`%h/PycharmProjects/DashDeck`). Se mudar a pasta, ajuste esse caminho.
- O serviço define `WAYLAND_DISPLAY=wayland-0` para conseguir abrir janelas na sessão.
- **Serviços (ligar e parar)**: o dashd chama `sudo -n systemctl ...`. É preciso `sudo` sem senha para o `systemctl`; neste uConsole o utilizador já tem `NOPASSWD`. Para limitar, crie `/etc/sudoers.d/dashd` com `<utilizador> ALL=(root) NOPASSWD: /usr/bin/systemctl, /usr/bin/fuser, /usr/bin/kill`. O `fuser` mostra quem tem o RTL-SDR aberto e o `kill` fecha esse programa quando o ADS-B ou o AIS precisam do SDR.

## 3. Firmware (ESP-IDF)

Versões usadas: **ESP-IDF v5.5.6**, LVGL **9.2.x**, `esp_lvgl_port` **2.6.x** e `esp_lcd_touch` **1.1.x**.
As versões dos componentes estão fixadas em `firmware/main/idf_component.yml` e `dependencies.lock`.
O `esp_lvgl_port` 2.9 usa outra API de toque, incompatível com o driver AXS5106.

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.6 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git
cd esp-idf
unset VIRTUAL_ENV                       # o install recusa correr dentro de um venv
./install.sh esp32c6
cp ~/PycharmProjects/DashDeck/setup/idf-env.sh ~/esp/idf-env.sh
```

- O cartão do uConsole tem pouco espaço, por isso só se instalou o toolchain RISC-V, sem gdb nem openocd.
- O `export.sh` oficial queixa-se dessas ferramentas em falta; daí o `idf-env.sh` mínimo:
  - ativa o IDF com `. ~/esp/idf-env.sh`;
  - retira os venvs do PATH.

Compilar e gravar:

```bash
cd ~/PycharmProjects/DashDeck/firmware
./flash.sh              # build + flash; para e volta a ligar o dashd
./flash.sh build        # só compilar
./flash.sh monitor      # monitor serial (Ctrl+] sai)
```

- O build fica em **`/dev/shm/dashdeck-build`** (RAM), para não gastar o cartão. Depois de reiniciar o uConsole, o primeiro build volta a ser completo (~3 min).
- A gravação usa `/dev/esp-dash`. Se a placa não responder, segure **BOOT**, carregue em **RESET**, solte o BOOT e grave de novo.
- A primeira gravação usa o `sdkconfig.defaults`. Se mudar opções do Kconfig, apague `firmware/sdkconfig` para que voltem a ser aplicadas.

### Fontes
As fontes com acentos já estão geradas em `firmware/main/fonts/`. Só é preciso refazê-las para mudar os tamanhos ou os caracteres incluídos:

```bash
/usr/bin/python3 firmware/tools/gen_fonts.py
```

## 4. Ponte Meshtastic (MeshDeckWeb)

- O meshtasticd só aceita **um** cliente na API TCP 4403, e esse cliente é o MeshDeckWeb.
- O dashd lê os dados do MeshDeckWeb por uma API local, que vem **desligada por padrão**.
- Para ligá-la, basta uma vez; a opção fica guardada:

```bash
cd ~/PycharmProjects/MeshDeckWeb
./start-desktop.sh stop
nohup ./start.sh --local-api on --connect >> ~/.config/meshdeck/server.log 2>&1 &
curl -s http://127.0.0.1:8080/api/local/dash | head -c 200     # deve mostrar JSON
```

- `--connect` liga o MeshDeckWeb ao último nó usado. Sem ele, liga-se pelo navegador, como sempre.
- Para desligar a API: `./start.sh --local-api off`.
- A API só responde a pedidos da própria máquina e não pede login. Ver [arquitetura.md](arquitetura.md#ponte-meshtastic).

## 5. Verificar

```bash
systemctl --user status dashd -n 20 --no-pager
```

Deve aparecer `ligado a /dev/esp-dash` e `ESP DashDeck fw 0.7.0 (320x172, rot 1, ...)`. Na tela,
a tela de carregamento (com a barra de progresso) dá lugar ao menu, e o ícone USB no canto fica verde.
