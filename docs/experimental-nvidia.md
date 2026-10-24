# Experimental NVIDIA capture

Branch: `experimental/nvidia-zero-copy`. This is development work, without a
claim of validated end-to-end zero-copy on NVIDIA hardware.

## First implementation

Host frames now upload with `cudaMemcpy2DToArrayAsync` on the conversion's
CUDA stream. The encoder keeps a shared reference until an upload-completion
event has signalled, preventing the capture pool from recycling the source
while DMA reads it. Stack-backed dummy frames still upload synchronously.
Teardown drains the stream before releasing the image and CUDA array.

The existing pinned allocations and mapping/copy caches are preserved. This
change removes the synchronous upload API from pooled frames; it does not
remove the CPU copy or promise overlap on pageable-memory fallbacks.

For an A/B comparison, launch Hermes with `HERMES_CUDA_ASYNC_UPLOAD=0` to use
the previous synchronous upload. Otherwise this branch uses asynchronous
uploads. Compare capture-to-bitstream latency and frame age, not just FPS.

## Opt-in DMA-BUF experiment

`HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF` selects the existing EGL/GL/CUDA
consumer for Hermes-KMS instead of forcing CPU-copy capture:

| Value | Route |
| --- | --- |
| unset / `0` | CPU copy (default, validated) |
| `1` | DMA-BUF import for every frame |
| `auto` | DMA-BUF import only for scanouts in an NVIDIA tiled layout; CPU copy otherwise |

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

Hermes-KMS's own dumb buffers are always linear and the driver never retiles,
so a scanout carrying an NVIDIA vendor modifier (block-linear 2D, Tegra
tiles: `modifier >> 56 == 0x03`) was necessarily allocated by an NVIDIA GPU.
That is the one case where the import is known not to be the system-memory
buffer NVIDIA has corrupted, and also a frame the CPU copy cannot read at all
(it would have to detile NVIDIA's layout). `auto` therefore imports exactly
those frames and copies everything else. The decision is made per display
selection from the latched framebuffer (`GET_STATUS`, no export), and a
running capture rebuilds itself through the normal reinit path when the
layout class changes, for instance when the compositor enters or leaves
direct scanout. Encoder probes keep the CPU path.

Without `auto`, the default CPU path now reports an NVIDIA-tiled scanout as
such, instead of a generic layout error, and names both remedies.

A tiled scanout only appears when the compositor is allowed to render
straight into the virtual output: KWin intersects the output plane's
`IN_FORMATS` with the render GPU's non-external-only modifiers when it
imports a primary-GPU buffer into a secondary device, and Hermes-KMS
advertises only linear unless `scanout_modifiers=` lists more. The
Hermes-KMS `hermes-gpu-scanout-test --list-modifiers` prints the NVIDIA
modifiers and a ready `scanout_modifiers=` value. A linear GPU-origin
buffer (KWin with the default linear-only list) still goes to the CPU copy
under `auto`: userspace cannot tell it from a dumb buffer, and on drivers
before 610 an NVIDIA-exported DMA-BUF cannot even be mapped. A future
Hermes-KMS frame flag for PRIME-imported framebuffers would close that gap.

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
   desktop.
4. `hermes-import-cache-check --gpu /dev/dri/renderDN`, then with
   `--modifiers linear` (no compositor or Hermes-KMS card needed).
5. Stream with `HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto`, then compare
   against `HERMES_CUDA_EGL_IMPORT_CACHE=0` and the CPU copy.

Multiple NVIDIA GPUs: the importer, FFmpeg's CUDA hwcontext and the
page-locked frames all use CUDA device 0, so select the GPU for all of them at
once with `CUDA_VISIBLE_DEVICES=<index from nvidia-smi -L>`. When
`adapter_name` names an NVIDIA GPU that is not CUDA device 0, Hermes now warns
with that remedy at importer start-up instead of capturing for one GPU and
encoding on another.

Remaining runtime steps: propagating `adapter_name` to the CUDA device itself,
a Hermes-KMS flag for PRIME-imported framebuffers so linear GPU-origin
scanouts can be routed too, and direct YUV output once the GPU-origin path
passes on the real host.

## References

- NVIDIA forum, "VRAM not reclaimed for sampled cross-process dma_buf imports
  in 595.71.05 driver" (also reproduced on 610.43.03).
- NVIDIA 610.43.02 release notes: `mmap()` of DMA-BUFs exported from discrete
  NVIDIA GPUs.
- Linux "dma-buf: ensure unique directory name for dmabuf stats" (64-bit
  dma_buf inode numbers) and EGL_EXT_image_dma_buf_import (EGL takes its own
  reference; the application closes the fds).
- "drm: define NVIDIA DRM format modifiers for GB20x" (sector layout bits
  26:27 for Blackwell 8/16-bpp surfaces).
- KWin `drm_egl_layer_surface.cpp`: multi-GPU import modes Egl, Dmabuf,
  LinearDmabuf, DumbBuffer, tried in that order.
