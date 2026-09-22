# HDR performance investigation

The September 2026 development build has user-reported working KDE HDR on a TV,
working exclusive output switching on TV/mobile, and successful HDR/SDR toggles
across the tested application paths. This is functional feedback, not latency,
colorimetric or cross-device certification. Isolated GBM presentation remains a
separate unverified path.

## Observed timing and recovery

Sanitized summaries from verbose host logs on an NVIDIA GTX 1060 6 GB:

| Session | Requested bitrate | Duration | Host processing average* | Peak | Client IDR requests |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1440p60 HDR | 75 Mbps | 126 s | 13.11 ms | 29.5 ms | 286 |
| 1440p60 SDR | 75 Mbps | 311 s | 6.87 ms | 16.8 ms | 23 |
| 1080p120 SDR | 28 Mbps | 184 s | 3.68 ms | 5.9 ms | 1 |

\* Arithmetic mean of periodic window averages, not a frame-weighted mean or
percentile. These are different recorded sessions, not a controlled identical
scene comparison. IDR counts include the initial stream request.

The HDR session repeatedly needed recovery; its median interval between IDR
requests was 0.24 seconds. Other HDR sessions had fewer requests, so bitrate alone
does not explain the failures. SDR at 1440p also requested recovery. The user
reported client-side network drops, but the available host logs do not distinguish
missing packets from late frames or decoder-requested refreshes. Moonlight's
VideoDepacketizer requests IDR for decoder recovery as well as lost reference
chains. The host did not record loss-stat payloads for these sessions.

## Code paths and limits of existing metrics

The measurements above were taken on the path as it was when #45 was written.
Two of the stages it describes have changed since; the numbers below are from a
real Hermes-KMS buffer on a Ryzen 7 5700X, reading a frame the CPU had not
cached, as it would be right after the compositor wrote it.

- The CPU copy mapped the whole scanout DMA-BUF for every frame. That memory is
  mapped one 4 KiB page per fault, so at 1440p the copy cost 6.5 ms, about 5 ms
  of it 3,600 page faults, and at 4K 21.5 ms. Mappings are now kept across
  frames and the copy runs in 2 MiB blocks: 1.5 ms at 1440p, 3 ms at 4K.
- NVIDIA ten-bit HDR used a RAM upload to GL, RGB-to-P010 shaders, CUDA/GL
  resource mapping and plane copies on the CUDA stream. It now uploads from
  page-locked memory to one CUDA kernel that writes P010, the same shape as the
  SDR path, with no OpenGL context and no per-frame GL/CUDA synchronization.
  This has not yet been measured on NVIDIA hardware.
- `display_hermes_ram_t::snapshot()` used to stamp the frame after fence waits,
  DMA-BUF mapping/synchronization, CPU copy and cursor composition, so the
  reported "Frame processing latency" above excludes those capture costs. It now
  stamps the frame once its fence has signalled, so the copy and cursor
  composition count. Neither is total display-to-client latency. The
  `capture-metric/hermes-kms-cpu` metric measures the acquire ioctl, not the
  complete CPU capture operation.
- On AMD, HEVC Main10 costs the same as Main (5.1 and 5.2 ms per 1440p frame on
  an RX 6700 XT's VCN 3 at 75 Mbps), and the HDR path stays zero-copy, so the
  host-side cost above is specific to NVIDIA.
- `stream.cpp::videoBroadcastThread()` uses the same packetization and FEC path
  for SDR and HDR. Its packet pacing target is approximately 800 Mbps, independent
  of the configured stream bitrate, with batches up to roughly 64 KiB. A lower
  average bitrate therefore does not necessarily remove short bursts. Burst loss
  on a slower downstream link is plausible but not established by these logs.
- "Network: frame's overall network latency" measures host send processing. It
  is not a measurement of downstream delivery, client decoding or presentation.
- Repeated IDR requests cause repeated larger keyframes and can amplify congestion
  and encoder load. An IDR request alone is not proof of network packet loss.
- No producer-fence timeout, FEC-disabled oversized frame, or active-session
  conversion/encoding failure was found in the reviewed current log. Unsupported
  AV1 and multi-reference NVENC messages were startup capability probes.

## Conclusion

Functional HDR is working in the reported tests; performance remains an open
release gate. The evidence establishes higher host processing cost and frequent
client recovery, not an HDR-specific packet corruption defect or a proven single
root cause. Do not suppress recovery requests or change stream pacing speculatively:
that could hide lost reference frames or increase latency. Further attribution
requires stage timing or client/packet evidence. NVIDIA/KWin zero-copy and the
separate isolated Gamescope atomic-flip problem remain outside this change.
