# Teste em VM: cursor com GNOME/Mutter + hermes-kms (CachyOS, kernel 7.1.8)

Resultados do teste empírico feito numa VM local (virtme-ng + qemu, kernel do
host `7.1.8-1-cachyos`, GNOME Shell/Mutter 50.4 dos repositórios CachyOS) em
2026-08-22. A VM replica a arquitetura da máquina do relator: GPU primária
(virtio-gpu-gl) + hermes-kms como card secundário.

## Como a VM foi montada

- Rootfs CachyOS+GNOME em `/home/ozzy/vm/cachyos-gnome/rootfs` (pacman com o
  mirror cachyos dentro de container docker; base + gnome-shell + mutter +
  libdrm + mesa + ydotool + cmake).
- Boot: `vng --run /lib/modules/7.1.8-1-cachyos/vmlinuz --root <rootfs> --rw
  --cpus 4 --memory 6G --systemd --disable-microvm --rwdir
  /host=/home/ozzy/vm/cachyos-gnome/share --qemu-opts="-display
  egl-headless,gl=on -device virtio-gpu-gl-pci" --exec
  /opt/hermes-test/run-gnome-test.sh`
- O script do guest (reproduzível em
  `scripts/vm-cursor-gnome-test.sh` do Hermes-KMS) monta o ambiente:
  insmod do uinput/virtio-gpu/hermes_kms → `hold 1280x720@60` → **sessão
  logind criada via `CreateSession`** (`mksession`, leader = o próprio
  script, type=wayland, vtnr=1) → `loginctl activate` → GNOME Shell
  (`gnome-shell --wayland --no-x11`, fallback `mutter --wayland`) → varredura
  do ponteiro com ydotool → probes.
- Armadilhas resolvidas e documentadas no script: root do virtme-ng fica
  read-only (`--rw` não chega ao virtiofsd) — resultados vão para `/host`
  via `--rwdir`; logind não inicia via systemctl no virtme (subir
  `/usr/lib/systemd/systemd-logind` em foreground); sessão do logind exige
  leader vivo (o PID do script) e `vtnr=1` no seat0.

## Resultado 1 — o Mutter NÃO usa o cursor plane do hermes-kms

Durante a varredura do ponteiro (ydotool cruzando o output virtual):

```
[change] primary_fb=45 cursor_fb=0 cursor_crtc=0 cursor_src=(0.0000, 0.0000)
[change] primary_fb=47 cursor_fb=0 cursor_crtc=0 cursor_src=(0.0000, 0.0000)
```

- `cursor_fb` permanece **0** o tempo todo → o Mutter nunca anexou framebuffer
  ao cursor plane do hermes;
- o **framebuffer primário alterna (45↔47)** durante o movimento — exatamente
  o padrão "42/44/47" que o relator observou na VM dele: o Mutter **desenha o
  ponteiro por software dentro do framebuffer primário** (repaints a cada
  movimento do cursor);
- `hermes-pixel-peek` confirmou que o scanout capturado é o FB real
  (1280x720 XR24, damage cheio, DMA-BUF exportável).

Conclusão para a pergunta aberta: neste ambiente, **os pixels do ponteiro
ESTÃO no framebuffer primário** (apareceriam no stream).

### Ressalva importante (por que isso não fecha o caso do relator)

O log do GNOME Shell mostra:

```
libmutter-Message: Failed to initialize accelerated iGPU/dGPU framebuffer sharing: Not hardware accelerated
```

O Mutter só consegue pôr o cursor no cursor plane de uma GPU **secundária**
quando o compartilhamento de framebuffer entre GPUs funciona. Na VM o
virtio-gpu renderiza por software (virgl/zink → "not hardware accelerated"),
então o compartilhamento falha e o Mutter cai para cursor por software. Na
máquina do relator (RX 7800 XT, radeonsi, compartilhamento acelerado) o
caminho pode **funcionar** — e aí o Mutter usaria o cursor plane do hermes, o
cursor sairia do framebuffer primário e o Apollo (que não lê o cursor plane)
o perderia no stream.

**Check decisivo na máquina do relator**: rodar
`tools/hermes-cursor-probe/hermes_cursor_probe --device /dev/dri/cardN`
(~10 s, mover o ponteiro sobre o display virtual):
- `cursor_fb != 0` mudando com o movimento ⇒ cursor plane em uso ⇒ cursor
  fora do stream (bug real; precisa do rastreamento de cursor plane no driver
  + envio da imagem via protocolo Moonlight);
- `cursor_fb == 0` com FB primário alternando ⇒ cursor por software ⇒ cursor
  no stream (o sintoma do relator teria outra causa).

## Resultado 2 — fix do mapeamento absoluto verificado (PASS)

O fix em `src/platform/linux/input/inputtino_mouse.cpp` (aplicar
`offset_x/offset_y` do touch port antes de escalar para a faixa ABS do
uinput) foi validado de ponta a ponta dentro da VM com um harness que cria o
mouse virtual do inputtino e lê os eventos emitidos
(`absmap_check`, 19200×12000 = constantes ABS do inputtino):

```
=== fixed math: virtual display at x=1024, env 2304x720, point (640,360) ===
emitted ABS=(13867,6000) expected=(13867,6000) old_buggy_ABS_X=5333
RESULT: PASS

=== regression: fullscreen 1:1 (offset 0, env=display 1280x720) ===
emitted ABS=(9600,6000) expected=(9600,6000)
RESULT: PASS
```

- Com o fix, o ABS emitido cai em x≈1664 do desktop (dentro do display
  virtual em x=1024..2304); o código antigo emitiria ABS_X=5333 → x≈640 do
  desktop → monitor físico. ✓ causa raiz do "ponteiro anda no host e não vai
  para o cliente" com input absoluto.
- Sem regressão no caso fullscreen 1:1.

## Artefatos

- `Hermes-KMS/tools/hermes-cursor-probe/` — probe do cursor plane (novo,
  no `make tools`);
- `Hermes-KMS/tools/hermes-pixel-peek/` — leitura de pixels do frame
  capturado via DMA-BUF mmap (novo, no `make tools`);
- `Hermes-KMS/scripts/vm-cursor-gnome-test.sh` — o teste completo da VM
  (reproduzível);
- `Apollo-Linux/src/platform/linux/input/inputtino_mouse.cpp` — fix do
  mapeamento absoluto (aplicado);
- VM em `/home/ozzy/vm/cachyos-gnome/` (rootfs + share/ com os resultados).

## Pendências/next steps

1. Rodar o `hermes-cursor-probe` na máquina do relator (check decisivo acima).
2. Se cursor plane em uso lá: implementar rastreamento do cursor plane no
   hermes-kms (FB + posição no `GET_STATUS`/`ACQUIRE_FRAME`) e o envio da
   imagem do cursor via protocolo Moonlight no Apollo (caminho `platform::cursor_t`
   já existe para outros backends).
3. (Menor) `hermes-pixel-peek`: corrigir o modo poll (WAIT_FRAME timeout →
   re-amostragem) que não emitiu linhas `[poll]` neste teste.
