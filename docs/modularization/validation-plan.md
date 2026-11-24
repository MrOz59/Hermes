# A reproducible build and modules pilot for Hermes

**Status · experiment design, 21 November 2026.** The reference is the [workstream at c123041](https://github.com/MrOz59/Hermes/tree/c1230419254ae191c9f95e03b031dc420742b678). The candidate tools and module configurations below have no Hermes benchmark result yet. A maintainer can first approve a small architectural extraction, then decide separately whether a language or build-tool experiment merits the next review.

## Existing evidence

The initial [baseline](baseline.md) records a local configure blocked by missing submodules and the required PyroWave SDK. The later **Build Hermes** push workflow on that exact workstream head completed successfully. The Actions jobs and steps establish the following reference. [Workflow run 43842793099](https://github.com/MrOz59/Hermes/actions/runs/43842793099) · [Machine-readable job and step results](https://api.github.com/repos/MrOz59/Hermes/actions/runs/43842793099/jobs?per_page=100)

| Existing job | Observed result | What can be compared later |
| --- | --- | --- |
| Arch headless host tests | SDK and configure succeeded. The GoogleTest binary ran under Xvfb, the Web UI suite ran, and the selected geometry tests ran on headless Sway. | SDK step 41 s, configure 22 s, build `test_sunshine` 12 min 29 s. The GoogleTest invocation took 12 s. Hardware-dependent cases can skip. [Host job](https://github.com/MrOz59/Hermes/actions/runs/43842793099/job/133215368568) |
| Five distribution packages | Arch `makepkg`, Ubuntu 24.04 and 26.04 DEBs, Fedora 43 and 44 RPMs succeeded. | Ubuntu 24.04's build step took 4 min 46 s and Fedora 44's 4 min 05 s. The jobs have different dependencies and environments. [Ubuntu job](https://github.com/MrOz59/Hermes/actions/runs/43842793099/job/133223581703) · [Fedora job](https://github.com/MrOz59/Hermes/actions/runs/43842793099/job/133223581886) |
| Four loader fixtures | Ubuntu 24.04/26.04 and Fedora 43/44 fixtures succeeded. DEB/RPM jobs also install the product and exercise non-root startup. | Check the generated package and the fixture independently. Reusing the original CMake fixture alone would bypass any new install rule under test. [Loader workflow](https://github.com/MrOz59/Hermes/blob/main/.github/workflows/package-loader.yml) · [Installed smoke check](https://github.com/MrOz59/Hermes/blob/main/scripts/tests/package-loader-smoke.sh) |
| Other jobs | Version job succeeded. Windows, release, release-candidate and nightly were skipped. The workflow contains no macOS build. | The run supplies neither Windows/macOS portability evidence nor real-device capture measurements. [Workflow definition](https://github.com/MrOz59/Hermes/blob/main/.github/workflows/build.yml) |

Times are elapsed GitHub step timestamps from a single historical run. Runner load, cache state and compiler versions were not held constant across jobs. Use these values to budget work. Use paired runs on the same host for a performance comparison.

## Stage A · establish the control

1. Pin the Hermes commit, compiler executable and version, standard library and ABI, generator, linker, target triple, build type, exact package versions, SDK and submodules. Record runner image digest, CPU, RAM, job count and cache policy. Use production optimization for production targets and keep the test target's coverage flags.
2. Start from the workflow's recursive checkout, distro prerequisites and pinned PyroWave setup. Use [the same flags, target and Xvfb invocation as the host job](https://github.com/MrOz59/Hermes/blob/main/.github/workflows/build.yml). For the standalone lifecycle suite, build its own CMake project and run its registered CTest entry with the dependency setup from [its own workflow](https://github.com/MrOz59/Hermes/blob/main/.github/workflows/network-lifecycle.yml). The host test binary is invoked directly. Enabling CTest at the top does not reproduce it.
3. Record the outcome of the existing `ProcessSessionRoute.*` tests. Add the [branch-versus-main `primary` case](architecture-review.md) to the characterization plan for the chosen integration base. Keep an explicit-method override case, the two HTTPS evaluations, config migration and the process profile caller visible in review.
4. Establish a conventional header/source extraction and a focused target that compiles the production decision with the pinned GoogleTest. Keep that implementation and a conventional consumer buildable. The focused build should complete without linking Boost.Process, RTSP, a compositor, capture SDKs or the GPU. A full host build then verifies the adapter still links and behaves correctly.

Stage A measures whether a contributor can change and test the routing table without building the whole host. A positive result justifies the extraction independently of a build-system change.

## Stage B · compare one source tree

| Configuration | Question it answers |
| --- | --- |
| Existing CMake/header extraction | Cost and correctness of the isolated component in the current system. |
| CMake 4.5.1 + Ninja, same header sources | Cost of a newer frontend with the source model held fixed. Preserve the project's declared minimum outside the experiment. |
| CMake 4.5.1 + Ninja, one named C++23 module | Incremental effect of named modules with the frontend held fixed. Start with conventional standard headers. Check `import std` separately on a documented combination. |
| mcpp v2026.11.20.1, same component and consumers | Whether the managed workflow improves setup, incremental rebuilds, and diagnostics. Record the backend and the toolchain actually selected. |
| Xmake v3.1.2, same component and consumers | Whether the native Lua build expresses the same target and dependencies economically. Test exported Ninja only if the team intends to support it. |

For each alternative, run the header variant as well as the module variant where supported. Record an unsupported combination as a result. Use separate build directories. Share the production algorithm, the test cases and the expected values. A header-facing adapter can keep the existing API and symbol ownership while an experimental target imports a private module. Never link two definitions of the policy into the same executable. Keep Boost, FFmpeg, GPU SDKs, generated Wayland C headers and `.cu` files outside the first module interface. The pilot should change one variable at a time.

In the controlled comparison, point the engines at the **same compiler binary** if they support it, even if that means having CMake and Xmake invoke the compiler installed by mcpp. Record the effective sysroot, standard library, ABI flags and linker too. If they differ, label that result a toolchain comparison. Separately measure the user-facing defaults, including downloads. This makes the managed bootstrap benefit visible without attributing compiler changes to the frontend.

### Workloads and observations

| Workload | Record and check |
| --- | --- |
| First checkout, empty tool/package cache | Time and bytes to acquire tools and dependencies, manual steps, final versions and any network access. Repeat from a staged cache with network disabled and identify missing inputs. |
| Clean build with dependencies already available | Configure/scan/compile/link times separately where exposed, peak memory, peak disk, number of compiled units and artifact size. Record any SDK or npm work the target invokes. |
| No edit, then an implementation-only edit | No-op time, rebuilt objects and importers, test result. A source edit must appear in the resulting behavior, even on a restored cache. |
| Touch-only, comment edit, then public interface edit | Distinguish timestamps from semantic changes. Record exactly which units were rescanned and rebuilt. Change an observable value or signature and verify the appropriate consumer is rebuilt or rejected. |
| Change a macro, build mode, compiler or standard library | Reuse a previously warm cache and verify the affected artifacts are rebuilt or rejected. Keep Debug/Release, host/test definitions and incompatible BMIs separated. |
| Delete a BMI or generated header, add/remove an import, parallel clean builds | Verify missing artifacts regenerate, the graph changes without a manual clean, and parallel runs neither race nor silently produce stale outputs. |
| Contributor tooling | Open the emitted compilation database in the project's editor/clangd setup. Check diagnostics, go-to-definition across an import and a breakpoint in the real implementation. Record scanner and editor versions. |

Run at least five paired samples per timed workload on the same machine and job count, alternating tool order. Report raw samples, median and range. Use equivalent clean or prewarmed states. If an optional optimization changes the result, show default and opt-in runs separately. Clock speed, caching and build scheduling can change compilation time. Capture-to-encode latency is a separate measurement. [Dependency scanning format](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p1689r5.html) · [Compiler module and ABI guidance](https://clang.llvm.org/docs/StandardCPlusPlusModules.html)

## Stage C · qualify an alternative against a real package

Start with **Ubuntu 24.04 amd64**. It combines an older distribution toolchain, a multiarch library directory, an independent GCC for NVCC, a private codec, installed loader checks and a possible KWin capability difference. If a tool only helps with the focused developer workflow, that can be its final role. This stage is warranted when the proposal extends to building or distributing the host.

| Exercise | Evidence to capture before expanding |
| --- | --- |
| Mixed C/C++ and generated sources | Build ENet and the host's C sources with their expected flags. On a protocol-v6-capable image, build the actual KWin target, inspect generated header/C edges, edit a protocol in a disposable fixture and build in parallel. On an older/missing protocol, verify the same capability decision and package contents as the reference for that image. |
| CUDA and codec availability | Compare the real device compile command, C++17 mode, NVCC 12.9.1, host GCC 14, generated GPU architecture set and the final host's library resolution. Show that a required-encoder package fails configuration/build when CUDA is missing and that the headless CUDA-off configuration still succeeds. Compare PyroWave SDK revision, `pkg-config` resolution and linked SONAME. |
| Staged install | Compare sorted payload paths, file modes, symlink targets, owner/group where relevant, unit and udev files, Web UI assets, package names, versions, dependency metadata, optional helpers and licenses. Verify the asset root compiled into the binary and the installed private libdir. |
| Installed binary | Install the candidate package in a fresh matching image. Inspect ELF `NEEDED`/`RUNPATH` and symbol versions. Run the existing non-root, clean-environment package smoke check with capabilities and confirm the private codec resolves from the installed prefix. Record package and loader output. |
| Remaining active legs | Repeat for Arch's real root `PKGBUILD`, Ubuntu 26.04 and Fedora 43/44. Verify distro-specific libdirs, debug packaging where applicable and the expected optional component set on each image. Keep source-present AppImage/Flatpak rules visible if a switch would affect them, and label their independent validation status. |

The current recipes and fixtures provide the reference for comparing outcomes. [Root Arch recipe](https://github.com/MrOz59/Hermes/blob/main/PKGBUILD) · [DEB/RPM install tree](https://github.com/MrOz59/Hermes/blob/main/packaging/linux/stage-install.sh) · [Private runtime rule](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/cmake/targets/pyrowave-runtime.cmake) · [Loader fixture](https://github.com/MrOz59/Hermes/blob/c1230419254ae191c9f95e03b031dc420742b678/scripts/tests/pyrowave-runtime-loader.sh) · [Full package jobs](https://github.com/MrOz59/Hermes/blob/main/.github/workflows/build.yml)

## Runtime checks and the adoption decision

The routing extraction calls for launch, cancel, reconnect and isolated-versus-shared checks. If a later step changes the compiler, linker, encoder build or ownership on the frame path, qualify the resulting host binary under the conditions it will ship. Compare the same scene, client, codec, resolution, refresh, driver, compositor, GPU, capture backend and network. Record encoder/fallback selection, copies or readbacks, frame drops, frame-time p50/p95/p99 and CPU usage. Repeat start/stop/reconnect and a compositor or output transition. Track FD growth and fence/lease failures. Record hardware gaps, including HDR, rather than extrapolating from headless CI.

| Decision | Evidence | Reversible outcome |
| --- | --- | --- |
| Land an architectural extraction | Equivalent behavior on the integration base, a narrower dependency surface, focused and full-host tests. | Review the extraction independently, keeping ordinary callers and the production build. |
| Keep a module or alternative developer workflow | Correct clean/incremental builds, usable editor support and a repeatable benefit maintainers value. Record setup time, rebuild time and ongoing rule maintenance. | Keep it opt-in while there is an owner. Retire the experiment if it duplicates manifests without a measurable or operational benefit. |
| Change the production frontend or project minimum | Full package matrix, corresponding installed checks, runtime evidence for changed paths, and a documented tool/bootstrap/update policy with a maintainer. | Keep the conventional CMake packaging path available through qualification. Switch only after maintainers accept the evidence and can reproduce a package from the fallback. |

**First follow-up.** Characterize `primary` on the intended integration base, retaining the shared-host result when the extraction reaches the release line. That enables the small routing extraction the branch already selected. A tool commitment can wait for results from the same piece of Hermes.
