import * as assert from '../assert.js';

// The JS<->Wasm cycle of js-wasm-cycle.js in several module loaders. entry-function.js imports f2
// from function.wasm, a module whose environment exists only once it has linked. Loaded with
// entry-function.js as the root, the wasm module links first and the JS module's code is shared
// by records that import f2 at the same place in it. Loaded with function.wasm as the root, the
// JS module links while the wasm module has no environment yet, so its imports cannot be compared
// with another record's then: it links code of its own, which later records are compared with
// once that is possible.
const executableOf = f => /->(0x[0-9a-f]+),/.exec($vm.codeBlockFor(f))[1];
const sameCode = (f, g) => executableOf(f) === executableOf(g);
const jsRoot = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./js-wasm-cycle/entry-function.js");
async function wasmRoot() {
    const loader = $vm.createModuleLoader();
    await $vm.moduleLoaderImport(loader, "./js-wasm-cycle/function.wasm");
    return $vm.moduleLoaderImport(loader, "./js-wasm-cycle/entry-function.js");
}

const instances = [await jsRoot(), await wasmRoot(), await jsRoot(), await jsRoot(), await wasmRoot()];
const [m1, m2, m3, m4, m5] = instances;
for (let i = 0; i < testLoopCount; ++i)
    assert.eq(instances.map(instance => [instance.f2(), instance.f(i)].join()).join(";"), Array(5).fill(`43,${i + 1}`).join(";"));
assert.eq([sameCode(m1.f, m2.f), sameCode(m1.f, m3.f), sameCode(m2.f, m3.f), sameCode(m3.f, m4.f), sameCode(m4.f, m5.f)].join(), "false,false,true,true,false");
assert.eq([m1.f2 === m3.f2, m2.f2 === m3.f2, m3.f2 === m4.f2].join(), "false,false,false");
