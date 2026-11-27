# Unificação DashDeck + Deck³

Documento de passagem para a sessão do DashDeck, que vai conduzir a união. Escrito na sessão do
Deck³ a 26/11/2026. Lê-se de uma vez antes de mexer em código.

## Decisões já tomadas pelo dono

1. **Um só projeto, um só serviço no uConsole** a servir os dois ecrãs: o DashDeck pela USB e o
   Deck³ por Wi-Fi (no futuro também pela internet). Não é uma biblioteca partilhada que um projeto
   instala do outro.
2. **A base é o repositório do DashDeck** (`github.com/tiuksferve/DashDeck`), que mantém o histórico e o URL.
3. **Golden version:** a tag `v1.0` no estado atual do `main` e o ramo `1.x` para correções.
   A união faz-se no ramo `unificado` e só entra no `main` quando o DashDeck funcionar igual
   através do serviço unido.
4. **O DashDeck não pode ser afetado.** O `dashd` atual continua a correr até ser substituído
   sem perdas. O firmware do C6 não precisa de mudar.
5. **Quem conduz é a sessão do DashDeck.** (Atualizado a 26/11: o firmware do cubo também passa a
   ser trabalhado aqui, em `firmware/deck3-s3/`; o `~/PycharmProjects/Deck3` deixa de receber código.)

Ainda por fazer (pedir confirmação ao dono antes de cada passo):
- criar a tag e os ramos, só localmente (o push é sempre do dono);
- decidir o nome final do projeto unido. Pode manter-se "DashDeck"; o GitHub redireciona se mais
  tarde for renomeado.

## Estado de partida

| | DashDeck | Deck³ |
|---|---|---|
| Repositório | `~/PycharmProjects/DashDeck`, 23 commits, `main` limpo e igual ao remoto, sem tags | `~/PycharmProjects/Deck3`, **sem commits** (tudo por versionar) |
| Ecrã | ESP32-C6, 320×172, LVGL 9.2, USB | ESP32-S3, AMOLED 480×480, LVGL 9.6, Wi-Fi |
| Firmware | 0.25.21 | 0.6.0 (`firmware/main/deck.h`) |
| Serviço | `dashd` (`host/dashd.py`), `dashd-sonde` | `deckd` (`host/deckd.py`), `deck3-sonde` |
| Estado | `~/.config/dashd`, `~/.cache/dashd` | `~/.config/deck3`, `~/.cache/deck3`, `~/.local/state/deck3` |

**O Deck³ é hoje um fork do host do DashDeck.** Os ficheiros `engine.py`, `providers.py` e os
módulos de apoio foram copiados do DashDeck a 26/11, e as diferenças são poucas e estão listadas
abaixo. Unir é, no essencial, voltar a ter um só motor e juntar-lhe o transporte WebSocket e as
partes próprias do cubo.

## O host do Deck³, ficheiro a ficheiro (`Deck3/host/`)

| Ficheiro | Origem | O que fazer na união |
|---|---|---|
| `engine.py` | `dashd.py` sem a classe `Link` (serial) nem `main()`; tem o gancho `make_link()` e lê `cfg["engine"]` | **Não copiar.** Aplicar ao `dashd.py` só o gancho `make_link()` (ver diferenças) |
| `providers.py` | cópia do DashDeck | **Não copiar.** Levar só as duas opções novas (ver diferenças) |
| `camclient`, `clockscene`, `emojis`, `icons`, `maptiles`, `meshbridge`, `routes`, `stocks`, `wltop`, `xdgapps`, `sonderx` | cópias; só caminhos e nomes mudados | Não copiar |
| `deckd.py` | do Deck³ | **Levar.** Tem `WsLink`, `Dev`, `DeckApp` (subclasse de `App`) e `Server` (WebSocket, `/fw/`, `/ctl`) |
| `ws.py` | do Deck³, WebSocket em Python puro (sem dependências) | Levar |
| `deckproviders.py` | do Deck³: box **Config** (Ecrã e som, Bateria, Wi-Fi, Bluetooth, Aparelho, Sensores) | Levar. Os providers usam `self.session` (o aparelho) |
| `deckctl.py` | do Deck³: controlo local por `POST /ctl` (`status`, `last`, `notify`, `send`, `ota`, `put`, `ls`, `rm`, `tap`, `swipe`, `back`, `home`) | Levar. Útil para testar telas sem tocar no ecrã, também no DashDeck |
| `fakedeck.py` | do Deck³: aparelho falso por WebSocket, para testes | Levar |
| `config.toml` | `[deckd]`, `[keys]`, `[about]` + o menu do DashDeck copiado | Juntar as secções novas ao config do DashDeck |
| `deck3-sonde.service`, `deckd.service` | do Deck³ | Passam a desnecessárias (um serviço, uma sonda) |

Ficheiros de `Deck3/setup/`: `install.sh` (dependências `python3-serial`, `python3-pil`, `python3-gi`;
regra udev com `DECK3_SERIAL`), `provision.py` (grava Wi-Fi e token no cubo pela USB), `deck3.service.xml`
(anúncio mDNS `_deck3._tcp`, porta 8765), `99-deck3.rules`, `idf-env.sh` (IDF para Xtensa/S3),
`sd_exemplos.py`.

## Diferenças do fork que devem voltar ao motor único

Todas foram feitas para não interferir com o DashDeck. No motor único passam a opções ou a
comportamento por aparelho.

1. **`make_link()`** (`engine.py`): `App.__init__` chama `self.make_link()` em vez de criar `Link(...)`.
   No motor único isto torna-se uma lista de transportes (ver arquitetura).
2. **`cfg["engine"]` em vez de `cfg["dashd"]`**: só nome. No motor único fica `[dashd]` ou outro
   nome, à escolha.
3. **GPS, `tty = ""`** (`providers.py`, `NmeaReader`): com o tty vazio, o leitor usa só o gpsmux e
   nunca abre a porta série.
4. **Meshtastic, `mark_read`** (opção do nó, por omissão `true`): com `false`, abrir uma conversa
   não a marca como lida no MeshDeckWeb.
5. **Sistema** (`SystemProvider`): o Deck³ tirou a página 2 (rotação do DashDeck), porque o estado
   do cubo está em Config › Aparelho. No motor único, a página da rotação só aparece no aparelho que
   tem rotação, ou seja, o C6.
6. **Caminhos e textos:** `dashd`/`DashDeck` passaram a `deck3`/`Deck³` (logs, `~/.config`, capturas
   `deck3-*.png`, unidade `deck3-sonde`). No motor único volta a haver um só conjunto de caminhos.

## O que o `DeckApp` acrescenta ao motor (`Deck3/host/deckd.py`)

Isto é o que hoje é "do cubo" e tem de passar a ser **por aparelho**:

- **Transporte `WsLink`:** `send(msg, raw)`, `acks`, `jpg_acks`, `dev_id`, `local_ip`. Tem a mesma
  interface que o `Link` serial e aplica `lcd_safe` através de `link.safe`.
- **Tamanho:** `canvas_area()` devolve `(444, 372)`, contra o `esp_w`/`esp_h` do C6.
- **Hora:** `send_time()` envia `epoch` + `tz` (regra POSIX de `/etc/localtime`); o ESP trata
  sozinho da mudança de hora.
- **Ainda não implementado no cubo** (desligado): `send_icons`, `send_emojis`, `send_bg`, `send_dock`,
  `send_header` estão vazios no `DeckApp`. É a "fase 2" do Deck³: fundos dos mapas, JPEG da câmara,
  ícones e emojis, dock. No motor único decide-se pelas `caps` do `hello`.
- **Botões físicos:** a secção `[keys]` (`boot_short = back`, `boot_long = home`,
  `user_short = open:config`, `pwr_short = bl`; também `shell:` e `launch:`).
- **Mensagens próprias** (`DECK_MSGS`): `key`, `imu`, `mic`, `bat`, `bt`, `wifi_scan`, `bat_low`,
  `offline_log`, `cfg`, `sd`, `ls`, `file`, `rm`, `slider`, `ota`, `pong`.
- **No `hello`:**
  - adota o brilho que o aparelho guardou (`bl`);
  - envia `idle` (`dim`/`off`), `about` (linhas do `[about]`) e `bat?`.
- **A cada minuto (`tick60`):** pede `bat?`.
- **Registos:**
  - `bateria-<id>.csv`;
  - `eventos-<id>.jsonl`: eventos de fora de casa, entregues em lotes com `last`;
  - em ambos os casos, `notify-send` com bateria fraca.
- **`control()`:** `status`, `last`, `inject` (simula toques), `send`, `ota` (serve o `.bin` em `/fw/`).
- **Compatibilidade com os providers do Deck³:**
  - `bl`, `any_session()`, `provider()` (põe `session`);
  - `handle_result()` aceita um texto como aviso;
  - `current_provider()`.

## Arquitetura proposta

```
host/
  dashd.py            motor: menus, telas, detalhes, serviços, avisos (um só)
  providers.py ...    providers partilhados
  deckproviders.py    box Config (só aparece em aparelhos que a anunciem)
  transports/
    serial_link.py    Link atual (DashDeck, USB, acks bgack/iconack/emojiack/jpgack)
    ws_link.py        WsLink + Server (Deck³, Wi-Fi; mais tarde internet com TLS ou VPN)
firmware/
  (atual do C6)       ou firmware/dashdeck-c6/
  deck3-s3/           Deck3/firmware/ inteiro (BSP local incluído)
```

### O ponto difícil: um motor, vários ecrãs

O `App` guarda hoje **um** estado de navegação: `stack`, tela aberta, página, detalhe, `esp_ready`,
`backlight`, `rotation`, `esp_w`/`esp_h`, últimos envios. Para servir os dois ao mesmo tempo:

- **Uma sessão por aparelho** com esse estado e o seu transporte. O motor recebe eventos
  `(sessão, tipo, dados)` e responde para a sessão de onde vieram.
- **Os providers continuam partilhados.** É isso que resolve o SDR: um só readsb/AIS/GPS e as duas
  telas a mostrá-lo ao mesmo tempo. O `enter`/`leave` dos providers que ligam serviços (ADS-B e AIS
  ligam o SDR ao entrar e desligam-no ao sair) passa a contar sessões: só desliga quando a última
  sessão sai.
- **Adaptação por aparelho**, a partir das `caps` e do tamanho do `hello`:
  - `canvas_area`;
  - o que enviar: ícones, emojis, fundos, dock;
  - fontes (`lcd_safe`/`_EXTRA`: as fontes do S3 têm o ³ e cobrem Latin-1);
  - `[keys]`;
  - páginas que só fazem sentido num aparelho (rotação no C6, Config no cubo).
- **Avisos globais** (squawk de emergência, mensagens Meshtastic, alarme do relógio): vão para
  todas as sessões ligadas.
- **Ordem sugerida:** primeiro refatorar o `App` para "uma sessão" explícita sem mudar
  comportamento (o DashDeck tem de continuar igual); depois permitir várias.

### `hello` do Deck³ (o que anuncia)

`dev: "deck3"`, `fw`, `w`/`h` (480×480), `bl`, `vol`, `sd`, e `caps` com: `vox`, `notify`, `audio`,
`mic`, `imu`, `keys`, `ota`, `confirm`, `slider`, `chart`, `bat`, `wifi_scan`, `about`, `sd`, `image`,
`scene`, `away`. O C6 não envia `caps`: a falta de `caps` significa "DashDeck clássico".

### Templates que o cubo tem e o C6 não

Estão todos em `Deck3/docs/protocolo.md`:
- `kv` com sliders e gráficos (`tpl: panel`);
- `log` (`update` com `append`);
- `image`;
- `scene` (`tpl: vox`);
- `img`/`bg_img` nas boxes do menu;
- `notify` com modelo de voxels.

`plot` é uma adaptação do `plot.c` do DashDeck e aceita os mesmos dados.

## Plano por fases (cada uma verificável)

| Fase | O que se faz | Feito quando |
|---|---|---|
| 0 | Tag `v1.0`, ramo `1.x`, ramo `unificado` (num `git worktree`, para não mexer na pasta em uso). Fazer o primeiro commit do Deck³ no repositório dele, para haver um ponto de partida | O dono confirma e faz o push |
| 1 | Trazer `Deck3/firmware`, `docs` e `setup` para o ramo `unificado` (só pastas, sem tocar no código do DashDeck) | O firmware do S3 compila a partir do novo sítio (`flash.sh build`) |
| 2 | Refatorar o `App` para uma sessão explícita e o `Link` para um transporte trocável | O `dashd` do ramo, numa cópia do config, funciona no C6 exatamente como o `v1.0` |
| 3 | Juntar o transporte WebSocket, o `DeckApp` e o `deckproviders` | O cubo funciona com o serviço unido (com o `deckd` parado). O `deckctl.py` e o `fakedeck.py` passam |
| 4 | Várias sessões ao mesmo tempo; os providers com serviços contam sessões | C6 e cubo ligados ao mesmo tempo, ADS-B nos dois, sem conflitos no SDR |
| 5 | Instalador único (pergunta que aparelhos há), migração do estado (`~/.config/deck3` → novo sítio), um só serviço de sondas, docs | Instalação limpa num uConsole só com o cubo, e só com o C6 |
| 6 | Desligar `dashd` e `deckd` antigos, juntar `unificado` ao `main` | O dono confirma |

Depois da união continua a fase 2 do Deck³ (fundos, câmara, ícones, dock) e a fase 3 (relógio
em pixel art com o `clockscene.py`), já no motor único.

## Cuidados

- **USB:**
  - o S3 e o C6 têm o mesmo VID:PID (303a:1001), por isso as regras udev têm de distinguir os dois
    pelo `ATTRS{serial}` (o serial não vai para o git);
  - o `provision.py` reconhece o cubo pelo `hello` com `"dev":"deck3"`;
  - abrir a porta USB do S3 reinicia o chip.
- **Firmware do S3:** o ambiente é `Deck3/setup/idf-env.sh` (Xtensa) e o build vai para
  `/dev/shm/deck3-build`. Não usar o `~/esp/idf-env.sh` do DashDeck, que é RISC-V/C6. O BSP tem
  correções locais (`firmware/components/.../DECK3.md`), entre elas o `bsp_display_lock` invertido.
- **Memória do S3:** a RAM interna é muito justa. Medir com `deckctl.py send '{"t":"stats"}'` e
  depois `deckctl.py last stats`. Valor de referência: heap ~47 KB, maior bloco ~27 KB.
- **AXP2101:** não mexer nas tensões. A corrente de carga é escolha do dono (botão em Config › Bateria).
- **Bluetooth:** está fora do firmware (esgotava a RAM). O OTA por Wi-Fi existe mas fica para rever.
- **Regras gerais:**
  - não pôr dados pessoais nos ficheiros (indicativo, utilizador, número de série); a exceção são os
    créditos do "Sobre", por decisão do dono;
  - commits em inglês, e o push é do dono;
  - documentação em português europeu.
- **Sessões:** desde 26/11 já não há sessão do Deck³ nem código novo em `~/PycharmProjects/Deck3`;
  tudo, incluindo o firmware do cubo, faz-se neste repositório (ver "Uma só pasta" no fim).

## Progresso

A partir da fase 1 este documento vive no ramo `unificado`, em `docs/deck3/unificacao.md`.

| Fase | Estado |
|---|---|
| 0 | Feita a 26/11: tag `v1.0` e ramo `1.x` em `e34eaf3`; ramo `unificado` no worktree `~/PycharmProjects/DashDeck-unificado` |
| 1 | Feita a 26/11: o commit `5e81354` do Deck³ entrou com o histórico (merge) e as pastas mudaram de sítio: `firmware/` → `firmware/deck3-s3/`, `docs/` → `docs/deck3/` (com o `README.md`), `setup/` → `setup/deck3/`. O `host/` do Deck³ fica para a fase 3. O S3 compila com `firmware/deck3-s3/flash.sh build` |

| 2 | Feita a 26/11: `Link` → `SerialLink` em `host/transports/serial_link.py` (o texto adaptado às fontes entra por `prepare`); `App.make_link()`; estado do ecrã numa `Session` (`host/session.py`), exposto pelo `App` com os nomes antigos. Verificado com um transporte falso: a mesma navegação no `v1.0` e no ramo envia as mesmas mensagens (só mudam valores ao vivo e a versão do host). No C6 real, o motor do ramo ligou-se, fez o `hello` e mostrou o início, o Sistema e o relógio (com o fundo transferido pelo `SerialLink`, com as confirmações) |
| 3 | Feita a 26/11: `host/deckd.py` (WebSocket, `DeckApp` sobre o `dashd.App`, servidor `/deck`, `/fw/`, `/ctl`), `ws.py`, `deckproviders.py` (box Config), `deckctl.py` (com `--port`), `fakedeck.py`, `deck3.toml` (o config do cubo) e os serviços `deckd.service`/`deck3-sonde.service` (já com os caminhos de `~/PycharmProjects/DashDeck`). Estado e caches do cubo continuam em `~/.config/deck3` e `~/.cache/deck3` (`host/paths.py`, variável `DASHDECK_STATE`). Nos providers: GPS com `tty = ""`, Meshtastic `mark_read` e a página da rotação só em ecrãs que rodam (`App.has_rotation`). Verificado: a mesma sequência no `fakedeck` dá as mesmas telas no `deckd` antigo e no do ramo; com o cubo real (o `deckd` antigo parado), ligou-se em 4 s, abriu o Sistema e a Config, e o `deckctl` (`status`, `tap`, `home`, `send`/`last stats`: heap 54 KB, maior bloco 28 KB) funcionou. Os serviços instalados ainda apontam para a pasta antiga: a troca faz-se na fase 6 |
| 4 | Feita a 27/11: um só processo serve os dois ecrãs. `dashd.App = DeckMixin + Engine`; `host/deck.py` tem o `WsLink`, o `Server` e o `DeckMixin` (só muda o comportamento quando a sessão atual é a do cubo). Cada transporte marca os eventos com a sua sessão (`SessionEvents`); os eventos dos providers valem para todos os ecrãs; avisos (Meshtastic, squawk, alarme), cabeçalho e toasts globais vão para todos. Um provider só sai (`on_leave`, ex.: liberta o SDR) quando não está na pilha de nenhuma sessão. Config único: `[deckd]`, `[keys]`, `[about]` e a box Config (`devices = ["deck3"]`) no `config.toml`; o `deck3.toml` saiu. `dashd.py --no-serial / --no-deck / --deck-port`; o `deckd.py` passou a atalho (`--no-serial`, estado em `~/.config/deck3`). Verificado: tráfego do DashDeck igual ao `v1.0`; `fakedeck` com as mesmas telas e o mesmo menu do cubo; C6 real + `fakedeck` ao mesmo tempo no ADS-B: o C6 sai e o readsb continua, o cubo sai e o readsb desliga; um toast global chega aos dois. Com os dois aparelhos reais (C6 pela USB e cubo por Wi-Fi, ligados ao mesmo processo em 4 s), o mesmo resultado no ADS-B, e o aviso global chegou aos dois |
| 5 | Feita a 27/11: um só conjunto de pastas (`host/paths.py`): o token do cubo passa a `~/.config/dashd/deck3-token`, o escurecer/apagar a `~/.config/dashd/deck3.json`, a bateria e os eventos a `~/.local/state/dashd`; `paths.migrate_deck3()` copia (sem apagar) do `~/.config/deck3` e `~/.local/state/deck3` o que falta, no arranque e no instalador. Uma só sonda (`dashd-sonde`); saíram `deckd.service` e `deck3-sonde.service`. Instalador único `setup/install.sh` (`--dashdeck`, `--deck3` ou pergunta; números de série por `DASHDECK_SERIAL`/`DECK3_SERIAL`; os aparelhos ficam num drop-in do `dashd.service` como `DASHD_ARGS`; desliga o `deckd` antigo). Verificado: instalador numa sandbox (só C6, só cubo, os dois) com as ações certas; migração numa pasta temporária (copia uma vez, depois nada); os dois aparelhos reais ligados ao serviço do ramo, o cubo com o token migrado. A instalação limpa num uConsole novo fica para quando houver um |
| 6 | Feita a 27/11: o `unificado` entrou no `main` (fast-forward) e `setup/install.sh --dashdeck --deck3` desligou e removeu o `deckd` antigo; o `dashd` da pasta `~/PycharmProjects/DashDeck` serve os dois ecrãs (C6 pela USB, cubo por Wi-Fi). Volta atrás, se for preciso: `main` na tag `v1.0` e o `deckd` de `~/PycharmProjects/Deck3` |

A união está terminada. Daqui em diante, o trabalho no Deck³ continua no `main` deste repositório:
fundos dos mapas, JPEG da câmara, ícones, emojis e dock no cubo (o que era a "fase 2" do Deck³), e o
relógio em pixel art com o `clockscene.py`.

### Uma só pasta (decisão de 26/11)

O dono decidiu, a 26/11, que tudo passa a ser gerido neste repositório: o `~/PycharmProjects/Deck3`
deixa de ser usado para desenvolver e não há sessão própria do Deck³. O firmware do cubo trabalha-se
diretamente em `firmware/deck3-s3/` (notas em `firmware/deck3-s3/CLAUDE.md`), com
`firmware/deck3-s3/flash.sh` e o build em `/dev/shm/deck3-s3-build`.

A pasta antiga só se mantém enquanto o `deckd` que está a correr vier de lá (`Deck3/host/deckd.py`),
ou seja, até a fase 3 o substituir. O código do host do Deck³ que falta trazer está no histórico
(commit `5e81354`), por isso a pasta pode apagar-se depois disso.
