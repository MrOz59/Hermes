# Rust in Hermes

Cargo manages first-party Rust crates in this workspace. The workspace uses
Rust 2024, Cargo's resolver 3, and a shared lockfile. Run Cargo from this
directory so rustup reads the pinned toolchain and Cargo writes build output to
`target/rust/` at the repository root. When updating Rust, keep the toolchain
pin and the workspace's `rust-version` metadata aligned.

```sh
cd rust
cargo fmt --all --check
cargo clippy --locked --offline --workspace --all-targets -- -D warnings
cargo test --locked --offline --workspace
```

The workspace uses only local crates. If a future crate adds a registry
dependency, commit the updated lockfile and run `cargo fetch --locked` during
dependency setup. Build and test with the same locked artifacts offline. A
build that also needs to prepare dependencies offline can use Cargo's
[vendor support](https://doc.rust-lang.org/cargo/commands/cargo-vendor.html).

For a new component, run `cargo new --lib crates/<component>` here. Add or
confirm its entry in `Cargo.toml`'s member list, inherit the workspace edition,
Rust version, license, publish policy, and lints as `hermes-rtp` does, and
commit the updated workspace `Cargo.lock`. Give the component a safe Rust API
and add a path dependency from the adapter that uses it. For a C++ migration,
use the existing C++ behavior and tests to define the contract, port the
component behind that boundary, then migrate the production caller separately.

## Linux C++ interop

Cargo produces the Linux static library for the mcpp
[interop smoke test](../docs/building.md#rust-interop). The C++26 named module
owns the C++ API, and the shared C header describes the linker boundary. The
smoke test calls the Rust ABI revision and RTP timestamp routines, including
a batch call that reads C++ storage and writes to caller-owned output. The
C++26 API accepts spans and the Rust adapter passes borrowed memory directly
to the portable `hermes-rtp` crate. The clock arithmetic remains shared
between the scalar and batch APIs.

For new boundaries, define element counts, lifetime, mutability, threading
and deallocation together. The RTP adapter treats equal empty buffers as a
no-op, rejects mismatched counts and detectable invalid or overlapping
ranges before writing, and keeps pointers only for the call. Callers still
guarantee live storage and exclusive output access. The Rust implementation
uses fallible arithmetic and a non-panicking loop. APIs with potentially
panicking implementations or throwing C++ callbacks need to translate
failures on the originating side of the boundary.

Keep C ABI types fixed and explicit. For an ABI change, update and rebuild
the Rust library, C declarations, and C++ module together. Coordinate
breaking revisions with independently built consumers. A header generator
does not replace tests of the boundary or its ownership contract.

### Choosing an interop tool

- **Handwritten C ABI** fits small interfaces using scalars, borrowed buffers
  or opaque handles. Keep RAII and idiomatic C++ types in the C++ module.
- **[cbindgen](https://github.com/mozilla/cbindgen/blob/main/docs.md)** can
  generate C/C++ declarations for a growing Rust C ABI. Pin the generator
  and check the generated header stays in sync with its Rust definitions.
- **[CXX](https://cxx.rs/)** fits native components that need typed calls
  across both languages. Pick supported borrowed/owned types and keep the
  CXX runtime crate and generator on the same exact release when mcpp
  compiles the generated C++.
- **[bindgen](https://rust-lang.github.io/rust-bindgen/)** can generate Rust
  declarations from an existing C header. Keep the inputs and libclang
  version reproducible, then wrap the generated interface in safe Rust.

Keep generated bindings and native builds in the project build tree. When
a component introduces a generator, provision its pinned version before
building offline. Keep measured stream-path changes focused on actual
copying, allocations and boundary call frequency.

## Browser WebAssembly

With rustup and Bun available, run these commands from `rust/`.

```sh
rustup target add wasm32-unknown-unknown
cargo rustc --locked --offline --release --target wasm32-unknown-unknown \
  -p hermes-rust-interop --crate-type cdylib
HERMES_RUST_WASM="$PWD/../target/rust/wasm32-unknown-unknown/release/hermes_rust_interop.wasm" \
  bun test tests/wasm.test.ts
```

The test loads the compiled module with the standard WebAssembly API and
checks scalar arithmetic and batch argument validation without import shims.
Passing nonempty buffers from JavaScript needs a separate WebAssembly memory
ownership contract. Browser clients can serve a
`.wasm` asset with the `application/wasm` MIME type. Use
[wasm-bindgen](https://wasm-bindgen.github.io/wasm-bindgen/) when a Rust crate
needs browser bindings for richer values or APIs.

A future browser Hestia-rs client can reuse portable Rust logic with separate
browser adapters. Hermes's streaming code uses native
[RTSP sockets](../src/rtsp.cpp) and [UDP sockets](../src/stream.cpp). A browser
client needs a browser-compatible stream endpoint or relay, plus pairing,
input, and media decoding that work with browser APIs. WebTransport and
WebCodecs are candidates to evaluate for that work. [Rust browser target](https://doc.rust-lang.org/rustc/platform-support/wasm32-unknown-unknown.html) · [WebTransport](https://w3c.github.io/webtransport/) · [WebCodecs](https://w3c.github.io/webcodecs/)
