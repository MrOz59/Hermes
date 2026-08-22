# AI Development Guide — Hermes / Hestia

> Guia operacional para agentes responsáveis por implementar os roadmaps do Hermes e do Hestia.

## 1. Preparação obrigatória

Antes de modificar código:

1. ler `AI_CONTEXT.md`;
2. identificar o repositório e a fase atual;
3. ler a fase correspondente no roadmap;
4. verificar se existe dependência no outro projeto;
5. localizar interfaces e testes existentes;
6. verificar se a mudança pode entrar no `main` atrás de flag;
7. definir como validar e como reverter.

O agente deve começar cada tarefa com um escopo explícito:

```text
Objetivo:
Fase do roadmap:
Arquivos ou módulos afetados:
Compatibilidade esperada:
Testes necessários:
Risco principal:
Condição de conclusão:
```

## 2. Estratégia de branches

Estrutura recomendada:

```text
main
├── feature/*
├── fix/*
├── refactor/*
└── develop/protocol-v2
```

### `main`

Deve permanecer:

- compilável;
- testável;
- utilizável;
- compatível com o comportamento documentado;
- protegido contra features experimentais não solicitadas.

Mudanças preparatórias podem entrar no `main` quando:

- não alteram comportamento padrão;
- reduzem acoplamento;
- possuem testes;
- preservam o modo legado;
- são úteis independentemente do HDT.

### `develop/protocol-v2`

Branch de integração temporária para funcionalidades que ainda precisam evoluir em conjunto.

Não deve virar uma branch eterna.

Toda parte suficientemente isolada deve voltar gradualmente ao `main`, atrás de feature flag quando necessário.

### Branches de feature

Formato sugerido:

```text
feature/hdt-packet-header
feature/network-feedback
feature/noise-handshake
feature/ice-connectivity
refactor/transport-interface
fix/hdt-session-cleanup
```

Uma branch deve representar um único objetivo revisável.

## 3. Desenvolvimento por fatias verticais

O protocolo deve ser desenvolvido ponta a ponta.

Exemplo correto:

```text
Hermes serializa um pacote mínimo
        ↓
Hestia valida e desserializa
        ↓
teste de compatibilidade
        ↓
telemetria e erro claros
```

Exemplo incorreto:

```text
implementar todos os módulos do host
        ↓
meses depois começar o cliente
```

Sequência recomendada:

1. contrato;
2. test vector;
3. sender mínimo;
4. receiver mínimo;
5. teste de integração;
6. tratamento de erro;
7. telemetria;
8. otimização.

## 4. Divisão de tarefas

Cada tarefa deve caber em uma revisão humana razoável.

Uma tarefa pode conter:

- uma interface;
- uma implementação;
- testes;
- documentação diretamente ligada à mudança.

Uma tarefa não deve conter simultaneamente:

- refatoração global;
- novo protocolo;
- nova criptografia;
- nova UI;
- alteração de build;
- mudança de comportamento legado.

Quando uma feature exige tudo isso, dividir em etapas.

## 5. Regra de refatoração

Refatorar antes de adicionar comportamento quando o código atual impede isolamento ou testes.

Entretanto:

- não refatorar módulos não relacionados;
- não renomear centenas de símbolos junto com mudança funcional;
- preservar comportamento em commits separados;
- incluir testes de regressão antes da troca;
- manter adapters temporários quando isso reduzir risco.

## 6. Feature flags

Toda funcionalidade experimental deve possuir uma flag.

Exemplos:

```ini
stream_protocol = auto
enable_hdt = false
enable_hdt_ice = false
enable_hdt_adaptive_fec = false
enable_hdt_path_migration = false
```

Requisitos para flags:

- valor padrão seguro;
- comportamento documentado;
- logging quando ativada;
- fallback definido;
- possibilidade de remoção futura;
- cobertura de testes para ligado e desligado.

Flags não devem virar desculpa para manter código quebrado indefinidamente.

## 7. Compatibilidade e fallback

Matriz mínima:

| Host | Cliente | Resultado |
|---|---|---|
| Hermes | Moonlight | GameStream |
| Hermes | Hestia legado | GameStream |
| Hermes com HDT | Hestia com HDT | HDT quando negociado |
| Hermes com HDT | Hestia sem HDT | GameStream |
| Sunshine/Apollo | Hestia | GameStream |

O fallback deve ocorrer apenas em pontos seguros.

Exemplo:

```text
negociação HDT falhou antes de iniciar mídia
        ↓
limpar estado HDT
        ↓
tentar GameStream
```

Não fazer fallback silencioso depois que input, decoder ou aplicação já foram parcialmente inicializados sem cleanup completo.

## 8. Versionamento e capabilities

Usar:

```text
major.minor
```

- major incompatível: não conectar por HDT;
- minor diferente: negociar capacidades;
- extensões opcionais: capability bits ou lista versionada.

Exemplos:

```text
encrypted_video
adaptive_fec
deadline_retransmission
decoder_feedback
path_migration
hdr
av1
multi_monitor
```

Toda nova capability precisa de:

- documentação;
- comportamento quando ausente;
- testes positivos;
- testes negativos;
- versão mínima;
- limite de payload e validação.

## 9. Coordenação entre repositórios

Mudanças de protocolo devem possuir issue ou identificador comum.

Exemplo:

```text
HDT-004: transport-wide packet feedback
```

O trabalho deve indicar:

```text
Hermes PR:
Hestia PR:
Spec PR:
Compatibilidade:
Test vectors:
```

Nenhuma das implementações deve ser considerada completa até que exista teste cruzado.

## 10. Processo para cada fase do roadmap

### Entrada

Antes de iniciar uma fase:

- dependências anteriores concluídas;
- métricas baseline disponíveis;
- critérios de conclusão definidos;
- riscos conhecidos registrados.

### Execução

Durante a fase:

- implementar primeiro o menor caminho funcional;
- evitar otimização prematura;
- preservar fallback;
- adicionar telemetria;
- validar em LAN antes de rede degradada;
- testar erro e cleanup.

### Saída

Uma fase só termina quando:

- critérios do roadmap atendidos;
- testes unitários aprovados;
- testes de integração aprovados;
- compatibilidade legado validada;
- documentação atualizada;
- métricas comparadas ao baseline;
- limitações conhecidas registradas.

## 11. Hierarquia de implementação

Ordem padrão:

1. observabilidade e baseline;
2. abstrações;
3. negociação;
4. transporte mínimo;
5. segurança;
6. feedback;
7. pacing;
8. congestion control;
9. FEC e retransmissão;
10. NAT traversal;
11. recuperação e migração;
12. UX e distribuição.

Uma fase pode antecipar pequenos scaffolds, mas não deve ativar comportamento dependente de componentes ainda inexistentes.

## 12. Política para dependências

Antes de adicionar biblioteca:

- justificar o problema resolvido;
- verificar licença;
- avaliar manutenção;
- avaliar plataformas suportadas;
- medir custo de build;
- medir alocações e cópias no hot path;
- verificar API de cancelamento;
- verificar thread safety;
- planejar encapsulamento.

Dependências devem ficar atrás de interfaces do projeto.

## 13. Desempenho

Mudanças em hot path devem evitar:

- alocação por pacote;
- cópia desnecessária;
- locks globais;
- logging síncrono;
- parsing textual;
- chamadas de sistema evitáveis;
- wakeups excessivos.

O agente deve apresentar comparação antes/depois quando alterar:

- packetization;
- criptografia;
- FEC;
- pacing;
- jitter buffer;
- decode;
- renderização;
- captura;
- encoder.

## 14. Segurança

Para código de rede:

- validar todos os tamanhos;
- rejeitar overflow;
- limitar contadores e filas;
- autenticar antes de confiar;
- não logar chaves;
- limpar segredos;
- impedir replay;
- usar timeouts;
- limitar tentativas;
- fuzzar parsers;
- tratar pacotes desconhecidos sem crash.

Qualquer mudança no handshake ou wire format exige revisão mais rigorosa.

## 15. Atualização dos documentos

Atualizar documentos quando a PR:

- muda arquitetura;
- adiciona capability;
- muda wire format;
- altera estratégia de fallback;
- muda dependência principal;
- conclui fase de roadmap;
- invalida decisão anterior.

Não atualizar roadmap apenas para marcar progresso. Registrar evidência, limitações e testes.

## 16. Checklist do agente antes do commit

- [ ] Escopo respeitado.
- [ ] Código legado não removido sem necessidade.
- [ ] Feature experimental desativada por padrão.
- [ ] Erros tratados.
- [ ] Cleanup testado.
- [ ] Testes adicionados.
- [ ] Logging útil.
- [ ] Sem segredos ou dados pessoais.
- [ ] Documentação atualizada.
- [ ] Formatação e lint aprovados.
- [ ] Build das plataformas afetadas aprovado.
- [ ] Diff revisado para mudanças acidentais.

## 17. Checklist antes da PR

- [ ] Fase e issue identificadas.
- [ ] PR pequena ou divisão justificada.
- [ ] Descrição de compatibilidade.
- [ ] Plano de rollback.
- [ ] Benchmark quando necessário.
- [ ] Testes reproduzíveis.
- [ ] Screenshots ou logs somente quando úteis.
- [ ] Dependência no outro repo referenciada.
- [ ] Test vector incluído quando altera protocolo.
- [ ] Documentação sem divergência do código.

## 18. Proibições

O agente não deve:

- fazer force push em branch compartilhada;
- alterar versão de protocolo sem spec;
- desabilitar testes para fazer CI passar;
- mascarar race condition com sleeps;
- capturar exceções e ignorá-las;
- usar polling agressivo quando eventos existem;
- bloquear thread de mídia com I/O lento;
- trocar dependências por preferência pessoal;
- remover comentários técnicos ainda válidos;
- apagar compatibilidade não relacionada à tarefa;
- declarar fase concluída com teste apenas em localhost.
