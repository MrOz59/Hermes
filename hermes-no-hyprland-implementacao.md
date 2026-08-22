# Implementando todas as funções do Hermes/Hermes-KMS no Hyprland — rotas de implementação

Documento de pesquisa — 2026-08-22. Complementa o diagnóstico em
`hermes-kms-vs-hyprland.md`. Fontes: código local (`/home/ozzy/Projects/Hermes-KMS`,
`/home/ozzy/Projects/Apollo-Linux`), aquamarine (`/tmp/aquamarine`, v0.14.0),
vkms 6.6/6.15/master (`/tmp/vkms/`), logs das sessões Hyprland, testes ao vivo
nesta máquina, GitHub (aquamarine/Hyprland) e wiki.hypr.land.

> ✅ = verificado nesta máquina · 🔬 = pesquisa/análise concluída · ⏳ = em análise

---

## 1. Resumo executivo

Existem **quatro rotas** para fazer o conjunto Hermes + Hermes-KMS funcionar
com o Hyprland:

| Rota | O que é | Esforço | Risco | Cobertura funcional |
|---|---|---|---|---|
| **A. Consertar o caminho DRM** (hermes-kms como monitor real do Hyprland) | patch aquamarine *display-only* (✅ existe) + correção do wedge de page-flip + propriedades KMS faltantes | Médio | Médio | 100% — captura zero-copy no render node permanece intocada |
| **B. Caminho nativo Hyprland** (headless output + `ext-image-copy-capture-v1`) | novo backend de captura no Hermes (`display_t` → `compositor_e::hyprland`) | Médio | **Baixo** | ~90% — sem SET_OUTPUT remoto, sem damage, modo via `hyprctl keyword monitor` |
| **C. Gamescope por sessão** (fluxo já existente no Hermes) | usar o caminho multi-sessão atual com Hyprland como desktop do host | **Baixo** | **Baixo** | 100% do streaming; output virtual vive no Gamescope, não no Hyprland |
| **D. Upstream** | generalizar o caso *display-only* no aquamarine + refactor vkms-like no hermes-kms | Baixo incremental | — | habilita A de forma sustentável |

Veredito da pesquisa de ecossistema: o Hyprland **já tem** o caminho nativo
zero-copy para hosts de streaming (headless output + captura DMA-BUF) e a
própria [wiki Virtual-GPU](https://wiki.hypr.land/Configuring/Advanced-and-Cool/Virtual-GPU/)
cita Sunshine/Moonlight, **Apollo/Artemis** e wayvnc como consumidores. A Rota B
é o padrão comprovado por
[moreland](https://github.com/adiimanav/moreland),
[sunshine-hyprland-virtual-display](https://github.com/jhonsnake/sunshine-hyprland-virtual-display)
e [hypr-wayvnc-virtual-display](https://github.com/sauyon/hypr-wayvnc-virtual-display).
A Rota A continua valendo porque preserva o contrato atual do Hermes (e o
multi-sessão/seat) sem mudar o userspace — mas o wedge de page-flip pertence a
uma **classe de bugs upstream do aquamarine ainda aberta** (ver §3.4), então A
tem prazo incerto.

Recomendação: **C já desbloqueia hoje; B é o alvo de longo prazo (menos risco);
A avança em paralelo como trabalho de driver/upstream.**

---

## 2. Inventário funcional: o que "todas as funções" significa

Contrato atual Hermes ↔ Hermes-KMS (inventário do Apollo-Linux, namespace
`hermes_kms` em `src/platform/linux/virtual_display.{h,cpp}` — ioctls via
render node: `GET_VERSION, GET_CAPS, GET_STATUS, SET_OUTPUT, ACQUIRE_FRAME,
GET_IDENTITY, WAIT_FRAME, GET_METRICS` + `SELECT_OUTPUT (0x08)` que existe no
código mas **falta no header v8 do checkout** — o checkout local está uma
revisão atrás do que o Apollo-Linux espera, UAPI 9):

| # | Função | Como funciona hoje | Rota A | Rota B | Rota C |
|---|---|---|---|---|---|
| 1 | Output virtual como monitor | conector DRM `HERMES-1` + hotplug | ✅ (patch 2.2) | ✅ `hyprctl output create headless` (✅ testado) | ✅ (dentro do Gamescope) |
| 2 | Modo/refresh por cliente | `SET_OUTPUT` WxH@Hz → CVT + hotplug | ✅ | ⚠️ `hyprctl keyword monitor NAME,WxH@Hz,pos,scale` | ✅ |
| 3 | Scanout no output virtual | import PRIME + commit atômico | ⚠️ funciona, **congela** (page-flip) | ✅ render nativo da GPU primária | ✅ |
| 4 | Captura zero-copy (DMA-BUF + fence) | `WAIT_FRAME`→`ACQUIRE_FRAME` | ✅ intacto | ✅ `ext-image-copy-capture-v1` | ✅ intacto |
| 5 | Damage | `FB_DAMAGE_CLIPS` → rect | ✅ | ❌ protocolo não expõe damage (mas o Hermes hoje já codifica o frame inteiro) | ✅ |
| 6 | Cursor | **o compositor assa o cursor no scanout** — o Hermes nunca usa o cursor plane (`img->data=nullptr`, `kmsgrab.cpp:1843`) | ✅ neutro | ✅ ICC tem *cursor session* | ✅ |
| 7 | Pacing 60/120/144Hz | hrtimer software | ✅ vblank medido perfeito (§3.2); o **evento** de flip é o problema | ✅ refresh do monitor config | ✅ |
| 8 | Multi-sessão (outputs + seats) | `outputs=2`/`devices=N` + seatd/udev | ✅ arquitetura suporta | ✅ um headless output por sessão | ✅ fluxo existente |
| 9 | Métricas | `GET_METRICS` + debugfs | ✅ | — (métricas próprias) | ✅ |
| 10 | Input virtual por sessão | uinput + udev `71-hermes-session-input.rules` | ✅ independente do vídeo | ✅ idem | ✅ idem |

---

## 3. Fatos estabelecidos nesta rodada

### 3.1 O patch do aquamarine foi recuperado ✅

`/tmp/claude-1000/-home-ozzy-Projects-Apollo-Linux/35b04f93-e9cc-453a-86a6-9245c669fe34/scratchpad/aqpkg/`
contém `PKGBUILD` + `hermes-kms-display-only.patch` (o conteúdo exato do pacote
`aquamarine-0.14.0-2.2`): adiciona `AQ_BACKEND_GPU_DRIVER_HERMES_KMS` e estende
o caso especial do `evdi` — `primary = {}; rendererRequired = false` — para
`hermes-kms`. Ou seja: o aquamarine **já tem** o caminho certo para displays
virtuais (import direto + scanout, sem renderer GL no device); o patch só inclui
o nome do driver na lista. (Diff binário 2.1↔2.2 inconclusivo: builds
x86_64_v3 vs x86_64; strings `hermes-kms` na lib instalada confirmam.)

### 3.2 O vblank do hermes-kms é clinicamente perfeito ✅ (teste novo, sem root)

Durante `hold 1280x720@60`, um probe `DRM_IOCTL_WAIT_VBLANK` relativo (60
iterações) mediu **60 vblanks em 1.000 s; Δ médio 16.674 ms, min 16.662, max
16.687 ms**. O timer/contador entrega 60Hz exatos. **O congelamento não é o
vblank — é o caminho de evento de page-flip ou o scheduler do aquamarine.**

### 3.3 hermes-kms vs vkms: o código de flip é um clone fiel do vkms 6.15 🔬

- O padrão arm/send (`drm_crtc_vblank_get` → `arm_vblank_event` → fallback
  `send_vblank_event` no `atomic_flush`) é **byte-idêntico** ao vkms 6.15 — o
  padrão em si é correto; o vkms 7.x/master moveu timer + timestamps para os
  helpers do core (`DRM_CRTC_VBLANK_TIMER_FUNCS` + `handle_vblank_timeout`),
  eliminando a lógica manual frágil que o hermes ainda carrega.
- **Diferença decisiva 1:** hermes define `DRIVER_RENDER` (expõe render node);
  vkms **não** define (`DRIVER_MODESET|DRIVER_ATOMIC|DRIVER_GEM`). É o render
  node que faz o aquamarine tentar criar renderer EGL num device sem GPU (o
  mesmo modo de falha evdi/DisplayLink, aquamarine #22). O render node existe
  por causa do canal de captura do Hermes — mas os ioctls de captura são
  chamáveis **no node primário sem DRM master** (ioctls custom não exigem
  master; só modeset exige), então `DRIVER_RENDER` pode ser removido ou virado
  parâmetro de módulo sem quebrar a captura (a validar).
- **Diferença decisiva 2:** hermes mantém o hrtimer manual 6.15 (com
  `get_vblank_timestamp` próprio que devolve `node.expires - period` e sempre
  `true`) em vez dos helpers do core.
- **Contexto importante:** o vkms **também** não é um output suportado para
  sway/Hyprland — o wlroots quebra do mesmo jeito no multi-GPU com vkms
  ([wlroots #2024](https://gitlab.freedesktop.org/wlroots/wlroots/-/issues/2024)),
  e o único uso documentado do vkms é X/IGT e KWin/KRFB. Ou seja: o caso
  *display-only* do aquamarine é uma lacuna upstream para **qualquer** display
  virtual, não uma particularidade do hermes-kms.
- **Candidato adicional ao wedge:** vkms sobrescreve `atomic_commit_tail` com
  `drm_atomic_helper_fake_vblank` + `wait_for_flip_done` (`vkms_drv.c:65-91`);
  hermes usa o tail stock do `drm_atomic_helper_commit` (que em commits
  bloqueantes pode esperar vblanks ~1 s). Vale testar o tail do vkms no hermes.

### 3.4 O wedge é uma classe de bugs upstream do aquamarine, ainda aberta 🔬

**Mecanismo preciso (análise de deep-dive):** `CFrameScheduler::m_pending`
(`frameInFlight()`) trava `true` em todo commit de buffer bem-sucedido
(`Atomic.cpp:392` → `FrameScheduler.cpp:7`) e **só** é limpo em
`handlePF()`→`onFrameComplete()` (`DRM.cpp:1221`). Todo commit de buffer
habilita `DRM_MODE_PAGE_FLIP_EVENT` (`DRM.cpp:2111-2112`) — **não existe**
caminho de commit de buffer habilitado sem o evento (só `onlyTest`). O ciclo é:
`commitState` → `Atomic.cpp:378-392` (`WANTSFLIP` → `armPageFlip` com FLIPID
único → `drmModeAtomicCommit(..., rc<void*>(FLIPID))` → sucesso →
`onFrameSubmitted()` trava `m_pending`) → evento volta no fd do backend →
`dispatchEvents` (`DRM.cpp:1260-1275`, estático `gDispatchingBackend`) →
`handlePF`. Os early-returns do `handlePF`, em ordem:

1. `DRM.cpp:1194` `if (!BACKEND || BACKEND->drmFD() != fd) return;` — **silencioso**, deixa `m_pending` preso;
2. `DRM.cpp:1200` FLIPID/CRTC mismatch — loga DEBUG `Ignoring a stale pf event` (não desarma);
3. `DRM.cpp:1209` connector sumiu — loga DEBUG (já desarmado, `m_pending` não limpo);
4. `DRM.cpp:1216` `!frameInFlight()` — loga DEBUG (inofensivo);
5. `DRM.cpp:1221` `onFrameComplete()` — o único que limpa.

Com `m_pending` preso, o próximo commit de buffer bate em `DRM.cpp:2106-2109`
→ ERR `Cannot commit when a page-flip is awaiting` **para sempre** (o único
outro reset é `invalidateFrame()`, `DRM.cpp:1947-1952`, que só roda em
disconnect/modeset/VT). Como os logs não mostram `stale pf` nem `connector is
gone`, o wedge observado é um dos **dois caminhos silenciosos**:

1. `handlePF` nunca é invocado (evento nunca entregue/lido do fd do backend) —
   **suspeito nº 1**: para um device fisicamente separado (fd e poll próprios),
   a análise de reentrância do `gDispatchingBackend` indica que o caminho (2)
   exige dispatch reentrante aninhado que o design baseado em poll não produz;
2. a guarda **`drmFD() != fd`** (`DRM.cpp:1194`) — estático
   `gDispatchingBackend` adicionado em `cf45416`/aquamarine#342: só dispara em
   dispatch reentrante de múltiplos backends (o `drmHandleEvent` interno de um
   backend muda o estático enquanto o externo ainda drena o fd original) —
   possível, porém menos provável aqui.

Discriminação em runtime: `AQ_TRACE=1` — o TRACE `pf event seq …`
(`DRM.cpp:1214`) só imprime **depois** de passar as guardas 1194 e 1200: se
aparecer, o `handlePF` roda e o wedge é downstream (1216/1223, que logam); se
não aparecer, `handlePF` nunca roda ou retorna em 1194 — um log temporário no
topo do `handlePF` separa os dois casos.

O aquamarine **não distingue vblank de software vs hardware** — o único gate é
`DRM_CAP_CRTC_IN_VBLANK_EVENT` (`DRM.cpp:519-522`) — e o hermes-kms entrega os
eventos corretamente (clone vkms), então o problema é contabilidade/dispatch do
aquamarine, não falta de fonte de evento.

`Cannot commit when a page-flip is awaiting` acontece também em hardware real:

- [aquamarine #353](https://github.com/hyprwm/aquamarine/issues/353) (aberto,
  2026-08-09): AMD 660M, kernel 7.1.6 — mesmo sintoma, tela preta no start;
- [aquamarine #343](https://github.com/hyprwm/aquamarine/issues/343) (aberto):
  deadlock após flip perdido em back-to-back modesets;
- [aquamarine #240](https://github.com/hyprwm/aquamarine/issues/240) (aberto,
  **reproduzido em NVIDIA e Intel i915**): commits presos em loop de page-flip
  — assinatura idêntica à nossa;
- [Hyprland #14521](https://github.com/hyprwm/Hyprland/issues/14521) (fechado
  NOT_PLANNED): mesmo loop após desconexão de dock;
- [aquamarine #361](https://github.com/hyprwm/aquamarine/issues/361) (aberto):
  SIGSEGV em `handlePF()` com connector removido por udev;
- [PR #312](https://github.com/hyprwm/aquamarine/pull/312) (merged 2026-07-08):
  "deterministic page-flip scheduling + coalescing" — tentativa recente de
  consertar exatamente esse scheduler;
- o caso especial EVDI sem renderer (`DRM.cpp:899-903`, de `ff9b933`/aquamarine#310)
  é o template exato que o patch do Hermes emula.

**Implicação:** um device com vblank de software é um reprodutor determinístico
dessa classe de bug. Corrigir no aquamarine beneficia todo mundo (evdi,
DisplayLink, hotplug real); corrigir só no hermes-kms pode não bastar se o bug
for do scheduler/dispatch.

---

## 4. Rota A — consolidar o caminho DRM (output nativo do Hyprland)

### A.1 aquamarine display-only — ✅ feito localmente; generalizar para upstream

O patch segue o precedente `evdi`. Proposta upstream melhor que allowlist de
nome: tratar como display-only **qualquer device onde a criação de renderer EGL
falha** (probe real de `eglInitialize` na inicialização do backend) — assim
futuros drivers virtuais ganham de graça.

### A.2 Corrigir o wedge de page-flip — 🔬 mecanismo identificado (2 caminhos silenciosos); confirmação por tracing pendente

Frentes em paralelo:

1. **Instrumentar e reproduzir com causa raiz** (primeiro passo obrigatório;
   comandos no Apêndice). O discriminador é simples: `stale pf` no log TRACE
   ⇒ evento chegou e foi rejeitado (bug de bookkeeping do aquamarine); `pf
   event seq` presente ⇒ entrega OK e o problema é o scheduler; nenhum dos dois
   ⇒ o kernel não enviou (foco no driver, §3.3).
2. **Refactor do hermes para o modelo vkms 7.x** (recomendado de qualquer
   forma — elimina a lógica manual de timer/timestamp; esboço completo no
   relatório do subagente vkms):
   - `hermes_kms_crtc_funcs` (linhas 629-638): remover
     `enable_vblank`/`disable_vblank`/`get_vblank_timestamp` e usar
     `DRM_CRTC_VBLANK_TIMER_FUNCS` (= `drm_crtc_vblank_helper_enable_vblank_timer`
     + `_disable_vblank_timer` + `_get_vblank_timestamp_from_timer`);
   - `hermes_kms_crtc_helper_funcs` (750-755): trocar `atomic_enable/disable`
     por `drm_crtc_vblank_atomic_enable/disable` (wrappers finos preservam
     `last_enable_ns`/`last_disable_ns`) e adicionar
     `.handle_vblank_timeout = hermes_kms_handle_vblank_timeout` (corpo:
     `drm_crtc_handle_vblank` + log de erro);
   - **manter** `hermes_kms_crtc_atomic_flush` (724-748) como está (idêntico
     ao vkms);
   - **deletar**: `hermes_kms_vblank_timer` (534-565), `_enable_vblank`
     (567-591), `_disable_vblank` (593-598), `_get_vblank_timestamp` (605-627),
     campos `vblank_timer`/`vblank_period` (133-134), counters de vblank
     (178-179) e linhas correspondentes no debugfs (1657-1662),
     `hrtimer_setup`/`hrtimer_cancel` (1846/1886); `#include
     <drm/drm_vblank_helper.h>`;
   - lock à la vkms: `atomic_begin` segura `output->lock` até `atomic_flush`
     (impede o tick de rodar no meio do commit — requisito documentado do
     `drm_crtc_arm_vblank_event`);
   - **opcional**: parâmetro de módulo `flip_immediate` — no `atomic_flush`,
     `drm_crtc_send_vblank_event` imediato em vez de armar no vblank (entrega
     garantida; pacing fica sem vsync; útil como diagnóstico/fallback).
   - **⚠️ não fazer**: NUNCA adicionar `drm_crtc_vblank_put` manual depois de
     `arm_vblank_event` — o put automático acontece em
     `drm_handle_vblank_events` (ou no `drm_crtc_vblank_off`, que **flush** os
     eventos armados antes de desligar). Um double-put desliga o timer com
     evento armado — exatamente o gatilho do wedge.
   - Hazard restante do caminho manual (motivo do refactor): em device sem
     contador de HW (`max_vblank_count==0`), `drm_update_vblank_count` avança a
     contagem por **interpolação de timestamp** (`DIV_ROUND_CLOSEST(diff,
     framedur_ns)`; se `framedur_ns==0` cai para `in_vblank_irq ? 1 : 0`) — e um
     tick com `vblank->enabled==false` é **descartado silenciosamente**
     (`drm_handle_vblank` retorna false, evento armado fica preso). Os helpers
     do core eliminam ambos. Se mantiver o caminho manual por ora, ao menos
     setar `*max_error` no `get_vblank_timestamp`.
   - **opcional (tail de commit)**: vkms sobrescreve `atomic_commit_tail`
     (`vkms_drv.c:65-91`) com `wait_for_flip_done`; o tail default do hermes
     (`drm_atomic_helper_commit`) pode stallar commit bloqueante até ~1 s em
     `wait_for_vblanks` quando o timer não está rodando — candidato a adotar o
     tail do vkms.
3. **Patch aquamarine-side** (esboço completo do deep-dive, contra `46d93d8` =
   v0.14.0-10; o flip accounting vem de `cf45416`/#342, já presente no 2.2):
   - **(a) detecção do device** (`DRM.hpp:504-509` + `registerGPU`): adicionar
     `bool noHWVblank = false;` a `drmProps` e estender o caso especial:
     ```cpp
     if (drmVerName && (std::string_view(drmVerName) == "hermes-kms" ||
                        std::string_view(drmVerName) == "vkms")) {
         primary               = {};     // shouldBlit()==false -> import PRIME direto
         rendererRequired      = false;  // pula initMgpu() (renderer GL)
         drmProps.noHWVblank   = true;
     }
     ```
   - **(b) immediate completion mínimo** (espelha o bloco ASYNC de
     `DRM.cpp:2310-2330`): após commit bem-sucedido com buffer em backend
     `noHWVblank`, `disarmPageFlip()` + `sched.onFrameComplete()` +
     emitir `present` (com `seq=0`, **sem** `AQ_OUTPUT_PRESENT_HW_CLOCK` —
     mesmo tratamento do NVIDIA `seq==0` em `DRM.cpp:1243-1244`) +
     `onPresent()` (rotação de FB obrigatória) + `frameReady`. Efeito: latência
     mínima, mas pacing vira CPU-bound (busy-loop) — Hyprland re-renderiza em
     loop; o hermes captura por last-frame-wins, sem corrupção.
   - **(c) loss-watchdog por timer (recomendado)**: manter o evento como fonte
     de pacing e agendar callback one-shot ≈ `1.5 × 1e9/refresh` ns após
     `onFrameSubmitted()`; se `frameInFlight()` ainda true, roda o bloco de
     conclusão de (b). ~20 linhas; atenção: `addIdleEvent` é next-loop-turn
     (sem delay) — precisa de timerfd dedicado ou callback re-armado (o padrão
     de `DRM.cpp:2444-2485` é a referência). Converte wedge permanente em 1
     frame perdido, preservando 60/120/144 Hz.
   - **(d) alternativas**: aceitar qualquer evento em `handlePF` quando
     `noHWVblank` (só ajuda se eventos chegarem com FLIPID/CRTC errados — os
     logs descartam; complemento de (c)); ou suprimir `PAGE_FLIP_EVENT` +
     completion manual por timer (contabilidade mais limpa, exige o timer de
     (c) para não virar busy-loop).
   - **(e) logar a guarda silenciosa `drmFD() != fd`** (`DRM.cpp:1194`) — menos
     provável que (1), mas hoje descarta eventos sem rastro; logar/dispachar
     corretamente fecha o segundo caminho silencioso.
   - Nota: o caminho ASYNC/tearing tem um leak latente da mesma classe (o bloco
     de `DRM.cpp:2310-2330` não limpa `m_pending`) — irrelevante para o hermes
     (sem async commit), mas mostra que a classe de bug é do design, não do
     driver.
   - **gatilho de confirmação**: `AQ_TRACE=1` — o TRACE `pf event seq …`
     (`DRM.cpp:1214`) diz definitivamente se o `handlePF` roda no backend do
     hermes; `stale pf`/`connector is gone` descartam os outros caminhos.
4. **Acompanhar upstream**: o wedge é classe conhecida e aberta — aquamarine
   #240 (reproduzido em NVIDIA e Intel i915, mesma assinatura), Hyprland #14521
   (fechado NOT_PLANNED), #304 (fixado pelo PR #312, mas é trigger DPMS);
   o caso EVDI é upstream (`ff9b933`/#310). Não há suporte upstream para
   "immediate completion" de vblank de software — é a contribuição nova.

### A.3 Propriedades KMS faltantes — 🔬 mapeadas, implementação trivial

| Lacuna | Fix | Referência |
|---|---|---|
| `CTM` → `failed to commit ctm` por commit | `drm_crtc_enable_color_mgmt(crtc, 0, false, LUT_SIZE)` | vkms_crtc.c:229 |
| `GAMMA_LUT/DEGAMMA_LUT` → `Couldn't get the gamma_size prop` | idem + `drm_mode_crtc_set_gamma_size` | vkms_crtc.c:223 |
| EDID rejeitado pelo libdisplay-info | validar bloco com `di-edid-decode` (CLI do libdisplay-info) | — |
| Sem `IN_FORMATS` | opcional: blob `{DRM_FORMAT_MOD_LINEAR}` para XRGB8888/ARGB8888 | — |

### A.4 Captura permanece intocada (mesmo sem render node)

Canal lateral via ioctls custom — qualquer correção de scanout em A.1–A.3 faz o
Hermes funcionar **sem mudar o userspace**. Sobre remover `DRIVER_RENDER`
(opção do §3.3, validada): `DRM_RENDER_ALLOW` significa "permitido no node
primário **e** no render node" e é independente de `DRM_MASTER` — nenhum ioctl
do Hermes é modeset (`SET_OUTPUT` só muda o modo requisitado e emite hotplug),
então todos funcionam num `cardN` aberto **sem master**, com `drm_file`
separado do compositor (sem EBUSY; o dono do output é o `owner_file`, não o
tipo de node). Mudanças: Apollo abre `/dev/dri/cardN` em vez de `renderD*`
(2 linhas no `open_devices`), ferramentas/regras udev idem. Se um node separado
for realmente necessário, o modelo EVDI (char/misc device próprio para o canal
de captura) é o caminho correto — não um render node, que é o que faz os
compositores tratarem o device como render-capable. **Risco a medir:** o README
do driver afirma que o render node é o que faz os compositores enumerarem o
conector — remover pode regredir o caminho KWin/GNOME validado; por isso a
forma segura é parâmetro de módulo (default `on` para KWin, `off` para setups
Hyprland).

---

## 5. Rota B — caminho nativo Hyprland (headless + ext-image-copy-capture-v1)

A receita (parcialmente verificada **nesta máquina**, Hyprland 0.56.2):

```bash
hyprctl output create headless                # ✅ testado: cria HEADLESS-1
hyprctl keyword monitor HEADLESS-1,1920x1080@60,1920,1   # modo/posição
# captura: cliente ext-image-copy-capture-v1 (DMA-BUF + modifiers)
```

- `hyprctl output create headless` → `ok` (criado/removido limpo) ✅
- `ext-image-copy-capture-v1.xml` em `/usr/share/wayland-protocols/staging/` ✅
- **Globals ao vivo confirmados** (wayland-info na instância atual do Hyprland):
  `ext_image_copy_capture_manager_v1`, `ext_output_image_capture_source_manager_v1`,
  `ext_foreign_toplevel_image_capture_source_manager_v1`, `zwlr_screencopy_manager_v1`
  (v3 — o fallback `wlgrab`) e `hyprland_toplevel_export_manager_v1` ✅
- Implementado no Hyprland pelo [PR #11709](https://github.com/hyprwm/Hyprland/pull/11709)
  (merged 2026-02-22; incluído no 0.56.2), com **cursor session** — e o corpo
  do PR documenta a receita `output create headless test` + `monitor
  test,1736x976@60,1920,1` + `wayvnc -o test` como o fluxo canônico.
- **moreland** é a referência de arquitetura para host de streaming: output
  headless do tamanho do cliente + captura damage-driven via ICC + encode
  zero-copy na mesma GPU (~19 ms @120 fps, 0.6% de 1 núcleo). Estudar
  `crates/` dele.
- **Cuidado (sunshine-hyprland-virtual-display):** o backend `wlr-capture` do
  Sunshine **cacheia `output_name` no startup** — o output headless precisa
  existir antes do host subir (o Hermes não deve herdar esse bug).
- A wiki também documenta `AQ_NO_KMS_REQUIREMENT=1` → cria `HEADLESS-0`
  1920×1080@60 automaticamente quando não há KMS (rota para máquinas sem GPU).

### 5.1 Integração exata no Hermes (seam já mapeado no código)

1. **Seleção**: novo valor em `compositor_e` (`virtual_display.h:265-270`) +
   `sessionCompositor()` (`virtual_display.cpp:951-966`) já conhece `hyprland`.
2. **Backend de captura**: nova subclasse `display_hyprland_t` de
   `platf::display_t` (`platform/common.h:465-546`), selecionada na fábrica
   `platf::display()` (`misc.cpp:1072-1110`) antes dos ramos KMS/wlr —
   exatamente como `display_hermes_vram_t`/`display_hermes_ram_t` são
   curto-circuitados hoje (`misc.cpp:1074-1081`). Entrega frames como
   `egl::surface_descriptor_t` (DMA-BUF, fourcc, modifier, pitch/offset —
   mesmo shape que `display_hermes_vram_t::snapshot` produz em
   `kmsgrab.cpp:1800-1851`) ou como `img_t` CPU (modelo
   `display_hermes_ram_t`). VAAPI (`vram=true`) e CUDA/NVENC são
   reutilizados sem mudança.
3. **Output/modo**: criar headless output + `keyword monitor` via `hyprctl`
   (subprocesso ou socket hyprctl) no lugar de `createVirtualDisplay`/
   `activateVirtualDisplayOutput` (que viram no-op para Hyprland).
4. **Fallback não-nativo já existente e verificável hoje**: o backend `wlgrab`
   do Hermes (`src/platform/linux/wlgrab.cpp:118`) já escuta
   `zwlr_screencopy_manager_v1` **com `linux_dmabuf`** (output + cursor), e a
   implementação do Hyprland 0.56.2 envia `linux_dmabuf`
   (`src/protocols/Screencopy.cpp:92`, verificado na tag v0.56.2). Ou seja:
   `hyprctl output create headless` + captura `wlgrab` desse output pode
   **funcionar hoje com zero código novo** — vale testar antes de escrever o
   cliente ICC (a Fase 2 do plano começa por aí).
5. **Permissões**: o ICC passa pelo sistema de permissão de screencast
   (portal/xdp-hyprland) — configurar regra que autorize o Hermes sem prompt
   (mesma abordagem do sunshine-hyprland-virtual-display).
6. **Multi-sessão/input**: um headless output por sessão; input virtual
   continua via uinput + `71-hermes-session-input.rules` (independente).

### 5.2 Lacunas da Rota B

- Sem damage por frame (ICC não expõe `FB_DAMAGE_CLIPS`) — aceitável hoje: o
  Hermes já codifica o frame inteiro (`docs/compatibility.md`).
- Modo arbitrário do cliente remoto exige `hyprctl keyword monitor` por sessão
  (round-trip de config; funcional, mas fora da UAPI).
- Output headless é monitor de verdade (workspaces/focus) — regras de layout
  necessárias (o moreland já resolve isso).
- O output só redesenha com damage → o host precisa apresentar no ritmo alvo
  (nota do moreland sobre ~1 fps idle) — usar o mecanismo de "frame ready" do
  ICC ou um `present` no próprio Hyprland.
- **Plugins não são necessários**: a API de plugin (`invokeHyprctlCommand`)
  só reimplementaria o que o `hyprctl output create headless` + ICC já fazem.
- Correção do premise: `hyprland-virtual-desktops` (levnikmyskin) é plugin de
  *workspaces* (não cria monitores); "Hyprspace" (KZDKM) é overview de
  workspaces; não existe `hyprland_screencopy_v1` (o protocolo é
  `wlr-screencopy-unstable-v1`).

---

## 6. Rota C — Gamescope por sessão (fluxo existente, desbloqueia hoje)

O Hermes **já implementa** a alternativa de compositor dedicado por sessão
(`src/process.cpp`):

- perfil *application*: `gamescope --backend=drm -W <w> -H <h> -r <hz> -O
  <conector> -- <cmd>` (linhas 760-770); perfil *desktop*: `weston
  --backend=drm --drm-device=<card> --seat=<seat>` (779-787);
- sessão isolada: env próprio (`XDG_SEAT`, `LIBSEAT_BACKEND=seatd`,
  `SEATD_SOCK=/run/hermes-kms-seatd/<n>/seatd.sock`, `WLR_DRM_DEVICES`,
  `HERMES_SESSION_SEAT`, `HERMES_DRM_DEVICE`; 158-212);
- scanout-ready poll via `hermesKmsOpenCapture`/`hermesKmsCaptureSize`
  (815-854).

Com o Hyprland como desktop do **host**: instalar a regra udev de dev
(`99-hermes-kms-ignore-seat.rules`) para o Hyprland **não** adotar o card
(evita inclusive o wedge de flip), e cada sessão de streaming lança seu
Gamescope no seat próprio dirigindo `HERMES-N`. Captura no render node
continua igual. Nota: há duas gerações de nomeação de seat no código
(`hermes-kms-<N>` + seatd vs `seat-hermes-sN` da regra udev) — vale alinhar.

Vantagem: zero código novo; é o caminho de produção multi-sessão do projeto.
Desvantagem: o output virtual não vive no Hyprland (não atende "no Hyprland"
no sentido estrito) — é a **mitigação imediata**.

---

## 7. Rota D — upstream

1. **aquamarine**: (a) generalizar o caso display-only (probe EGL em vez de
   allowlist); (b) atacar a classe `Cannot commit when a page-flip is awaiting`
   (#353/#343/#240/#361, baseando-se no PR #312) — o hermes-kms é um
   reprodutor determinístico para os mantenedores.
2. **hermes-kms**: refactor vkms 7.x (§A.2.2) + `drm_crtc_enable_color_mgmt`
   (§A.3) + EDID libdisplay-info-valid + `DRIVER_RENDER` parametrizável.
3. **Hyprland**: nada, se A ou B funcionarem.

---

## 8. Plano priorizado sugerido

- **Fase 0 (hoje, zero código)**: Rota C para desbloquear streaming; atualizar
  `docs/compatibility.md` do Apollo-Linux.
- **Fase 1 (dias)**: instrumentar o wedge (Apêndice); aplicar o refactor
  vkms-like + propriedades KMS no hermes-kms (elimina classes de suspeitos e o
  spam de CTM/gamma mesmo se o wedge for do aquamarine).
- **Fase 2 (dias)**: protótipo da Rota B (headless + ICC) validado primeiro
  via `wlgrab`/`wlr-screencopy` dmabuf (fallback já existente), depois ICC;
  benchmark vs KWin (zero-copy RDNA2 de referência).
- **Fase 3 (semanas)**: produção da Rota B (permissões, modo dinâmico,
  multi-sessão) e decisão A-vs-B com dados de latência/custo.
- **Fase 4**: PRs upstream (Rota D).

## Apêndice — instrumentação do wedge

```bash
# kernel (exige root)
echo 0x1e | sudo tee /sys/module/drm/parameters/debug
sudo trace-cmd record -e 'drm:drm_vblank_event*' -e 'drm:drm_vblank_event_delivered' &
# Hyprland em TRACE (mostra pf event seq / stale pf / scheduler)
AQ_LOG_LEVEL=4 Hyprland 2>&1 | tee /tmp/hypr-trace.log
# repro
hermes-kmsctl --device /dev/dri/card0 hold 1280x720@60
# vblank saudável? (sem root; 60 vblanks/s = saudável)
gcc -O2 -o /tmp/vblank_probe /tmp/vblank_probe.c && /tmp/vblank_probe /dev/dri/card0 60
# após o congelamento: greps decisivos
grep -cE 'stale pf|no frame in flight' /tmp/hypr-trace.log   # >0 => evento chegou e foi rejeitado
grep -E 'pf event seq' /tmp/hypr-trace.log | tail            # >0 => evento entregue
# modetest isolado (sem Hyprland no card): regra udev dev + hotplug_events=0
sudo make install-dev-udev   # (uma vez, dev only)
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1 hotplug_events=0
modetest -M hermes-kms -s 40:1280x720@60 -v   # loop de flips; eventos chegam?
```
