# Hermes — Modularization Workstream

> **Work in progress — `refactor/modularization-modernization`**
>
> This branch is dedicated to the **incremental modularization of the Hermes
> codebase**. This README documents the refactoring process, its constraints,
> and how changes will be evaluated. It is **not** the general product README.
> For installation, features, and user documentation, see the
> [main branch](https://github.com/MrOz59/Hermes/blob/main/README.md).

## Why modularize Hermes?

Hermes has evolved from the Sunshine/Apollo codebase while gaining its own
capture backends, virtual-display integration, session management, and GPU
encoding paths. These areas interact in ways that can make small changes
difficult to isolate, review, and test.

The goal is to make responsibilities and dependencies explicit **without
rewriting the entire project** or changing how existing functionality behaves.

A successful modularization should make it easier to:

- Understand which subsystem owns a specific responsibility.
- Change one component without modifying unrelated code.
- Test subsystems in isolation, with fewer hardware requirements.
- Keep platform- and GPU-specific implementation details behind stable
  interfaces.
- Integrate future changes with smaller, easier-to-review diffs.
- Preserve the runtime characteristics that matter for game streaming.

## Scope

**In scope**

- Documenting existing dependencies, ownership boundaries, and lifecycle.
- Extracting cohesive components and clarifying their public interfaces.
- Reducing circular dependencies, unnecessary global state, and oversized
  translation units where this improves maintainability.
- Separating portable logic from Linux-specific and GPU-specific backends.
- Introducing focused tests around the boundaries being refactored.
- Evaluating **C++ Modules** for suitable components, gradually and only after
  architectural boundaries are clear.
- Adjusting CMake and CI as required to support these changes safely.

**Out of scope for this workstream**

- A complete rewrite of Hermes.
- Adding features unrelated to the subsystem currently being refactored.
- Bulk-importing features from other forks as part of the same refactor.
- Requiring C++26 solely to enable modules.
- Replacing working capture, encoding, or protocol paths without a separately
  justified change and appropriate tests.

Feature ports and broader language upgrades can be proposed separately after
the relevant interfaces have stabilized.

## Architectural boundaries to investigate

These are **candidate boundaries**, not a claim that these independent modules
already exist or a final directory structure. The first phase is to map actual
dependencies and validate where separation is useful.

| Area | Responsibility to isolate | Special considerations |
| --- | --- | --- |
| Session lifecycle | Connection-to-session state, startup, teardown, recovery | Order of operations and cleanup on failures |
| Display and topology | Output discovery, virtual displays, layout and state | KWin/Wayland, Hermes-KMS and EVDI integration |
| Frame capture | Backend selection, capture contracts and frame ownership | DMA-BUF, synchronization and zero-copy |
| Encoding | Encoder capabilities, configuration and frame submission | VAAPI, NVENC, software fallback and CUDA |
| Network and protocol | GameStream/Hestia requests, negotiation and transport | Wire compatibility and session transitions |
| Input and devices | Input translation, device lifecycle and isolation | Virtual HID backends and cleanup |
| Configuration | Parsing, validation, persistence and defaults | Backwards compatibility with existing configs |
| Platform adapters | Operating-system-specific services and dependencies | Do not force unsupported platforms into CI |

Boundaries should follow **responsibility and dependency direction**, not
one-file-per-module rules. Shared interfaces should expose only what consumers
need. Avoid introducing abstraction layers that merely forward calls without
reducing coupling.

## Migration plan

### Phase 0 — Baseline and inventory

- [ ] Record the build/test baseline for the branch, including known failures.
- [ ] Map the current compilation units, major shared state, and dependency
      edges between the areas above.
- [ ] Identify cross-subsystem initialization and teardown dependencies.
- [ ] Document which paths require physical AMD, Intel, or NVIDIA hardware,
      Hermes-KMS, EVDI, or a particular compositor.
- [ ] Choose an initial extraction that is small, testable, and low risk.

**Exit condition:** a concrete dependency map and an agreed first component;
no mandatory build-tool upgrades.

### Phase 1 — Establish clean interfaces

- [ ] Define narrow contracts for the selected component, including
      ownership, lifetime, and error behavior.
- [ ] Separate its portable core from platform/backend-specific details
      where practical.
- [ ] Replace hidden cross-component assumptions with explicit dependencies.
- [ ] Add characterization or regression tests before moving sensitive logic.
- [ ] Validate behavior and performance against the baseline.

**Exit condition:** the component is independently understandable and its
consumers use a documented interface; existing behavior still works.

### Phase 2 — Repeat one subsystem at a time

- [ ] Apply the same process to the next high-value component.
- [ ] Avoid moving several tightly coupled systems in one PR.
- [ ] Keep old code paths functional until replacements are validated.
- [ ] Revisit interface design when real callers expose missing requirements.
- [ ] Keep documentation synchronized with the actual architecture.

**Exit condition:** clear boundaries are in place for the selected areas, with
test coverage and no unexplained runtime regressions.

### Phase 3 — Optional C++ Modules pilot

C++ Modules are a **possible build/language mechanism**, not the goal of the
refactor. They are available from C++20 and do not require switching the
whole project to C++26. Hermes currently configures its main target as C++23.

- [ ] Select a self-contained, relatively dependency-light component.
- [ ] Prototype a module interface without forcing module adoption across
      unrelated targets or third-party libraries.
- [ ] Evaluate compiler support, dependency scanning, IDE tooling, caching,
      incremental builds, and clean rebuilds.
- [ ] Test migration alongside traditional headers and translation units.
- [ ] Measure whether modules improve maintainability or build performance.
- [ ] Keep a practical fallback if supported build environments cannot
      consume the module reliably.

The top-level build currently declares **CMake 3.20**. A conventional CMake
`FILE_SET CXX_MODULES` integration needs a newer CMake (3.28 or later), so any
minimum-version change needs a separate compatibility assessment.

**Exit condition:** the pilot is reproducible in the supported build matrix
and provides a demonstrated benefit. If not, retain conventional C++ units.

### Phase 4 — Consolidation

- [ ] Remove temporary compatibility shims only when no longer needed.
- [ ] Review remaining dependency cycles and redundant interfaces.
- [ ] Compare test coverage, build times, executable size, and performance
      with the baseline.
- [ ] Update architecture diagrams and contributor guidance.
- [ ] Prepare small, reviewed integrations back toward `main`; do not merge
      the workstream as one unreviewable rewrite.

**Exit condition:** each integrated subsystem has a stable, documented
boundary and passes its relevant functional and performance checks.

## Invariants: what a refactor must not break

Changes should preserve existing behavior unless a deliberate, separately
documented behavior change has been approved:

- **Streaming performance:** do not accidentally add frame copies, CPU
  readbacks, blocking waits, or latency in the capture-to-encode path.
- **Frame ownership:** maintain correct DMA-BUF/FD lifetimes, GPU import
  synchronization, buffer recycling, and teardown across error paths.
- **Capture and encoding:** preserve working Hermes-KMS, EVDI, VAAPI, NVENC,
  and fallback paths; test hardware-specific behavior when available.
- **Sessions:** preserve connection, reconnect, stop, recovery, virtual
  display cleanup, and device isolation semantics.
- **Compatibility:** preserve existing GameStream/Hestia protocol behavior
  and stored user configuration.
- **Packaging:** keep relevant Linux distributions and supported deployment
  methods building; do not infer platform support from untested code.

CUDA is an independent compilation concern. The existing build intentionally
keeps CUDA at C++17 and uses a compatible host compiler for NVCC; do not
change those settings as an accidental consequence of C++ refactoring.

Passing CI is necessary but not sufficient for GPU/compositor-sensitive
changes. Record what was tested on real hardware and what remains unverified.

## PR workflow

Keep each PR narrow enough to answer **what boundary changed** and
**how compatibility was demonstrated**.

1. **Describe the coupling:** which dependencies or responsibilities are
   currently mixed, and what the proposed boundary will own.
2. **Capture current behavior:** include existing tests, new characterization
   tests, or a reproducible before/after procedure.
3. **Refactor without feature creep:** prefer mechanical moves and interface
   changes separately from functional improvements.
4. **Validate the result:** run the relevant build/test matrix and note
   performance or hardware validation where required.
5. **Explain migration impact:** document any changed include paths, build
   prerequisites, compatibility shims, or follow-up work.

Suggested PR checklist:

- [ ] Scope is limited to one coherent responsibility or prerequisite.
- [ ] Public and internal interfaces are documented.
- [ ] No unintended protocol, configuration, or runtime behavior changes.
- [ ] Error handling and resource ownership are covered.
- [ ] Relevant tests and CI checks pass (or failures are explained).
- [ ] Hardware/compositor validation is recorded where applicable.
- [ ] No unrelated feature ports or toolchain bumps were bundled in.

## Working style

This effort is **human-led**: contributors propose the boundaries, write and
review the implementation, and make the architectural decisions. AI tools can
assist with researching existing code, exploring alternatives, debugging,
and repetitive tasks, but generated changes still require human ownership,
review, and testing.

No phase is considered complete merely because code was moved into new
directories or converted to `export module`. The measure of progress is
**lower coupling, clearer ownership, testability, and preserved behavior**.

---
This document is specific to the modularization branch. For the project's
normal roadmap, releases, usage, and installation instructions, use the
[main README](https://github.com/MrOz59/Hermes/blob/main/README.md).
