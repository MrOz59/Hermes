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

- NVIDIA Hermes-KMS SDR/BGRA8 uses the existing RAM-to-CUDA conversion. Ten-bit
  HDR uses RAM upload to GL, RGB-to-P010 shaders, CUDA/GL resource mapping, plane
  copies on the CUDA stream, and NVENC. Those extra conversion/interop stages are
  a credible source of additional host latency, particularly while the game
  competes for GPU time. The existing aggregate metric cannot isolate their cost.
- `display_hermes_ram_t::snapshot()` stamps the frame after fence waits, DMA-BUF
  mapping/synchronization, CPU copy and cursor composition. The reported "Frame
  processing latency" therefore excludes those capture costs; it is not total
  display-to-client latency. The `capture-metric/hermes-kms-cpu` metric measures
  the acquire ioctl, not the complete CPU capture operation.
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
