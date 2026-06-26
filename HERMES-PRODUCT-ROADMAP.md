# Hermes — Roadmap priorizado de produto

Análise das 20 dores de usuário (Apollo/Sunshine) vs. o código atual do Hermes,
com esforço estimado e prioridade. Objetivo: transformar o fork técnico num
produto, focando onde o retorno é maior e onde o Hermes tem vantagem única.

## Princípio

Não dá para "corrigir 150 itens". O que faz virar produto é resolver bem os
poucos que (a) mais doem, (b) diferenciam o Hermes, (c) têm baixa fricção porque
a infraestrutura já existe. O zero-copy do Hermes-KMS é a vantagem única: só o
Hermes pode mostrar "tempo de captura real" porque construiu o pipeline.

## O que já está resolvido ou parcialmente coberto

- **Sessão/cliente**: `rtsp.cpp::session_count()` já rastreia sessões ativas;
  `CLIENT CONNECTED/DISCONNECTED` já são logados.
- **Endpoint de diagnóstico**: `/api/hestia/v1/diagnostics` existe, mas hoje só
  retorna status de clipboard — pronto para receber métricas.
- **Encoder probe**: já testa nvenc→vaapi→software em ordem e loga cada falha
  ("Encoder [X] failed", "Reverting back to GPU->RAM->GPU"). Falta tornar isso
  visível ao usuário (hoje só vai pro log).
- **Métricas de pipeline (parcial)**: instrumentamos `capture-metric` (captura
  zero-copy ~8us vs EVDI ~180us). Base para o painel de métricas.
- **Hermes-KMS metrics ioctl**: o driver já expõe GET_METRICS (frames, waits,
  exports, hotplugs) — fonte rica e não usada pela UI.

## Tier 1 — atacar primeiro (alto valor, baixa fricção, diferencia)

### A. Métricas em tempo real + diagnóstico acionável (#20 + #13 + #14 + #5)
Por quê: a infra existe (endpoint diagnostics, session_count, GET_METRICS do
driver, capture-metric), e é a vantagem única do Hermes. Resolve a raiz de
muitas outras dores ("o usuário sente o problema mas não sabe onde está").

Entregas:
1. Expandir `/api/hestia/v1/diagnostics` (ou novo `/metrics`) para retornar, ao
   vivo: encoder REAL em uso (codec + hw/sw), cliente(s) conectado(s),
   resolução/FPS reais do stream, frames dropped, tempo de captura, tempo de
   encode, bitrate real.
2. Fonte: stream/video session counters + Hermes-KMS GET_METRICS quando o
   backend é hermes_kms.
3. Tornar o fallback de encoder VISÍVEL: quando cai em software, expor um campo
   `encoder_fallback: "vaapi failed -> software"` em vez de só logar.

### B. Encoder honesto (#5 + parte de #7)
Por quê: dor aguda e frequente ("acho que uso hardware, mas é software").
Reaproveita o probe existente.

Entregas:
1. Após o probe, registrar o resultado real (encoder escolhido, por que os
   outros falharam) num estado consultável pela UI/diagnostics.
2. Mensagem acionável no log e na resposta: "VAAPI falhou (motivo) -> usando
   software; verifique driver/permissões".

## Tier 2 — alto valor, esforço médio

- **#10 + #11 reconexão / múltiplos clientes**: o server já tem session_count;
  falta diferenciar "cliente caiu" de "usuário encerrou" e limpar estado
  (processo órfão, áudio preso). Toca rtsp.cpp + process.cpp.
- **#4 encerramento inconsistente**: garantir cleanup de processo/áudio ao
  desconectar. Relacionado a #10.
- **#16 systemd**: herança de ambiente (Steam/Lutris/áudio/sessão gráfica). Dor
  clássica de Linux; tutorial + ajuste de unit.

## Tier 3 — importante mas alto esforço / baixa diferenciação

São "fazer melhor o que o Sunshine já faz". Valor real, mas não é o que
diferencia o Hermes e custa caro:
- #1 setup confuso, #2 pareamento, #19 modo appliance, #12 resolução/bitrate.
- #14 web UI (a parte de redesign além de métricas).

## Tier 4 — fora do escopo server-side imediato / depende de rede do usuário

- #17 segurança/acesso remoto, #18 NAT/rede externa, #15 migração de config.
  Importantes para produto final, mas muitos dependem do ambiente do usuário e
  rendem menos por hora investida agora.

## Recomendação de sequência

1. **Tier 1A** (métricas + diagnóstico) — maior alavancagem, usa a vantagem
   zero-copy, e dá visibilidade que reduz issues de todas as outras categorias.
2. **Tier 1B** (encoder honesto) — pequeno, alto impacto, encaixa em 1A.
3. Reavaliar com o usuário após Tier 1 antes de descer para Tier 2.
