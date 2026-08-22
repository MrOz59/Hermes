# AI Context — Hermes, Hestia e HDT

> Documento de contexto permanente para agentes de desenvolvimento.  
> Este arquivo registra o que o projeto é, o que já foi decidido e quais limites não devem ser rediscutidos ou alterados sem uma decisão arquitetural explícita.

## 1. Projetos

### Hermes

Hermes é um fork de Apollo, que por sua vez deriva do Sunshine. É o host da plataforma.

Responsabilidades principais:

- descoberta e pareamento;
- gerenciamento de aplicações e sessões;
- captura de vídeo e áudio;
- integração com Hermes-KMS;
- encoding de hardware;
- transmissão de mídia;
- recepção de input;
- controle de congestionamento;
- telemetria do host;
- compatibilidade com clientes Moonlight.

### Hestia

Hestia é um fork de Moonlight e será o cliente de referência do Hermes.

Responsabilidades principais:

- descoberta e pareamento;
- conexão com hosts legados e Hermes;
- NAT traversal;
- recepção, ordenação e reconstrução de pacotes;
- FEC e NACK;
- decodificação e apresentação;
- captura e envio de input;
- feedback de rede e pipeline;
- diagnóstico e telemetria;
- fallback para GameStream.

### Hermes-KMS

Hermes-KMS é o caminho de display virtual e captura de baixa latência no Linux.

O novo protocolo não substitui Hermes-KMS. Os componentes devem se complementar:

```text
Hermes-KMS
   ↓
captura zero-copy
   ↓
encoder
   ↓
HDT
   ↓
Hestia
```

## 2. Objetivo do produto

A experiência final deve se aproximar da categoria de qualidade do Parsec:

- conexão simples;
- baixa latência;
- estabilidade em Wi-Fi e internet;
- adaptação rápida a perda, jitter e variação de banda;
- input responsivo;
- recuperação sem congelamentos longos;
- diagnóstico claro.

O projeto não pretende copiar código ou protocolos proprietários. O objetivo é alcançar comportamento equivalente usando tecnologias abertas e uma arquitetura própria.

## 3. Descentralização

Não haverá:

- conta obrigatória;
- serviço de identidade central obrigatório;
- servidor de signalling obrigatório;
- relay oficial obrigatório;
- dependência de uma nuvem operada pelos mantenedores.

A plataforma deve funcionar através de:

- LAN;
- IPv6 direto;
- IP e porta informados manualmente;
- convite exportado por arquivo, QR code ou link;
- STUN configurável;
- relay TURN ou relay Hermes autohospedado;
- redes overlay como WireGuard, Tailscale, Headscale, ZeroTier ou Nebula.

## 4. Limite técnico da conexão direta

O projeto não deve prometer conexão P2P direta em 100% dos casos.

CGNAT, NAT simétrico, firewalls restritivos e redes corporativas podem impedir hole punching. Quando isso ocorrer, as opções válidas são:

1. IPv6 direto;
2. port forwarding;
3. VPN ou rede overlay;
4. relay autohospedado;
5. diagnóstico informando que nenhum caminho foi encontrado.

O software deve explicar a falha, não esconder o problema com timeouts genéricos.

## 5. Compatibilidade durante a transição

Hermes e Hestia devem manter dois caminhos:

```text
GameStream legado
Hermes ─────────────────► Moonlight ou Hestia

HDT nativo
Hermes ═════════════════► Hestia
```

Regras:

- Moonlight existente não pode ser quebrado sem uma decisão de versão maior.
- Hestia deve continuar conectando a Sunshine, Apollo e hosts compatíveis.
- HDT começa experimental e desativado por padrão.
- O modo `auto` deve possuir fallback seguro.
- Falha em HDT não pode deixar a sessão em estado parcialmente inicializado.
- O caminho legado só poderá ser despriorizado após uma longa fase estável.
  "Estável" é definido pelos estágios E0–E3 em "Maturidade do HDT e saída do
  GameStream" (`HERMES_ROADMAP`), não por avaliação subjetiva. O GameStream
  não é removido: no limite ele chega a legado em manutenção, porque o
  Moonlight continua existindo.

Regra de horizonte:

- HDT pertence às fases H7/C7, não ao próximo incremento;
- H3/H4, C2/C3/C4, conectividade ICE e identidade nativa precisam amadurecer
  antes do primeiro caminho HDT utilizável;
- não existe data de entrega prometida;
- especificação, test vectors, revisão de segurança, benchmarks
  multiplataforma e validação real são pré-requisitos;
- até esses gates serem cumpridos, READMEs e UI devem descrevê-lo apenas como
  direção de longo prazo.

## 6. Protocolo nativo

O protocolo nativo é chamado provisoriamente de:

**HDT — Hermes Datagram Transport**

Características pretendidas:

- UDP;
- orientado a frames e deadlines;
- multiplexação de vídeo, áudio, input, controle, feedback e FEC;
- criptografia autenticada;
- pacing explícito;
- feedback frequente;
- congestion control sender-side;
- FEC adaptativo;
- retransmissão seletiva;
- descarte de dados vencidos;
- versionamento e capability negotiation;
- migração de caminho no futuro.

HDT não deve ser apenas “RTP com outro cabeçalho”. Ele precisa carregar semântica útil ao streaming interativo:

- `frame_id`;
- índice e quantidade de fragmentos;
- timestamp de envio;
- deadline;
- prioridade;
- tipo de frame;
- indicação de FEC;
- indicação de retransmissão;
- possibilidade de descarte.

## 7. Tecnologias escolhidas

### Base

- C++20 quando compatível com o código existente;
- CMake;
- bibliotecas pequenas e substituíveis;
- APIs assíncronas coerentes com o event loop de cada projeto;
- formatos binários no hot path.

### Segurança

Escolha preferencial:

- Noise Protocol Framework;
- X25519 para acordo de chaves;
- ChaCha20-Poly1305 como AEAD padrão;
- HKDF-SHA256 para derivação;
- identidades persistentes locais;
- fingerprint verificável no pareamento.

AES-GCM pode ser disponibilizado quando aceleração de hardware ou plataforma justificar, mas não deve ser a única opção.

Não criar primitivas criptográficas próprias.

### NAT traversal

Escolha preferencial inicial:

- ICE;
- libjuice como candidato de implementação;
- STUN configurável;
- coturn para relay autohospedado e interoperabilidade.

A biblioteca final deve ser confirmada por protótipo e benchmark. A decisão de usar ICE como arquitetura é mais importante que a dependência específica.

### Serialização

- cabeçalhos de mídia binários e próprios;
- Protobuf ou FlatBuffers para mensagens de controle, após benchmark;
- JSON apenas para configuração, debug e APIs administrativas;
- test vectors obrigatórios para qualquer formato de wire.

### Áudio

- Opus no protocolo nativo;
- caminho legado preservado;
- áudio priorizado sobre vídeo descartável.

### QUIC e WebRTC

QUIC Datagrams pode existir como backend experimental depois que o HDT estiver mensurável.

WebRTC pode ser usado como referência e, futuramente, em cliente web. Não adotar libwebrtc inteira como núcleo obrigatório da plataforma sem justificativa baseada em protótipo.

## 8. Decisões de organização dos repositórios

Hermes e Hestia permanecem em seus repositórios atuais.

Estratégia:

```text
main
develop/protocol-v2
feature/*
```

Um repositório separado deve abrigar a especificação compartilhada, test vectors e ferramentas do HDT.

Um repositório separado deve abrigar o relay opcional.

Não criar “Hermes 2” e “Hestia 2” como reescritas completas.

## 9. Componentes que devem permanecer desacoplados

No Hermes:

- captura;
- encoder;
- packetizer;
- transporte;
- pacer;
- congestion controller;
- FEC;
- sessão;
- compatibilidade GameStream.

No Hestia:

- conectividade;
- transporte;
- segurança;
- depacketizer;
- reordering;
- FEC;
- jitter buffer;
- decoder;
- renderer;
- input;
- feedback.

O encoder não deve conhecer sockets. O renderer não deve conhecer ICE. O packetizer não deve controlar diretamente a interface gráfica.

## 10. Princípios de desempenho

O objetivo não é entregar todo pacote. É entregar o frame mais recente que ainda possui utilidade.

Prioridades gerais:

```text
input e controle crítico
áudio
feedback
dados essenciais de vídeo
vídeo normal
FEC
dados vencidos: descartar
```

O sistema deve preferir:

- latência controlada a filas crescentes;
- queda de qualidade a congelamento;
- descarte consciente a backlog;
- adaptação gradual a oscilações;
- recuperação limitada a tempestade de IDR.

## 11. Observabilidade

Toda otimização deve ser mensurável.

Métricas mínimas:

- tempo de captura;
- tempo de encoding;
- tempo de packetization;
- atraso no pacer;
- RTT;
- jitter;
- perda;
- reordering;
- recuperação por FEC;
- recuperação por retransmissão;
- bitrate de payload;
- bitrate no fio;
- fila do decoder;
- tempo de decode;
- atraso de apresentação;
- frames descartados em cada estágio.

Mudanças de desempenho sem benchmark e telemetria não devem ser aceitas apenas por parecerem mais rápidas.

## 12. O que agentes não devem fazer

- Reescrever o projeto inteiro.
- Remover compatibilidade legado sem tarefa explícita.
- Ativar HDT por padrão antes da fase prevista.
- Inventar criptografia.
- Introduzir servidor central obrigatório.
- Acoplar transporte diretamente ao encoder ou UI.
- Copiar código entre Hermes e Hestia sem criar contrato compartilhado.
- Alterar wire format sem atualizar versão, especificação e test vectors.
- Implementar uma fase avançada ignorando dependências anteriores.
- Adicionar dependência grande sem análise de custo.
- Fazer uma PR com refatoração ampla e feature nova misturadas.
- Silenciar erro de rede com fallback invisível e sem diagnóstico.
