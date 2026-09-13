# Performance Tuning
In addition to the options available in the [Configuration](configuration.md) section, there are a few additional
system options that can be used to help improve the performance of Hermes.

> [!NOTE]
> The two notes below are Windows settings, inherited from upstream. For the
> Linux host, the settings that matter most are the capture backend and the
> virtual display — see [Compatibility](compatibility.md) — and the AMD
> low-latency encoder note in [Troubleshooting](troubleshooting.md).

## AMD

In Windows, enabling *Enhanced Sync* in AMD's settings may help reduce the latency by an additional frame. This
applies to `amfenc` and `libx264`.

## NVIDIA

Enabling *Fast Sync* in Nvidia settings may help reduce latency.

<div class="section_buttons">

| Previous            |          Next |
|:--------------------|--------------:|
| [Guides](guides.md) | [API](api.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
