# Configuração (`host/config.toml`)

Tudo o que aparece na tela vem deste arquivo. Depois de editá-lo:

```bash
systemctl --user restart dashd
```

Não é preciso regravar o ESP. Se o TOML tiver um erro, o serviço não arranca; veja a causa com
`systemctl --user status dashd`.

## `[dashd]`

| Chave | Padrão | Descrição |
|---|---|---|
| `port` | `/dev/esp-dash` | Porta serial do ESP |
| `title` | `DashDeck` | Título do menu principal |
| `refresh` | `2.0` | Segundos entre atualizações da tela aberta (só é enviado o que mudou) |
| `backlight` | `80` | Brilho inicial, de 0 a 100 |
| `layout` | `grid` | Menus em `grid` (boxes) ou `cards` (lista de cartões). Cada submenu aceita o seu `layout` |
| `rotation` | `1` | 0 retrato, 1 horizontal, 2 retrato invertido, 3 horizontal invertido. O ESP grava o valor e reinicia sozinho |
| `screen_off` | `0` | Apaga o ecrã após N segundos sem toques na tela inicial (no config: 180). Um toque ou o BOOT liga-o, sem ativar nada. `0` = nunca |
| `dock_screens` | `["home"]` | Ids das telas que mostram o dock (no config: `["home", "apps"]`: tela inicial e Aplicações) |

## Itens de menu: `[[menu]]` e `[[menu.items]]`

Campos comuns:

| Campo | Descrição |
|---|---|
| `id` | Identificador único (obrigatório) |
| `label` | Texto do box |
| `sub` | Subtítulo fixo. Os providers e serviços põem aqui o seu estado |
| `icon` | Símbolo: `home settings wifi gps play stop pause list bell power refresh eye charge battery usb bluetooth sd folder file mail audio video image warning bars keyboard call upload download loop edit trash ok close plus drive shuffle right volume` |
| `img` | Imagem no lugar do símbolo: `"emoji:✈"`, um nome do tema de ícones (ex.: `"utilities-terminal"`) ou um caminho `.png` ou `.svg` |
| `color` | Cor do símbolo, `"#RRGGBB"` |
| `type` | Um dos tipos abaixo |

| `type` | Campos | O que faz |
|---|---|---|
| `submenu` | `items`, `layout`, `xdg_category`, `xdg_exclude` | Abre outro menu. Com `xdg_category`, junta as aplicações dos `.desktop` dessa categoria |
| `launch` | `cmd`, `app_icon` | Abre um programa no uConsole. O ícone vem de `app_icon` ou do nome do comando |
| `shell` | `cmd`, `done` | Corre um comando em segundo plano e mostra `done` (ou o código de erro) |
| `service` | `unit`, `group` | Tela de um serviço systemd. Com `group`, ligar um serviço para os outros do mesmo grupo |
| `provider` | `provider` e as opções do provider | Tela com dados ao vivo (ver abaixo) |
| `text` | `text` | Tela de texto fixo |
| `home` | — | Volta ao menu principal (útil no dock) |
| `backlight` | `levels` | Alterna o brilho pelos níveis indicados |

### Aplicações de Acessórios

```toml
[[menu]]
id = "apps"
type = "submenu"
xdg_category = "Utility"                    # categoria dos .desktop
xdg_exclude = ["System", "Accessibility"]   # igual ao menu Acessórios do Pi OS
# xdg_after = true                          # pôr os itens fixos antes das apps
```

Os itens fixos (`[[menu.items]]`) cujo comando já esteja num `.desktop` não aparecem repetidos.

## Providers

### `system`
Não tem opções.

### `gps`
| Opção | Padrão | Descrição |
|---|---|---|
| `tty` | `/dev/ttyAMA0` | Porta NMEA, usada quando o meshtasticd não está usando o GPS |
| `baud` | `9600` | Velocidade da porta |

### `adsb`
| Opção | Padrão | Descrição |
|---|---|---|
| `unit` | `readsb` | Serviço que o card do topo liga e desliga |
| `group` | `sdr` | Grupo exclusivo usado ao ligar |
| `keep_running` | `false` | `true` mantém o serviço ligado ao sair da tela. Por omissão, o que o DashDeck ligou é desligado ao sair, para libertar o SDR |
| `json` | (procura) | Caminho do `aircraft.json`. Sem ele, tenta o readsb e o dump1090 em `/run` |
| `map` | `true` | `false` desliga o fundo do mapa |
| `map_style` | `simples` | `simples` (Esri Dark Gray), `satelite` (Esri com nomes) ou `osm` (OpenStreetMap escurecido) |
| `map_url` | — | Outro servidor de tiles `https://.../{z}/{x}/{y}.png` (substitui o estilo) |
| `track_minutes` | `30` | Minutos de trajeto guardados por aeronave |

### `ais`
| Opção | Padrão | Descrição |
|---|---|---|
| `unit` | `ais-catcher` | Serviço que o card do topo liga e desliga |
| `group` | `sdr` | Grupo exclusivo usado ao ligar |
| `keep_running` | `false` | `true` mantém o serviço ligado ao sair da tela. Por omissão, o que o DashDeck ligou é desligado ao sair, para libertar o SDR |
| `url` | `http://127.0.0.1:8119` | Servidor web do AIS-catcher (`/api/ships.json` e `/api/allpath.json`) |
| `map`, `map_style`, `map_url` | | Iguais aos do `adsb` |

### `sdr`
Monitor do RTL-SDR (ver [uso.md](uso.md#sdr)). Sem opções. Usa `sudo -n fuser` para ver quem tem o dongle aberto e, no "Libertar SDR", `sudo -n systemctl stop` ou `sudo -n kill`.

### `aio`
Interruptores da placa AIO v2 pelo `aiov2_ctl` (a aplicação AIO v2 Controller): o estado vem de
`aiov2_ctl --status` e cada toque corre `aiov2_ctl <GPS|LORA|SDR|USB> on|off`.

| Opção | Padrão | Descrição |
|---|---|---|
| `cmd` | `/usr/local/bin/aiov2_ctl` | Caminho do aiov2_ctl. O utilizador precisa de estar no grupo `gpio` (o `pinctrl` não usa sudo); o LoRa usa `sudo -n systemctl` para o meshtasticd |

### `clock`
Relógio em ecrã inteiro (ver [uso.md](uso.md#relógio)). Pode ser um `[[menu]]` ou um `[[dock]]`; no config de origem está no dock, no lugar do Terminal.

| Opção | Padrão | Descrição |
|---|---|---|
| `face` | `paisagem` | Mostrador inicial: `paisagem`, `retro` ou `previsao`. Depois de um swipe, vale o último usado (guardado em `~/.config/dashd/clock.json`, com o brilho) |
| `lcd_color` | `ambar` | Cor do digital retro: `ambar`, `verde` ou `ciano` |
| `brightness` | `50` | Brilho inicial no relógio (10 a 100). O brilho normal volta ao sair |
| `dim_after` | `300` | Segundos sem toques até o relógio escurecer (`0` = nunca). Um toque repõe o brilho |
| `dim_brightness` | `10` | Brilho quando escurecido (%) |
| `weather` | `true` | Previsão do Open-Meteo (sem chave de API). `false` desliga os pedidos à internet |
| `lat`, `lon` | — | Posição fixa da previsão. Sem elas, usa o GPS ou o nó Meshtastic e, sem posição, a última da cache |

As coordenadas são arredondadas a 0,01° (cerca de 1 km) antes de serem enviadas ao Open-Meteo. A
previsão fica em `~/.cache/dashd/weather.json`.

### `mesh`
| Opção | Padrão | Descrição |
|---|---|---|
| `channel` | `0` | Canal inicial das respostas rápidas (o destino troca-se na tela) |
| `quick` | `["OK", "A caminho", "Cheguei", "Preciso de ajuda"]` | Frases do Responder, do Escrever e da Mensagem direta, até 12. `{pos}` é trocado pela posição do nó |
| `start_cmd` | — | Comando do botão "Iniciar MeshDeckWeb" quando a ponte não responde |

## Dock: `[[dock]]`

Até 6 botões na parte de baixo do menu principal. Aceitam os mesmos campos e tipos dos itens de menu:

```toml
[[dock]]
id = "shot"
label = "Captura"
icon = "image"
type = "shell"
cmd = "grim \"$HOME/Pictures/captura-$(date +%Y%m%d-%H%M%S).png\""
done = "Captura em ~/Pictures"
```

## Exemplos

Um serviço novo no grupo do SDR:

```toml
  [[menu.items]]
  id = "svc_rtltcp"
  label = "rtl_tcp"
  icon = "audio"
  type = "service"
  unit = "rtl_tcp"
  group = "sdr"
```

Um comando com confirmação de sucesso:

```toml
  [[menu.items]]
  id = "wifi_off"
  label = "Wi-Fi off"
  icon = "wifi"
  type = "shell"
  cmd = "nmcli radio wifi off"
  done = "Wi-Fi desligado"
```
