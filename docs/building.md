# Building

Hermes builds for Linux. The native mcpp build uses C++23 for the host and
was verified on Linux x86-64 with GCC 16.2.1.

## Build locally

Clone the repository and its submodules. The submodules provide pinned codec
archives, protocol definitions, and GoogleTest.

```bash
git clone --recurse-submodules https://github.com/MrOz59/Hermes.git
cd Hermes
```

Prepare the [local toolchain and development inputs](modularization/mcpp-migration.md)
for your distribution. For the Web UI, install the project's npm dependencies
in the checkout before building.

```bash
npm install
MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  mcpp build --offline --release -p hermes-host --features web-assets
```

Wayland, X11, DRM, and VA-API are part of the default Linux host build. The
migration guide lists the optional KWin, Qt tray, XTest, PyroWave, and CUDA
features and shows how to build the session and card brokers separately.

### CUDA

Enable `cuda` for NVFBC capture. The release build uses CUDA 12.9.1 with
GCC 14 for CUDA translation units. A separate host compiler builds the rest
of Hermes. See the [local CUDA setup](modularization/mcpp-migration.md#build-and-check)
for the project-local toolkit and compiler paths.

## Packages

The [Linux package staging instructions](modularization/mcpp-migration.md#stage-a-fedora-package-payload)
show how to build the host, brokers, and companion tools with their installed
asset and library paths. Package CI jobs specify distribution-specific
dependencies and artifact assembly. See the [build workflow](../.github/workflows/build.yml).

<div class="section_buttons">

| Previous                              |                            Next |
|:--------------------------------------|--------------------------------:|
| [Troubleshooting](troubleshooting.md) | [Contributing](contributing.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
