# Retomada: migração libvirtualhid antes do merge

Checkpoint histórico de 25/10/2026, Australia/Sydney. Em 26/10/2026 o mantenedor
incluiu libvirtualhid, PyroWave e isolamento Gamescope na meta do próximo release.
A integração atual está em `release/october-integration`; veja
[os critérios de promoção e a homologação restante](october-release-integration.md).
Os resultados abaixo preservam a evidência das rodadas anteriores.

## Biblioteca promovida; homologação do Hermes

Com autorização do mantenedor, a `master` de `MrOz59/libvirtualhid` avançou
de `38904f3f251c6387b4c77de54eb9ce2fc1e67bdf` para
`14ee6c4717d32b678c2983bfa6c54019b944d421` por fast-forward em 25/10/2026.
São os mesmos quatro commits já compilados e testados com GCC 13 e Clang 18,
incluindo os quatro testes PlayStation experimentais aprovados; os dois skips
dependentes de xpadneo/hid-playstation continuam registrados abaixo. A promoção
dispara novamente a CI na `master`; não cria uma release da biblioteca.

O pin do Hermes já aponta para esse SHA e permanece fixo. A main do Hermes
continua sem merge. O mantenedor dispõe de um DS4 e pode conseguir um DualSense
emprestado; o [roteiro de homologação](libvirtualhid-experimental.md#manual-validation-with-a-ds4-and-dualsense)
usa esse hardware para validar perfis PlayStation e Xbox, reconexão, rumble,
duas sessões, instalação e reversão do pacote. O segundo cliente pode começar
com teclado/mouse. Nenhum resultado manual é presumido pelo roteiro.

## Continuação: inicialização e liveness do broker

Esta rodada mantém o pin da biblioteca em `14ee6c4` e corrige uma falha antes
da validação com clientes reais: ENSURE podia demorar mais que os 15 s do
cliente e bloquear STATUS de outras sessões. Uma falha de RPC era interpretada
como sessão encerrada, derrubando streams durante o provisionamento de outro
cliente ou um reinício do broker.

- O daemon aceita conexões sem bloquear e delega pedidos e reconciliação a
  até 64 workers, reaped individualmente. Mutações continuam sob o mesmo lock
  validado; contenção pode esperar até dois minutos, arquivo inseguro falha
  imediatamente. Cada worker fecha o listener antes de executar comandos.
- O cliente usa I/O não bloqueante e um prazo único para connect/send/receive,
  inclusive backlog Unix cheio. Mutações têm 180 s; consultas continuam em 15 s.
  Respostas incompletas e UIDs parcialmente numéricos são recusados.
- `query_active` distingue ativo, inativo e indisponível. O monitor do stream
  mantém a sessão quando não há confirmação de parada. O daemon consulta o
  estado atual do systemd, sem copiar cache obsoleto entre workers.
- A reconciliação periódica passa de 500 ms para 2 s; o hotplug síncrono via
  udev continua imediato. A CI passa a compilar/testar o cliente RPC e o pool
  separadamente do host e exercita o daemon real com mapa/socket privados,
  sem criar contas nem dispositivos, antes dos testes com systemd.

Validação de `6700e5c835c58e091958084de72fcbdd7f4a7b83`:

- [35 testes independentes de RPC, workers, locks e ACLs, sem skips; três checks
  do daemon real e 58 checks com contas reais](https://github.com/MrOz59/Hermes/actions/runs/40157668651/job/121062412179).
- [Build completo, 77 testes do Hermes e 176 da biblioteca](https://github.com/MrOz59/Hermes/actions/runs/40157668651/job/121062440390).
- [32 testes de kernel e quatro de runtime/uinput](https://github.com/MrOz59/Hermes/actions/runs/40157668651/job/121065555329).

Os ajustes posteriores do harness também verificam STATUS de uma sessão ativa
enquanto ENSURE de outra aguarda o lock, e congelam o cgroup inteiro do broker
para impedir que workers auxiliem o teste de hotplug. A política de artefatos
tem testes próprios e compara também receita, imagem e flags do compilador;
mudanças nesses campos ou execução manual exigem build novo.

Compilação local com warnings-as-errors e testes de processos/lock aprovados.
Este ambiente nega sockets AF_UNIX; os testes de IPC executaram obrigatoriamente
na CI, sem permitir skip. Hestia, jogos reais, o container de produção e os
recursos dependentes de hid-playstation continuam pendentes. Os resultados
seguintes preservam checkpoints anteriores.

## Checkpoint anterior: ACLs automáticas e cache transacional

O lote atual implementa a tarefa de ACLs abaixo, anteriormente pendente. O
Hermes fixa `14ee6c4717d32b678c2983bfa6c54019b944d421` da feature branch do
nosso fork. Os hashes e resultados nas seções seguintes são históricos.

- START valida e persiste cliente/conta/UID/seat antes de conceder acesso;
  scopes só herdam o seat. Não há API de ACL aceitando caminho ou UID remoto.
- O broker descobre somente char devices virtuais com phys e dev_t corretos,
  revalida depois de abrir com O_NOFOLLOW e aplica ACL pelo fd. Máscaras ACL
  não podem ampliar os direitos efetivos de outro usuário/grupo.
- A regra 99-hermes-session-input aplica ACL via helper root antes do evento
  udev chegar ao SDL/libinput, usando um lock curto separado das operações
  demoradas de conta/systemd. Um loop de 500 ms reconcilia hotplug e reinícios.
  STOP/PURGE e falhas de launch encerram as unidades gerenciadas antes de revogar; uma
  falha retém o registro para retry e impede liberar a conta prematuramente.
- O libvirtualhid só confirma cache após submit bem-sucedido. Sensores/touchpad
  ativos com valores não finitos são rejeitados antes de conversões HID;
  botões seguintes continuam funcionando. A normalização de eixos é mantida.
- A CI ganha um job independente com o broker empacotado, systemd e duas
  contas reais, incluindo event/hidraw, SDL inicializado antes dos devices,
  hotplug, restart e revogação. SDL é identificado por seat no sysfs: seu nome
  pode ser normalizado pela biblioteca e não é uma chave de identidade.

Validação local: biblioteca e exemplos compilados com warnings-as-errors;
136 testes da biblioteca aprovados. Os testes puros de lease/identidade/ACL
passaram; o teste que escreve uma ACL de UID real fica explicitamente pulado
neste namespace, que mapeia apenas UID 0. Esse teste executou e passou na CI,
sem skip. O broker compila isoladamente com -Werror.

Validação remota concluída:

- [Build completo de sunshine/broker/testes e 62 testes do Hermes + 176 da biblioteca](https://github.com/MrOz59/Hermes/actions/runs/40151792958/job/121044822644),
  fontes em `8bef7dd19898dbf2e0cc469a7b7125b73d6d61dd`.
- [58 checks do broker com contas reais](https://github.com/MrOz59/Hermes/actions/runs/40153673474/job/121050309949),
  incluindo SDL iniciado antes dos controles e hotplug com o daemon pausado.
- [32 testes do adapter/kernel e quatro de runtime/uinput](https://github.com/MrOz59/Hermes/actions/runs/40153673474/job/121050328653).
  Todos os sete perfis passaram por 1000 lotes a 1000 Hz, sem SYN_DROPPED
  nem teclas vazando para outro seat; o rumble Xbox funcionou sob UID sem
  privilégios e sem hidraw.
- [CI da biblioteca com GCC 13 e Clang 18](https://github.com/MrOz59/libvirtualhid/actions/runs/40136434143).
  No GCC, 195/197 testes passaram; xpadneo e descoberta de filhos renomeados
  com hid-playstation ficaram explicitamente pulados por falta dos drivers.

O job de kernel usa um único udev para evitar disputas de chmod/chgrp entre
Ubuntu e Arch. O runner Azure não inclui hid_nintendo: a CI o compila dos
fontes upstream contra os headers do kernel em execução, sem retirar o teste
de botões Switch. Mudanças somente no harness/notas reutilizam artefatos de
um build aprovado após comparar todos os arquivos com o SHA compilado;
mudanças de fontes, testes compilados, dependência ou CMake sempre recompilam.

A validação de um desktop transmitido com Hestia e jogos reais, dos recursos
nativos dentro do container de produção e dos filhos específicos de
hid-playstation continua pendente. As seções abaixo preservam o histórico.

## Continuação de 25/10: descoberta e estado de input

O Hermes agora fixa `923531ab41d440a2b564f596c191b476f226598e` da feature
branch do nosso fork. As referências a `1831bc0` abaixo descrevem o checkpoint
anterior e sua validação, preservados para contexto.

- A biblioteca identifica uinput pelo sysfs do próprio handle. UHID exige o
  phys e o unique ID do controle, inclusive no desktop compartilhado. Isso
  evita misturar controles de mesmo nome/seat, aceitar metadados incompletos
  ou devolver nós de um handle fechado.
- O adapter não reserva slots de touchpad para NaN/inf, confirma alterações
  de ownership somente após envio bem-sucedido, permite release/cancel sem
  coordenadas válidas e evita colisões dos IDs de 7 bits após wrap.
- ENSURE/PURGE/START/STOP agora recusam mutações se não obtiverem um lock
  exclusivo e seguro. O broker rejeita symlink, hard link, FIFO e arquivo
  gravável por outro usuário/grupo, sem bloquear indefinidamente.
- A CI da biblioteca passa a executar nessa feature branch, com GCC/Clang e
  compilação adicional sem XTest. A CI do Hermes compila o broker e inclui
  seus testes de lock junto aos testes de input. O checkpoint anterior falhou
  antes dos testes de kernel porque o runner Azure não tinha o módulo UHID;
  o workflow agora instala os módulos extras do kernel e compartilha o /dev
  do guest descartável para enxergar também os dispositivos criados após
  iniciar o container. Confirmar essa preparação na próxima execução da CI.

Validação local deste lote: biblioteca e exemplos compilados com GCC 13,
XTest desabilitado e warnings-as-errors; 133 testes da biblioteca aprovados,
incluindo 62 do backend Linux; 5 testes do adapter usando os fontes reais e
runtime fake; 5 testes de lock aprovados; broker compilado com warnings-as-errors.
O arquivo completo de testes do adapter também passou na verificação de sintaxe.
Não foram executados aqui os testes com `/dev/uinput`, `/dev/uhid`, systemd ou
clientes reais. A compilação completa do Hermes e os testes de kernel ficam
para a CI; os resultados anteriores de VM não comprovam este novo lote.

**ACLs automáticas continuam pendentes.** Este lote corrige pré-requisitos de
identidade/serialização; não concede acesso a nenhum dispositivo. A próxima
tarefa autorizada continua sendo o broker registrar conta/UID/seat e conceder,
revalidar e revogar ACLs por dispositivo, inclusive após hotplug/reinício.

## Repositórios e branches

- Hermes: `https://github.com/MrOz59/Hermes.git`, branch
  `experimental/libvirtualhid-input`. Este documento acompanha o commit de
  checkpoint com as mudanças em andamento. A base anterior era `2aa2d4a1`.
- libvirtualhid: `https://github.com/MrOz59/libvirtualhid.git`, branch
  `feat/hermes-isolated-device-discovery`, commit publicado
  `1831bc0d5b43b23ba694d397c8a9d4900186521c`.
- O Hermes já fixa essa revisão da biblioteca em
  `cmake/compile_definitions/linux.cmake`; não depende de override local.
- A master da biblioteca recebeu anteriormente `38904f3` por autorização
  explícita. A mudança nova `1831bc0` está **somente na feature branch**.
- A main do Hermes inspecionada estava em `71c920fc`; era ancestral da feature,
  sem conflitos. Reconsultar os heads remotos antes de uma futura revisão.
- O checkout original desta máquina, `/home/ozzy/Projects/Apollo-Linux`, contém
  mudanças do usuário na branch de Bazzite. Foi preservado. O trabalho desta
  migração está no worktree `/home/ozzy/Projects/Hermes-libvirtualhid` e no clone
  `/home/ozzy/Projects/libvirtualhid-hermes`.

## Bloqueios encontrados na revisão

1. Xbox nativo prefere UHID. Sem acesso do jogo a hidraw, o SDL reconhecia o
   controle, mas perdia rumble básico, mesmo podendo abrir o event node. Isso
   foi reproduzido em VM com UID sem privilégios e importa para o compose, que
   intencionalmente não libera hidraw. Com uinput, rumble funcionava.
2. A CI anterior do Hermes falhava em dois `LinuxRuntimeTest`: o container de
   compilação não possuía `/dev/uinput` e `/dev/uhid`. O Hermes compilava e os
   18 testes do adapter passavam; não era falha de compilação.
3. As contas criadas pelo broker não pertencem a `input`. Os testes de seat
   como root não provam acesso dos jogos sob esses UIDs. O broker ainda não
   concede ACLs aos dispositivos da sessão.

## O que foi implementado neste checkpoint

### Biblioteca: validado e publicado

`CreateGamepadOptions::prefer_input_events` permite escolher uinput para
generic/Xbox mesmo com UHID disponível. Mantém família do controle e rumble
evdev; o perfil efetivo deixa de anunciar trigger rumble e bateria. No Linux,
PlayStation/Switch rejeitam esse pedido, pois não têm esse backend. O padrão
da biblioteca continua inalterado; backends de outras plataformas podem
ignorar a preferência, conforme a documentação da API.

Validação da revisão `1831bc0`:

- Build GCC com warnings-as-errors e exemplos `gamepad_adapter` e
  `keyboard_mouse_adapter` compilados. Os exemplos não foram executados no host.
- Suíte completa em VM: **197 testes, 196 aprovados, 1 skip** por ausência de
  xpadneo. Inclui rumble SDL de Xbox One/Series via uinput com UHID disponível,
  ausência de hidraw e rejeição de perfis incompatíveis.
- [CI GCC 13 e Clang 18 aprovada](https://github.com/MrOz59/libvirtualhid/actions/runs/40131803963).

### Hermes: compilado, testes novos ainda não executados

- Novo `linux_gamepad_backend = auto | native | uinput`, com UI/documentação.
- `auto`: generic/Xbox usam eventos/uinput; PlayStation/Switch usam HID nativo.
- `native`: deixa a biblioteca preferir HID nativo, preservando seus fallbacks.
- `uinput`: seleção automática usa Xbox Series; seleção manual de
  PlayStation/Switch é recusada e a UI avisa sobre o conflito.
- A criação de controles passa `prefer_input_events` à biblioteca.
- Novo consumidor SDL em `tests/consumers/linux_hid_rumble.cpp` e teste
  `XboxEventProfiles/VirtualHidEventKernelTest.*`: processo separado, UID 65534,
  grupos suplementares limpos, acesso ao próprio event node e negação do outro
  seat, com rumble real chegando à fila correta do adapter.
- **Esse teste ainda concede as ACLs manualmente com setfacl dentro da VM. Não
  confundir isso com implementação do broker em produção.**
- Workflow: unitários excluem os dois grupos dependentes de kernel; novo job
  `kernel` recebe os binários, carrega módulos no runner descartável e roda os
  testes reais em Arch/Docker privilegiado. Os smoke tests não foram apenas
  suprimidos: estão previstos no segundo job.
- O job novo e os testes novos do Hermes **ainda precisam ser executados**.
  Conferir particularmente udev/hotplug dentro do container e o guard de VM.

Antes da pausa, o build local de `sunshine`, `test_sunshine` e do consumidor SDL
terminou sem erros. A UI também compilou. Não houve tempo de rodar os testes
desse novo estado antes do pedido de parada. Os resultados anteriores de
**37 testes locais + 25 em VM** correspondem à base `2aa2d4a1`, não comprovam
este checkpoint.

## Próxima tarefa: ACL no broker — autorizada, NÃO implementada

O usuário respondeu explicitamente: **"Sim, implementar ACL por dispositivo e
sessão"**. Pode alterar o broker privilegiado para essa finalidade, sem adicionar
contas ao grupo input nem liberar todos os hidraw. Nenhum arquivo do broker foi
alterado ainda. O trabalho parou na análise do desenho de segurança.

Pontos de integração já localizados:

- `tools/hermes-session-broker.cpp`: serviço root pequeno, serial, protocolo
  por socket, autorização SO_PEERCRED + allow-file root-owned. START recebe
  ambiente; STOP/PURGE encerram contas/unidades. Há lock de arquivo para
  mutações de contas. Novas mutações de concessão/registro/revogação precisam
  de serialização e falha fechada se não puderem adquirir o lock.
- `src_assets/linux/misc/hermes-session-broker.service`: ProtectSystem=strict;
  ReadWritePaths=/etc /var/lib/hermes /run/hermes; capabilities limitadas a
  CHOWN/FOWNER/DAC_OVERRIDE. Não ampliar para grupos ou dispositivos globais.
- `src/process.cpp`: o ambiente do compositor já contém `XDG_SEAT` derivado de
  `runtime.seat_id`. O START inicial é feito via session_broker::start; os apps
  usam scopes ligados à unidade principal.
- `src/platform/linux/session_broker.{h,cpp}`: cliente do protocolo.
- `src/stream.cpp`: input::alloc recebe apenas o seat no caminho isolado.
- `src/platform/linux/input/virtualhid_gamepad.cpp`: após criar um controle,
  descobrir seus nós e pedir a concessão específica ao broker. Considerar
  também os dispositivos de input criados no contexto e os filhos tardios.

Desenho a concluir e testar, não tomar como código existente:

1. Registrar no START a associação cliente/conta/UID/seat validado, sem aceitar
   UID ou caminhos arbitrários vindos do cliente. Recusar colisão entre seats.
2. Conceder ACL apenas a char devices virtuais cujo sysfs comprove o phys/seat
   exato. Para UHID, caminhar até o HID pai e verificar HID_PHYS; para uinput,
   verificar phys. Recusar hardware físico, symlinks, caminhos fora das raízes
   permitidas, travessia e outro seat. Uma ideia era pedir INPUT pelo seat e
   deixar o broker descobrir os nós, sem receber caminhos/UIDs do solicitante.
3. Evitar TOCTOU: abrir com O_NOFOLLOW/CLOEXEC, validar fstat e identidade sysfs;
   alterar ACL pelo fd validado, não reabrir um pathname que pode ser reciclado.
   Se usar setfacl externo com /proc/self/fd, preservar somente esse fd no
   filho antes do exec e manter todos os demais CLOEXEC. Considerar dependência
   de runtime `acl` nos pacotes. Não implementar via shell concatenado.
4. Revogar no STOP, PURGE, falha de launch e reutilização do seat; lidar com
   reinício do broker. Encerrar os processos antes da revogação: ACL não revoga
   descritores já abertos. Não atingir nós de outra sessão ou reaproveitados.
5. Testes de entradas maliciosas e conta/seat errado; teste real com dois UIDs,
   incluindo rumble, hotplug, teardown e negação cruzada. Aproveitar o novo
   consumidor SDL, substituindo a concessão manual pela integração real.

`scripts/session-broker-test.sh` contém um teste end-to-end do broker com contas
e systemd reais. **Só executar em guest descartável**: instala unidades e mexe
nas bases de contas do guest. Há detalhes de virtme-ng/shadow documentados nele.

## Retomar em outra máquina

```bash
git clone --recursive --branch experimental/libvirtualhid-input https://github.com/MrOz59/Hermes.git
cd Hermes
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DSUNSHINE_ENABLE_CUDA=OFF
cmake --build build --target sunshine test_sunshine hermes-session-broker -j4
cd build/tests
./test_sunshine --gtest_filter='VirtualHidInputTest.*:ConfigConsistencyTest.*:LocaleConsistencyTest.*:DirectDevicePort.*:HestiaCapabilitiesSchema.*'
```

Para trabalhar também na biblioteca, clonar recursivamente a feature branch
indicada acima, ler seu AGENTS.md e usar diretórios `cmake-build-*`.

Com udev ativo e root **somente dentro da VM descartável**:

```bash
scripts/vm-isolated-input-test.sh /caminho/absoluto/Hermes/build/tests/test_sunshine
```

Nesta máquina foi usado virtme-ng (`vng`), kernel
`/usr/lib/modules/7.2.9-2-cachyos/vmlinuz`, memória **1G**, 2 CPUs e udevd iniciado
explicitamente. Selecionar um kernel disponível na nova máquina; não copiar
esse caminho cegamente. Os testes básicos usaram vng sem --systemd. O teste
completo do broker exige systemd e ainda não foi rodado nesta etapa.

Ao retomar, conferir a CI disparada pelo push do checkpoint. O objetivo segue
sendo tornar o merge seguro; **não declarar a migração pronta só porque o
build ou a concessão manual de ACL passaram**. Streaming Hestia ponta a ponta
também continua como validação de integração pendente; não há caneta física,
então usar os testes sintéticos.
