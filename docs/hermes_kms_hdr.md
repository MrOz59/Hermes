# Hermes-KMS HDR10 integration

This implementation consumes Hermes-KMS UAPI v14 frame colour snapshots. It is
under validation; kernel and offscreen GPU tests are not a claim of completed
end-to-end HDR client/display certification.

## Pipeline

KWin composes full-range BT.2020 RGB with PQ transfer into the virtual output.
`ACQUIRE_FRAME2` pairs those samples with the committed colour state, static
metadata, format/modifier and producer fence. Hermes waits for that fence,
converts RGB to BT.2020 non-constant-luminance YUV, and uses a ten-bit encoder
profile. It does not decode and reapply PQ during the RGB-to-YUV matrix operation.

AMD uses the existing DMA-BUF/GL/VAAPI path. NVIDIA retains Hermes's existing CPU
copy of linear scanout, because NVIDIA's EGL import of these buffers samples the
wrong pages. The copied frame is uploaded from page-locked memory, and one CUDA
kernel decodes the packed ten-bit pixels, without truncating the RGB channels,
and writes P010 for NVENC; there is no OpenGL context or GL/CUDA interop on this
path. Ordinary eight-bit SDR to NV12 keeps its original CUDA kernel. NVIDIA/KWin
zero-copy development is deferred.

Both limited- and full-range P010 are supported. P010 samples occupy bits 15:6;
the low six bits remain zero. Shader arithmetic and sampling use high precision.
All four RGB2101010 channel layouts and padded CPU row strides are handled.

Colour and format changes trigger capture/encoder reinitialization before the
first changed frame is submitted. Static metadata keeps the DRM units and
unspecified zero values. Ten-bit storage alone never implies HDR. A legacy driver
cannot supply the new HDR contract; keep HDR disabled when using that driver.

## Setup and compatibility

For Gamescope launch modes, colour flow and the optional isolated-session
dependency patch, see [Gamescope HDR](gamescope_hdr.md).

Use the matching driver with `hdr_enable=1 color_depth=10`. Preserve
`initial_enabled=0` and the normal manual/on-demand output lifecycle. Enable HDR
for the virtual output in KDE and enable HDR in the client. This change does not
alter module autoload or desktop settings automatically.

The initial target is KDE/KWin with AMD VAAPI or NVIDIA NVENC and Moonlight/Hestia.
HDR requires the actual GPU/driver encoder to support HEVC Main10 or AV1 ten-bit,
and an HDR-capable client and display. General NVENC support does not imply HDR:
Maxwell can remain an SDR target, but does not provide HEVC ten-bit encoding.
Use runtime encoder capability probing rather than accepting an architecture
name as proof. See the [NVIDIA support matrix](https://developer.nvidia.com/video-encode-decode-support-matrix).

If the output is PQ but the client requests SDR, capture fails with an actionable
message instead of labelling PQ samples as SDR. Disable HDR on that virtual
output or enable HDR in the client. Tone mapping is not implemented here.
BT.2020 SDR input is also rejected until its matching conversion is supported.
The new CUDA upload converter supports NV12 and P010, not YUV 4:4:4.

Cursor composition preserves DRM premultiplied alpha and ten-bit primary pixels.
The cursor stream has no independent colour description, so it must be in the
same output encoding as the primary plane. Verify this with the target KWin
version; use compositor-rendered cursors if that contract is not met. An sRGB
cursor cannot be blended into PQ samples without colour conversion.

## Tests and release gates

`test_sunshine --gtest_filter='HermesColor.*:HermesCursorComposition.*:HermesScanout.*'`
covers metadata validation/lifetime, cursor pixels and capture startup.

The opt-in GPU test exercises the actual upload and conversion code with known
synthetic images; it does not capture the desktop or change display settings:

```sh
HERMES_HDR_TEST_RENDER_NODE=/dev/dri/renderDN \
  test_sunshine --gtest_filter='HermesHdrGpu.*'
```

It checks all four ten-bit layouts, padded strides, unpack state restoration,
gray ramps, saturated colours, limited/full range and P010 packing. Intermediate
values permit one ten-bit code of normalized texture-sampling variation; black,
white and neutral chroma are exact. Without the environment variable it skips.

The NVIDIA converter's arithmetic is covered on any machine by `CudaPixel.*`.
On an NVIDIA GPU, a CUDA build also runs the converter itself and a one-frame
HEVC Main10 encode, keeping the bitstream for `ffprobe`:

```sh
HERMES_HDR_TEST_CUDA=1 test_sunshine --gtest_filter='HermesHdrCuda.*'
```

Before release, validate live KDE HDR toggles, metadata-only updates, reconnects,
cursor appearance, multi-output isolation and sustained sessions. Inspect the
encoded profile, primaries, transfer, matrix, range and static metadata. Test HDR
ramps/reference white/near-black on real Moonlight and Hestia clients with HDR
screens. Driver VM and shader tests do not replace those measurements.

## KDE reconnects and client color mode

Before a shared KDE virtual-display launch or resume, Hermes matches that owned
output's HDR and wide-gamut settings to the incoming client's HDR request. It
waits for frame-associated metadata to confirm the new encoding before probing
encoders. This prevents KDE's remembered HDR setting from breaking a subsequent
SDR mobile session. Physical output color settings are not changed.

Encoder capability probes use synthetic images, including SDR/H.264 probes on
an HDR output. Their internal probe-only display cannot enter real capture.
Actual SDR sessions still reject PQ input if the compositor fails to switch;
this does not add tone mapping or relabel PQ pixels as SDR.

See [the performance investigation](hdr-performance.md) for real-session feedback
and the remaining latency/recovery issue.
