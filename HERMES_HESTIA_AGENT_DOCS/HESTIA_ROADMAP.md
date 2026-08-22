# Hestia — Roadmap do cliente para a plataforma Hermes descentralizada

> Documento de arquitetura e evolução do Hestia, partindo do fork atual de Moonlight até um cliente nativo Hermes com baixa latência, conexão peer-to-peer automática, segurança end-to-end e adaptação conjunta com o host.

## 1. Visão do produto

O Hestia deve continuar sendo um cliente competente para hosts GameStream/Sunshine/Apollo/Hermes, mas evoluir para o cliente de referência do protocolo Hermes.

Objetivos:

- preservar compatibilidade com hosts existentes;
- suportar recursos Hermes por capability negotiation;
- medir chegada, reconstrução, decode e apresentação de cada frame;
- fornecer feedback de rede de alta frequência;
- controlar jitter sem acumular latência;
- tentar conexão direta automaticamente;
- aceitar STUN e TURN autohospedados;
- funcionar sem conta, cloud ou servidor do projeto;
- armazenar identidades e permissões localmente;
- recuperar sessões após troca de rede;
- oferecer uma experiência simples apesar da arquitetura descentralizada.

## 2. Papéis do Hestia

O Hestia não será apenas um decoder. Ele terá cinco responsabilidades críticas:

1. **Conectividade:** descoberta, ICE, seleção de caminho e reconexão.
2. **Segurança:** pareamento, verificação da identidade do host e chaves de sessão.
3. **Recepção:** demultiplexação, decriptação, reordering, FEC e NACK.
4. **Apresentação:** decode, frame scheduling, áudio e renderização.
5. **Feedback:** informar ao Hermes o estado real da rede e do pipeline do cliente.

O ganho semelhante ao Parsec depende de o cliente reportar não apenas perda, mas o que realmente aconteceu com o frame.

## 3. Plataformas e estratégia de código

O núcleo de protocolo deve permanecer compartilhado em C/C++ e separado da interface de cada plataforma.

Estrutura sugerida:

```text
hestia/
├── core/
│   ├── legacy/
│   ├── hermes_protocol/
│   ├── transport/
│   ├── connectivity/
│   ├── crypto/
│   ├── media/
│   ├── input/
│   └── telemetry/
├── platform/
│   ├── android/
│   ├── linux/
│   ├── windows/
│   ├── macos/
│   └── ios/
└── ui/
```

Se o fork atual for baseado em uma implementação específica de Moonlight, a modularização deve respeitar o ciclo de vida já existente em vez de tentar unificar todas as plataformas de uma vez.

Prioridade recomendada:

1. plataforma atualmente mantida pelo projeto;
2. Linux desktop;
3. Android;
4. Windows;
5. demais plataformas conforme contribuidores e CI disponíveis.

Não declare suporte oficial a uma plataforma sem decoder de hardware, input e testes automatizados mínimos.

## 4. Compatibilidade e negociação

O Hestia deve detectar três classes de host:

```text
NVIDIA GameStream legado
Sunshine/Apollo/Hermes em protocolo legado
Hermes com extensões ou protocolo nativo
```

Capability negotiation sugerida:

```text
client_name = Hestia
legacy_protocol = supported
hermes_extensions = [feedback_v1, frame_ack_v1, ice_v1]
hermes_native = [hdt_v1]
codecs = [...]
crypto = [...]
input = [...]
```

Regras:

- nunca assumir suporte pelo nome do host;
- features independentes, não um único booleano “Hermes enabled”;
- versões major incompatíveis devem falhar claramente;
- versões minor devem ignorar campos desconhecidos quando especificado;
- fallback para GameStream deve permanecer possível;
- a interface deve mostrar o modo de conexão usado.

## 5. Conectividade descentralizada

## 5.1 Descoberta LAN

Use:

- mDNS/DNS-SD;
- descoberta legado Moonlight;
- cache local de hosts conhecidos;
- endereços IPv4 e IPv6;
- tentativa paralela controlada, no estilo Happy Eyeballs;
- validação criptográfica antes de confiar no nome anunciado.

Um anúncio mDNS só informa que existe um host. Ele não substitui a identidade criptográfica.

## 5.2 Hosts remotos

Formas de adicionar um host:

- IP ou hostname manual;
- link de convite;
- QR code;
- arquivo de convite;
- código copiado por outro canal;
- rendezvous autohospedado opcional;
- host acessível por VPN/overlay.

O convite deve conter apenas o necessário:

```text
protocol version
host public identity/fingerprint
candidate or rendezvous information
short-lived pairing token
optional display name
expiration
signature
```

Não coloque chave privada, senha persistente ou credenciais TURN permanentes em QR code.

## 5.3 ICE e libjuice

Use `libjuice` no core para manter comportamento consistente entre plataformas quando possível.

Fluxo:

1. coletar candidatos locais;
2. obter candidatos server-reflexive via STUN configurado;
3. aceitar candidatos enviados pelo host;
4. executar checks em paralelo;
5. selecionar o melhor caminho validado;
6. manter consent freshness/keepalive;
7. trocar para candidato alternativo quando o caminho falhar;
8. usar TURN apenas se configurado e necessário.

A tela de diagnóstico deve mostrar:

```text
Direct LAN
Direct IPv6
Direct NAT traversal
VPN/overlay
Relay TURN
No viable path
```

## 5.4 Sem relay operado pelo projeto

O Hestia não deve assumir endpoints oficiais Hermes.

Configuração:

- lista de STUN do usuário;
- lista de TURN do usuário;
- suporte a QR/config profile para distribuir endpoints;
- credenciais TURN temporárias;
- nenhum endpoint hardcoded além de exemplos desativados;
- conexão por Tailscale/Headscale/ZeroTier/Nebula tratada como rota normal.

Quando o P2P falhar, a mensagem correta é algo como:

> Não foi possível criar um caminho direto. Este host parece estar atrás de uma rede que exige VPN ou relay. Configure um servidor TURN, use uma VPN mesh ou conecte por uma rede com IPv6/port mapping.

## 6. Segurança e pareamento

## 6.1 Identidade local

O Hestia gera uma identidade por instalação ou por perfil:

- chave X25519 para Noise;
- Ed25519 opcional para assinar solicitações e dispositivos;
- chave armazenada no Android Keystore, Apple Keychain/Secure Enclave quando aplicável, Windows Credential/DPAPI ou keyring do desktop;
- fallback criptografado com senha apenas quando armazenamento seguro não existir.

## 6.2 Primeiro pareamento

Use Noise XX:

1. Hestia e Hermes trocam chaves efêmeras;
2. exibem fingerprint ou código curto vinculado ao transcript;
3. o usuário confirma no host ou compara o código;
4. ambos persistem a identidade estática autorizada;
5. permissões são atribuídas ao dispositivo.

Após o pareamento:

- reconexão autenticada;
- forward secrecy por chaves efêmeras;
- rotação de session keys;
- revogação pelo Hermes;
- aviso no Hestia quando a identidade do host mudar;
- nunca aceitar silenciosamente uma nova chave para um host conhecido.

## 6.3 Sessão nativa

Todo pacote HDT deve ser autenticado e criptografado. O cliente precisa:

- rejeitar replay;
- limitar memória antes de autenticação;
- validar tamanho antes de alocar;
- ignorar flows desconhecidos;
- rate-limit de handshakes;
- não processar input reverso ou clipboard sem permissão negociada;
- limpar chaves e buffers sensíveis no encerramento quando praticável.

## 7. Pipeline de recepção nativo

```text
UDP socket
   ↓
path/session validation
   ↓
AEAD decrypt
   ↓
demultiplexer
   ├── control
   ├── input acknowledgements
   ├── audio
   └── video
          ↓
   packet reorder window
          ↓
   FEC reconstruction
          ↓
   NACK decision by deadline
          ↓
   frame assembler
          ↓
   decoder queue
          ↓
   renderer/presentation
```

Cada etapa deve produzir métricas e possuir limites rígidos de memória.

## 8. Header e estado de pacotes

O Hestia deverá compreender, no mínimo:

- session ID;
- flow ID;
- packet sequence global;
- frame ID;
- packet index e count;
- send timestamp;
- presentation deadline;
- flags de keyframe/referência/FEC/retransmissão;
- epoch de chave;
- authentication tag.

Janela de reordering:

- limitada por quantidade e tempo;
- independente para áudio e vídeo quando necessário;
- detecta duplicatas;
- não espera pacote que já perdeu o deadline;
- reporta reorder sem classificá-lo imediatamente como perda permanente.

## 9. Feedback do cliente

O Hestia é a fonte da verdade sobre chegada, decode e apresentação.

## 9.1 Feedback de transporte

Enviar lotes a cada aproximadamente 20–50 ms, adaptando a frequência à taxa de pacotes e overhead.

Conteúdo:

```cpp
struct PacketArrival {
    uint64_t sequence;
    int32_t arrival_delta_us;
    PacketState state;
};

struct TransportFeedback {
    uint64_t feedback_sequence;
    uint64_t reference_time_us;
    uint32_t path_id;
    std::vector<PacketArrival> packets;
};
```

Estados possíveis:

```text
received
missing
late
recovered_by_fec
duplicate
```

O feedback deve ser compacto, usando deltas e bitmaps.

## 9.2 Feedback por frame

Para cada frame relevante:

- primeiro pacote recebido;
- frame completo;
- recuperado por FEC;
- perdido;
- enviado ao decoder;
- decode concluído;
- apresentado;
- descartado e motivo;
- atraso de fila do decoder;
- atraso de apresentação.

Isso permite ao Hermes diferenciar:

```text
rede congestionada
decoder lento
renderer lento
frame corrompido
cliente deliberadamente descartando frame atrasado
```

## 9.3 Sincronização de relógios

Não trate relógios dos dois dispositivos como iguais.

Implementar:

- ping/pong com quatro timestamps;
- estimativa de offset e RTT;
- filtro robusto usando amostras de menor RTT;
- detecção de salto de relógio;
- uso de monotonic clock;
- reporte de one-way delay apenas quando a confiança for suficiente.

Quando a confiança for baixa, enviar deltas de chegada e RTT sem afirmar latência absoluta.

## 10. Jitter buffer e política de latência

O Hestia não deve usar um jitter buffer tradicional grande como um player de vídeo. Streaming interativo precisa aceitar alguma perda visual para não acumular atraso.

Política recomendada:

- alvo baixo e dinâmico;
- pequena margem em LAN;
- margem adaptada a jitter real em WAN;
- teto configurável;
- descarte de frames vencidos;
- evitar apresentar frames fora de ordem;
- solicitar recovery quando referência necessária foi perdida;
- não pausar o vídeo esperando indefinidamente um pacote.

Perfis:

```text
Competitive: menor buffer, mais drops tolerados
Balanced: buffer moderado
Quality: maior tolerância a jitter, dentro de teto seguro
```

## 11. FEC, NACK e recuperação

O Hestia deve decidir rapidamente entre:

- esperar brevemente por reordering;
- reconstruir via FEC;
- pedir retransmissão;
- abandonar o frame;
- solicitar recovery frame.

Regra conceitual:

```text
se pacote pode chegar/retransmitir antes do deadline → aguardar ou NACK
se FEC já permite reconstruir → reconstruir
se deadline expirou → abandonar
se perda quebra cadeia de referência → informar necessidade de recovery
```

NACK:

- bitmap por frame;
- envio imediato quando o deadline permitir;
- supressão de NACK duplicado;
- cancelamento quando pacote original ou FEC chegar;
- nunca pedir pacote que já não pode ser exibido.

FEC:

- reconstrução em worker separado ou pool controlado;
- prioridade para frames de referência;
- limite de CPU;
- métricas de tempo de reconstrução;
- fallback se o dispositivo estiver termicamente limitado.

## 12. Decode e renderização

## 12.1 Decoder

Use decoder de hardware por plataforma:

- Android: MediaCodec;
- Linux: VA-API/Vulkan Video quando maduro e apropriado;
- Windows: D3D11/D3D12 Video ou Media Foundation conforme base existente;
- macOS/iOS: VideoToolbox;
- software decode apenas como fallback explícito.

Requisitos:

- fila mínima;
- low-latency mode quando disponível;
- surface/texture zero-copy até o renderer;
- reporte de frames enfileirados;
- detecção de decoder stall;
- reset controlado após corrupção;
- não reinicializar decoder por qualquer perda pequena.

## 12.2 Renderização

Metas:

- apresentar no primeiro vsync útil;
- evitar fila tripla invisível;
- permitir tearing opcional em modo competitivo quando a plataforma suportar;
- VRR quando disponível;
- frame pacing mensurável;
- HDR e tone mapping negociados;
- overlay de estatísticas sem cópia completa do frame.

No Linux, avaliar caminhos Vulkan/DMABUF; no Android, Surface/ANativeWindow; no Windows, swap chain de baixa latência.

## 12.3 Frame scheduler

O scheduler deve considerar:

- deadline do host;
- relógio sincronizado;
- duração do decode;
- próximo vsync;
- jitter atual;
- idade do frame;
- preferência do usuário.

Se um frame novo estiver pronto e o anterior não puder mais ser apresentado utilmente, descarte o anterior.

## 13. Áudio

No protocolo nativo:

- Opus;
- pacotes de 5 ou 10 ms quando a plataforma permitir;
- buffer independente do vídeo;
- PLC;
- in-band FEC opcional;
- resampling pequeno para corrigir drift;
- evitar sincronização que acrescente dezenas de milissegundos;
- seleção de dispositivo;
- surround como capability futura.

Métricas:

- jitter;
- underruns;
- buffer atual;
- packets concealed;
- drift correction;
- audio presentation delay.

## 14. Input

O Hestia deve produzir eventos com:

- sequência;
- timestamp monotônico;
- device ID;
- tipo;
- estado;
- flag de evento substituível.

Exemplos:

- mouse motion pode ser coalescido;
- scroll pode ser acumulado por janela curta;
- key down/up não pode ser descartado silenciosamente;
- estado completo de gamepad deve ser reenviado periodicamente;
- touch deve preservar IDs de contato;
- sensores devem ter rate limit configurável.

O cliente deve fornecer modo de segurança para bloquear combinações perigosas e respeitar permissões dadas pelo host.

## 15. Interface do produto

A arquitetura é complexa, mas a UI deve ser simples.

Tela de host:

```text
Nome do host
Online / offline
Direct / VPN / relay
Latency estimate
Codec e resolução
Connect
```

Tela de diagnóstico:

```text
Discovery: OK
Host identity: verified
Candidate gathering: OK
Direct IPv6: failed
Direct UDP: selected
Relay: not used
RTT: ...
Loss: ...
Decoder: hardware
```

Configurações avançadas:

- STUN/TURN;
- bitrate máximo;
- resolução/FPS;
- perfil de latência;
- codec;
- HDR;
- input permissions;
- network interface;
- logs e exportação de diagnóstico.

Nunca exponha detalhes de ICE na tela principal, mas não esconda o motivo de uma falha.

## 16. Atualização e compatibilidade de protocolo

O Hestia deve manter uma tabela de features negociadas, não depender apenas da versão do aplicativo.

Exemplo:

```cpp
struct HermesCapabilities {
    Version protocol;
    bool packet_feedback_v1;
    bool frame_feedback_v1;
    bool deadline_nack_v1;
    bool adaptive_fec_v1;
    bool path_migration_v1;
    bool opus_audio_v1;
};
```

Regras:

- unknown fields ignorados quando seguro;
- limites de tamanho definidos;
- downgrade protegido pelo transcript criptográfico;
- fallback legado nunca automático após erro de autenticação;
- mensagem clara quando host e cliente são incompatíveis.

## 17. Roadmap por fases

## Fase C0 — Baseline do cliente atual

**Objetivo:** medir o Hestia antes de alterá-lo.

Entregáveis:

- timestamps de receive, assemble, decode e present;
- métricas de filas;
- logs estruturados;
- cenário de teste com stream gravado/reproduzível;
- comparação de decoder por plataforma;
- p50/p95/p99 de decode e apresentação;
- medição de input-to-send.

Critério:

- cada frame pode ser rastreado até a apresentação ou descarte.

## Fase C1 — Modularização

**Objetivo:** separar Moonlight legado do futuro protocolo.

Entregáveis:

- `IHostProtocol`;
- `IClientTransport`;
- `IConnectivityAgent`;
- `IVideoReceiver`;
- `IAudioReceiver`;
- `IDecoder`;
- `IRenderScheduler`;
- `IInputSender`;
- `ISessionTelemetry`;
- adapters para o fluxo legado.

Critério:

- o modo Moonlight continua funcional;
- protocolo e UI não dependem diretamente um do outro.

Progresso incremental:

- ✅ `IHostProtocol` tipado e injetável cobre `prepare`, `launch` e `stop`;
- ✅ `GameStreamHostProtocol` preserva o fluxo legado por meio de `NvHTTP`;
- ✅ nomes e estrutura do payload Hermes ficam no adapter, fora de `Session`;
- ✅ `IClientTransport` tipado e injetável cobre `start`, `interrupt` e `stop`;
- ✅ `GameStreamClientTransport` confina callbacks e chamadas `Li*` ao adapter;
- ✅ `IConnectivityAgent` tipado e injetável fornece endereço e classificação
  de caminho sem expor `NvComputer` à sessão ou ao transporte;
- ✅ `GameStreamConnectivityAgent` preserva a detecção LAN/VPN legada e mantém
  uma seleção estável compartilhada pela política de MTU e pelo transporte;
- ✅ `IVideoReceiver` tipado e injetável descreve modo e capacidades do
  receptor sem expor a tabela de callbacks do Moonlight;
- ✅ `GameStreamVideoReceiver` confina `DECODER_RENDERER_CALLBACKS`, setup do
  formato, seleção push/pull e submissão sincronizada de `DECODE_UNIT`;
- ✅ `Session` não possui mais trampolins estáticos de recepção de vídeo;
- ✅ `IAudioReceiver` tipado e injetável cobre configuração de canais,
  capacidades, teste do dispositivo e mute;
- ✅ `GameStreamAudioReceiver` confina `AUDIO_RENDERER_CALLBACKS`, decoder
  Opus, renderer, recuperação do dispositivo e janela de descarte pós-reinit;
- ✅ estado e trampolins de áudio foram removidos de `Session`;
- ✅ `IDecoder` tipado expõe capacidades, cores, HDR, resolução, apresentação
  no thread principal e mudanças de janela sem estruturas do Limelight;
- ✅ `LegacyVideoDecoderAdapter` preserva FFmpeg/SLVideo e confina a submissão
  de `PDECODE_UNIT` ao caminho `GameStreamVideoReceiver`;
- ✅ `Session` depende do contrato `IDecoder` para probing, configuração e
  recriação do decoder;
- ✅ `IRenderScheduler` tipado recebe frames move-only sem cópia, timing da
  origem, deadline no relógio monotônico local, condições de rede e estado da
  apresentação sem expor `AVFrame`, `Pacer` ou SDL;
- ✅ `LegacyPacerAdapter` preserva filas adaptativas, VSync por plataforma,
  renderização no thread correto, telemetria e descarte do `Pacer` existente;
- ✅ o caminho GameStream preenche a timeline relativa da origem; o futuro
  receiver HDT deverá mapear `deadline_delta` para `presentationDeadlineUs`;
- ✅ `IInputSender` tipado e injetável cobre teclado/texto, mouse/scroll,
  touch, caneta, estado/chegada de controles, touchpad, sensores e bateria;
- ✅ todo evento nasce com sequência global da sessão, timestamp monotônico em
  microssegundos, device ID e flag explícita de substituição;
- ✅ `GameStreamInputSender` confina chamadas `LiSend*`, feature flags e
  conversão dos enums/máscaras legados ao adapter;
- ✅ handlers SDL, gestos e timers de emulação não enviam mais input
  diretamente pelo Limelight;
- ✅ `ISessionTelemetry` tipado e injetável recebe estágios, falhas, qualidade
  da conexão, término, janelas do pipeline, resumo final e traces por frame;
- ✅ FFmpeg e `Pacer` publicam snapshots/traces pelo contrato e não formatam
  mais o overlay nem gravam traces diretamente;
- ✅ `LegacySessionTelemetry` preserva overlay, diagnóstico, histórico de
  spikes, logs estruturados e resumo global do modo GameStream;
- ✅ o adapter nulo mantém probing de decoder sem efeitos colaterais;
- ✅ contratos fake e serialização do payload possuem testes unitários;
- ✅ a fase C1 possui agora os nove contratos previstos, todos injetáveis e com
  adapters para o fluxo legado.

## Fase C2 — Pipeline de baixa latência no modo legado

**Objetivo:** melhorar o cliente sem exigir mudanças no Hermes.

Entregáveis:

- redução de filas internas;
- decoder low-latency;
- frame dropping consistente;
- melhor pacing de apresentação;
- áudio com buffer menor;
- overlay de métricas;
- configuração competitive/balanced/quality.

Critério:

- latência menor ou igual ao baseline sem aumento grave de stutter.

Progresso incremental:

- ✅ o receiver GameStream publica janelas bounded de um segundo e resumo final
  de áudio pelo mesmo `ISessionTelemetry` usado pelo pipeline de vídeo;
- ✅ SDL mede fila de playback, buffer do dispositivo, high-water mark, espera
  por backpressure, falhas de fila e underruns sem mudar os limites legados;
- ✅ SLAudio expõe submissões e descartes por backpressure e marca
  explicitamente que a profundidade da fila interna não é observável;
- ✅ decode Opus/PLC, perdas e FEC do RTP, mute, janela de recuperação, falhas e
  reinicializações do renderer alimentam o overlay e o diagnóstico traduzido;
- ✅ o delay local estimado soma as filas conhecidas e o buffer do dispositivo;
- ✅ uma política tipada e testada substitui as constantes ocultas do SDL e do
  SLAudio; `Default` preserva os valores legados, enquanto `Low latency` e
  `Smooth playback` são escolhas persistidas e explicitamente opt-in;
- ✅ o SDL não inicia mais consumindo somente o primeiro pacote de 5 ms:
  acumula uma reserva inicial bounded de 5/10/20 ms conforme o perfil. Se a
  reserva realmente esgotar, pausa brevemente, recompõe e eleva o alvo em
  passos de 5–10 ms até os tetos de 20/30/50 ms;
- ✅ a detecção de underrun usa duração de áudio submetida menos tempo
  monotônico de reprodução, com a fila SDL apenas como lower bound. Uma fila
  SDL vazia deixou de ser tratada isoladamente como prova de underrun, pois o
  dispositivo pode já possuir um buffer interno não observável;
- ✅ o perfil efetivo e seus limites entram na telemetria local para comparar
  latência, backpressure e underruns entre execuções;
- ⚠️ o callback de áudio GameStream não fornece timestamp de apresentação.
  Portanto, o offset A/V absoluto permanece marcado como indisponível, sem
  comparar clocks que não compartilham epoch;
- ⏳ offset A/V por dispositivo e robustez a stalls/troca de dispositivo
  continuam pendentes.

Próximo incremento recomendado:

- implementar o offset manual de áudio com zero como default, persistência
  local e telemetria do atraso efetivamente aplicado, sem apresentá-lo como
  medição automática de drift A/V.

## Fase C3 — Extensões de feedback Hermes

**Objetivo:** dar ao host informações suficientes para adaptação real.

Entregáveis:

- packet arrival feedback;
- ACK/NACK por frame;
- estado FEC;
- decode report;
- presentation report;
- sincronização de relógios;
- compressão de feedback;
- rate limiting.

Critério:

- overhead baixo e host capaz de separar perda de atraso de decode.

## Fase C4 — FEC/NACK orientados a deadline

**Objetivo:** recuperar apenas o que ainda tem valor.

Entregáveis:

- reorder window temporal;
- FEC worker;
- NACK bitmap;
- deadline decision;
- keyframe/recovery report;
- testes com perda aleatória e em rajada;
- limites de CPU e memória.

Critério:

- menos frames corrompidos sem aumento significativo de latência.

## Fase C5 — ICE e convites

**Objetivo:** conectar remotamente sem port forwarding manual na maioria dos casos.

Entregáveis:

- libjuice;
- mDNS;
- convite/QR;
- Trickle ICE;
- STUN configurável;
- suporte a VPN/overlay;
- diagnóstico de NAT;
- cache seguro de candidatos conhecidos;
- TURN opcional.

Critério:

- conexão direta automática nos cenários de NAT suportados;
- nenhuma dependência de serviço Hermes.

## Fase C6 — Identidade Hestia e Noise

**Objetivo:** implementar pareamento nativo.

Entregáveis:

- geração e armazenamento de chaves;
- Noise XX;
- comparação de fingerprint/código;
- host trust store;
- aviso de host key change;
- permissões do dispositivo;
- revogação refletida na UI;
- resume token seguro.

Critério:

- nenhuma sessão nativa sem host autenticado;
- mudança de identidade nunca aceita silenciosamente.

## Fase C7 — HDT v1 receiver

**Objetivo:** receber o protocolo Hermes nativo.

Entregáveis:

- socket UDP multiplexado;
- decriptação AEAD;
- anti-replay;
- demux de flows;
- control channel confiável leve;
- audio/video receiver;
- feedback;
- MTU handling;
- keepalive;
- fallback legado selecionável.

Critério:

- 1080p60 estável em LAN;
- nenhum plaintext de mídia;
- perda/reordering não causa crescimento de memória;
- contribuir com as métricas terminais dos alvos absolutos definidos em
  "Alvos de qualidade" no `HERMES_ROADMAP`.

### Papel do Hestia na medição dos alvos

Os alvos de qualidade do HDT são glass-to-glass: o Hermes publica janelas de
envio, mas só o Hestia observa o fim da cadeia. Sem o lado cliente, os alvos
não são verificáveis.

O Hestia precisa reportar, por sessão:

- reassembly, decode, pacer e render em p50/p95/p99;
- tempo até o primeiro frame apresentado;
- frames congelados acima de 100 ms, contados por minuto;
- tempo entre a última perda e o retorno à cadência nominal;
- profundidade de pico da fila de reordering/FEC.

Essas séries são pareadas com as janelas do Hermes por sequência monotônica,
como já faz o harness da aceitação H2 (`scripts/hestia-h2-test.sh` no
repositório do Hestia, pareado com `scripts/hermes-h2-test.sh` no do Hermes).
O mesmo mecanismo serve ao H7 sem mudança de formato.

Isso permanece observabilidade local: nenhum desses campos vira feedback de
transporte sem passar pela negociação de capability descrita no contrato de
telemetria.

### Fallback do lado do cliente

O `HERMES_ROADMAP` define que não existe downgrade de HDT para GameStream com
a mídia em andamento. O Hestia implementa o lado correspondente:

- escolher HDT ou GameStream **antes** de inicializar decoder e input;
- ao receber encerramento por degradação, destruir o estado da sessão antes de
  qualquer nova tentativa;
- a reconexão é uma sessão nova e renegocia do zero, podendo escolher
  GameStream;
- nunca reaproveitar chaves, numeração ou estado de decoder entre sessões;
- distinguir na UI encerramento por degradação de desconexão do usuário.

## Fase C8 — Decoder e renderer otimizados por plataforma

**Objetivo:** remover filas que a rede não consegue corrigir.

Entregáveis:

- zero-copy por plataforma;
- low-latency decode;
- scheduler de vsync;
- VRR quando possível;
- opção tearing/competitive;
- HDR capability;
- thermal monitoring em mobile;
- decoder fallback controlado.

Critério:

- decode-to-present previsível;
- sem fila oculta de múltiplos frames.

## Fase C9 — Opus e áudio nativo

**Objetivo:** substituir o áudio legado no modo Hermes.

Entregáveis:

- Opus decode;
- PLC/FEC;
- buffer adaptativo curto;
- drift correction;
- device switching;
- feedback de underrun;
- sincronização sem aumentar artificialmente a latência de vídeo.

## Fase C10 — Input nativo completo

**Objetivo:** input responsivo e robusto.

Entregáveis:

- teclado/mouse/gamepad;
- touch e sensores onde aplicável;
- state refresh;
- coalescing seguro;
- timestamps;
- rumble/feedback;
- permissões;
- clipboard opcional e isolado.

## Fase C11 — Relay autohospedado e rendezvous

**Objetivo:** cobrir redes onde P2P é impossível.

Entregáveis:

- TURN RFC 8656;
- perfis de servidor;
- credenciais temporárias;
- importação por QR/config;
- indicador visual de relay;
- suporte a rendezvous autohospedado;
- nenhuma conta central.

## Fase C12 — Migração de caminho

**Objetivo:** manter sessão durante mudança de rede.

Entregáveis:

- path validation;
- troca de candidato ICE;
- resumption curto;
- preservação segura do decoder;
- fallback direct/relay;
- tratamento Android doze, mobile handoff e suspend/resume.

Critério:

- troca Wi-Fi/dados recupera sem novo pareamento quando a plataforma permitir.

## Fase C13 — Backend QUIC experimental

**Objetivo:** testar interoperabilidade com o backend experimental do Hermes.

Entregáveis:

- QUIC streams para controle;
- QUIC Datagrams para mídia;
- benchmarks por plataforma;
- avaliação de MsQuic onde suportado e outra implementação quando necessário;
- comparação objetiva com HDT.

Não torne QUIC padrão apenas por reduzir código próprio. A métrica principal continua sendo latência e estabilidade reais.

## Fase C14 — Produto 1.0

Requisitos:

- hosts legados continuam utilizáveis;
- Hermes nativo estável;
- pareamento simples;
- conexão remota sem cloud obrigatória;
- diagnóstico compreensível;
- relay autohospedado documentado;
- métricas e exportação de logs;
- acessibilidade básica;
- threat model;
- fuzzing;
- CI por plataforma suportada;
- matriz host/cliente/protocolo.

## 18. Testes

## 18.1 Testes de protocolo

- golden packets;
- test vectors de Noise;
- corrupção de header;
- truncamento;
- duplicação;
- replay;
- reordering extremo;
- perda de fragmentos;
- epoch inválido;
- flow desconhecido;
- frame ID wraparound;
- MTU menor;
- feedback malformed;
- reconnect durante key rotation.

Use libFuzzer/AFL++ nos parsers nativos.

## 18.2 Testes de rede

Cenários:

- LAN Ethernet;
- Wi-Fi estável;
- Wi-Fi com interferência;
- 4G/5G;
- RTT alto;
- jitter;
- perda em rajada;
- bandwidth step-down e step-up;
- bufferbloat;
- packet duplication;
- packet reordering;
- NAT comum;
- NAT simétrico;
- IPv6;
- relay;
- mudança de interface.

## 18.3 Testes de mídia

- H.264/HEVC/AV1;
- 720p60, 1080p60, 1440p120 e 4K60 conforme hardware;
- keyframe loss;
- resolution change;
- HDR/SDR;
- decoder reset;
- audio underrun;
- A/V drift;
- thermal throttling em Android;
- app background/foreground.

## 19. Métricas de sucesso

- tempo até primeira imagem;
- taxa de sucesso de conexão direta;
- reconexão após path failure;
- network receive-to-present p50/p95/p99;
- tempo médio de decode;
- fila máxima do decoder;
- frame pacing;
- input enqueue-to-wire;
- feedback overhead;
- FEC recovery success;
- retransmissões úteis;
- frames descartados por deadline;
- crashes por hora de stream;
- uso de memória sob perda maliciosa;
- consumo de bateria e temperatura em mobile.

## 20. Dependências recomendadas

- `moonlight-common-c` ou código legado equivalente enquanto necessário;
- `libjuice` para ICE/STUN/TURN;
- implementação Noise auditável, preferencialmente `noise-c` no core C/C++ após revisão;
- libsodium para primitivas auxiliares e armazenamento/AEAD quando apropriado;
- Opus;
- Reed-Solomon existente inicialmente;
- MsQuic somente para backend experimental nas plataformas suportadas;
- APIs nativas de decoder e renderer por plataforma.

Toda dependência deve ser avaliada por:

- licença;
- manutenção recente;
- plataformas;
- segurança;
- tamanho do binário;
- cópias e alocações no caminho quente;
- facilidade de atualização.

## 21. Coordenação com o Hermes

As fases devem avançar em pares:

| Hermes | Hestia | Resultado |
|---|---|---|
| H0 | C0 | baseline completo |
| H1 | C1 | arquitetura modular |
| H2/H3 | C2 | melhora sem protocolo novo |
| H4 | C3/C4 | feedback e recuperação avançados |
| H5 | C5 | conexão P2P automática |
| H6 | C6 | identidade e pairing nativo |
| H7 | C7 | HDT v1 funcional |
| H8 | C3/C4/C8 | adaptação fim a fim |
| H9 | C11 | relay autohospedado |
| H10 | C12 | migração/reconexão; degradação encerra a sessão, nunca troca de protocolo |
| H11 | C13 | avaliação QUIC |
| H12 | C14 | produto 1.0; estágios E0–E3 decidem quando o HDT vira padrão |

Não implemente uma feature unilateral sem definir o comportamento quando o outro lado não a suporta.

## 22. Priorização prática

A melhor ordem para o Hestia é:

1. medir receive/decode/present;
2. modularizar protocolo e pipeline;
3. reduzir filas locais;
4. implementar feedback Hermes;
5. FEC/NACK com deadline;
6. ICE e convites;
7. identidade Noise;
8. HDT;
9. otimização por plataforma;
10. Opus e input nativos;
11. relay autohospedado;
12. path migration;
13. QUIC experimental.

O cliente precisa ser desenvolvido junto do host desde as extensões de feedback. Caso contrário, o Hermes tentará inferir o estado do cliente e repetirá o principal problema da pilha atual.

## 23. Referências técnicas

- Moonlight common core: https://github.com/moonlight-stream/moonlight-common-c
- Sunshine: https://github.com/LizardByte/Sunshine
- RFC 8445 — ICE: https://www.rfc-editor.org/rfc/rfc8445
- RFC 8489 — STUN: https://www.rfc-editor.org/rfc/rfc8489
- RFC 8656 — TURN: https://www.rfc-editor.org/rfc/rfc8656
- libjuice: https://github.com/paullouisageneau/libjuice
- libdatachannel: https://github.com/paullouisageneau/libdatachannel
- Noise Protocol Framework: https://noiseprotocol.org/noise.html
- libsodium: https://doc.libsodium.org/
- RFC 8888 — RTCP feedback for congestion control: https://www.rfc-editor.org/rfc/rfc8888
- RFC 9392 — taxa de feedback RTCP: https://www.rfc-editor.org/rfc/rfc9392
- RFC 9221 — QUIC Datagrams: https://www.rfc-editor.org/rfc/rfc9221
- MsQuic: https://github.com/microsoft/msquic
- Opus: https://opus-codec.org/
