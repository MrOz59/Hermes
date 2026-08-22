# Architecture — Hermes / Hestia / HDT

> Arquitetura-alvo da plataforma e limites entre os componentes.

## 1. Visão geral

```text
┌──────────────────────────── Hermes Host ────────────────────────────┐
│                                                                     │
│  Application / Desktop                                              │
│          │                                                          │
│  Capture backend ── Hermes-KMS / PipeWire / platform backend        │
│          │                                                          │
│  Frame metadata                                                     │
│          │                                                          │
│  Encoder ── VAAPI / NVENC / AMF / VideoToolbox / platform backend   │
│          │                                                          │
│  Frame packetizer                                                   │
│          │                                                          │
│  FEC + retransmission cache                                         │
│          │                                                          │
│  Packet pacer ◄──── Congestion controller ◄──── Client feedback     │
│          │                                                          │
│  HDT transport / GameStream legacy                                  │
└──────────┬──────────────────────────────────────────────────────────┘
           │
           │ UDP / legacy channels / optional relay
           │
┌──────────▼──────────────────── Hestia Client ───────────────────────┐
│                                                                     │
│  Connectivity / ICE / path selection                                │
│          │                                                          │
│  HDT transport / GameStream legacy                                  │
│          │                                                          │
│  Authentication + decryption                                        │
│          │                                                          │
│  Reordering + FEC + NACK                                             │
│          │                                                          │
│  Frame assembler                                                     │
│          │                                                          │
│  Decoder                                                             │
│          │                                                          │
│  Presentation scheduler ── Renderer                                  │
│          │                                                          │
│  Metrics + network feedback ────────────────────────────────┐        │
│                                                            │        │
│  Input capture ─────────────────────────────────────────────┼────────┘
└─────────────────────────────────────────────────────────────┘
```

## 2. Camadas

### 2.1 Sessão

Responsável por:

- pareamento;
- autenticação;
- negociação;
- seleção de protocolo;
- ciclo de vida;
- permissões;
- aplicação iniciada;
- cleanup.

A sessão não deve implementar packet pacing ou decode.

### 2.2 Conectividade

Responsável por:

- descoberta LAN;
- endereço manual;
- candidatos ICE;
- STUN;
- TURN;
- seleção de caminho;
- keepalive;
- diagnóstico;
- futura migração de caminho.

A conectividade fornece um caminho ao transporte. Não interpreta frames.

### 2.3 Transporte

Responsável por:

- envio e recepção de datagramas;
- multiplexação;
- criptografia;
- sequence numbers;
- anti-replay;
- acknowledgements e feedback de transporte;
- MTU e fragmentação no nível HDT;
- estatísticas de pacote.

O transporte não decide resolução ou renderização.

### 2.4 Mídia

No host:

- recebe frames codificados;
- identifica importância e dependências;
- fragmenta;
- gera FEC;
- mantém cache limitado de retransmissão;
- entrega ao pacer.

No cliente:

- ordena;
- recupera;
- remonta;
- descarta vencidos;
- entrega bitstream ao decoder.

### 2.5 Controle de congestionamento

Executado principalmente no Hermes.

Entradas:

- feedback por pacote;
- RTT;
- one-way delay quando confiável;
- perda;
- ECN futuro;
- fila do pacer;
- atraso do decoder;
- frames apresentados.

Saídas:

- target bitrate;
- pacing rate;
- FEC budget;
- política de retransmissão;
- possível redução de FPS;
- sinal para encoder.

### 2.6 Aplicação

Responsável por:

- catálogo;
- execução;
- ambiente;
- display virtual;
- controle da sessão do usuário.

Não deve conhecer detalhes de packet header.

## 3. Modos de protocolo

### Legacy

```text
Hermes ou Hestia
    ↓
GameStream compatible stack
    ↓
Moonlight / Sunshine / Apollo compatibility
```

### HDT

```text
Session negotiation
    ↓
Noise handshake
    ↓
HDT encrypted connection
    ↓
multiplexed flows
```

Fluxos sugeridos:

```text
0 control
1 input
2 audio
3 video
4 feedback
5 FEC
6 diagnostics
```

Os IDs finais devem estar na especificação e possuir namespace de extensões.

## 4. Handshake conceitual

```text
Hestia                                        Hermes
  │                                             │
  │ discovery / manual invitation               │
  │────────────────────────────────────────────►│
  │                                             │
  │ capabilities + protocol versions            │
  │◄───────────────────────────────────────────►│
  │                                             │
  │ ICE candidate exchange                      │
  │◄───────────────────────────────────────────►│
  │                                             │
  │ connectivity checks                         │
  │◄═══════════════════════════════════════════►│
  │                                             │
  │ Noise authenticated handshake               │
  │◄═══════════════════════════════════════════►│
  │                                             │
  │ session parameters                          │
  │◄───────────────────────────────────────────►│
  │                                             │
  │ audio/video/input/feedback                   │
  │◄═══════════════════════════════════════════►│
```

O signalling pode ocorrer por LAN, conexão manual, arquivo de convite ou outro canal. Ele não deve exigir serviço central.

## 5. Identidade e pareamento

Cada instalação possui uma identidade persistente.

Pareamento deve:

- mostrar fingerprint;
- usar código ou confirmação local;
- armazenar chave pública do peer;
- permitir revogação;
- separar identidade de sessão;
- derivar chaves efêmeras por conexão;
- impedir replay.

O endereço IP não representa identidade.

## 6. Convites

Um convite pode conter:

- versão;
- fingerprint;
- candidatos ou endereço;
- porta;
- nome amigável;
- token de pareamento de uso único;
- expiração;
- STUN/TURN sugeridos;
- assinatura.

Convites devem ser compactos o suficiente para QR code quando possível.

Nunca incluir chave privada.

## 7. HDT packet model

Cabeçalho conceitual:

```text
magic
major_version
minor_version
header_length
flags
flow_id
connection_id
packet_sequence
frame_id
fragment_index
fragment_count
send_timestamp
deadline_delta
payload_length
extensions
ciphertext
authentication_tag
```

O formato final deve considerar MTU e custo criptográfico.

Requisitos:

- parser bounds-checked;
- extensões versionadas;
- campos desconhecidos ignoráveis apenas quando seguro;
- sequência suficientemente grande;
- wraparound definido;
- nonce único por chave;
- rekey antes do limite criptográfico.

## 8. Frames e deadlines

Cada frame codificado deve carregar metadata interna:

```cpp
struct EncodedFrame {
    uint64_t frame_id;
    FrameType type;
    bool is_reference;
    TimePoint captured_at;
    TimePoint encoded_at;
    TimePoint deadline;
    Buffer payload;
};
```

O deadline não precisa representar relógio absoluto compartilhado. Pode ser uma duração relativa negociada.

Para manter paridade com o scheduler do Hestia, a metadata recebida deve
permitir construir:

- `frame_id`;
- timeline relativa de apresentação da origem, quando disponível;
- deadline de apresentação convertido para o relógio monotônico local do
  cliente;
- timestamp de transporte usado para correlação.

Epoch, unidade e wraparound dos timestamps precisam ser definidos pelo HDT.
O Hermes não deve exigir que o relógio monotônico do host tenha o mesmo epoch
do relógio monotônico do Hestia.

Pacote vencido deve ser descartado antes de ocupar largura de banda ou fila.

## 9. Pacing

O pacer recebe packets com:

- prioridade;
- tamanho;
- deadline;
- frame;
- elegibilidade para descarte.

Filas devem ser bounded.

O pacer não pode permitir que frames antigos atrasem indefinidamente frames novos.

Política inicial:

```text
controle crítico
input
áudio
feedback
vídeo essencial
vídeo normal
FEC
```

A ordem exata pode ser ajustada após benchmark, especialmente para input e controle.

## 10. Feedback

Hestia deve enviar lotes compactos com:

- base sequence;
- received/missing bitmap ou status;
- receive deltas;
- RTT data;
- reordering;
- FEC recovered;
- retransmission recovered;
- latest decoded frame;
- latest presented frame;
- decoder queue;
- render queue;
- audio state.

Feedback não deve ser enviado por pacote individual em um socket separado sem agregação.

## 11. Congestion control

Primeira versão recomendada:

- delay trend;
- loss guard;
- RTT minimum;
- pacer queue;
- receiver feedback;
- conservative startup;
- bitrate floor e ceiling;
- encoder update em loop mais lento.

Loops:

```text
fast loop: feedback, pacing, retransmission, FEC
slow loop: encoder bitrate, FPS, resolution
```

A interface deve permitir algoritmos alternativos.

```cpp
class CongestionController {
public:
    virtual void on_packet_sent(const SentPacket&) = 0;
    virtual void on_feedback(const FeedbackBatch&) = 0;
    virtual NetworkTarget current_target() const = 0;
};
```

## 12. FEC e retransmissão

FEC:

- adaptativo;
- limitado;
- consciente de frame;
- maior em dados críticos;
- nunca usado para esconder bitrate excessivo.

Retransmissão:

- apenas quando RTT e deadline permitem;
- cache bounded;
- prioridade inferior a áudio e controle;
- cancelada quando frame vence.

IDR:

- rate-limited;
- protegido;
- não solicitado repetidamente;
- intra-refresh preferido quando apropriado.

## 13. Jitter buffer

O jitter buffer do Hestia deve buscar baixa latência, não reprodução perfeita.

Deve:

- adaptar uma janela pequena;
- descartar atraso irrecuperável;
- diferenciar reordering de perda;
- considerar decode deadline;
- evitar crescimento contínuo;
- expor métricas.

## 14. Áudio

Áudio possui prioridade temporal alta.

Requisitos:

- Opus;
- timestamps;
- jitter buffer separado;
- clock sync;
- concealment;
- descarte em vez de backlog;
- sincronização com vídeo sem adicionar latência excessiva.

## 15. Input

Input deve possuir:

- sequência global por direção e sessão;
- timestamp monotônico do cliente, em microssegundos, sem pressupor epoch
  compartilhado com o host;
- device ID e tipo semântico do evento;
- flag explícita de evento substituível;
- deduplicação;
- eventos confiáveis para transições críticas;
- caminho de baixa latência;
- estado recuperável para teclas e botões pressionados;
- suporte futuro a multiple controllers.

Movimento de mouse e amostras de sensor podem ser substituíveis; key-up,
button-up e transições de contato não podem desaparecer sem mecanismo de
correção. O Hermes deduplica antes de injetar e nunca aceita input de uma
sessão ou caminho não autenticado.

## 16. NAT traversal

Fluxo:

```text
host candidates
client candidates
        ↓
connectivity checks
        ↓
best direct path
        ↓
optional relay if configured
```

Prioridades:

1. LAN;
2. IPv6 direto;
3. server-reflexive;
4. port mapping;
5. overlay;
6. relay autohospedado.

A interface deve mostrar:

- tipo de caminho;
- RTT;
- relay ou direto;
- motivo da falha;
- recomendação acionável.

## 17. Relay opcional

O relay:

- não descriptografa mídia;
- encaminha datagramas;
- possui autenticação;
- limita banda;
- pode ser operado por usuário ou comunidade;
- deve ser distribuído separadamente;
- deve possuir Docker Compose e documentação.

Inicialmente, coturn pode atender o papel de relay. Um relay HDT próprio só deve ser criado se houver ganho claro.

## 18. Telemetria

Modelo de timeline por frame:

```text
captured
encoded
packetized
queued
first packet sent
last packet sent
first packet received
frame complete
decoded
presented
```

As IDs e timestamps devem permitir correlação sem expor conteúdo.

Cada processo deve publicar esses sinais por uma fronteira local de sessão
(`ISessionTelemetry` ou equivalente), separada de UI, logging e transporte.
O contrato mínimo cobre:

- início e falha de estágios da sessão;
- mudança de qualidade da conexão;
- término esperado ou inesperado;
- janelas bounded de métricas do pipeline;
- resumo agregado no shutdown;
- trace terminal opcional por `frame_id`.

Produtores publicam eventos e snapshots tipados. Adapters decidem se o destino
é log estruturado, overlay, endpoint administrativo, benchmark ou exportador
futuro. O pipeline não deve formatar UI nem escrever diretamente no destino.
Como eventos podem nascer em threads de conexão, decode e apresentação,
implementações injetadas precisam declarar e respeitar sua política de
concorrência.

Telemetria local não é automaticamente feedback de transporte. Apenas os
campos definidos e negociados pelo protocolo podem cruzar a rede; snapshots
administrativos, textos traduzidos, endereços e detalhes privados permanecem
locais por padrão.

## 19. Shutdown e cleanup

Toda sessão deve possuir um único caminho idempotente de shutdown.

Ordem conceitual:

1. impedir novos envios;
2. cancelar timers;
3. parar captura/input;
4. fechar filas;
5. drenar ou descartar;
6. parar encoder/decoder;
7. fechar transporte;
8. apagar chaves de sessão;
9. liberar display/aplicação;
10. emitir métricas finais.

Shutdown deve funcionar:

- durante discovery;
- durante ICE;
- durante handshake;
- durante streaming;
- após erro de decoder;
- após perda de rede.

## 20. Organização sugerida

### Hermes

```text
src/
├── legacy/
├── session/
├── capture/
├── encoder/
├── media/
├── hdt/
│   ├── protocol/
│   ├── crypto/
│   ├── transport/
│   ├── pacing/
│   ├── congestion/
│   ├── fec/
│   └── telemetry/
├── connectivity/
└── application/
```

### Hestia

```text
core/
├── legacy/
├── session/
├── connectivity/
├── hdt/
│   ├── protocol/
│   ├── crypto/
│   ├── transport/
│   ├── receive/
│   ├── feedback/
│   └── telemetry/
├── media/
├── input/
└── platform/
```

Adaptar essa organização ao código real em etapas; não mover tudo de uma vez.

## 21. Repositório de protocolo

Conteúdo recomendado:

```text
hdt-protocol/
├── spec/
├── schemas/
├── test-vectors/
├── fuzz-corpus/
├── wireshark/
├── reference-parser/
└── compatibility/
```

A especificação é a fonte de verdade do wire format, não o código do Hermes ou Hestia isoladamente.

## 22. Critérios arquiteturais de aceitação

Uma feature está arquiteturalmente correta quando:

- respeita as camadas;
- possui ownership claro;
- não adiciona dependência central;
- mantém fallback;
- é observável;
- é cancelável;
- possui limites;
- não bloqueia hot path;
- é testável sem interface gráfica quando possível;
- funciona nos dois lados do contrato.
