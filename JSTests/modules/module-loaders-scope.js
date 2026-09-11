import { shouldBe, shouldThrow } from "./resources/assert.js";

globalThis.realGlobal = globalThis;
globalThis.marker = "global";

// A loader can give its modules a scope between them and the global scope; free
// identifiers of every module it loads resolve there first.
const a = await $vm.moduleLoaderImport($vm.createModuleLoader({ who: "a", marker: 1 }), "./module-loaders-scope/main.js");
const b = await $vm.moduleLoaderImport($vm.createModuleLoader({ who: "b", marker: 2 }), "./module-loaders-scope/main.js");
shouldBe(JSON.stringify([a.describe(100000), b.describe(100000)]), `["a,dep:a,number,true","b,dep:b,number,true"]`);

// The scope's bindings are variables: assignable from the loader's modules, invisible elsewhere.
(await a.lazy()).setWho("a2");
shouldBe(JSON.stringify([a.describe(1), b.describe(1)]), `["a2,dep:a,number,true","b,dep:b,number,true"]`);
shouldThrow(() => who, `ReferenceError: who is not defined`);
shouldBe(a.evaluated(), "undefined");

// Loaders whose scopes were made from the same names run the same code; a loader
// without the scope, or with other names, links its own.
shouldBe($vm.codeBlockFor(a.describe) === $vm.codeBlockFor(b.describe), true);
globalThis.who = "global";
const plain = await $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders-scope/main.js");
const other = await $vm.moduleLoaderImport($vm.createModuleLoader({ marker: 3, extra: 0, who: "c" }), "./module-loaders-scope/main.js");
shouldBe(JSON.stringify([plain.describe(100000), other.describe(100000), a.describe(3)]), `["global,dep:global,string,true","c,dep:c,number,true","a2,dep:a,number,true"]`);
shouldBe($vm.codeBlockFor(plain.describe) === $vm.codeBlockFor(a.describe), false);
shouldBe($vm.codeBlockFor(other.describe) === $vm.codeBlockFor(a.describe), false);
