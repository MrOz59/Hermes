# Coding Standard — Hermes / Hestia

> Padrões gerais para código novo e alterações relacionadas aos roadmaps.

## 1. Regra principal

Respeitar o estilo já estabelecido no repositório, exceto quando uma decisão explícita deste documento exigir maior segurança, testabilidade ou consistência.

Não formatar arquivos inteiros sem necessidade. Alterações de estilo devem ser separadas de alterações funcionais.

## 2. Linguagem

Preferência para código novo:

- C++20 quando suportado;
- RAII;
- ownership explícito;
- tipos fortes;
- `std::chrono` para tempo;
- `std::span` para buffers não proprietários;
- `std::optional` para ausência esperada;
- `std::expected` quando disponível ou um tipo `Result` equivalente;
- `enum class`;
- `std::unique_ptr` por padrão;
- `std::shared_ptr` apenas com ownership realmente compartilhado.

Evitar:

- `new` e `delete` diretos;
- ponteiros crus para ownership;
- macros para lógica;
- exceções atravessando APIs C ou callbacks de sistema;
- inteiros sem unidade para tempo;
- booleanos ambíguos em APIs.

## 3. Nomes

Seguir a convenção local do repositório. Para módulos novos, preferir nomes descritivos.

Exemplos:

```cpp
class CongestionController;
class PacketPacer;
class HdtSession;
class IceCandidateGatherer;
```

Evitar:

```cpp
class Manager;
class Helper;
class Utils;
class Thing;
```

Booleanos:

```cpp
is_connected
has_keyframe
should_retransmit
can_fallback
```

Unidades no nome:

```cpp
rtt_us
bitrate_bps
frame_interval_ns
payload_size_bytes
```

## 4. Tipos de tempo

Não misturar unidades.

Preferir:

```cpp
using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;
using Microseconds = std::chrono::microseconds;
```

Protocolos podem usar inteiros serializados, mas a conversão deve ocorrer nas bordas.

Nunca usar relógio de parede para pacing ou RTT.

## 5. Resultados e erros

APIs que podem falhar devem retornar informação suficiente.

Exemplo:

```cpp
enum class HdtErrorCode {
    invalid_packet,
    authentication_failed,
    unsupported_version,
    timeout,
    socket_error,
    cancelled
};

struct HdtError {
    HdtErrorCode code;
    std::string context;
};
```

Regras:

- não retornar apenas `false` quando o motivo importa;
- separar erro esperado de bug;
- preservar código de erro do sistema quando útil;
- não incluir segredo em mensagem;
- não logar o mesmo erro em todas as camadas.

## 6. Logging

Níveis:

- `trace`: detalhes de pacote, somente builds ou sessões de diagnóstico;
- `debug`: transições internas;
- `info`: início, fim, protocolo escolhido e estado principal;
- `warning`: degradação recuperável;
- `error`: operação falhou;
- `critical`: estado inviável ou possível corrupção.

Campos recomendados:

```text
session_id
connection_id
path_id
frame_id
packet_sequence
protocol_version
remote_endpoint
error_code
```

Não logar:

- chaves;
- tokens completos;
- material de pareamento;
- conteúdo de input;
- payload de áudio ou vídeo;
- endereços pessoais em telemetry pública sem consentimento.

Logs no hot path devem ser desativáveis e não podem formatar strings quando o nível está desligado.

## 7. Threads e concorrência

Cada módulo deve documentar:

- thread dona;
- métodos thread-safe;
- callbacks;
- regra de shutdown;
- ordem de locks.

Preferir:

- message passing;
- filas bounded;
- cancelamento explícito;
- atomics para estado pequeno;
- locks de escopo curto.

Evitar:

- mutex global;
- callback chamado segurando lock;
- detach de thread;
- shutdown baseado em sleep;
- fila ilimitada;
- bloquear thread de captura, encoder, rede ou render.

## 8. Memória e buffers

No hot path:

- reutilizar buffers;
- usar pools apenas após benchmark;
- limitar tamanhos;
- verificar overflow;
- evitar cópia entre packetizer, criptografia e socket;
- não manter frame vencido.

Todo parser deve receber:

```cpp
std::span<const std::byte>
```

e validar limites antes de acessar.

## 9. Wire format

Regras obrigatórias:

- endianness especificada;
- versão explícita;
- comprimento total validado;
- campos reservados zerados;
- extensões ignoráveis quando permitido;
- limites documentados;
- test vectors;
- parser independente para fuzzing;
- nenhuma struct C++ enviada diretamente pela rede.

Proibido:

```cpp
send(socket, &struct_instance, sizeof(struct_instance), 0);
```

Padding, alinhamento e endianness tornam isso instável.

## 10. Interfaces

Interfaces devem expressar responsabilidade única.

Exemplo:

```cpp
class Transport {
public:
    virtual ~Transport() = default;
    virtual Result<void> start() = 0;
    virtual void stop() noexcept = 0;
    virtual Result<void> send(PacketView packet) = 0;
};
```

Evitar interfaces gigantes que controlam captura, encoding e rede ao mesmo tempo.

## 11. Configuração

Toda opção deve ter:

- nome estável;
- tipo;
- valor padrão;
- limite;
- descrição;
- efeito de restart;
- status experimental;
- comportamento inválido.

Valores de rede devem ser validados antes da sessão.

## 12. Testes unitários

Cobrir:

- serialização e parsing;
- limites;
- wraparound de sequence;
- timestamps;
- capability negotiation;
- estados de sessão;
- pacing;
- cálculo de bitrate;
- seleção de retransmissão;
- FEC;
- timeout;
- cancelamento;
- cleanup.

Testes devem ser determinísticos. Injetar relógio e fonte de aleatoriedade quando necessário.

## 13. Testes de integração

Casos mínimos:

- Hermes legado com Moonlight;
- Hermes legado com Hestia;
- Hermes HDT com Hestia HDT;
- mismatch de versão;
- capability ausente;
- pacote malformado;
- chave inválida;
- perda de conexão;
- fallback;
- encerramento durante handshake;
- mudança de bitrate;
- sessão repetida sem leak.

## 14. Testes de rede

Usar cenários reproduzíveis com `tc`, `netem`, TBF ou CAKE.

Registrar:

```text
largura de banda
RTT
jitter
perda
perda em rajada
reordering
queue size
duração
hardware
codec
resolução
FPS
```

Não comparar resultados obtidos em condições diferentes sem indicar isso.

## 15. Fuzzing

Alvos prioritários:

- header HDT;
- mensagens de controle;
- handshake;
- ICE signalling importado;
- parser de convite;
- FEC metadata;
- NACK e feedback;
- configuração recebida remotamente.

O parser não pode crashar, consumir memória ilimitada ou entrar em loop.

## 16. Benchmarks

Benchmarkar isoladamente:

- serialização;
- criptografia;
- packetization;
- FEC encode/decode;
- pacer;
- feedback processing;
- jitter buffer;
- cópias e alocações.

Medir no pipeline real antes de concluir que microbenchmark representa ganho de sessão.

## 17. Commits

Formato recomendado:

```text
feat(hdt): add version negotiation
fix(pacer): prevent queue growth after bitrate drop
refactor(network): extract transport interface
test(protocol): add malformed feedback vectors
docs(architecture): document fallback states
```

Um commit deve:

- compilar quando possível;
- possuir intenção única;
- não conter arquivos gerados acidentalmente;
- explicar por que quando isso não for óbvio.

## 18. Pull requests

Template recomendado:

```markdown
## Objetivo

## Fase do roadmap

## Mudanças

## Compatibilidade

## Testes

## Métricas

## Riscos

## Rollback

## PRs relacionadas
- Hermes:
- Hestia:
- Spec:
```

## 19. Revisão

Revisor deve verificar:

- ownership;
- limites;
- cancelamento;
- thread safety;
- fallback;
- compatibilidade;
- versionamento;
- telemetria;
- testes;
- impacto de latência;
- comportamento sob perda;
- cleanup após erro.

## 20. Definição de pronto

Uma tarefa está pronta quando:

- código completo;
- testes completos;
- documentação coerente;
- CI aprovado;
- sem regressão legado conhecida;
- erros observáveis;
- rollback possível;
- critérios da issue atendidos;
- dívida temporária registrada com issue e prazo técnico claro.
