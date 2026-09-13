# GameStream Migration
Nvidia discontinued their GameStream service for Nvidia Games clients in February 2023. A self-hosted host such as
Hermes performs as well as or better than Nvidia GameStream did.

## Migration
Upstream's [GSMS](https://github.com/LizardByte/GSMS) migrates custom and auto-detected GameStream games and apps
automatically, writing the working directory, command and image into an `apps.json` and copying the box art to a
directory you choose. It writes Sunshine's `apps.json`, which Hermes reads the same way; point it at
`~/.config/hermes/apps.json`, or copy the entries across.

## Internet Streaming
If you are using the Moonlight Internet Hosting Tool, you can remove it from your system when you migrate.
To stream over the Internet with a UPnP-capable router, enable the UPnP option in the Web UI.

> [!NOTE]
> Running Hermes together with versions of the Moonlight Internet Hosting Tool prior to v5.6 will cause UPnP
> port forwarding to become unreliable. Either uninstall the tool entirely or update it to v5.6 or later.

## Limitations
Hermes does have some limitations, as compared to Nvidia GameStream.

* Automatic game/application list.
* Changing game settings automatically to optimize streaming.

<div class="section_buttons">

| Previous                                        |              Next |
|:------------------------------------------------|------------------:|
| [Third-party Packages](third_party_packages.md) | [Legal](legal.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
