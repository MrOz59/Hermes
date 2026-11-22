# mcpp migration

The Linux x86-64 mcpp workspace builds the streaming host and both privileged
brokers. It also carries the standalone networking suite. The host build
uses DRM, X11, Wayland, VA-API, and systemd's D-Bus client. KWin capture and
libvirtualhid's XTest fallback can be enabled independently. CUDA, Qt tray,
and PyroWave also have native build features.

## Local build inputs

Use Fedora GCC and glibc for the native ABI. The current build was verified
with GCC 16.2.1, glibc 2.44, GNU Binutils 2.47, exported Linux 7.3.5 UAPI
headers, and Ninja 1.13.2. The compiler adapter in `.toolchain/bin` points
GCC at the local GNU assembler and linker and uses the locally extracted
`.toolchain/static/libstdc++.a` for mcpp build programs. The application
links against Fedora's shared C++ runtime.

CI pins mcpp 2026.11.21.1, which recognizes Fedora GCC's version and target
directly. CI still adapts GCC 14's module scanner flags. With that release,
add `allow_host_tools = ["ninja"]` to `[build]` in `.mcpp/local-home/config.toml`
to use the Ninja on `PATH`. The same table accepts `offline = true` as a
default for builds. Patchelf remains an input to PyroWave package staging
when its bundled library requires a RUNPATH adjustment.
[mcpp release notes](https://github.com/mcpp-community/mcpp/releases/tag/v2026.11.21.1)

Put the following development inputs in `.toolchain/`.

| Location | Build input |
| --- | --- |
| `linux-headers-7.3.5/include` | Exported Linux userspace headers |
| `fedora/usr` | Extracted Fedora development RPMs for the native libraries and `bin/wayland-scanner`. For libraries already on the workstation, the unversioned linker names can resolve to the matching installed SONAMEs. |
| `boost/boost` and `boost/stage/lib` | Boost 1.92.0 headers and libraries. The host uses static archives with the glibc iconv backend for Boost.Locale. The focused networking target uses shared Boost.Log. |
| `json-include/nlohmann` | nlohmann/json 3.11.3 headers |
| `libvirtualhid` | Hermes' libvirtualhid fork at commit `14ee6c4717d32b678c2983bfa6c54019b944d421` |
| `acl/include` and `acl/lib` | libacl development files for the session broker |

Initialize the Git submodules, including `third-party/build-deps` for Hermes'
pinned FFmpeg and codec archives. For a Web UI build, install the Bun version in
`src_assets/common/assets/web/package.json` and run
`bun install --cwd src_assets/common/assets/web --frozen-lockfile` before
the native build. The offline asset action runs `bun run build` in that directory.

## Build and check

Run these commands from the repository root. The environment assignments
apply to a single invocation. `--offline` keeps mcpp from populating its
managed package store over the network.

```sh
MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  mcpp build --offline --release -p hermes-host --features kwin,web-assets,xtest
```

Wayland is enabled by default. The `kwin` feature adds the isolated KWin
PipeWire transport and needs the Plasma Wayland Protocols XML, including
screencast protocol version 6 or newer, in
`.toolchain/fedora/usr/share/plasma-wayland-protocols`. The `xtest` feature
needs the extracted Xtst headers and linker files in the Fedora prefix.
Use `tray` with the Qt 6 Widgets and SVG headers and linker files in the
Fedora prefix. The native build runs Qt's `moc` from
`.toolchain/fedora/usr/lib64/qt6/libexec/moc`. Set `HERMES_MCPP_QT_MOC`
when the matching Qt tool lives elsewhere.

The `cuda` feature follows Hermes's CUDA 12.9 release policy to preserve its
supported GPU architectures. Use
`scripts/install_cuda_redist.sh 12.9.1 "$PWD/.toolchain/cuda-12.9"` to
provision a local toolkit. Its device compiler compiles only `cuda.cu`
using C++20, the latest mode supported by CUDA 12.9 nvcc, and a compatible
GCC 14 host compiler. An extracted Fedora GCC
14.4.1 can live at `.toolchain/cuda-gcc14/usr/bin/g++-14`. Set
`CUDAHOSTCXX` or `HERMES_MCPP_CUDA_PREFIX` for alternative locations. The
main host uses its C++26 toolchain. The pinned upstream PyroWave and Granite
build actions retain their original C++14 and C99 modes.

With the same `MCPP_HOME` and `MCPP_TOOLCHAIN` as the host build, run
`scripts/build-pyrowave-sdk.sh` to prepare the pinned upstream SDK. The script
builds its standalone mcpp project under `target/pyrowave-sdk` and writes the
SDK to `target/pyrowave-sdk/install`, the default `pyrowave` feature input.
To reuse an existing SDK, set `HERMES_MCPP_PYROWAVE_PREFIX` when building the
host, for example `export HERMES_MCPP_PYROWAVE_PREFIX="$PWD/.toolchain/pyrowave"`.
The SDK needs `include/pyrowave/pyrowave.h` and `lib/libpyrowave-shared.so`.
The Fedora toolchain prefix provides the Vulkan loader and headers.

Local builds use the SDK's `lib` directory at runtime. For packages, configure
`HERMES_MCPP_PYROWAVE_RUNPATH` to the absolute installed library directory if
it differs from Fedora's `/usr/lib64/hermes`. The stager uses the SDK prefix and
library checksum from the host's build metadata. For older mcpp results that
lack both values, pass `--pyrowave-prefix` if you built with an alternate SDK.

`web-assets` builds Vite and stages Linux runtime assets, including a default
`apps.json`, in the mcpp output tree. A package build can use
`package-assets` for identical staged assets and a runtime path of
`/usr/share/hermes`.

The release host executable is at
`target/x86_64-linux-gnu/<fingerprint>/bin/hermes-host/hermes`. Its
`--version` and `--help` commands can run with a temporary
`XDG_CONFIG_HOME` to keep smoke-test output and generated configuration
separate from an installed Hermes instance. Set `XDG_RUNTIME_DIR` to a
private, writable directory if the build runs under a sandbox with restricted
access to the session runtime directory.

```sh
MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  mcpp build --offline --release -p hermes-kms-card-broker -p hermes-session-broker
```

Pass the resulting executables to `scripts/card-broker-test.sh` and
`scripts/session-broker-concurrency-test.py`. Both use temporary fixtures. The
session broker's end-to-end test requires a disposable Linux VM.

Run the full Linux GoogleTest suite from the repository root so it can read
the source fixtures. Use a short temporary directory for Unix socket tests
and a virtual X display when running outside a graphical session.

```sh
MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  xvfb-run -a mcpp test --offline -p hermes-host --features kwin,web-assets \
  --profile coverage -- --gtest_color=yes
```

## Stage a Fedora package payload

Build the host with its installed asset path and save its result manifest.
Build the companion executables in a second invocation, without the host
features.

```sh
MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  mcpp build --offline --release -p hermes-host \
  --features cuda,kwin,package-assets,xtest,pyrowave,tray
host_result=$(python3 -c 'import json, pathlib; print(json.loads(pathlib.Path("target/.last-result.json").read_text())["data"]["result"])')

MCPP_HOME="$PWD/.mcpp/local-home" MCPP_TOOLCHAIN="path:$PWD/.toolchain" \
  mcpp build --offline --release -p hermes-kms-card-broker \
  -p hermes-session-broker -p hermes-gamemode -p hermes-detached-session

packaging/linux/stage-install.sh --mcpp "$PWD/target/hermes-payload" \
  --result "$host_result" --result target/.last-result.json \
  --libdir "$(rpm --eval '%{_lib}')" --tray

packaging/linux/rpm/build-rpm.sh
```

The stager creates a new payload directory. Use a fresh staging directory
when repeating this step. It verifies the linked PyroWave library and converts
the mcpp binary's legacy RPATH to RUNPATH on the staged copy. An installed
package must still pass the file-capability loader test.

The RPM command prints the finished package path in `target/dist/rpm/<arch>/`.
Fedora CI uses the same command. Its temporary rpmbuild tree lives under
`target/` and is removed when the command exits. `mcpp clean` removes everything
under `target/`, including the SDK, staged files and finished packages. For a
repack of the same source version with both copies retained, pass a new RPM
release number, for example `packaging/linux/rpm/build-rpm.sh target/hermes-payload 2`.

The [mcpp packaging guide](https://github.com/mcpp-community/mcpp/blob/main/docs/10-pack-and-release.md)
documents its portable bundle formats and `target/dist` outputs. Hermes's
native RPM uses rpmbuild for Fedora dependencies, scriptlets and file ownership.

## Parity status

The CUDA-disabled CMake reference and the matching mcpp profile compile the
same 88 source units across the host, ENet, libdisplaydevice, and
libvirtualhid, including the generated Wayland protocol C sources. KWin adds
six source units, including two generated protocol C sources. The host linked
and returned its version and help text. Live streaming and hardware capture
need separate verification.

The host also built and linked with both Qt tray and PyroWave enabled.
The full Xvfb test suite with KWin, Qt tray, PyroWave, XTest, and the SDL2
helper enabled passed 658 cases and skipped 30. A separate headless Sway
pass completed all three live Wayland layout cases and passed 11 cases
overall, with six skips. The skips include live GPU and kernel-input cases.
The CUDA release build linked against the CUDA 12.9.1 toolkit using GCC 14
for its CUDA translation unit. The release workflows and downstream Linux
build entry points still need verification.
