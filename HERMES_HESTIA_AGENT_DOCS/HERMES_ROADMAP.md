# Hermes — Roadmap do host para uma plataforma de streaming remoto descentralizada

> Documento de arquitetura e evolução do Hermes, partindo do fork atual de Apollo/Sunshine até um host de streaming remoto comparável ao Parsec em estabilidade, latência e facilidade de conexão, sem infraestrutura central obrigatória operada pelo projeto.

## 1. Visão do produto

O Hermes deve se tornar um host de streaming remoto que combine:

- captura Linux de baixa latência, incluindo o caminho Hermes-KMS com DMA-BUF/zero-copy;
- encoder de hardware com controle dinâmico de qualidade;
- transporte seguro e adaptativo para LAN, Wi-Fi e internet;
- descoberta e conexão peer-to-peer sem conta obrigatória;
- relay opcional e autohospedado pelo usuário ou por terceiros;
- compatibilidade com clientes Moonlight durante a transição;
- protocolo Hermes nativo para liberar recursos que o GameStream não permite;
- telemetria suficientemente boa para medir cada etapa da latência.

O objetivo não é copiar internamente o Parsec. O objetivo é entregar a mesma categoria de experiência:

1. o usuário seleciona um computador;
2. a conexão direta é tentada automaticamente;
3. o sistema escolhe o melhor caminho disponível;
4. bitrate, pacing, FEC e recuperação se adaptam continuamente;
5. a imagem continua responsiva mesmo quando a rede degrada;
6. não existe uma nuvem obrigatória controlada pelo projeto Hermes.

## 2. Limites técnicos que o produto precisa assumir

### 2.1 Descentralizado não significa ausência total de servidores públicos

Uma conexão direta pela internet pode exigir um servidor STUN para descobrir o endereço público e testar caminhos NAT. Alguns pares atrás de CGNAT, NAT simétrico ou firewalls restritivos não conseguirão uma conexão direta. O próprio TURN existe porque há situações em que um relay é tecnicamente necessário.

O Hermes não deve esconder essa realidade. A arquitetura correta é:

- LAN e IPv6 direto sem servidor;
- conexão por endereço/IP manual sem servidor;
- ICE com STUN configurável para hole punching;
- suporte a servidores STUN públicos ou autohospedados;
- TURN/relay opcional, nunca obrigatório e nunca operado necessariamente pelo projeto;
- suporte fácil a Tailscale, Headscale, ZeroTier, Nebula e WireGuard como caminhos alternativos;
- diagnóstico claro quando nenhuma rota é possível.

### 2.2 Compatibilidade Moonlight limita o protocolo

Clientes Moonlight existentes esperam o conjunto de protocolos GameStream herdado: descoberta e setup via HTTP/HTTPS/RTSP, canais de controle e mídia já definidos e extensões específicas. O Hermes pode melhorar internamente pacing, captura, encoder e parte da adaptação sem quebrar esses clientes, mas recursos como feedback detalhado por pacote, deadlines, troca de caminho e criptografia unificada exigem um cliente Hestia compatível.

Portanto, o Hermes deverá manter dois modos:

```text
Legacy GameStream
Hermes Host  ─────────────────────► Moonlight / Hestia em modo legado

Hermes Native Protocol
Hermes Host  ═════════════════════► Hestia
```

## 3. Escolhas tecnológicas para o produto final

## 3.1 Linguagem e organização

Manter o núcleo existente em C++ é a decisão mais realista. Reescrever o Sunshine/Apollo inteiro adicionaria risco sem melhorar a rede por si só.

Recomendação:

- C++20 ou versão já compatível com a base atual;
- interfaces pequenas para captura, encoder, transporte e congestion control;
- módulos novos isolados da compatibilidade GameStream;
- estruturas serializadas com Protobuf apenas para mensagens de controle, ou FlatBuffers se zero-copy nessas mensagens realmente importar;
- cabeçalhos de mídia binários próprios, compactos e versionados;
- testes unitários com Catch2 ou GoogleTest;
- testes de integração executáveis sem interface gráfica.

Não use JSON no caminho de mídia. JSON pode continuar na configuração e APIs administrativas.

## 3.2 Transporte final: Hermes Datagram Transport sobre UDP

A escolha recomendada para o caminho principal é um transporte próprio sobre UDP, orientado a frames e deadlines, chamado provisoriamente **HDT — Hermes Datagram Transport**.

Motivos:

- controle exato de pacing;
- controle exato de congestionamento;
- possibilidade de descartar pacotes vencidos antes de transmiti-los;
- retransmissão seletiva dependente do deadline;
- FEC por frame e por prioridade;
- um único socket multiplexado para vídeo, áudio, input, feedback e controle rápido;
- menor dependência do comportamento interno de uma implementação QUIC;
- integração direta com a produção de frames do encoder.

O HDT não deve reinventar primitivas criptográficas. Ele deve usar:

- Noise Protocol Framework para o handshake autenticado;
- X25519 para acordo de chaves;
- ChaCha20-Poly1305 como AEAD padrão, especialmente em dispositivos sem aceleração AES;
- AES-256-GCM opcional quando ambos os lados tiverem aceleração eficiente;
- BLAKE2s ou SHA-256 conforme a implementação Noise escolhida;
- chaves de sessão separadas por direção e por epoch;
- rotação de chaves durante sessões longas;
- proteção contra replay por número de pacote e janela deslizante.

Padrão sugerido para o primeiro pareamento:

```text
Noise_XX_25519_ChaChaPoly_BLAKE2s
```

Após o Hestia já conhecer a chave estática do host, usar retomada autenticada ou um padrão equivalente a IK, desde que a implementação escolhida e a análise de segurança sustentem a decisão.

### Por que não adotar QUIC como transporte principal imediatamente

QUIC Datagrams é seguro e útil, e deve existir como backend experimental. Entretanto:

- a biblioteca QUIC controla parte do congestionamento e da fila;
- datagramas continuam limitados ao tamanho de um pacote;
- o projeto ainda precisa implementar semântica de frames, FEC, NACK e deadlines;
- callbacks, filas internas e cópias podem prejudicar o pacing fino;
- suporte uniforme em todas as plataformas do Hestia pode complicar a adoção;
- o controle de congestionamento de propósito geral nem sempre é ideal para rajadas de frames interativos.

A interface deve permitir um backend QUIC futuro:

```cpp
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual SendResult send_datagram(DatagramView& datagram) = 0;
    virtual SendResult send_datagram_batch(DatagramBatchView& batch) = 0;
    virtual SendResult send_reliable(const ReliableMessage& message) = 0;
    virtual TransportStats stats() const = 0;
};
```

O batch é parte explícita do contrato porque o caminho GameStream atual usa
`sendmmsg`/GSO/USO quando disponível. Um backend que não consegue enviar o
lote retorna `fallback_required`, permitindo repetir exatamente os mesmos
views como datagramas individuais sem copiar ou repacotizar o frame.

Backends planejados:

```text
GameStreamTransport
HdtUdpTransport
QuicDatagramTransport (experimental)
```

Para protótipos de QUIC em Windows e Linux, MsQuic é a primeira opção a avaliar porque possui API C/C++, suporte a datagramas não confiáveis e ECN. Ele não deve ser colocado no caminho crítico do produto antes de benchmarks comparativos.

## 3.3 NAT traversal: ICE com libjuice

Use `libjuice` como base inicial para:

- coleta de candidatos host;
- STUN;
- candidatos server-reflexive;
- checks de conectividade;
- nomination do melhor par;
- Trickle ICE;
- TURN opcional configurado pelo usuário.

O projeto não deverá embutir credenciais ou depender de uma infraestrutura Hermes. A configuração pode aceitar:

```yaml
connectivity:
  mode: auto
  stun_servers:
    - stun:stun.example.org:3478
  turn_servers: []
  allow_upnp: true
  allow_pcp: true
  allow_nat_pmp: true
  allow_ipv6: true
```

Complementos:

- PCP como primeira preferência para mapear portas quando disponível;
- NAT-PMP para compatibilidade;
- UPnP IGD como fallback opcional;
- IPv6 global direto com firewall configurado;
- mDNS para descoberta em LAN;
- conexão manual por IP/porta;
- importação de convite contendo candidatos e fingerprint;
- suporte a signalling fora de banda por QR code, arquivo, clipboard ou link.

### Signalling descentralizado

ICE precisa trocar candidatos, mas isso não exige um servidor central. O Hermes/Hestia deve suportar três modelos:

1. **LAN automática:** mDNS e descoberta local.
2. **Convite fora de banda:** o host gera um blob ou link; o usuário envia por qualquer canal.
3. **Rendezvous opcional autohospedado:** pequeno serviço WebSocket/HTTPS que apenas encaminha ofertas, respostas e candidatos.

O servidor de rendezvous não deve receber mídia nem chaves privadas. Ele deve ser publicável separadamente como `hermes-rendezvous` e funcionar em Docker.

## 3.4 Relay autohospedado

Em vez de criar uma rede proprietária, ofereça compatibilidade com TURN padronizado e uma imagem oficial de configuração para coturn.

Entregáveis:

- documentação para coturn;
- Docker Compose pronto;
- geração de credenciais temporárias;
- configuração de limite de banda e usuários;
- seleção de relay por configuração no cliente;
- indicação explícita de que o tráfego está passando por relay;
- benchmark de capacidade por stream.

Mais tarde, um `hermes-relay` especializado pode ser considerado se TURN introduzir overhead ou dificultar políticas específicas. Não o crie antes de medir um problema real.

## 3.5 Controle de congestionamento

Criar uma interface modular no host:

```cpp
class ICongestionController {
public:
    virtual ~ICongestionController() = default;

    virtual void on_packet_sent(const SentPacket& packet) = 0;
    virtual void on_feedback(const FeedbackBatch& feedback) = 0;
    virtual void on_path_changed(const PathInfo& path) = 0;

    virtual CongestionTarget target() const = 0;
};
```

`CongestionTarget` deve fornecer:

```cpp
struct CongestionTarget {
    uint64_t encoder_bitrate_bps;
    uint64_t pacing_bitrate_bps;
    uint32_t fec_ratio_ppm;
    uint32_t max_frame_queue_us;
    uint32_t estimated_rtt_us;
    uint32_t estimated_queue_delay_us;
    uint32_t estimated_available_bitrate_bps;
};
```

Algoritmo inicial recomendado:

- sender-side;
- feedback por pacote inspirado no RTCP Congestion Control Feedback do RFC 8888;
- estimador de tendência de atraso semelhante ao GCC;
- componente de perda para evitar manter bitrate excessivo;
- AIMD conservador no primeiro protótipo;
- redução rápida quando cresce o atraso de fila;
- subida mais lenta e limitada;
- margem de 10% a 20% entre wire bitrate e capacidade estimada;
- modo específico para LAN que evita oscilar por microvariações irrelevantes.

Após a base estar estável, testar um controlador frame-aware inspirado em SQP e comparar por experimentos reproduzíveis.

## 3.6 Packet pacing

O pacer deve ser uma peça explícita, não apenas `sendto()` logo após o encoder terminar.

Requisitos:

- pacing por relógio monotônico de alta resolução;
- filas por prioridade;
- limite de burst;
- deadlines por pacote;
- remoção de pacotes vencidos;
- prioridade absoluta para input e feedback crítico;
- áudio acima de vídeo não essencial;
- FEC abaixo do vídeo original;
- suporte a `sendmmsg()` no Linux;
- avaliação de `SO_TXTIME`/ETF quando disponível;
- batching controlado sem criar microbursts;
- métricas do tempo em fila.

Prioridades sugeridas:

```text
P0: input, ACK crítico, keepalive
P1: áudio
P2: headers, keyframes e referência crítica
P3: vídeo normal
P4: retransmissão ainda útil
P5: FEC
```

## 3.7 FEC e retransmissão híbrida

Manter Reed-Solomon inicialmente se ele já estiver integrado e funcionando, mas mover a política para um módulo adaptativo.

Política:

- FEC baixo em LAN estável;
- aumento por perda em rajada;
- proteção maior para keyframes e parâmetros do codec;
- limite superior para não transformar perda em mais congestionamento;
- retransmissão apenas quando `deadline restante > RTT estimado + margem de decode`;
- NACK bitmap por frame;
- descarte de retransmissão vencida;
- cooldown de IDR;
- preferência por intra-refresh quando suportado;
- possibilidade futura de RaptorQ somente após benchmark de CPU, latência e overhead.

## 3.8 Encoder adaptativo

O host deve possuir um controlador comum para NVENC, VA-API, AMF e encoders futuros.

O loop rápido controla:

- pacing;
- FEC;
- retransmissão;
- descarte de pacotes;
- estimativa de rede.

O loop lento, a cada aproximadamente 200–500 ms, controla:

- bitrate do encoder;
- VBV;
- QP limits;
- resolução;
- FPS;
- GOP;
- intra-refresh;
- troca de codec quando houver renegociação apropriada.

Perfis de usuário:

```text
Lowest latency
Balanced
Highest quality
Preserve framerate
Preserve resolution
```

A política nunca deve manter uma fila longa para entregar frames antigos. O sistema precisa favorecer o frame decodificável mais recente.

## 3.9 Codecs

Prioridade recomendada:

1. H.264 para compatibilidade e baixa exigência de decoder;
2. HEVC para melhor qualidade por bitrate em hardware amplamente disponível;
3. AV1 quando encoder e decoder de hardware existirem e a latência for validada;
4. suporte futuro a 4:4:4 e HDR como capacidades negociadas.

Não faça AV1 obrigatório. Em alguns dispositivos, a menor banda não compensa maior tempo de encode/decode.

Negociação deve considerar:

- suporte real de encode/decode;
- bit depth;
- chroma;
- HDR metadata;
- limite de resolução e FPS;
- medição de latência do codec;
- bitrate disponível.

## 3.10 Áudio e input

Áudio:

- Opus como codec nativo do protocolo Hermes;
- pacotes pequenos, geralmente 5 ou 10 ms para baixa latência;
- FEC in-band do Opus quando útil;
- PLC no cliente;
- relógio independente e sincronização com vídeo sem aumentar buffer desnecessariamente.

Input:

- canal datagrama de prioridade máxima;
- envelope canônico com `sequence`, `client_timestamp_us`, `device_id`,
  `event_type` e `replaceable`;
- `sequence` global por direção e sessão, usado pelo Hermes para deduplicar
  antes da injeção;
- timestamp monotônico do cliente em microssegundos, sem assumir epoch
  compartilhado com o host;
- coalescing apenas para eventos de movimento substituíveis;
- nunca atrasar key press/release em uma fila antiga;
- estado periódico para recuperar perda de eventos;
- suporte seguro a gamepad, mouse, teclado, touch, caneta, giroscópio e sensores;
- controle de permissões por cliente pareado.

O contrato tipado do Hestia já produz esse envelope no limite de captura. O
`GameStreamInputSender` ignora os metadados que o protocolo legado não
transporta; o futuro sender HDT deve serializá-los sem reinterpretar unidade,
epoch ou semântica de substituição.

## 3.11 Segurança e identidade

Cada instalação Hermes gera uma identidade local:

- chave estática X25519 para Noise;
- opcionalmente Ed25519 para assinatura de convites e metadados;
- fingerprint legível e QR code;
- lista de dispositivos Hestia autorizados;
- revogação individual;
- nomes e permissões por cliente;
- confirmação física ou PIN no primeiro pareamento;
- nenhum identificador global obrigatório;
- nenhuma conta Hermes obrigatória.

Permissões sugeridas:

```text
View only
Keyboard and mouse
Gamepad
Clipboard
File transfer
Wake host
Launch applications
Administrative control
```

A interface web administrativa deve continuar separada do protocolo de mídia e não ficar exposta automaticamente à internet.

## 3.12 Observabilidade

Expor métricas por sessão:

- captura;
- conversão de cor;
- encode;
- packetization;
- fila do pacer;
- wire bitrate;
- payload bitrate;
- RTT;
- one-way delay quando relógios permitirem;
- atraso estimado de fila;
- perda;
- reordering;
- FEC produzido e recuperado;
- retransmissões;
- frames descartados;
- keyframes;
- caminho ICE selecionado;
- uso de relay;
- decode e presentation reportados pelo Hestia;
- latência end-to-end estimada.

Formatos:

- overlay legível;
- JSON por sessão;
- logs estruturados;
- endpoint Prometheus opcional;
- exportação de trace para análise offline.

## 4. Arquitetura final proposta

```text
                         ┌─────────────────────────────┐
                         │ Application/session manager │
                         └──────────────┬──────────────┘
                                        │
┌──────────────┐   DMA-BUF   ┌─────────▼─────────┐
│ Hermes-KMS / │────────────►│ Capture pipeline   │
│ other capture│             └─────────┬─────────┘
└──────────────┘                       │ frame + timestamps
                              ┌────────▼─────────┐
                              │ Encoder adapter   │◄─────────────┐
                              └────────┬─────────┘              │
                                       │ access units           │ target
                              ┌────────▼─────────┐              │
                              │ Frame packetizer  │              │
                              └────────┬─────────┘              │
                                       │ packets                 │
                    ┌──────────────────▼──────────────────┐      │
                    │ FEC + deadline retransmission       │      │
                    └──────────────────┬──────────────────┘      │
                                       │                         │
                              ┌────────▼─────────┐               │
                              │ Priority pacer    │               │
                              └────────┬─────────┘               │
                                       │                         │
                     ┌─────────────────▼──────────────────┐      │
                     │ HDT / legacy / experimental QUIC   │      │
                     └─────────────────┬──────────────────┘      │
                                       │                         │
                                    network                      │
                                       │                         │
                              ┌────────▼─────────┐               │
                              │ Hestia feedback   │───────────────┘
                              └──────────────────┘
```

## 5. Roadmap por fases

## Fase H0 — Baseline e congelamento de métricas

**Objetivo:** saber exatamente onde o Hermes atual perde tempo e estabilidade.

Entregáveis:

- suíte de benchmark repetível;
- coleta de métricas por frame;
- identificação de threads e filas atuais;
- captura de pcap sem payload descriptografado em builds de teste;
- cenários `tc netem` versionados;
- testes LAN, Wi-Fi, WAN, perda em rajada e bufferbloat;
- comparação do caminho EVDI, captura tradicional e Hermes-KMS;
- relatório de baseline com p50, p95 e p99.

Critérios de conclusão:

- cada frame possui IDs e timestamps correlacionáveis;
- o projeto sabe medir capture-to-encode, encode, send queue e feedback;
- regressões de 1–2 ms são detectáveis.

Progresso incremental:

- ✅ coletores bounded por sessão publicam janelas de encode, rede, filas,
  perdas e p50/p95/p99;
- ✅ `frame_id` correlaciona os traces terminais do Hermes e do Hestia sem
  pressupor clocks com o mesmo epoch;
- ✅ o vocabulário local de sessão está alinhado com `ISessionTelemetry` do
  Hestia, mantendo telemetria administrativa separada do feedback no wire;
- ✅ o harness importa os traces terminais `HESTIA_FRAME_TRACE`, calcula
  percentis nearest-rank do caminho receive-to-present, infere gaps de
  `frame_id` e os pareia com as janelas bounded do Hermes;
- ✅ o gate H2 exige amostras mínimas, limita regressões LAN a 2 ms, verifica
  FPS/perda, exige melhora nas caudas sob link limitado e falha por status de
  processo; apply/clear de `tc` possuem marcador transacional e não removem um
  qdisc substituído externamente;
- ⏳ falta executar a matriz reference/candidate em uma sessão Hestia pareada e
  anexar o relatório empírico; o ambiente local atual não fornece esse stream
  nem o certificado de cliente.

## Fase H1 — Modularização sem alterar protocolo

**Objetivo:** criar pontos de extensão antes de mexer na rede.

Entregáveis:

- `ICaptureBackend`;
- `IEncoderBackend`;
- `ITransport`;
- `ICongestionController`;
- `IPacketPacer`;
- `IFecController`;
- `ISessionTelemetry`;
- testes unitários com implementações falsas;
- modo GameStream preservado.

Critério de conclusão:

- o comportamento legado continua funcional;
- componentes podem ser testados fora de uma sessão completa.

Progresso incremental:

- ✅ `ISessionTelemetry` tipado e injetável recebe lifecycle, resolução,
  frames codificados, estágios de rede e descartes por sessão;
- ✅ `bounded_session_telemetry_t` preserva os limites H0 de sessões e
  amostras, centraliza a sincronização entre RTSP, encoder, broadcaster e
  diagnóstico e mantém o hot path sem alocação dinâmica;
- ✅ `legacy_session_telemetry_adapter_t` mantém os entry points GameStream,
  converte IDs opacos e milissegundos somente na borda e não altera protocolo,
  endpoints nem payload;
- ✅ `IFecController` tipado e injetável isola a política e o encoding
  Reed-Solomon dos broadcasters de vídeo e áudio;
- ✅ o bloco FEC de vídeo é move-only, empresta somente os data shards do
  payload vivo e mantém ownership explícito de padding, paridade e prefixos;
- ✅ o encoder in-place de áudio permanece persistente por thread e preserva a
  matriz geradora Nvidia usada pelo GameStream;
- ✅ implementações fake validam as duas interfaces sem iniciar uma sessão;
  testes de equivalência confirmam bytes, padding, mínimo de paridade e matriz
  de áudio contra o Reed-Solomon legado;
- ✅ `IPacketPacer` tipado e injetável retira do broadcaster a decisão temporal
  de envio dos lotes de vídeo, usando somente relógio monotônico;
- ✅ `legacy_packet_pacer_t` preserva a janela de 1 ms, o teto equivalente a
  80% de 1 Gbps, a primeira espera de cada frame, o carry-over entre frames e
  a contagem cumulativa entre blocos FEC;
- ✅ `IPacerTimer` permite validar o scheduler com tempo falso e confirma
  deadlines vencidos, oversleep e tempo efetivamente aguardado sem sleeps
  reais;
- ✅ a extração não altera batching, headers, payload, bitrate, áudio ou wire;
  prioridades, deadlines, limite explícito de burst e pacing de áudio
  continuam reservados para H2;
- ✅ `ICongestionController` tipado e injetável pertence a cada sessão e recebe
  lotes enviados, feedback de entrega e mudanças de caminho a partir das
  threads já existentes;
- ✅ `legacy_fixed_congestion_controller_t` publica o bitrate de encoder, o
  pacing-base de 800 Mbps e a proporção FEC do baseline H1, mas mantém o target
  imutável para preservar integralmente a política GameStream dessa fase;
- ✅ adapters bounded validam e convertem tanto o relatório agregado legado de
  32 bytes quanto `SS_FRAME_FEC_STATUS` de 21 bytes já emitido pelo
  Moonlight/Hestia, sem casts desalinhados;
- ✅ a observação de envio ocorre uma vez por lote e distingue data/FEC sem
  alocação por pacote; estimadores, AIMD/GCC e aplicação dinâmica do target
  continuam reservados para a fase de controle adaptativo;
- ✅ `ICaptureBackend` tipado e injetável agora concentra a enumeração de
  saídas e a criação das sessões de captura;
- ✅ `legacy_platform_capture_backend_t` preserva a seleção atual de KMS,
  Wayland, X11, NVFBC, DXGI e WGC ao delegar para `platf::display_names()` e
  `platf::display()`;
- ✅ os caminhos paralelo e síncrono possuem o backend pelo lifetime de suas
  threads; seleção por sessão, troca de display, duas tentativas com intervalo
  de 200 ms, filas, prioridades, pool de imagens e zero-copy permanecem
  inalterados;
- ✅ testes headless com backend e sessão falsos validam enumeração, criação,
  configuração, identidade da sessão e indisponibilidade sem inicializar a
  plataforma gráfica;
- ✅ `IEncoderBackend` tipado e injetável concentra a criação do dispositivo
  de interop e da sessão de encode, preservando `encoder_t` como descritor de
  capabilities e política;
- ✅ `encode_session_t` agora expõe `encode_frame()` diretamente; FFmpeg e
  NVENC continuam nas mesmas rotinas, mas o hot path deixa de depender de
  `dynamic_cast` e pode ser implementado por backends novos ou falsos;
- ✅ `legacy_video_encoder_backend_t` preserva seleção de formato, colorspace,
  HDR, opções e fallback do FFmpeg, NVENC nativo, probing, ordem de fallback e
  teardown atuais;
- ✅ os caminhos paralelo e síncrono possuem o backend pelo lifetime de suas
  threads, e o probing usa o mesmo adapter legado sem alterar a seleção global;
- ✅ testes headless validam encaminhamento de display, descriptor,
  configuração e dimensões, transferência de ownership do device e o ABI
  completo da sessão sem GPU ou codec;
- ✅ `ITransport` tipado e injetável agora cobre datagramas individuais,
  batches e mensagens confiáveis, com resultado explícito para sucesso, falha
  e fallback individual;
- ✅ `game_stream_transport_t` preserva os views scatter/gather da plataforma,
  os mesmos sockets UDP, batching `sendmmsg`/GSO/USO e o canal confiável ENet,
  sem cópia, repacotização ou mudança de wire;
- ✅ uma instância compartilhada pelo contexto de broadcast atende vídeo,
  áudio/FEC e controle, mantendo ownership e lifetime explícitos e publicando
  contadores atômicos de mensagens, bytes, falhas e fallbacks;
- ✅ quando o batch legado retorna falso, o broadcaster ainda envia cada shard
  individualmente na mesma ordem; falhas individuais continuam sem alterar o
  fluxo ou a política GameStream;
- ✅ testes headless usam transporte, datagrama e canal confiável falsos para
  validar identidade dos views, resultados, fallback e estatísticas sem abrir
  sockets ou iniciar ENet;
- ✅ todas as interfaces previstas para H1 estão implementadas; o modo
  GameStream permanece como backend de produção e os componentes podem ser
  exercitados fora de uma sessão completa.

## Fase H2 — Pacing e controle de filas no GameStream

**Objetivo:** obter ganho imediato sem exigir Hestia nativo.

Entregáveis:

- pacer de vídeo e áudio;
- prioridades;
- limite de burst;
- descarte de pacotes vencidos;
- proteção contra acúmulo de frames;
- métricas de queue delay local;
- tratamento de IDR para não criar rajada destrutiva.

Critérios:

- redução de p95/p99 de latência sob link limitado;
- ausência de crescimento ilimitado de fila;
- sem regressão perceptível em LAN.

Progresso incremental:

- ✅ `rate_limited_packet_pacer_t` usa o target por sessão e limita cada lote de
  vídeo a no máximo 1 ms do bitrate de pacing, preservando os limites de 64
  pacotes/64 KiB e o backend `sendmmsg`/GSO/USO;
- ✅ o target GameStream fixo agora deriva do bitrate do encoder, reserva a
  proporção FEC configurada e 10% para headers e variação normal, mantendo 1
  Mbps como piso defensivo e 800 Mbps como teto legado;
- ✅ lotes consecutivos usam relógio monotônico de alta resolução e um
  leaky-bucket que reinicia no instante real após idle ou oversleep; atraso do
  scheduler nunca vira crédito para uma rajada de catch-up;
- ✅ vídeo e áudio possuem estado independente por sessão em registros LRU
  limitados a 32 entradas por broadcaster, impedindo crescimento por churn
  enquanto outras sessões mantêm as threads compartilhadas vivas;
- ✅ o áudio espaça o pacote de dados e os dois repair shards do fim de cada
  bloco dentro de uma janela `packetDuration`; packets GameStream, sequência,
  timestamps, matriz FEC, sockets e QoS permanecem inalterados;
- ✅ a métrica de vídeo `pacer_ms` continua medindo o tempo monotônico
  efetivamente aguardado e agora torna visível o custo da política rate-aware;
- ✅ testes com relógio falso validam o limite de 1 ms, espaçamento exato,
  oversleep sem catch-up, burst de repair do áudio, identidade por sessão e
  limite do registro sem sleeps reais;
- ✅ o target GameStream fixo define `max_frame_queue_us` como duas janelas do
  framerate negociado, arredondadas para cima e limitadas entre 8 e 100 ms
  (fallback defensivo de 50 ms quando o framerate é inválido);
- ✅ antes de packetização, Reed-Solomon, criptografia e pacing, o broadcaster
  compara `encoded_timestamp` com esse orçamento e descarta o access unit
  vencido, evitando gastar CPU e banda com vídeo que já acumulou atraso local;
- ✅ a política classifica IDR como referência e demais frames como normais.
  Após qualquer descarte que quebra a cadeia de referência, ela solicita um
  único IDR pelo mecanismo GameStream existente e descarta frames dependentes
  até receber um IDR fresco; um IDR também vencido é descartado e novamente
  solicitado;
- ✅ o estado de recuperação é por sessão e LRU-bounded em 32 entradas. Os
  diagnósticos separam `frames_dropped_send_deadline` de
  `frames_dropped_recovery_wait`, e o trace terminal publica
  `send_deadline_expired` ou `awaiting_idr`;
- ✅ testes determinísticos cobrem orçamento por framerate, prioridade tipada,
  expiração, recuperação por IDR, deadline desabilitado e churn de sessões;
- ✅ o overflow da fila global agora marca, ainda sob o lock da fila, cada
  sessão cujo access unit foi removido. O frame novo só fica visível ao
  broadcaster depois dessas marcas, eliminando a janela em que um P-frame
  dependente poderia escapar;
- ✅ a política compartilhada é thread-safe e mantém a causa da recuperação.
  No primeiro P-frame após overflow, o broadcaster solicita um único IDR e
  descarta dependentes até um IDR fresco; registro e remoção RTSP limpam o slot
  para impedir vazamento de estado quando um endereço de sessão é reutilizado;
- ✅ testes cobrem overflow real da fila de 32 access units com duas sessões
  afetadas, solicitação única de IDR, limpeza por ciclo de vida e chamadas
  concorrentes sem crescimento do registro bounded;
- ✅ `media_priority_e` fixa a ordem do caminho compatível: P0 controle/input/
  feedback, P1 áudio, P2 IDR/referência, P3 vídeo normal e P5 FEC. O mapeamento
  para prioridades de worker é centralizado e testado;
- ✅ controle e áudio rodam em classe crítica, enquanto vídeo permanece high.
  Áudio e vídeo continuam em threads e sockets/QoS separados, portanto uma
  rajada de vídeo não adquire lock nem serializa controle ou áudio;
- ✅ ao entrar na fila global, um IDR remove atomicamente access units mais
  antigos somente da própria sessão e é inserido na frente. A ordem FIFO das
  outras sessões é preservada, e o overflow bounded normal ainda é aplicado
  caso a fila permaneça cheia;
- ✅ dados originais continuam precedendo repair shards dentro de cada bloco.
  Frames removidos por um IDR são expostos separadamente em
  `frames_dropped_reference_superseded`;
- ✅ testes determinísticos validam a ordem P0–P5, o mapeamento de worker, a
  inserção na frente, a preservação FIFO de outras sessões e a contabilização
  dos frames superseded;
- ✅ o incremento é somente host-side: bytes, mensagens e capacidades
  GameStream não mudaram, portanto a Hestia mantém paridade sem alteração;
- ✅ pedidos explícitos do cliente e pedidos internos de recuperação agora
  compartilham um cooldown monotônico de 100 ms por sessão. O estado permanece
  no mesmo registro LRU-bounded de 32 entradas e não usa sleep nem bloqueia o
  worker de controle;
- ✅ quando um retry interno encontra o gate fechado, `idr_requested` permanece
  falso: o próximo frame dependente tenta novamente ao abrir a janela. Isso
  também vale quando o IDR de substituição já chegou vencido, evitando perder
  a recuperação;
- ✅ o IDR obrigatório de início da captura continua isento. Depois de aceito,
  todo IDR usa o pacer normal de vídeo. Para absorver a variação natural de
  tamanho do encoder, qualquer access unit que não caiba no deadline restante
  ao target médio recebe somente a taxa mínima necessária para ocupar no máximo
  85% da janela, ainda limitada ao teto legado de 800 Mbps. No caso do IDR isso
  evita o loop tela-preta → IDR vencido → retry sem reabrir o burst irrestrito;
  os diagnósticos por sessão expõem
  `idr_requests_accepted` e `idr_requests_rate_limited`;
- ✅ testes determinísticos cobrem cooldown por sessão, independência entre
  sessões, retry adiado, IDR substituto vencido, limpeza por ciclo de vida e
  propagação dos contadores pela fronteira tipada de telemetria;
- ✅ a proteção é somente host-side e não altera mensagens ou bytes GameStream,
  mantendo a Hestia compatível sem mudança;
- ✅ `packet_pacing_config_t` agora recebe o deadline monotônico absoluto
  `encoded_timestamp + max_frame_queue_us`. Orçamento zero ou timestamp ausente
  preserva explicitamente o caminho sem deadline;
- ✅ antes de outro bloco FEC, de cada lote paced e de cada datagrama no
  fallback individual, o broadcaster verifica a janela. O pacer não dorme
  quando a próxima partida já ficaria vencida e detecta oversleep que cruza o
  limite;
- ✅ `sendmmsg`/GSO/USO continua indivisível depois da submissão; portanto a
  granularidade controlável é o lote já limitado a no máximo 1 ms. No fallback
  parcial, somente sequências RTP processadas são consumidas e os shards/blocos
  restantes são abandonados;
- ✅ um aborto incrementa `frames_dropped_packet_deadline`, publica
  `packet_deadline_expired`, marca a cadeia de referência quebrada e solicita
  IDR pelo mesmo cooldown de 100 ms. Se o gate estiver fechado, a causa
  permanece e um frame dependente tenta novamente;
- ✅ testes com relógio falso cobrem partida futura já vencida, oversleep,
  igualdade exata do limite, pacer legado, retenção da causa de recuperação e
  contador por sessão;
- ✅ bytes, headers, criptografia, capacidades e mensagens GameStream não
  mudaram; a Hestia mantém paridade sem patch;
- ✅ auxiliares persistentes de clipboard no Linux são iniciados com herança de
  descritores limitada, impedindo `wl-copy`/`xclip` de reter listeners e sockets
  de mídia depois de uma sessão. A criação do host ENet valida falha de
  bind/alocação antes de configurar QoS, rejeitando a sessão sem crash; teste
  com porta UDP ocupada cobre a regressão;
- ✅ o gate reproduzível combina as janelas do Hermes com traces terminais já
  suportados pela Hestia, cobra p95/p99/FPS/perda em LAN, melhora mínima de 1%
  nas caudas sob perfis limitados e o teto de 100 ms no pior p99 publicado da
  fila host-side; entradas ausentes ou curtas falham sem fabricar resultado;
- ⏳ a implementação compatível da H2 está completa, mas a aceitação permanece
  aberta até a execução reference/candidate da matriz LAN/netem com stream
  Hestia real. Uma fila realmente unificada por pacote continua reservada ao
  HDT, pois serializar sockets GameStream independentes criaria contenção
  artificial.

## Fase H3 — Adaptação de bitrate compatível com legado

**Objetivo:** melhorar o comportamento com feedback já disponível no ecossistema atual.

Entregáveis:

- estimativa de RTT, perda e jitter;
- controlador de bitrate conservador;
- integração comum com NVENC, VA-API e AMF;
- hysteresis para evitar oscilação;
- presets de latência/qualidade;
- fallback para configuração fixa.

Critérios:

- recuperação automática após redução de banda;
- bufferbloat menor que no baseline;
- estabilidade sem alternância constante de bitrate.

Progresso incremental:

- ✅ `bandwidth_estimator_t` deriva perda bruta e perda irrecuperável do
  feedback que o cliente GameStream já envia, separando o que a FEC reparou do
  que o usuário realmente viu, com janela deslizante bounded e relógio
  injetado;
- ✅ `adaptive_congestion_controller_t` sobe proteção rápido, libera devagar,
  usa hold-down e deadband entre os limiares e nunca desce abaixo do
  `fec_percentage` configurado. Ele deliberadamente não mexe no bitrate do
  encoder nem baixa o pacing: o encoder é configurado uma vez e um pacing menor
  só criaria fila até o deadline;
- ✅ o comportamento fica atrás de `adaptive_fec`, desativado por padrão, com
  fallback para `legacy_fixed_congestion_controller_t` byte a byte idêntico ao
  anterior;
- ✅ a proteção passou a ser tipada por frame: `congestion_target_t` publica
  `key_frame_fec_ratio_ppm` e o broadcaster escolhe o nível por
  `frame_fec_ratio_ppm(target, is_idr)`. Key frames recebem +10 pontos sobre o
  nível corrente, teto de 60%, desde o primeiro frame da sessão — quando a
  perda vira medição o IDR já se perdeu. O nível de key frame nunca fica abaixo
  do nível normal, inclusive quando o host configura proteção acima do teto
  adaptativo;
- ✅ `plan_frame_fec()` substitui o cálculo inline de blocos: quando um frame
  não cabe no limite de 4 blocos, a proteção é reduzida ao maior percentual que
  ainda cabe em vez de ser desligada. Antes, o frame mais caro do stream — um
  IDR grande — era exatamente o que saía sem nenhuma proteção. Acima de
  4 × 255 pacotes nem os data shards cabem e o frame continua sem FEC, agora
  com log explícito;
- ✅ o efeito é mensurável sem instrumentação nova: `fec_overhead_percent`,
  `data_shards` e `fec_shards` da telemetria por sessão já expõem o custo real,
  e o plano reduzido/desligado aparece em log;
- ✅ o incremento é somente host-side. O percentual de FEC já viaja por pacote
  em `fecInfo`, e variação por frame já ocorria (frames enormes e o mínimo de
  parity shards), portanto Moonlight e Hestia mantêm paridade sem patch;
- ✅ testes determinísticos cobrem o plano de blocos (mantém o percentual
  quando cabe, reduz quando não cabe, reporta frame sem proteção possível,
  nunca ultrapassa o limite de blocos, entrada degenerada) e a política de key
  frame (proteção desde o início, acompanha o nível adaptativo, teto, nunca
  abaixo do nível normal, retorno ao baseline em troca de caminho, controlador
  fixo tratando todo tipo de frame igual);
- ✅ RTT e a parcela de enfileiramento saem da conexão de controle, que
  compartilha o caminho com o vídeo, sem mensagem nova na rede; com
  `packet_feedback` v1 negociado, a taxa de entrega medida e o gradiente de
  atraso substituem a estimativa derivada de perda;
- ✅ o encoder passou a ser reconfigurado em tempo real. O controlador publica
  uma estimativa contínua e o encoder anda em degraus fixos sobre ela: cada
  mudança custa algo no backend — nvenc força key frame, VA-API reenvia o
  buffer de rate control — e perseguir a estimativa gastaria mais em transições
  do que a adaptação economiza. Desce rápido e sobe devagar, e só reocupa um
  degrau que o caminho sustentou por uma janela de amostras com margem, porque
  a estimativa derivada de perda sobe assim que o encoder recua e passa a
  descrever uma carga que nada testou;
- ✅ a integração é comum: escrever `bit_rate`/`rc_max_rate` no contexto entre
  frames basta para nvenc, qsv e libx264. VA-API era o único backend que fixava
  rate control no `avcodec_open2()`; `patches/ffmpeg` anexa o buffer de rate
  control por picture e traz a VA-API para a mesma interface, com medições e
  harness de verificação. Falta AMF, que não existe no host Linux;
- ✅ o pacing deixou de estourar em caminho sem capacidade: burst e catch-up
  assumiam jitter absorvível transmitindo mais forte, o que em fila de gargalo
  cheia produz exatamente o drop que deveriam evitar. Somado ao backoff entre
  pedidos de IDR e ao teto por frame do VA-API
  (`vaapi_max_frame_size_factor`), fecha o laço em que um frame atrasado virava
  cascata de key frames;
- ⏳ faltam os presets de latência/qualidade, que ainda não existem como
  configuração — a escada é fixa em código;
- ⏳ a validação empírica (matriz `hermes-netem.sh`, perfis `wifi` e
  `burst-loss`) ainda não foi executada para esta fase, e depende da aceitação
  pendente da H2 para ter piso de comparação. Os critérios de "recuperação
  automática após redução de banda" e "bufferbloat menor que o baseline"
  agora têm mecanismo, mas continuam sem medição.

## Fase H4 — Extensões Hermes sobre sessão legada

**Objetivo:** criar uma ponte de evolução para Hestia sem abandonar Moonlight.

Entregáveis:

- negociação de capabilities Hermes;
- feedback por pacote;
- report de frame recebido/decodificado/apresentado;
- NACK bitmap;
- deadlines e prioridades;
- FEC adaptativo;
- versionamento independente das extensões;
- fallback limpo quando o cliente não anunciar suporte.

Critério:

- Hestia obtém recursos avançados usando o setup existente;
- Moonlight continua conectando sem mudanças.

Progresso incremental:

- ✅ negociação de capabilities: `GET /api/hestia/v1/capabilities` anuncia um
  array `extensions` com versão por extensão, o cliente anuncia o subconjunto
  que fala em `session/prepare`, e a resposta reporta o que ficou em vigor;
- ✅ versionamento independente e fallback limpo: nome desconhecido ou versão
  não implementada é descartado em vez de falhar a sessão, e um cliente que não
  anuncia nada — Moonlight incluído — recebe exatamente a sessão que teria
  antes das extensões existirem. O host só anuncia o que consegue honrar, então
  com `adaptive_fec` desligado o registro fica vazio;
- ✅ o conjunto negociado sobrevive ao `/resume`, que não tem prepare próprio.
  Sem isso um cliente reconectado perdia silenciosamente tudo que negociou;
- ✅ `packet_feedback` v1: report no formato RFC 8888 do que chegou, do que se
  perdeu e de quando cada pacote chegou, de onde saem a taxa de entrega medida
  e o gradiente de atraso;
- ✅ `frame_report` v1: por frame, quando o último pacote chegou, quanto durou
  a decodificação, quanto esperou para ser exibido e se foi exibido ou
  descartado. É a primeira medição que enxerga além do fio, e dela sai a
  separação entre cliente que não acompanha e caminho que não acompanha —
  distinção que importa porque baixar bitrate e mandar key frame não ajudam no
  primeiro caso, e o key frame piora. Publicado em
  `pipeline.congestion.client`, sem ninguém agir sobre ele ainda;
- ✅ FEC adaptativo (ver H3);
- ⏳ NACK bitmap. Depende de haver um caminho de retransmissão: hoje o pipeline
  é só FEC, e um bitmap sem retransmissão não diz nada que o `packet_feedback`
  já não diga;
- ⏳ deadlines e prioridades na banda de extensão. O host já tem deadline por
  frame internamente (`frame_queue_policy`), mas nada disso é comunicado ao
  cliente nem negociado;
- ⏳ nenhuma das extensões foi exercitada ponta a ponta: falta o lado Hestia
  correspondente para `packet_feedback` e `frame_report`.

## Fase H5 — ICE, convites e conexão direta automática

**Objetivo:** remover a necessidade comum de abrir portas manualmente.

Entregáveis:

- integração libjuice;
- candidatos host, IPv6 e server-reflexive;
- Trickle ICE;
- STUN configurável;
- PCP/NAT-PMP/UPnP opcional;
- convite por arquivo/texto/QR;
- descoberta LAN por mDNS;
- diagnóstico de NAT;
- suporte a VPN/overlay como candidato explícito.

Critérios:

- conexão direta automática na maioria dos NATs domésticos testados;
- nenhum serviço Hermes obrigatório;
- erro claro em CGNAT/NAT simétrico sem relay.

## Fase H6 — Identidade e pareamento Hermes

**Objetivo:** substituir dependência conceitual do pairing GameStream no modo nativo.

Entregáveis:

- identidade persistente do host;
- Noise XX no primeiro pareamento;
- fingerprint e QR;
- device authorization;
- revogação;
- capabilities e permissões;
- armazenamento seguro de chaves;
- protocolo de retomada autenticada.

Critérios:

- MITM detectável no primeiro pareamento;
- sessões futuras autenticadas sem PIN repetido;
- remoção de um cliente invalida futuras conexões.

## Fase H7 — HDT v1

**Objetivo:** introduzir o transporte nativo.

Escopo do HDT v1:

- um socket UDP;
- multiplexação de flows;
- números de pacote globais por direção;
- frame ID e packet index;
- timestamps e deadlines;
- epoch, unidade e wraparound definidos para cada campo temporal;
- `deadline_delta` convertível pelo Hestia em deadline no relógio monotônico
  local, sem assumir epoch compartilhado com o Hermes;
- controle confiável pequeno com ACK seletivo;
- mídia não confiável;
- AEAD em todos os pacotes;
- path MTU discovery seguro;
- keepalive;
- migração simples de endereço validada;
- compatibilidade com ICE.

Critérios:

- funcional em LAN e WAN;
- segurança revisada;
- zero plaintext de mídia;
- menor ou igual latência que o modo legado em condições equivalentes;
- atingir os alvos absolutos da seção "Alvos de qualidade" abaixo, medidos com
  a mesma matriz `hermes-netem.sh` usada no baseline H0.

"Menor ou igual ao legado" é um piso de não-regressão, não a meta. Um HDT que
apenas empata com o GameStream passa nesse critério sem entregar nada da
experiência pretendida. Os alvos absolutos existem para tornar essa diferença
verificável.

Progresso incremental:

- ✅ formato de wire v1 definido e testado (`src/hdt_wire.*`,
  `docs/hdt_wire_format.md`): header comum autenticado mas não cifrado —
  o receptor precisa ler epoch e número de pacote para derivar o nonce antes de
  decifrar qualquer coisa —, multiplexação de flows sobre um socket, frame ID e
  packet index, e tipos reservados para keepalive, path challenge/response e
  MTU probe;
- ✅ números de pacote globais por direção, não por flow: o contador alimenta o
  nonce AEAD e a janela de replay, e ambos precisam ser inequívocos sobre tudo
  que o peer manda. Só os 32 bits baixos vão no wire, reconstruídos pelo
  algoritmo do RFC 9000 A.3 — reimplementado em vez de aproximado porque todos
  os casos interessantes ficam na borda da janela;
- ✅ contrato temporal fechado: epoch, unidade e wraparound definidos por
  campo. Nada de timestamp absoluto — a leitura de relógio do emissor não é
  interpretável pelo receptor sem protocolo de sincronização, e todo uso de
  tempo aqui é duração. `deadline_delta` diz quanto o frame ainda vale a partir
  do envio, e o Hestia soma ao próprio instante de chegada. Erra pelo tempo de
  voo, na direção que descarta cedo em vez de exibir frame velho;
- ✅ janela deslizante de replay, sem a qual o AEAD não significa nada: um
  pacote capturado decifra corretamente para sempre. Pacote rejeitado não move
  a janela, senão prenderia os que estão legitimamente atrás dele;
- ✅ ACK seletivo com ranges descendentes e ack delay, para o emissor não
  cobrar do próprio RTT o tempo que o receptor passou agrupando;
- ✅ retransmissão dependente de deadline — a razão de existir do HDT. O teste
  é se a substituição ainda chega a tempo, não se algo se perdeu: um pacote
  cuja vida restante é menor que a estimativa de ida não serve por mais rápido
  que saia, e os bytes sairiam do frame seguinte, que ainda servia. Shard de
  paridade nunca é retransmitido: o emissor já sabe qual data shard faltou, e
  o data shard se reconstrói com certeza enquanto a paridade só ajuda se
  bastantes irmãs também chegarem;
- ✅ AEAD por pacote (`src/hdt_crypto.*`): ChaCha20-Poly1305 por padrão,
  AES-256-GCM quando os dois lados têm aceleração. Header como AAD, resto como
  ciphertext — não existe plaintext de mídia em ponto nenhum. Chave e IV por
  HKDF-SHA256 sobre um segredo de tráfego por direção, com rótulos distintos;
  nonce é o IV derivado XOR o número do pacote, o que só é sólido porque o
  número nunca se repete dentro de uma chave — e é a razão concreta de o
  contador ser por direção e não por flow, já que contadores por flow
  começariam todos em zero e repetiriam o nonce no primeiro pacote de cada um.
  Direções derivam de segredos diferentes e segredos iguais são recusados na
  construção, então pacote refletido no próprio emissor não autentica;
- ✅ rotação de chave: o segredo avança em mão única, o receptor aceita também
  o epoch anterior (senão todo rekey custaria uma rajada de frames perdidos), e
  um epoch que ele ainda não alcançou é recusado em vez de derivado sob demanda
  — o peer não pode empurrar o key schedule do receptor para frente;
- ⏳ falta o handshake Noise (`XX` no primeiro pareamento, `IK` depois) sobre
  X25519, que é quem produz os segredos de tráfego que a camada de proteção
  consome. Enquanto ele não existe, os segredos não têm origem, e é por isso
  que nada é alcançável por sessão;
- ⏳ falta tudo que toca a rede: socket UDP e implementação de `ITransport`,
  PMTU discovery, keepalive e migração de endereço validada. Os tipos de pacote
  já estão reservados, e o `ITransport` da H1 é o encaixe. É também onde acaba
  a propriedade de tudo ser decidível a partir dos bytes: daí em diante o teste
  honesto é de integração, não unitário;
- ⏳ nada disso está ligado a uma sessão, anunciado ou habilitável. Continua
  experimental e inalcançável, como a E0 exige.

## Alvos de qualidade

**Objetivo:** transformar "categoria Parsec" em números verificáveis, para que
a maturidade do HDT seja medida em vez de estimada.

Todos os valores são medidos com os perfis versionados de `hermes-netem.sh`
(`lan`, `wifi`, `wan`, `burst-loss`, `bufferbloat`, `reordering`), na ordem
descrita em `docs/netem_baseline.md`, a 1080p60 com encoder de hardware.

### Como usar esta tabela

Cada linha tem duas colunas de referência:

- **Piso**: o valor medido do GameStream atual na mesma configuração. Preenchido
  pela execução da aceitação H2, que ainda está pendente. Enquanto estiver
  vazio, os alvos permanecem hipóteses não calibradas.
- **Alvo**: onde o HDT precisa chegar para justificar sua existência.

| Métrica | Perfil | Piso (GameStream) | Alvo (HDT) |
|---|---|---|---|
| Glass-to-glass p50 | `lan` | a medir | ≤ 1 intervalo de frame acima do piso |
| Glass-to-glass p99 | `lan` | a medir | ≤ piso, sem regressão |
| Glass-to-glass p99 | `wifi` | a medir | ≤ 80% do piso |
| Glass-to-glass p99 | `wan` | a medir | ≤ 80% do piso |
| Tempo de recuperação após perda em rajada | `burst-loss` | a medir | ≤ 50% do piso |
| Frames congelados > 100 ms por minuto | `burst-loss` | a medir | ≤ 50% do piso |
| Crescimento de fila sob bufferbloat | `bufferbloat` | a medir | ≤ 50% do piso |
| Tempo até o primeiro frame | `lan` | a medir | ≤ piso |
| Taxa de IDR por minuto | `wifi` | a medir | ≤ piso |

Justificativa das assimetrias:

- em `lan` o GameStream já é competitivo; o objetivo é não regredir enquanto se
  ganha estrutura para os demais perfis;
- os ganhos reais devem aparecer em `wifi`, `wan` e `burst-loss`, que é onde
  deadlines, FEC adaptativo e retransmissão seletiva têm efeito;
- p99 e congelamentos importam mais que p50: a percepção de "responsivo" vem da
  cauda, não da média.

### Regras de medição

- mínimo de 60 s de amostra após 60 s de warm-up, por perfil;
- a mesma cena de aplicação em todas as execuções;
- reference e candidate na mesma sessão de bancada, mesmo hardware térmico;
- execução final sem netem para detectar deriva térmica ou de carga;
- qualquer alvo não atingido é registrado com o número medido, não descrito
  como "próximo o suficiente".

### Dependência

Esta tabela só fica utilizável depois que a aceitação empírica da H2 for
executada e os pisos forem preenchidos. Enquanto isso não acontecer, não há
base de comparação e qualquer afirmação sobre ganho do HDT é especulativa.

## Fase H8 — Congestion control avançado

**Objetivo:** aproximar a estabilidade do produto à categoria do Parsec.

Entregáveis:

- feedback de chegada por lote;
- estimador delay-based;
- estimador de perda;
- target de encoder e pacer separados;
- FEC adaptativo;
- retransmissão por deadline;
- frame drop consciente de referência;
- proteção contra IDR storms;
- detecção de route change;
- perfis LAN/WAN/Wi-Fi.

Critérios:

- baixa fila sob bufferbloat;
- recuperação rápida de redução de banda;
- qualidade volta gradualmente após melhora;
- p99 controlado em perda em rajada.

## Fase H9 — TURN autohospedado e rendezvous opcional

**Objetivo:** tornar os casos impossíveis de P2P resolvíveis sem centralização do projeto.

Entregáveis:

- suporte TURN RFC 8656;
- coturn Compose;
- credenciais temporárias;
- rendezvous mínimo autohospedado;
- deploy em Docker;
- documentação de segurança;
- seleção automática direct > VPN > relay;
- indicador de custo de banda do relay.

Critério:

- usuários conseguem implantar sua própria infraestrutura;
- Hermes e Hestia funcionam sem contato com domínio operado pelo projeto.

## Fase H10 — Migração de caminho e reconexão

**Objetivo:** sobreviver a mudanças de Wi-Fi, IP e interface.

Entregáveis:

- path challenge/response;
- validação de novo endereço;
- retomada de sessão curta;
- preservation de decoder quando seguro;
- fallback entre candidatos ICE;
- troca direct/relay;
- tratamento de suspend/resume.

Critério:

- troca de rede recupera a sessão sem novo pareamento;
- input nunca é aceito de caminho não autenticado.

### Degradação de HDT no meio da sessão

O `AI_DEVELOPMENT_GUIDE` define o fallback apenas antes do início da mídia:
negociação falha, limpa estado, tenta GameStream. Esse é o caso fácil. Com ICE
e migração de caminho, o caso frequente passa a ser a sessão degradar **depois**
de estabelecida, quando input, decoder e aplicação já estão inicializados — que
é exatamente onde o mesmo documento proíbe fallback silencioso.

Estados possíveis após a mídia começar:

```text
HDT_ATIVO
   ├── path degradado ──────────► HDT_REVALIDANDO ──► HDT_ATIVO
   │                                    │
   │                                    └─ falha ──► SESSÃO_ENCERRADA
   └── falha irrecuperável ─────────────────────────► SESSÃO_ENCERRADA
```

Regras:

- **não existe downgrade de HDT para GameStream com a mídia em andamento.**
  Trocar de protocolo exigiria reconstruir chaves, numeração de pacotes,
  cadeia de referência de vídeo e estado de input em conjunto; um erro nessa
  transição é pior que uma reconexão limpa;
- degradação de caminho é tratada dentro do HDT (path challenge, troca de
  candidato ICE, direct↔relay), nunca trocando de protocolo;
- quando a recuperação dentro do HDT falha, a sessão é **encerrada
  explicitamente** com causa reportada ao usuário; o cliente pode então
  reconectar, e a negociação da nova sessão volta a poder escolher GameStream;
- input é bloqueado assim que o caminho deixa de ser validado, antes de
  qualquer tentativa de recuperação;
- o decoder só é preservado se a numeração de frames continuar íntegra; caso
  contrário, exige-se IDR na retomada.

Critério:

- nenhuma troca de protocolo ocorre com mídia ativa;
- toda sessão encerrada por degradação reporta causa distinguível de
  desconexão do usuário;
- reconexão após encerramento não reutiliza estado criptográfico da sessão
  anterior.

## Fase H11 — Backend QUIC experimental

**Objetivo:** verificar por dados se QUIC melhora manutenção, segurança ou mobilidade.

Entregáveis:

- MsQuic em Windows/Linux;
- streams confiáveis para controle;
- QUIC Datagrams para mídia/input;
- benchmarks de CPU, cópias, startup, p95 e p99;
- comparação com HDT;
- decisão documentada de manter, limitar ou remover.

QUIC só vira padrão se superar HDT em métricas reais sem comprometer plataformas.

## Fase H12 — Produto 1.0

**Objetivo:** consolidar a plataforma.

Requisitos de lançamento:

- modo legado estável;
- modo Hermes nativo estável;
- pairing e convites claros;
- STUN configurável;
- relay autohospedado documentado;
- diagnósticos de conexão;
- atualização de protocolo versionada;
- matriz de compatibilidade;
- threat model publicado;
- fuzzing de parser e protocolo;
- benchmarks públicos reproduzíveis;
- rollback de configuração;
- documentação de contribuição.

## Maturidade do HDT e saída do GameStream

**Objetivo:** definir o que "maduro" significa, para que a despriorização do
caminho legado seja uma decisão com critério em vez de intuição.

O GameStream não é dívida técnica a ser removida assim que possível: é o que
mantém a compatibilidade com o Moonlight e com hosts Sunshine/Apollo. A questão
não é *se* ele sai, e sim *quando* deixa de ser o padrão — e essas são duas
decisões distintas.

### Estágios

#### E0 — Experimental (a partir de H7)

- HDT desativado por padrão, atrás de `enable_hdt`;
- README e UI descrevem como direção de longo prazo;
- nenhuma promessa de estabilidade entre versões.

#### E1 — Opt-in estável

- spec publicada com test vectors e revisão de segurança concluída;
- todos os alvos da seção "Alvos de qualidade" atingidos em `lan`, `wifi` e
  `wan`;
- ≥ 30 dias sem regressão nas caudas na matriz netem;
- fuzzing de parser sem falha conhecida em aberto;
- HDT pode ser ativado manualmente e é suportado.

#### E2 — Padrão para clientes compatíveis

- ≥ 90% das sessões HDT negociadas concluem sem fallback nem encerramento por
  degradação, medido em telemetria local agregada, não em bancada;
- taxa de encerramento por degradação (H10) ≤ taxa de desconexão observada no
  GameStream na mesma população;
- paridade de features: multi-sessão, clipboard, HDR, input e áudio com o
  mesmo comportamento do caminho legado;
- pelo menos duas plataformas de cliente em produção;
- GameStream continua disponível e testado em CI, apenas deixa de ser o
  primeiro candidato na negociação.

#### E3 — Legado em manutenção

- só é considerado após ≥ 2 releases estáveis em E2;
- GameStream permanece funcional e testado por tempo indeterminado, porque
  Moonlight não desaparece;
- deixa de receber otimizações; correções de segurança continuam;
- **não há E4**: remover o GameStream quebraria o Moonlight, e isso exigiria
  uma decisão de versão maior tomada separadamente deste roadmap.

### Regras

- a passagem entre estágios é explícita e registrada no CHANGELOG, nunca
  consequência lateral de outro trabalho;
- qualquer estágio pode regredir se seu critério deixar de valer;
- enquanto os pisos da tabela de alvos não forem medidos, o projeto está em E0
  por definição, independentemente do estado do código.

## 6. Contrato de desenvolvimento conjunto com o Hestia

Cada capability precisa ser definida antes de implementar host e cliente separadamente.

Estrutura sugerida no repositório de especificações:

```text
hermes-protocol/
├── specs/
│   ├── 0001-capability-negotiation.md
│   ├── 0002-packet-header.md
│   ├── 0003-feedback.md
│   ├── 0004-pairing-noise.md
│   ├── 0005-ice-signalling.md
│   └── 0006-session-resumption.md
├── schemas/
├── test-vectors/
├── pcaps/
└── conformance/
```

Toda mudança deve incluir:

- número de versão;
- feature flag;
- comportamento de fallback;
- test vectors;
- limites de tamanho;
- regras de timeout;
- implicações de segurança;
- implementação de referência ou teste de conformidade.

## 7. Priorização prática

A sequência de maior retorno é:

1. instrumentação;
2. modularização;
3. pacing;
4. controle de filas;
5. bitrate adaptativo;
6. extensões de feedback Hermes;
7. ICE e convites;
8. identidade Noise;
9. HDT;
10. FEC/retransmissão avançados;
11. relay autohospedado;
12. migração de caminho;
13. QUIC apenas como experimento comparativo.

Não comece simultaneamente por novo protocolo, nova interface, relay e reescrita do encoder. Isso criaria quatro projetos incompletos. O caminho correto é entregar ganhos mensuráveis em cada fase e manter o modo legado sempre utilizável.

## 8. Métricas de sucesso do produto

Metas devem ser ajustadas após o baseline, mas o projeto precisa acompanhar:

- tempo de descoberta até primeira imagem;
- sucesso de conexão direta por tipo de NAT;
- tempo de reconexão;
- capture-to-present p50/p95/p99;
- p95 de fila de rede estimada;
- tempo para reagir a queda de banda;
- tempo para recuperar qualidade;
- percentual de frames recuperados por FEC;
- percentual de retransmissões úteis;
- desperdício de wire bitrate;
- consumo de CPU por 1080p60, 1440p120 e 4K60;
- estabilidade de frame pacing;
- número de sessões que exigem relay;
- regressões por plataforma e encoder.

## 9. Riscos principais

- tentar imitar o Parsec sem medir a pilha atual;
- criar criptografia própria em vez de compor primitivas e protocolos revisados;
- permitir que a fila cresça para evitar frame drop;
- controlar encoder rápido demais e gerar oscilação;
- tratar FEC como solução universal para perda;
- depender de STUN público hardcoded;
- prometer P2P universal sem relay;
- quebrar Moonlight cedo demais;
- acoplar Hermes-KMS ao protocolo de forma que outros backends deixem de funcionar;
- implementar QUIC por moda, sem benchmark;
- deixar parsers binários sem fuzzing.

## 10. Referências técnicas

- Sunshine: https://github.com/LizardByte/Sunshine
- Moonlight common core: https://github.com/moonlight-stream/moonlight-common-c
- RFC 8445 — ICE: https://www.rfc-editor.org/rfc/rfc8445
- RFC 8489 — STUN: https://www.rfc-editor.org/rfc/rfc8489
- RFC 8656 — TURN: https://www.rfc-editor.org/rfc/rfc8656
- libjuice: https://github.com/paullouisageneau/libjuice
- Noise Protocol Framework: https://noiseprotocol.org/noise.html
- RFC 8888 — RTCP feedback for congestion control: https://www.rfc-editor.org/rfc/rfc8888
- RFC 9392 — taxa de feedback RTCP para congestion control: https://www.rfc-editor.org/rfc/rfc9392
- RFC 9221 — QUIC Datagrams: https://www.rfc-editor.org/rfc/rfc9221
- MsQuic: https://github.com/microsoft/msquic
- RFC 9000 — QUIC: https://www.rfc-editor.org/rfc/rfc9000
- RFC 9002 — QUIC loss detection and congestion control: https://www.rfc-editor.org/rfc/rfc9002
- SQP: https://arxiv.org/abs/2207.11857
