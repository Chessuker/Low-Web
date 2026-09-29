// Runs build/ops.wasm on V8 and prints results in the same format as wasmrun.
import fs from 'fs';
const { instance } = await WebAssembly.instantiate(fs.readFileSync(process.argv[2] ?? 'build/ops.wasm'));
const x = instance.exports;
for (const f of ['int32', 'int64', 'floats', 'control', 'memory_ops'])
  console.log(`${f}() -> ${BigInt.asUintN(64, x[f]())}`);
for (const [f, args] of [['trap_div', [1, 0]], ['trap_div', [-2147483648, -1]], ['trap_oob', []]]) {
  try { console.log(`${f}(${args}) -> ${x[f](...args)}`); } catch (e) { console.log(`${f}(${args}) -> trap: ${e.message}`); }
}
console.log(`add3(1,2,0.5) -> ${x.add3(1, 2n, 0.5)}`);
