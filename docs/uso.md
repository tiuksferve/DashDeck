# Uso da tela

## Navegação

| Ação | Como |
|---|---|
| Abrir um item | Toque no box ou no cartão |
| Voltar | Botão **←** no topo, **botão BOOT** da placa ou **Início** no dock (volta ao menu principal) |
| Mudar de página | **Swipe** para a esquerda (seguinte) ou para a direita (anterior). Os pontos na barra de cima mostram a página atual. |
| Voltar com swipe | Swipe para a direita na primeira página de uma tela (não funciona nos menus com scroll lateral, onde o gesto rola os boxes) |
| Rolar | Arraste. Os menus em boxes rolam para os lados; listas e detalhes rolam na vertical. |
| Ações perigosas | Os botões vermelhos (ex.: Parar serviço) e os envios Meshtastic pedem confirmação (**Não** / **Sim**) |

**Arranque:** ao ligar (ou reiniciar) o ESP aparece a tela de carregamento, escolhida ao acaso entre **Pixels** (o nome forma-se com pixels coloridos que voam, com ondas de rádio e a frase "Tela pequena, grandes possibilidades!") e **Nascer do sol** (a paisagem do relógio passa da noite ao dia, com a mesma frase). Uma barra em baixo mostra o progresso. A animação dura pelo menos 5 s e fica até o dashd responder; só então aparece o menu. Se o dashd não responder em 10 s, aparece "A AGUARDAR O UCONSOLE" por cima da barra. Reiniciar só o dashd não a mostra.

**Barra de cima:** botão ←, título, pontos de página, hora e bateria do uConsole (`+` quando está
carregando) e o ícone USB. O ícone fica verde com o dashd ligado e vermelho sem ele. Com o dashd
parado, a barra mostra "offline".

**Dock** (na tela inicial e em **Aplicações**; configurável em `dock_screens`):
- **Início:** volta ao menu principal.
- **Relógio:** abre o modo relógio (ver [Relógio](#relógio)). O Terminal continua em Aplicações.
- **Captura:** grava uma captura da tela do uConsole em `~/Pictures/captura-AAAAMMDD-HHMMSS.png`.
- **Brilho:** alterna o brilho da tela do ESP entre 25, 50, 75 e 100%.
- **Som:** volume e brilho do ecrã do uConsole (ver [Som](#som)).
- **Bloquear:** bloqueia o uConsole com o `swaylock`, o mesmo do atalho Ctrl+Alt+L.

**Ecrã apagado:** depois de 3 min sem toques na tela inicial, o ecrã apaga-se. Um toque em qualquer sítio (ou o BOOT) volta a ligá-lo; esse toque não abre nada. Nas outras telas e no relógio, o ecrã nunca se apaga.

**Avisos:** os resultados das ações (por exemplo "readsb ligado" ou "Enviada") aparecem num aviso por cima da tela durante 2,5 s.

## Módulos

### Aplicações
- Mostra os mesmos programas do menu **Acessórios** do uConsole (categoria `Utility`, sem `System`
  e `Accessibility`), com o ícone real e por ordem alfabética.
- A seguir vêm Terminal, Gqrx, Arquivos e PyCharm (instalado em `/opt/pycharm`, sem `.desktop`, por isso está no `config.toml`).
- Tocar abre o programa no uConsole, com `gio launch`, como o menu do sistema. Os programas de terminal, como o Vim, abrem numa janela de terminal. Cada programa corre fora do serviço dashd (num scope próprio do systemd), por isso continua aberto quando o dashd reinicia ou o firmware é gravado.
- Instalar ou remover um programa atualiza a lista em até 30 s.
- **Abertas** (o primeiro box): os programas abertos no uConsole, com o ícone de cada um. O que está em uso fica com o fundo azul e "em uso" a verde; os outros dizem "aberta" ou "minimizada". Com duas janelas do mesmo programa, o box mostra o título da janela. Um toque abre a janela com **Mostrar** (traz para a frente, mesmo minimizada), **Minimizar** e **Fechar** (com confirmação; o programa pode ainda perguntar se quer gravar). Lê as janelas pelo compositor (labwc), por isso vê também as janelas Wayland que o `wmctrl` não vê.

### Meshtastic
Precisa do MeshDeckWeb a correr, ligado ao nó e com `--local-api on` (ver [instalacao.md](instalacao.md#4-ponte-meshtastic-meshdeckweb)).

| Página | Conteúdo |
|---|---|
| **Painel** | Nó local, nós ativos nas últimas 2 h e total, não lidas, último nó ouvido, bateria, uso do canal e posição |
| **Nós** | Os 30 ouvidos mais recentemente: nome curto e longo, há quanto tempo, SNR e saltos (RF) ou MQTT, e distância. O card tem fundo **verde** se o nó chega por RF e **lilás** se chega por MQTT. Os inativos (mais de 2 h) têm o nome em cinza. Tocar abre o detalhe (via, hardware, bateria, tensão, posição). |
| **Conversas** | Um card por canal e por nó com quem há mensagens diretas (estas com fundo azul e "· direta"), com a última mensagem. As que têm mensagens por ler vêm primeiro, em laranja, com "N novas". Tocar abre a **conversa**: um card "Escrever para …" no topo e as mensagens dela, da mais recente (recebidas por RF com fundo verde, por MQTT lilás, as minhas em azul). Abrir uma conversa marca só essa como lida, também no MeshDeckWeb. Tocar numa mensagem abre-a inteira, com a via, os saltos (ou o estado de entrega) e o botão **Responder**. |
| **Mapa** | O mesmo mapa do ADS-B e do AIS com os nós que têm posição: **verde** se o nó é ouvido por RF, **lilás** por MQTT, **cinza** se está inativo há mais de 2 h, com o nome curto. O centro é a posição do nó local; os anéis e as distâncias estão em km. Funcionam os controlos do mapa (zoom, setas, Centrar). No detalhe de um nó com posição, **Seguir no mapa** abre o mapa centrado nele, com a via, há quanto tempo foi ouvido e a distância; os nós que se movem deixam rasto enquanto o dashd corre. |

- Os emojis dos nomes e das mensagens (incluindo bandeiras e tons de pele) aparecem a cores, do tamanho de uma letra.
- **Responder:** no detalhe de uma mensagem, o botão **Responder** abre as respostas rápidas do `config.toml` já com essa conversa como destino (o canal da mensagem ou, numa direta, o outro nó). O card azul do topo (**Para: …**) deixa mudar o destino. Tocar numa frase pede confirmação, envia e volta à mensagem. O `{pos}` é trocado pela posição.
- **Mensagem direta:** no detalhe de um nó, o botão **Mensagem direta** abre as mesmas frases para esse nó (e, se tiver posição, **Seguir no mapa**).
- **Aviso de mensagem direta:** quando chega uma mensagem direta (só as recebidas, não as de canal), aparece por cima de qualquer tela, também no relógio, uma animação: ondas de rádio fecham-se no centro, os pixels juntam-se num envelope verde que pulsa e o nome do remetente escreve-se por baixo. Depois sobe uma faixa com "Mensagem de …" e o texto. Se o ecrã estiver apagado ou o relógio escurecido, acende. Some ao fim de 5,5 s; um toque abre essa conversa e a mensagem, já com o **Responder**.
- **Escrever:** o card "Escrever para …" no topo de cada conversa abre as frases para esse canal ou nó, mesmo sem responder a uma mensagem.
- No menu principal, o box Meshtastic mostra "N ativos · M nós" e um **selo vermelho** com o número de mensagens não lidas.
- A contagem é a mesma do MeshDeckWeb: ler no navegador também apaga o selo.
- Se o MeshDeckWeb estiver parado, a tela avisa e mostra um botão **Iniciar MeshDeckWeb**.

### GPS
- **Fonte**, por esta ordem:
  - **MeshDeckWeb** (`/api/local/gps`), quando ele tem o GPS ao vivo: com o gpsmux do MeshDeckWeb instalado, o GPS é partilhado com o meshtasticd e chega aqui com fix, DOP e céu. Vê-se a cada pedido (cache de 1 s).
  - Se o meshtasticd está ativo e tem `GPS: SerialPath` no `/etc/meshtasticd/config.yaml` (sem gpsmux), o GPS é dele e a posição vem do nó Meshtastic, pela ponte, sem céu.
  - Com o meshtasticd parado, o GPS lê-se normalmente: pelo **gpsmux** do MeshDeckWeb (serviço `meshdeck-gpsmux`, porta TCP 127.0.0.1:10110) quando ele está a correr, porque é ele que tem o tty aberto para o partilhar; sem gpsmux, direto do `/dev/ttyAMA0` (9600 baud). Abrir o tty ao mesmo tempo que o gpsmux fazia os dois disputarem os dados. A fonte aparece como "gpsmux (partilhado)" ou o nome do tty.
- **Dados:** fonte, fix (2D, 3D ou sem fix), satélites em uso e visíveis, latitude, longitude, altitude, velocidade, rumo, HDOP e hora UTC.
- **Página Céu:**
  - Mostra os satélites num círculo com o norte para cima e os anéis de 30° e 60° de elevação.
  - O rótulo indica o sistema: G = GPS, R = GLONASS, E = Galileo, C = BeiDou.
  - A cor é a força do sinal: verde a partir de 30, amarelo de 20 a 29 e vermelho abaixo de 20.
  - Ponto cheio é satélite em uso no fix; contorno vazio é satélite só visível.
  - Os satélites sem sinal não aparecem no gráfico, mas contam nos totais ao lado ("em uso / visíveis").
  - **Só funciona a ler o NMEA** (gpsmux ou tty, com o meshtasticd parado): o Meshtastic não repassa os dados de satélites.

### ADS-B e AIS
Os dois funcionam da mesma forma. O ADS-B usa o serviço `readsb` (aeronaves) e o AIS o serviço
`ais-catcher` (navios). Os dois precisam do RTL-SDR, que só serve um programa de cada vez.

Três páginas (swipe): **Controlo** (ligar e desligar, o monitor de emergências no ADS-B, o alvo
seguido e o resumo: quantos há e quantos têm posição; tocar no resumo abre a lista ou o mapa) ·
**Lista** (só os aviões ou navios) · **Mapa**.

- **Ligar e desligar:** o primeiro card da página Controlo mostra o serviço.
  - **Ligado** (fundo verde): tocar desliga o serviço, com confirmação, e liberta o SDR.
  - **Desligado** (fundo castanho): tocar liga. Se o SDR estiver ocupado (o outro serviço, o
    `rtl_tcp` do SDR Server, o Intercept, o Gqrx…), a confirmação diz por quem; ao aceitar, o
    dashd fecha esse programa e liga o serviço.
  - No mapa, sem dados, aparece o botão **Ligar** ou **Desligar**.
- **SDR livre ao sair:** se foi o DashDeck a ligar o serviço, ao sair da tela ADS-B ou AIS (para o
  início ou outro menu) ele é desligado e aparece "… desligado: SDR livre". Assim o dongle fica livre
  para outros programas (Gqrx, SDR++, tar1090…). Abrir o detalhe ou o mapa não conta como sair. Um
  serviço ligado fora do DashDeck (pela tela Serviços ou por outro programa) não é desligado. Para o
  manter ligado em segundo plano, use `keep_running = true` no `config.toml`.
- **Lista** (página 2; sem dados diz se o serviço está desligado ou à espera):
  - ADS-B: voo ou hex com a rota (ex.: `TAP1352  LIS » LHR`), altitude, velocidade e squawk. Emergências (7500, 7600, 7700) em vermelho.
  - **Monitor de emergências (ADS-B):** o segundo cartão da página Controlo do ADS-B. Ligado, o readsb fica a correr mesmo fora da tela (o SDR fica ocupado pelo ADS-B) e aparece um **avião laranja ao centro do cabeçalho**, em qualquer tela. Quando um avião transmite o squawk 7500 (sequestro), 7600 (falha de rádio) ou 7700 (emergência), aparece um aviso animado em qualquer tela: os pixels formam um avião vermelho que pisca, o indicativo escreve-se por baixo e a faixa diz o código, o significado, a rota, a altitude e a distância. Fica 12 s; um toque abre o detalhe do avião. O avião do cabeçalho fica vermelho durante 5 min. Cada avião avisa uma vez por código a cada 30 min.
    - Sem o monitor, o ADS-B funciona como antes: liga-se na tela e desliga-se ao sair, e não há avisos fora dela.
    - Desligar o monitor deixa o readsb ligado até sair da tela. Desligar o readsb (ou libertar o SDR, ou outro programa ficar com ele) desliga também o monitor, com um aviso.
    - O estado fica gravado: se o dashd reiniciar com o monitor ligado, volta a ligar o readsb (só se o SDR estiver livre).
  - AIS: nome ou MMSI com o destino (ex.: `SANTA MARIA  » PT LIS`), tipo, velocidade, distância, há quanto tempo foi ouvido e país. Navios sem sinal há mais de 15 min saem da lista.
  - Tocar abre o detalhe, com o botão **Seguir no mapa**.
- **Origem e destino:**
  - ADS-B: o avião só transmite o indicativo do voo, por isso o dashd procura a rota no [adsbdb.com](https://www.adsbdb.com) e, se faltar, no [hexdb.io](https://hexdb.io) (só o indicativo sai do uConsole, nunca a posição). O detalhe mostra a **Companhia**, a **Origem**, as **Escalas** e o **Destino** com o código IATA, o nome do aeroporto e o país; o painel do mapa mostra a rota curta. Enquanto procura aparece "a procurar..."; sem resultado, "desconhecida". As bases são feitas por voluntários: uma rota do hexdb registada há mais de 2 anos fica a cinza, porque pode ter mudado. Voos privados e militares (sem indicativo de companhia) não têm rota. As respostas ficam em `~/.cache/dashd/routes.json` (24 h); `routes = false` desliga as consultas.
  - AIS: o navio transmite o **Destino** (texto livre escrito pela tripulação, muitas vezes o código do porto, ex.: `PT LIS`) e a **Chegada** prevista, que aparece na hora local; "(passou)" quando a hora já passou. O porto de saída não faz parte do AIS.
- **Mapa:**
  - O mesmo mapa escuro, com cada alvo, um traço na direção do rumo e o nome.
  - Cor no ADS-B: altitude (verde abaixo de 10 mil pés, azul abaixo de 25 mil, roxo acima, cinza no solo).
  - Cor no AIS: tipo (verde carga, vermelho tanque, azul passageiros, laranja pesca, lilás veleiro e recreio, ciano reboque e serviço, cinza balizas e estações).
  - **Trajeto:** uma linha discreta mostra o caminho percorrido por cada alvo. No ADS-B, o dashd guarda os últimos 30 min de cada aeronave enquanto o readsb estiver ligado. No AIS, o trajeto vem do próprio AIS-catcher.
  - O centro é a posição do receptor; sem ela, a do GPS; sem GPS, a média dos alvos (com aviso). O zoom é automático para caber tudo.
  - **Controlos sobre o mapa:** **−** (canto superior esquerdo) e **+** (canto superior direito) fixam o zoom; passar do limite volta ao automático. As **setas** no meio de cada borda deslocam o mapa um terço da largura ou da altura. Com o mapa deslocado, o zoom deixa de ser automático e aparece o botão **Centrar**, que volta ao centro normal.
  - Os tiles vêm da internet na primeira vez e ficam em `~/.cache/dashd/tiles`. Depois funcionam offline.
- **Seguir um alvo:**
  - No detalhe, **Seguir no mapa** abre o mapa centrado nesse alvo, com o trajeto mais grosso e na cor dele.
  - O mapa só anda quando o alvo se afasta do centro, para não refazer o fundo a cada atualização.
  - O painel ao lado mostra o alvo (rota, altitude e velocidade, ou tipo, destino, velocidade e estado do navio).
  - As setas também funcionam enquanto segue: o mapa fica parado onde o deixou até tocar em **Centrar**.
  - **Soltar** (no mapa, na página Controlo ou no detalhe) volta ao mapa normal.

### Relógio
Abre-se pelo botão **Relógio** do dock. É um ecrã inteiro, sem barra nem dock, com a hora, a data e a previsão do dia em pixel art.
Nas cenas (paisagem, previsão, cidade), a hora, a data e o tempo ficam sobre uma sombra escura translúcida com as bordas esbatidas, e os números grandes têm contorno de 2 px com sombra projetada, para se lerem sobre céu claro, neve, nevoeiro ou nuvens.

| Gesto | Efeito |
|---|---|
| Swipe para os lados | Troca de mostrador: paisagem → digital retro → previsão → cidade → temporizador e alarme (os pontos em baixo mostram qual) |
| Swipe para cima / para baixo | Mais / menos brilho, em passos de 10% (aparece uma barra à direita) |
| Toque em qualquer sítio | Mostra "Toque de novo para sair"; um 2.º toque em 2,5 s volta ao DashDeck, com o brilho normal (no temporizador, os toques nas setas e nos botões não contam) |
| Toque longo no digital retro | Muda a cor do LCD: âmbar → verde → ciano |
| Botão BOOT | Volta logo ao DashDeck |

O mostrador, o brilho, a cor do LCD, o temporizador e o alarme ficam guardados para a próxima vez.

Depois de 5 min sem toques, o relógio escurece para 10%. O toque seguinte repõe o brilho escolhido; se for um toque simples, mostra também o aviso de saída, que desaparece sozinho em 2,5 s.

- **Paisagem:**
  - Montanhas, colinas com pinheiros, uma casa e um lago onde se reflete o céu.
  - O céu segue o nascer e o pôr do sol do dia: madrugada, dia, dourado ao nascer e ao pôr, e noite com estrelas e a lua na fase real. A janela da casa acende-se à noite.
  - A previsão muda a cena: nuvens, chuva, neve (também nas montanhas e no telhado), nevoeiro e trovoada com relâmpagos.
  - **Inverno** (dezembro a fevereiro): a terra, as colinas e o telhado ficam com neve, mesmo sem nevar (`winter_snow`).
  - Em cima ficam a hora, os segundos e a data; à direita, o ícone, a temperatura, a máxima (↑), a mínima (↓) e a probabilidade de chuva.
  - A cena é refeita a cada 5 min.
- **Digital retro:**
  - Dígitos grandes em blocos, como num LCD, com os "8" apagados por trás e os dois pontos a piscar.
  - Uma fila de 60 pontos marca os segundos.
  - Em baixo ficam o ícone, a temperatura, a máxima, a mínima, a chuva e o vento.
  - A cor (âmbar, verde ou ciano) muda com um toque longo; a inicial vem do `config.toml`.
- **Previsão:**
  - Uma costa em pixel art: mar com o reflexo do céu, um cabo ao longe, uma falésia com um farol listado (que acende e lança o feixe à noite) e um veleiro. O céu e o tempo seguem as mesmas regras da paisagem (chuva e neve a cair, nevoeiro, sol e lua).
  - Em cima, a hora, os segundos e a data.
  - Em baixo, numa faixa escura, as próximas 6 horas (a primeira é "AGORA"): hora, ícone do tempo (de dia ou de noite), temperatura e probabilidade de chuva (azul a partir de 30%). Na vertical ficam em 3 × 2.
- **Cidade:**
  - Uma cidade à beira-rio no mesmo estilo: prédios ao longe, prédios perto com janelas (acesas à noite, ao acaso), luzes vermelhas no topo, um cais com candeeiros e o reflexo de tudo na água.
  - O céu, o sol, a lua e o tempo seguem as regras da paisagem; com neve, os telhados ficam brancos.
  - A hora, a data e a previsão aparecem como na paisagem.
- **Temporizador e alarme** (5.º mostrador): duas metades, o temporizador à esquerda (ciano) e o alarme à direita (âmbar); na vertical ficam uma por cima da outra.
  - **Temporizador:** as setas por cima e por baixo dos dígitos mudam os minutos (±1) e os segundos (±10); manter o dedo numa seta repete. O botão ▶ começa (passa a ‖ para pausar, e de novo ▶ para retomar) e o ■ zera. A contar, as setas ficam apagadas e a linha de baixo diz a que horas termina; em pausa, os dígitos piscam.
  - **Alarme:** as setas mudam as horas e os minutos (±1, com repetição); **LIGAR** / **DESLIGAR** ativa o alarme, que toca todos os dias a essa hora. Ligado, a linha de baixo diz quanto falta ("TOCA EM 7H20").
  - **Quando toca** (em qualquer tela, até com o ecrã apagado): o ecrã acende e aparece a animação do alarme: as ondas fecham-se no centro, os pixels formam um despertador dourado que abana, e escreve-se "ALARME 07:30" ou "TEMPO!". A faixa em baixo diz o que tocou. O uConsole toca um bip duplo. Um toque no aviso pára tudo; sem toque, pára ao fim de 60 s.
  - O temporizador e o alarme correm no dashd: continuam com o relógio fechado e depois de reiniciar o dashd.
- **Hora:** o ESP conta a hora sozinho, acertada pelo dashd ao ligar e a cada 10 min. O relógio continua certo mesmo com o dashd parado.
- **Previsão:**
  - Vem do Open-Meteo, a cada 30 min, para a posição do GPS (ou do nó Meshtastic) ou para a do `config.toml`.
  - Fica em cache e funciona sem internet até 12 h.
  - Sem previsão, a paisagem mostra só a hora e o retro mostra "SEM PREVISAO".
- No menu, o box mostra a temperatura e o estado do tempo, o alarme quando está ligado e o temporizador quando está a contar.
- **Memória:** se os ícones das aplicações ocuparem a memória do ESP, o relógio liberta-os e o dashd volta a enviá-los ao sair (o dock reaparece em menos de 0,2 s; os ícones das Aplicações voltam ao abrir esse menu).

### AIO v2
Os interruptores da placa AIO v2, como na aplicação **AIO v2 Controller**: um box para cada recurso,
**GPS**, **LoRa**, **SDR** e **USB**.

- **Ligado:** fundo verde e "Ligado". **Desligado:** fundo normal e "Desligado".
- Cada box mostra uma linha ao vivo: no GPS o fix (por exemplo "3D · 9 sat" ou "pelo Meshtastic"), no LoRa se o meshtasticd está ativo, no SDR quem o usa ("livre" ou "readsb · 1090 MHz"). Depois de medido, o consumo aparece no subtítulo ("Ligado · 0,9 W").
- **Energia** (5.º box): bateria e potência total agora. Tocar abre o consumo de cada recurso e os botões **Medir**. A medição usa o `aiov2_ctl --measure`: mede a bateria com o recurso ligado e desligado (uns 5 s cada, por isso pede confirmação; no LoRa o meshtasticd reinicia). Só funciona a bateria, sem carregador. Os valores ficam guardados em `~/.config/dashd/aio_power.json`.
- Tocar num box desligado liga o recurso. Num ligado, pede confirmação antes de desligar, porque
  pode cortar o que o está a usar (desligar o LoRa pára o meshtasticd, como na aplicação; desligar
  o SDR pára a receção do ADS-B e do AIS). Ligar o **USB** também pede confirmação: com ele ligado, a ligação USB ao DashDeck perde dados.
- Enquanto muda, o box mostra "a mudar..."; o resultado aparece num aviso.
- O estado é lido a cada 5 s, por isso também mostra o que for mudado na aplicação do uConsole.
- No menu, o box AIO v2 mostra os recursos ligados.
- **Perfis** (box antes da Energia): um toque põe vários recursos e serviços como ficam no perfil. Os de origem são **Meshtastic** (GPS e LoRa ligados, SDR desligado, ADS-B e AIS parados), **ADS-B** (GPS e SDR ligados, readsb ligado), **AIS** (GPS e SDR ligados, AIS-catcher ligado) e **Poupança** (tudo desligado). O perfil que corresponde ao estado atual aparece a verde como "Ativo"; os outros dizem quantas mudanças fazem e pedem confirmação com a lista ("ADS-B: liga SDR; liga readsb?"). O USB não entra nos perfis de origem. O box Perfis mostra o perfil ativo, ou "personalizado". Os perfis mudam-se no `config.toml` ([configuracao.md](configuracao.md#aio)).

### SDR
Monitor do RTL-SDR, para saber se está livre para outros programas.

- **Alimentação (AIO):** se o SDR está ligado na placa AIO v2. **Dongle:** se o RTL2832U está ligado (com o fim do número de série).
- **Em uso por:** o programa que tem o dongle aberto, o PID e o serviço systemd (se for um), a **frequência** e há quanto tempo. A frequência é conhecida no readsb (1090 MHz), no AIS-catcher (162 MHz), nos `rtl_*` com `-f`, no rtl_433 e no Gqrx com o *Remote control* ligado (porta 7356). Sem uso aparece "livre para outros programas".
- **Histórico:** as últimas vezes que um programa abriu ou libertou o SDR.
- **Libertar SDR** (com confirmação): pára o serviço que o tem aberto (por exemplo o ais-catcher, que senão se reiniciava sozinho) ou fecha o programa.
- No menu principal, o box SDR mostra "livre" ou quem o está a usar.

### Câmara
Câmara do CamDeck (projeto irmão: XIAO ESP32-S3 Sense ligada ao uConsole por USB),
pelo servidor `camd`. Três páginas (swipe):

- **Visor:** o vídeo ao vivo no ecrã inteiro, com **Foto** (canto inferior esquerdo) e **Gravar**
  (canto inferior direito; fica vermelho, **Parar**, durante a gravação). A foto é tirada na resolução
  das fotos e aparece 1,5 s no visor com uma moldura branca; durante uma gravação, a foto é o quadro
  atual. A gravar, o visor mostra "● REC" e o tempo. Os vídeos ficam em MP4 com som, como no browser.
- **Galeria:** as fotos e os vídeos, um de cada vez no ecrã inteiro, o mais recente primeiro, com a
  posição, a data e (nos vídeos) a duração por cima. As setas dos lados passam ao anterior e ao
  seguinte. Nos vídeos, **Ver** toca-o no ecrã (sem som, à velocidade a que foi gravado) e **Parar**
  pára. **Apagar** abre os dados do ficheiro e pede confirmação. A lista é relida a cada 10 s e
  depois de uma foto ou gravação.
- **Estado:** sensor e firmware, resolução e fps do vídeo, resolução das fotos, quadros por segundo do
  visor e o tempo de descodificação no ESP, temperatura, gravação e disco livre. Botões **Vídeo**
  (VGA, SVGA, XGA, HD) e **Fotos** (HD, UXGA, QXGA) mudam a resolução num toque; **Abrir** abre a
  interface web do CamDeck no uConsole (galeria e todos os ajustes).
- O visor acompanha a câmara: cada quadro só é enviado depois de o ESP confirmar o anterior. A USB
  da câmara limita o vídeo (HD ~4-10 fps conforme a compressão; VGA e SVGA até 28 fps), e o visor chega
  a cerca de 16 quadros por segundo, por isso VGA ou SVGA dão o visor mais fluido.
- Com o camd parado, o visor mostra "CamDeck parado" e o botão **Ligar o CamDeck**. No menu, o box
  mostra "pronta", "a gravar 0:12", "desligada" (câmara fora da USB) ou "parada".

### Rede
Rede do uConsole pelo NetworkManager, em três páginas (swipe):

- **Wi-Fi:** rede ligada, sinal e canal, se há Internet, IP, gateway e a VPN ligada. Botões **Procurar** (atualiza a lista de redes) e **Desligar Wi-Fi** (com confirmação; o uConsole fica sem rede) ou **Ligar Wi-Fi**.
- **Redes:** a rede ligada (a verde), depois as guardadas e as outras, por sinal. As guardadas fora de alcance também aparecem (por exemplo o hotspot do telemóvel). Tocar numa guardada ou numa aberta liga-se a ela; na ligada, desliga. Uma rede nova com palavra-passe tem de ser ligada uma vez no uConsole (o DashDeck não tem teclado); depois fica guardada.
- **VPN:** as ligações VPN e WireGuard do NetworkManager e o Tailscale (se estiver instalado). Um toque liga ou desliga, com confirmação.
- No menu, o box Rede mostra a rede e o sinal, e "· VPN" com uma VPN ligada.

### Som
Aberto pelo **Som** no dock: volume do uConsole (PipeWire) e brilho do ecrã do uConsole.

- Duas páginas (swipe), com três botões cada, para caberem no ecrã:
  - **Som:** **-** e **+** mudam o volume 5%; **Mudo** corta o som (o botão passa a **Som**). O **+** também tira o mudo.
  - **Ecrã:** **-** e **+** mudam um nível do brilho do ecrã do uConsole (0 a 9); **Máx** põe no máximo.
- O **Brilho** do dock continua a ser o do ecrã do DashDeck.

### Serviços
Os serviços do uConsole que interessam ao DashDeck, em boxes com o estado ("ativo · auto" quer dizer
que arranca sozinho no boot):

| Box | Serviço | Para quê |
|---|---|---|
| ADS-B, AIS | `readsb`, `ais-catcher` | Recetores de aviões e navios (grupo `sdr`) |
| tar1090 | `tar1090` | Mapa web do ADS-B (só trabalha com o readsb ligado) |
| meshtasticd, GPS mux, gpsd | `meshtasticd`, `meshdeck-gpsmux`, `gpsd` | Rádio LoRa e GPS |
| DashDeck, CamDeck | `dashd`, `camd` (do utilizador) | Este ecrã (só **Reiniciar**, com confirmação) e o servidor da câmara |
| SSH, VNC, Pi Connect | `ssh`, `wayvnc`, `rpi-connect` (do utilizador) | Acesso remoto |
| Bluetooth, Modem 4G, Impressão | `bluetooth`, `ModemManager`, `cups` | Sistema |
| PostgreSQL, lighttpd | `postgresql`, `lighttpd` | Base de dados (Intercept) e servidor web (página do tar1090) |

- O detalhe mostra a descrição, o estado, se arranca no boot, uma nota, desde quando, o PID e a memória, com **Ligar**, **Parar** (com confirmação) e **Reiniciar**, e **Auto no boot** / **Sem arranque** (com confirmação) para escolher se o serviço arranca sozinho.
- Ficaram de fora o NetworkManager, o wpa_supplicant e outros essenciais (pará-los pelo ecrã cortava a rede), o `direwolf` (sem `/etc/direwolf.conf`, não arranca) e o `uconsole-4g-cm4` (é para o CM4).
- Os serviços do grupo **`sdr`** (readsb, AIS-catcher) são exclusivos, porque só há um RTL-SDR: ligar um para os outros. As telas ADS-B e AIS também ligam e desligam o seu serviço.

### Sistema
Duas páginas (swipe):
- **uConsole:** hostname, uptime, CPU e load, temperatura, RAM, espaço livre em `/`, bateria e o IP de cada interface. Os valores ficam amarelos ou vermelhos acima dos limites (CPU acima de 80%, temperatura acima de 65 °C e 75 °C, menos de 1 GB livre, bateria abaixo de 15%).
- **DashDeck:** versão do firmware, tamanho e orientação do ecrã e o cartão SD do ESP. **Vertical**/**Horizontal** troca a orientação e **Inverter** roda 180° (para o cabo do outro lado). Pede confirmação: o ESP reinicia em cerca de 5 s e volta à mesma tela. A escolha fica gravada no uConsole (`~/.config/dashd/state.json`) e passa à frente do `rotation` do `config.toml`.
