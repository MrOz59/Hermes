# Hermes / Hestia — Pacote de orientação para agentes

Este diretório reúne os documentos que governam a evolução coordenada do Hermes e do Hestia.

## Ordem obrigatória de leitura

Antes de iniciar qualquer tarefa, o agente deve ler:

1. `AI_CONTEXT.md`
2. `AI_DEVELOPMENT_GUIDE.md`
3. `ARCHITECTURE.md`
4. `CODING_STANDARD.md`
5. o roadmap do repositório em que vai trabalhar:
   - `HERMES_ROADMAP.md`
   - `HESTIA_ROADMAP.md`

Quando uma tarefa modifica o protocolo, o agente deve ler os dois roadmaps, mesmo que altere apenas um repositório.

## Função de cada documento

| Documento | Finalidade |
|---|---|
| `AI_CONTEXT.md` | Contexto permanente, decisões já tomadas e limites do produto |
| `AI_DEVELOPMENT_GUIDE.md` | Fluxo de trabalho, branches, fases e comportamento esperado dos agentes |
| `ARCHITECTURE.md` | Arquitetura-alvo, componentes, fluxos e contratos entre Hermes e Hestia |
| `CODING_STANDARD.md` | Regras de código, testes, logging, commits e pull requests |
| `HERMES_ROADMAP.md` | Evolução do host |
| `HESTIA_ROADMAP.md` | Evolução do cliente |

## Regra de precedência

Em caso de conflito:

1. requisitos explícitos da issue ou tarefa;
2. decisões registradas em `AI_CONTEXT.md`;
3. arquitetura em `ARCHITECTURE.md`;
4. fase atual dos roadmaps;
5. padrões gerais de desenvolvimento.

Uma tarefa não pode usar uma issue genérica como justificativa para quebrar uma decisão arquitetural permanente. Se a mudança for realmente necessária, ela deve atualizar primeiro os documentos de arquitetura e contexto em uma PR separada ou claramente isolada.

## Objetivo

Construir uma plataforma de streaming remoto:

- descentralizada;
- de baixa latência;
- segura;
- modular;
- observável;
- compatível com Moonlight durante a transição;
- sem conta ou infraestrutura central obrigatória;
- com relay opcional e autohospedado;
- desenvolvida por incrementos pequenos, revisáveis e testáveis.

## Estado atual e horizonte do HDT

O caminho de produção continua sendo GameStream, com a API Hestia v1 como
camada aditiva de preparação, diagnóstico e recursos opcionais. H0/C0 e H1/C1
estão concluídas; H2 está implementada com a aceitação empírica
reference/candidate ainda aberta, e C2 continua em validação e refinamento.

O HDT pertence somente às fases H7/C7, depois de bitrate adaptativo,
feedback/recuperação, ICE/conectividade e identidade nativa. Não existe prazo
de entrega. Ele deve começar experimental, desativado por padrão e com fallback
limpo para GameStream. Nenhum documento ou implementação intermediária deve
apresentá-lo como recurso do próximo release.
