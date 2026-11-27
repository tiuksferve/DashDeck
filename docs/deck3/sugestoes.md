# Sugestões: do DashDeck ao DashDeck³

O DashDeck³ é portátil (bateria, Wi-Fi), tem ecrã quadrado AMOLED de 480×480, PSRAM de 8 MB, som,
dois microfones, IMU, três botões e forma de cubo. Ideias por ordem de valor/esforço.

## 1. Portar do DashDeck (melhorado para o DashDeck³)

| Módulo | O que muda no DashDeck³ |
|---|---|
| **Meshtastic** | Aviso de mensagem direta em pixel art **com som** e a acender o ecrã no bolso; respostas rápidas com os botões físicos (IO18 = responder "OK", duplo = "Já vou"); **ditado por voz**: o microfone grava, o uConsole transcreve (whisper.cpp local) e envia pela rede LoRa |
| **ADS-B / AIS** | Radar quadrado em ecrã inteiro (480×480 dá muito mais mapa); aviões e navios em **sprites de 8 bits** sobre o mapa; **bússola com o IMU**: rodar o cubo roda o radar para onde se está a olhar (precisa de magnetómetro: o QMI8658 não tem, mas dá para usar o rumo do GPS do uConsole a andar) |
| **Monitor de emergências ADS-B** | Sirene no altifalante + vibração visual, mesmo longe do uConsole |
| **Relógio** | Cenas em pixel art (mais paisagens do DashDeck, com o céu real); **mesa de cabeceira**: virar o cubo de lado mostra o relógio grande e escuro (IMU); alarme com som próprio no cubo |
| **Stocks** | Gráfico em barras de blocos de 8 bits; alerta sonoro quando uma ação passa um limite |
| **Câmara (CamDeck)** | Visor JPEG maior (o S3 tem descodificador rápido e PSRAM); botão IO18 = foto, como um disparador remoto sem fios |
| **Sistema / Serviços / Rede / AIO** | Iguais, com boxes maiores; os perfis da AIO num duplo-clique de botão |
| **Aplicações** | Lançador por voz ("abrir Gqrx") e as **Abertas** com miniaturas |

## 2. Novidades que o hardware permite

- **Cubo de comando por gestos (IMU):** cada face virada para cima é um modo (ex.: ecrã para cima =
  painel; de lado = relógio; ao contrário = "não incomodar" e ecrã apagado). Agitar = voltar ao início.
  Inclinar para os lados = mudar de página sem tocar. O firmware já envia `face` e `shake`.
- **Paralaxe e "vidro" 3D:** as estrelas e montanhas do arranque deslocam-se com o IMU.
- **Assistente de voz local:** botão IO18 premido = fala; o áudio vai pelo WebSocket para o uConsole
  (whisper.cpp + um LLM local ou a API do Claude) e a resposta volta em texto e voz (TTS com piper).
  Precisa de áudio binário no WebSocket (o frame 0x2 já está reservado).
- **Intercomunicador:** dois DashDeck³, ou DashDeck³ ↔ uConsole, áudio em tempo real pela rede local.
- **Espectro/SDR de bolso:** waterfall do rtl_sdr do uConsole no AMOLED (o preto puro fica ótimo),
  com volume e frequência nos botões; e o **espectro dos microfones** (FFT com esp-dsp) para ver ruído/RF.
- **Monitor de som / alarme de ruído:** detetar palmas, alarmes ou o nível de ruído; aviso no uConsole.
- **Tamagotchi de rádio:** uma criatura em pixel art que "come" os pacotes recebidos (LoRa, ADS-B, AIS):
  cresce com o tráfego, fica triste sem sinal. Divertido para mostrar o estado da estação.
- **Contador de passos e de quedas:** o QMI8658 tem deteção de movimento; com o Meshtastic, um alerta
  automático de queda/SOS para os contactos.
- **Controlo remoto do uConsole:** rato/apresentação (inclinar o cubo move o cursor), media keys,
  volume, e PTT do rádio pelos botões.
- **Notificações do telemóvel/PC:** o deckd recebe notificações (ntfy, Gotify, D-Bus) e reencaminha.
- **Modo "fora de casa":** sem o uConsole por perto, o cubo mostra relógio, tempo (Open-Meteo direto
  pela Internet) e a bateria, e guarda os eventos para quando voltar a ligar.

## 3. Melhorias de plataforma

| Tema | Ideia |
|---|---|
| **Primeira configuração sem USB** | Portal cativo: sem redes guardadas, o cubo cria o Wi-Fi "DashDeck3-XXXX" com uma página para escolher a rede |
| **Wi-Fi partilhado** | O deckd envia ao cubo as redes que o uConsole conhece (já aceite pelo firmware: `wifi` pela rede) |
| **Ícones e cenas no cartão SD** | Ícones e imagens guardados no SD do cubo, sem reenvio |
| **Poupança de bateria** | Light sleep com o ecrã apagado, acordar por toque/IMU/botão; Wi-Fi em power save máximo quando ocioso; medir o consumo (o AXP2101 mede a corrente) |
| **Proteção do AMOLED** | Já escurece e apaga; falta deslocar o cabeçalho 1-2 px de tempos a tempos |
| **Vários hosts** | O mesmo cubo a falar com o uConsole em casa e com um PC no trabalho (já escolhe por mDNS) |
| **Segurança** | TLS (wss) ou um par de chaves em vez do token no URL |
| **Simulador** | Compilar o `ui.c` para Linux com o SDL do LVGL e testar telas no uConsole sem a placa |
| **Templates em falta** | `plot` (mapas, gráficos), `log`, emojis e ícones de aplicações (como no DashDeck) |
