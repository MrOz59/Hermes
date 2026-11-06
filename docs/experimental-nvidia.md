# Experimental NVIDIA capture

Branch: `experimental/nvidia-zero-copy`. This is development work, without a
claim of validated end-to-end zero-copy on NVIDIA hardware.

## Quick start for testers

Installing this branch does **not** enable NVIDIA DMA-BUF capture by itself.
There are separate improvements:

| Improvement | Default on this branch | How to confirm |
| --- | --- | --- |
| Asynchronous host upload on Hermes-KMS NVENC CPU-copy | Disabled by default after Blackwell regression testing; opt in with `HERMES_CUDA_ASYNC_UPLOAD=1` | `CUDA host upload: synchronous (backend default)` |
| EGL import cache and render-node importer | Enabled when a GL->CUDA capture consumer is used; not used by CPU-copy capture | `GL->CUDA DMA-BUF import cache: up to 4 buffers` |
| Experimental Hermes-KMS DMA-BUF capture with NVENC | Disabled until opted in | `Hermes-KMS NVENC auto route: scanout ... -> DMA-BUF import` during a real stream |

The `hermes.conf` settings remain:

```ini
capture = kms
encoder = nvenc
virtual_display_backend = hermes_kms
```

Keep your other settings, including the real GPU's `adapter_name`. The
`HERMES_*` switches below are **environment variables for the Hermes process**,
not settings to add to `hermes.conf`, the application launch command, or the
kernel module options.

### 1. Build the matching experimental packages

On Arch/CachyOS, install the kernel headers matching the running kernel and
the normal build dependencies first. From fresh checkouts:

```bash
git clone --branch experimental/nvidia-zero-copy --recurse-submodules https://github.com/MrOz59/Hermes.git Hermes-nvidia
cd Hermes-nvidia
makepkg -sfi
```

This branch's root PKGBUILD requires the CUDA toolkit and explicitly enables
Hermes' CUDA backend, failing the build if it is unavailable. A successful
`h264_nvenc`/`hevc_nvenc` probe alone does not prove that backend was compiled:
FFmpeg can encode with NVENC while Hermes converts/uploads frames through its
software fallback. Installing CUDA after building does not add the missing
code to an existing binary. Rebuild the package; custom CMake builds must use
`-DSUNSHINE_ENABLE_CUDA=ON -DCUDA_FAIL_ON_MISSING=ON` too.

Build the driver in a separate terminal or directory:

```bash
git clone --branch experimental/nvidia-zero-copy https://github.com/MrOz59/Hermes-KMS.git Hermes-KMS-nvidia
cd Hermes-KMS-nvidia
makepkg -sf
sudo pacman -U ./hermes-kms-dkms-git-*.pkg.tar.zst
make tools/hermes-kmsctl/hermes-kmsctl
```

This experimental driver's PKGBUILD now explicitly fetches
`experimental/nvidia-zero-copy`. Older copies fetched `main` even when the
outer checkout was experimental. Pull the updated PKGBUILD before rebuilding.
Outside Arch, use `sudo make dkms-install` from the experimental driver
checkout, with DKMS and matching kernel headers installed.

**Reboot after replacing an already loaded module.** Installing a new DKMS
package does not replace the module currently in use by the compositor. After
rebooting, from the driver checkout, verify the **loaded** interface:

```bash
./tools/hermes-kmsctl/hermes-kmsctl version
```

Expect `uapi_version=15` for this experimental driver. UAPI 15 is needed for
`auto` to recognise NVIDIA-exported **linear** buffers. Older interfaces can
still route NVIDIA-tiled buffers by modifier, but keep an unknown linear
buffer on the CPU copy. The NVIDIA graphics driver and Hermes-KMS are separate
drivers; please include the NVIDIA version from `nvidia-smi` in reports.

Run the matching driver's full-frame and import-cache validation tools before
opting into the DMA-BUF consumer; see [GPU-origin producer now available](#gpu-origin-producer-now-available)
and the [companion PR's hardware steps](https://github.com/MrOz59/Hermes-KMS/pull/15).
The startup session assessment only reports export capability. It does not
establish which capture path a future stream will select.

### 2. Enable the experiment in the systemd user service

Run these commands as the desktop user, **without sudo**:

```bash
systemctl --user edit hermes.service
```

Add this override, save and close the editor:

```ini
[Service]
Environment="HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto"
```

Then restart Hermes and inspect its configured environment:

```bash
systemctl --user daemon-reload
systemctl --user restart hermes.service
systemctl --user show hermes.service -p Environment
journalctl --user -u hermes.service -b -f
```

An `export` in your terminal does not change a service systemd already starts
independently. If `HERMES_KMS_FORCE_CPU_COPY=1` appears in a service override,
environment file or manager environment, remove it or set it to `0`: it wins
over `auto`. `systemctl --user cat hermes.service` shows service overrides.

For a manual launch instead, stop the service first so two hosts do not bind
the same ports:

```bash
systemctl --user stop hermes.service
env HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto HERMES_KMS_FORCE_CPU_COPY=0 /usr/bin/hermes
```

### 3. Check a real streaming session

Connect a client and inspect the log **after `CLIENT CONNECTED`**. Encoder
availability probes use dummy images and are not evidence of the active route.

| Log during the stream | Meaning |
| --- | --- |
| `auto route: scanout ... -> DMA-BUF import`, followed by `Hermes-KMS zero-copy capture ready` | The experimental import path was selected; still check moving content across the whole image and memory use |
| `auto route: scanout ... -> CPU copy` | `auto` is enabled, but this scanout does not prove NVIDIA origin; this fallback is intentional |
| `(origin not reported; Hermes-KMS predates UAPI 15)` with a linear modifier | Verify/rebuild the experimental driver and reboot |
| `(Hermes-KMS dumb buffer) -> CPU copy` | The compositor supplied system-memory storage; enabling the switch cannot change its allocation |
| `Hermes-KMS CPU-copy capture path selected` with no auto-route line | Expected during probes; during the real stream, check the variable, force-copy override and installed binary |
| `Hermes was built without its CUDA capture/conversion backend` | Rebuild Hermes with CUDA enabled; NVENC encoding alone is insufficient |

Start with the default module modifiers. UAPI 15 can recognise a linear buffer
exported by NVIDIA; tiled modifiers are not required for that case. If KWin
keeps choosing a dumb buffer, use the GPU producer's `--list-modifiers` and
validation procedure below before changing `scanout_modifiers=`. Do not use
`HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=1` just to bypass a CPU-copy result: it
also imports the system-memory buffers associated with the corruption.

The `[capture-metric/hermes-kms-cpu]` values measure frame acquisition/export,
not the later CPU copy, CUDA upload or complete encode latency. They cannot
establish an end-to-end performance gain by themselves.

For a report, include the updated Hermes commit, loaded Hermes-KMS UAPI,
NVIDIA and KWin versions, auto-route/capture-path lines, checker verdicts and
whether moving content, resize and reconnect work. Keep subjective smoothness
separate from measured latency, CPU usage and VRAM over time.

### 4. Disable or compare one improvement at a time

Edit the same service override to set
`Environment="HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=0"`, then daemon-reload
and restart as above. This disables this consumer experiment without removing
your other service customisations. Keep the default linear module modifiers
for a CPU-copy baseline; a CPU copy cannot detile an NVIDIA-tiled scanout.

For upload A/B testing, keep DMA-BUF mode at `0`. Hermes-KMS NVENC
CPU-copy now defaults to synchronous uploads; set
`Environment="HERMES_CUDA_ASYNC_UPLOAD=1"` to opt in to async and `0`
to explicitly force synchronous uploads. Without the variable the backend
default is used. For cache
A/B testing while DMA-BUF import is active, add
`Environment="HERMES_CUDA_EGL_IMPORT_CACHE=0"`; remove it or set it to `4`
to restore the default. Reload/restart after each change. If the import-cache
checker reports stale content while fresh imports pass, keep the cache at `0`.

## Updated Blackwell retest: the LRU eliminated CPU-copy faults (2026-11-06)

The same RTX 5060 Ti / NVIDIA Open 615.78.08 tester retested Hermes
`a5ef038f` with Hermes-KMS `dd9f96a`, UAPI 15, 2266x1488@60,
same fullscreen scene for 60 seconds per mode. The compositor still
selected a KWin dumb buffer and the NVENC auto route remained CPU-copy.

| | Original sync | New sync | Original async | New async |
| --- | ---: | ---: | ---: | ---: |
| Hermes CPU (% of one core) | 33.8% | **10.5%** | 78.5% | 49.5% |
| Capture-thread minor faults/s | ~201k | **0** | ~399k | ~198k |
| Faults during DMA-BUF memcpy | not measured | **0** | not measured | **0** |

The new driver LRU delivered 30,927 export hits vs 6 misses; Hermes's
persistent mmap cache got 120 hits / 0 misses per diagnostic window.
The sync CPU copy now takes ~1.16 ms/frame with 0 reported stalls.
The measured sync process CPU reduction is ~69%. This **validates the
LRU fix on this hardware**, not GPU-origin streaming zero-copy.

The async-only ~198k minor faults/s are nearly exactly
`ceil(2266*1488*4 / 4096) * 60 = 197,580`: one page per host
upload frame per refresh. No faults occurred inside `copy_rows`,
despite faults accumulating on the thread. Kernel-side mapping or
pinning by the CUDA driver is a plausible explanation, **not yet
confirmed**; the tester could not attribute them to a kernel symbol.
This is not a second DMA-BUF read fault. We must not change memory
pinning or buffer lifetime on that assumption alone.

### New CUDA upload telemetry

Set the following environment variables in the **Hermes user service**
for a repeatable two-pass test (default is off, no production overhead):

```ini
[Service]
Environment="HERMES_KMS_CPU_COPY_DIAGNOSTICS=1"
Environment="HERMES_CUDA_UPLOAD_DIAGNOSTICS=1"
Environment="HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto"
```

First run with `HERMES_CUDA_ASYNC_UPLOAD` **unset** for the proven
synchronous path, then explicitly set it to `1` to enable async.
Restart Hermes between runs. Look for
`[cuda-upload-metric]` every 120 upload frames:

- `avg_submit_us` / `peak_submit_us`: host-side duration of
  `sws.load_ram`, which calls the CUDA transfer API. This is
  **submission time**, not total completion time for async DMA.
- `submit_minor_faults_per_frame`: faults attributed to the
  upload-calling thread *during submission* (if `RUSAGE_THREAD`
  is supported), not the full thread's per-frame fault total.
- `avg_previous_upload_wait_us`: time spent waiting for an earlier
  async transfer to complete. This is divided by the number of
  submitted frames (includes frames with no pending wait).
- `host_alloc_delta` / `host_free_delta` and totals: successful
  `cuMemHostAlloc` / `cuMemFreeHost` operations. These counts tell
  whether the capture pool is churning; they do **not** establish the
  cause of faults inside the NVIDIA driver.

Correlate with `/proc/<pid>/task/<tid>/stat` and a kernel-symbol
profile if available. Faults outside `sws.load_ram` remain outside
these per-submission counters.

### System-memory DMA-BUF import remains unsafe

`hermes-sysmem-import-check --verify` on 615.78.08/Blackwell reported
wrong GPU readback after byte 64 with both 4 KiB pages and THP;
`eglDestroyImage` then crashed inside NVIDIA EGL. Earlier failures
started beyond ~2 MiB, so the byte pattern changed but the route is
**still not valid**. Do not force import of Hermes-KMS dumb buffers
with `HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=1`.

For the compositor-side root cause and a proposed direct PRIME
scanout design, see the companion driver's
[experimental KWin 6.8 research](https://github.com/MrOz59/Hermes-KMS/blob/experimental/nvidia-zero-copy/docs/kwin-nvidia-prime-scanout.md).

## Hardware findings and fallback fixes (2026-11-06)

RTX 5060 Ti (GB206), open NVIDIA driver 615.78.08, KWin 6.8.2:
whole-frame GPU-origin DMA-BUF import, GL/CUDA processing, teardown and
117 cached-image coherence checks passed. **These tests do not establish
end-to-end zero-copy in a live stream.** KWin still chose a Hermes-KMS
dumb buffer even with all 12 NVIDIA tiled modifiers advertised. KWin's
multi-GPU scanout copy path requires a compatible destination rendering
device, while Hermes-KMS exposes a capture/control render node, not EGL/GBM
rendering. Investigate compositor-side passthrough separately.

During 2266x1488@60 streaming, async CUDA host uploads used 78.5% of a
CPU core versus 33.8% for sync, with roughly 399k versus 201k minor
faults/s. Most faults were in DMA-BUF `copy_rows`, outside the
ACQUIRE_FRAME capture metric. Restore synchronous upload as the
Hermes-KMS NVENC fallback default, retaining `HERMES_CUDA_ASYNC_UPLOAD=1`
for controlled tests. Other CUDA backends retain their existing default.

The matching experimental Hermes-KMS driver now caches four recent
DMA-BUF exports per plane. Previously only the most recently exported
BO was retained, so alternating A/B/C scanout buffers could continually
change DMA-BUF identities, defeating Hermes's persistent mmap cache.
This is a likely factor in the observed faults, not a hardware-confirmed
performance improvement yet.

For evidence, set `Environment="HERMES_KMS_CPU_COPY_DIAGNOSTICS=1"`
in `hermes.service` and restart the user service. Every 120 frames,
`[capture-metric/hermes-kms-cpu-copy]` now reports average and peak memcpy
duration, thread minor faults *during memcpy* (where RUSAGE_THREAD is
available), and mmap-cache hits/misses. This excludes fence waits,
DMA_BUF_IOCTL_SYNC, CUDA upload and encoding. Compare with
`frame_export_hits`, `frame_export_misses`, `framebuffer_id` and
`framebuffer_generation` in driver `hermes_kms_stats` debugfs. If both
caches hit but faults persist, investigate page-table invalidation
before changing the kernel's page-pinning behavior.

GPU-origin passthrough and NVIDIA import of **system-memory** buffers
remain unvalidated in a live stream. Do not force
`HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=1` as a workaround.

## First implementation

Host frames now upload with `cudaMemcpy2DToArrayAsync` on the conversion's
CUDA stream. The encoder keeps a shared reference until an upload-completion
event has signalled, preventing the capture pool from recycling the source
while DMA reads it. Stack-backed dummy frames still upload synchronously.
Teardown drains the stream before releasing the image and CUDA array.

The existing pinned allocations and mapping/copy caches are preserved. This
change removes the synchronous upload API from pooled frames; it does not
remove the CPU copy or promise overlap on pageable-memory fallbacks.

For an A/B comparison, launch Hermes with `HERMES_CUDA_ASYNC_UPLOAD=1`
to opt in to asynchronous uploads, or `0` to force synchronous.
Without an override Hermes-KMS NVENC CPU-copy defaults to synchronous;
other CUDA capture backends are unchanged. Compare total process CPU,
per-thread minor faults and capture-to-bitstream latency, not just FPS
or the ACQUIRE_FRAME-only capture metric.

## Opt-in DMA-BUF experiment

`HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF` selects the existing EGL/GL/CUDA
consumer for Hermes-KMS instead of forcing CPU-copy capture:

| Value | Route |
| --- | --- |
| unset / `0` | CPU copy (default, validated) |
| `1` | DMA-BUF import for every frame |
| `auto` | DMA-BUF import only for scanouts an NVIDIA GPU allocated (NVIDIA tiled layout, or reported as imported from NVIDIA by Hermes-KMS UAPI 15); CPU copy otherwise |

It is explicitly opt-in because system-memory imports have produced
corruption beyond about 2 MiB and driver teardown crashes on a tested NVIDIA
stack.

Use only on a test session after running the full-frame checker from the
matching experimental Hermes-KMS branch. The current CUDA backend selects
device 0; start with one NVIDIA GPU. `HERMES_KMS_FORCE_CPU_COPY=1` takes
precedence. A failed DMA-BUF initialization does not silently switch to a
physical monitor. Unset the experimental variable to restore the default.

This route still converts into GL Y/UV targets and copies them into the CUDA
encoder frame. It can avoid host access to pixels only when the compositor
also supplies GPU-resident buffers. KWin may still perform a CPU readback
before Hermes sees the scanout. A linear modifier alone does not prove
otherwise; an NVIDIA vendor modifier does (see below).

### `auto`: route by what the scanout must be

`auto` imports exactly the scanouts an NVIDIA GPU must have allocated and
copies everything else. Two things prove that:

- **An NVIDIA vendor modifier** (block-linear 2D, Tegra tiles:
  `modifier >> 56 == 0x03`). Hermes-KMS's own dumb buffers are always linear
  and the driver never retiles, so such a frame cannot be the system-memory
  buffer NVIDIA has corrupted. It is also a frame the CPU copy cannot read at
  all, since it would have to detile NVIDIA's layout.
- **Hermes-KMS reporting the buffer as imported from NVIDIA** (UAPI 15,
  `HERMES_KMS_CAP_FRAME_ORIGIN`). The driver knows whether the compositor
  handed it a PRIME-imported buffer or one of its own dumb buffers, and names
  the exporting module. A linear buffer exported by `nvidia_drm` is the same
  same-GPU import every NVIDIA Wayland compositor performs, not the
  system-memory import. A buffer imported from another vendor (a hybrid laptop
  compositing on the iGPU) is not, and keeps the CPU copy.

The decision is made per display selection from the latched framebuffer
(`GET_STATUS`, no export), and a running capture rebuilds itself through the
normal reinit path when the class changes: when the compositor enters or
leaves direct scanout, or KWin falls back from `LinearDmabuf` to a dumb buffer
with the same linear modifier. Encoder probes keep the CPU path. The log names
both inputs, for example `scanout modifier 0x0 (imported from nvidia_drm) ->
DMA-BUF import`.

Without `auto`, the default CPU path now reports an NVIDIA-tiled scanout as
such, instead of a generic layout error, and names both remedies.

A tiled scanout only appears when the compositor is allowed to render
straight into the virtual output: KWin intersects the output plane's
`IN_FORMATS` with the render GPU's non-external-only modifiers when it
imports a primary-GPU buffer into a secondary device, and Hermes-KMS
advertises only linear unless `scanout_modifiers=` lists more. The
Hermes-KMS `hermes-gpu-scanout-test --list-modifiers` prints the NVIDIA
modifiers and a ready `scanout_modifiers=` value. With the default linear-only
list, KWin can still scan out a linear buffer it rendered on the NVIDIA GPU.
On a UAPI 15 driver `auto` imports that too. On an older driver it goes to the
CPU copy, because userspace cannot tell it from a dumb buffer there, and on
NVIDIA drivers before 610 an NVIDIA-exported DMA-BUF cannot even be mapped.

### One import per scanout buffer

The consumer used to create and destroy an EGLImage and texture for every
captured frame. It now keeps the imports of the buffers the compositor
rotates through, keyed by the dma_buf identity of every plane (`st_dev`,
`st_ino`) plus width, height, format, modifier, pitches and offsets
(`src/platform/linux/dmabuf_import_cache.h`). Since Linux 5.19 (and 5.15.y)
dma_buf inode numbers come from a 64-bit counter that is never reused, so a
stale entry can only miss. Hermes-KMS returns the same dma_buf for a
framebuffer on every acquire, PRIME-imported buffers included.

Besides removing per-frame `eglCreateImage`/`eglDestroyImage` work, this
bounds a driver problem that matches capture exactly: on NVIDIA 595.71.05
through at least 610.43.03, sampling a DMA-BUF that *another process*
allocated leaves importer VRAM behind after the image is destroyed, reclaimed
only at process exit (reportedly up to ~2 GiB). Per-frame imports keep feeding
that pool; one import per buffer does not.

The cache holds four buffers by default. `HERMES_CUDA_EGL_IMPORT_CACHE=N`
(0..16) changes that, and `0` restores per-frame imports for A/B tests. The
destructor logs hits, imports and evictions at debug level. Whether a cached
image observes what the compositor rendered after the import is a driver
property: run Hermes-KMS's
[`hermes-import-cache-check`](https://github.com/MrOz59/Hermes-KMS/blob/experimental/nvidia-zero-copy/tools/hermes-import-cache-check/README.md)
`--gpu /dev/dri/renderDN` on the target driver and keep
`HERMES_CUDA_EGL_IMPORT_CACHE=0` if it reports stale content. It needs no
Hermes-KMS card or compositor: a forked producer renders a known pattern with
a new seed into its own DMA-BUFs, and the consumer checks every pixel of the
imports it keeps, and of fresh imports, after every render. Its `--churn N`
replays the old per-frame pattern so the VRAM retention can be watched with
`nvidia-smi`. (A reported, unconfirmed explanation of that retention is a
private copy made on first sample, which is exactly what would make kept
images stale; hence the check.)

### Render node for the importer

The GBM/EGL importer is now created on the CUDA GPU's render node, with the
primary node only as a fallback when GBM/EGL cannot start there. Importing
and rendering need no KMS, and opening an unowned primary node implicitly
makes the opener DRM master, which can take the card from a greeter or
compositor that starts after Hermes (pre-login hosting). The node is opened
`O_CLOEXEC`.

## Driver versions

Hermes logs the loaded module when the GL->CUDA importer starts
(`GL->CUDA importer: NVIDIA kernel module 610.43.03 (open kernel module)`),
and the Hermes-KMS validation tools print the same line. Results are only
comparable on the same driver:

- NVIDIA's 2026-09-30 security bulletin fixed DMA-BUF import, DMA mapping and
  mapping-lifetime bugs in the open and proprietary kernel modules. The fixed
  Linux releases per branch are 580.178.04, 595.91.07, 610.57.04 and
  615.71.09.
- The VRAM retention the import cache bounds was reported on 595.71.05
  through 610.43.03, all older than those fixes. Whether it, the ~2 MiB
  system-memory corruption or the teardown crash survive on a fixed driver is
  unknown. Run the hardware steps on one driver from before the fixes and one
  after, and do not drop the cache or the CPU-copy default on the strength of
  either alone.
- CVE-2026-47601 is specifically about read-only buffers imported from another
  device's exporter. Hermes-KMS exports its dumb buffers read-write and, for a
  PRIME-imported scanout, hands back the compositor's own DMA-BUF unchanged, so
  that one fix should not change these imports. The rest of the bulletin
  touches the same paths and might.

## Validation and next steps

The standalone regressions run without a GPU:

```sh
for test in cuda-upload-lifetime dmabuf-import-cache hermes-nvenc-route; do
  c++ -std=c++17 -Wall -Wextra -Werror -pedantic -I. \
    tests/standalone/$test.cc -o /tmp/$test && /tmp/$test
done
```

They cover the upload lease, the import cache's identity/LRU/ownership rules
(memfds stand in for dma_bufs) and the `auto` routing table, including the
GB20x sector-layout bits of NVIDIA block-linear modifiers.

Branch CI also compiles the CUDA-enabled host. Neither a headless build nor
the regressions establish hardware correctness or performance.
Real-host checks still need SDR/HDR, padded pitches, resize, repeated
reconnection, multi-client teardown, and a game near GPU saturation.

## GPU-origin producer now available

The matching Hermes-KMS experimental branch includes
`tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test`. It allocates a modified
GBM BO on an explicit GPU, renders a page-varying pattern without CPU pixel
upload, imports it into an unused Hermes-KMS card, and requires capture to
return the original DMA-BUF identity and layout. It checks every source and
returned pixel, including the suffix beyond 2 MiB, and performs real GL/EGL
teardown. It does not change the compositor or Hermes capture defaults.

Follow the [producer's build/run instructions](https://github.com/MrOz59/Hermes-KMS/blob/experimental/nvidia-zero-copy/tools/hermes-gpu-scanout-test/README.md).
Its `--hold-seconds` and private `--session-file` let the CUDA-enabled
`hermes-egl-import-check --expect-gpu-pattern --full-teardown` inspect the same static
GPU-origin source. Use the same explicit NVIDIA render node for both. The
producer owns the output; do not launch another compositor that tries to own
that card while this diagnostic is running.

Passing the producer establishes storage passthrough and known-pattern pixel
correctness. Passing the CUDA checker adds GL/CUDA image and lifecycle checks.
Neither proves physical VRAM residency, NVENC encode/decode or KWin's choice
of scanout allocation. Hardware results are still pending.

Suggested order on an NVIDIA host:

1. `hermes-gpu-scanout-test --list-modifiers` to see what the GPU can render
   and what Hermes-KMS advertises.
2. `hermes-gpu-scanout-test` with `--modifiers egl`, then `plane`, then
   `linear` (the latter two mirror what KWin can allocate under the current
   `scanout_modifiers=`), each followed by the CUDA checker.
3. Load Hermes-KMS with the suggested `scanout_modifiers=`, start KWin, and
   confirm `hermes-egl-import-check` reports an NVIDIA modifier for the live
   desktop. On a UAPI 15 driver it also prints the origin: repeat with the
   default linear-only list and check whether KWin's linear scanout is
   `imported from nvidia_drm` (routed to the import by `auto`) or a
   `Hermes-KMS dumb buffer` (kept on the CPU copy).
4. `hermes-import-cache-check --gpu /dev/dri/renderDN`, then with
   `--modifiers linear` (no compositor or Hermes-KMS card needed).
5. Stream with `HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto`, then compare
   against `HERMES_CUDA_EGL_IMPORT_CACHE=0` and the CPU copy.

Every report should carry the driver line the tools and Hermes print; see
[Driver versions](#driver-versions) for why one run on each side of the
2026-09-30 fixes matters.

Multiple NVIDIA GPUs: the importer, FFmpeg's CUDA hwcontext and the
page-locked frames all use CUDA device 0, so select the GPU for all of them at
once with `CUDA_VISIBLE_DEVICES=<index from nvidia-smi -L>`. When
`adapter_name` names an NVIDIA GPU that is not CUDA device 0, Hermes now warns
with that remedy at importer start-up instead of capturing for one GPU and
encoding on another.

Remaining runtime steps: propagating `adapter_name` to the CUDA device itself,
and direct YUV output once the GPU-origin path passes on the real host. The
importer finds its node from the CUDA device's PCI address, so it can never
open the Hermes-KMS render node; a future Vulkan import path (Sunshine
upstream now encodes through Vulkan Video from DMA-BUFs, with Vulkan compute
for RGB->YUV) would have to select its device the same way. Waiting on the
frame's sync_file on the GPU (an EGL native fence) instead of in Hermes's
capture thread is a separate follow-up that also affects VAAPI.

When measuring latency on KDE Plasma 6.8, note the KWin version and whether
triple buffering is active: 6.8 enables it by default on NVIDIA, which can
shift frame age independently of anything Hermes changes.

## References

- NVIDIA forum, "VRAM not reclaimed for sampled cross-process dma_buf imports
  in 595.71.05 driver" (also reproduced on 610.43.03).
- NVIDIA GPU display driver security bulletin, 2026-09-30 (CVE-2026-47601,
  CVE-2026-47599, CVE-2026-47489 among others; fixed Linux releases
  580.178.04, 595.91.07, 610.57.04, 615.71.09).
- Hermes-KMS UAPI 15, `HERMES_KMS_CAP_FRAME_ORIGIN`: `STATUS_FRAMEBUFFER_IMPORTED`,
  `FRAME_IMPORTED` and the exporter name.
- NVIDIA 610.43.02 release notes: `mmap()` of DMA-BUFs exported from discrete
  NVIDIA GPUs.
- Linux "dma-buf: ensure unique directory name for dmabuf stats" (64-bit
  dma_buf inode numbers) and EGL_EXT_image_dma_buf_import (EGL takes its own
  reference; the application closes the fds).
- "drm: define NVIDIA DRM format modifiers for GB20x" (sector layout bits
  26:27 for Blackwell 8/16-bpp surfaces).
- KWin `drm_egl_layer_surface.cpp`: multi-GPU import modes Egl, Dmabuf,
  LinearDmabuf, DumbBuffer, tried in that order.

