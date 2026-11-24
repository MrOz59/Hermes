import { expect, test } from "bun:test";

test("browser WebAssembly uses the Hermes Rust ABI", async () => {
  const wasmPath = Bun.env.HERMES_RUST_WASM;
  if (!wasmPath) throw new Error("Set HERMES_RUST_WASM to the compiled WebAssembly module");

  const module = await WebAssembly.compile(await Bun.file(wasmPath).arrayBuffer());
  expect(WebAssembly.Module.imports(module)).toEqual([]);

  const instance = await WebAssembly.instantiate(module);
  const abiVersion = instance.exports.hermes_rust_abi_version;
  if (typeof abiVersion !== "function") throw new Error("Missing hermes_rust_abi_version export");
  expect(abiVersion()).toBe(1);

  const timestampDistance = instance.exports.hermes_rust_rtp_timestamp_distance;
  if (typeof timestampDistance !== "function") {
    throw new Error("Missing hermes_rust_rtp_timestamp_distance export");
  }
  expect(timestampDistance(100, 145)).toBe(45);
  expect(timestampDistance(0xffff_fffe, 1)).toBe(3);
  expect(timestampDistance(0, 0x8000_0000) >>> 0).toBe(0x8000_0000);
});

test("browser WebAssembly validates empty and malformed batch arguments", async () => {
  const wasmPath = Bun.env.HERMES_RUST_WASM;
  if (!wasmPath) throw new Error("Set HERMES_RUST_WASM to the compiled WebAssembly module");

  const { instance } = await WebAssembly.instantiate(await Bun.file(wasmPath).arrayBuffer());
  const timestampDistances = instance.exports.hermes_rust_rtp_timestamp_distances;
  if (typeof timestampDistances !== "function") {
    throw new Error("Missing hermes_rust_rtp_timestamp_distances export");
  }

  expect(timestampDistances(0, 0, 0, 0, 0)).toBe(0);
  expect(timestampDistances(0, 0, 1, 0, 0)).toBe(1);
  expect(timestampDistances(0, 0, 1, 0, 1)).toBe(1);
});
