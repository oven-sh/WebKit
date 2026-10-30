//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyModuleFunctionDeclarations=0")
//@ runBytecodeCache

// An anonymous default function declaration is bound to the private name "*default*", and its name is "default".
// A bytecode cache holds the name. A module that is made from the cache finds the binding of the declaration by it:
// when it makes its environment (useLazyModuleFunctionDeclarations off), or when the binding is first read.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

async function test() {
    const module = await import("./resources/bytecode-cache-default-export-function-declaration-name.js");
    const stack = module.default();
    assert(stack.startsWith("default@"), "the frame: " + stack);
    assert(module.default.name === "default", "the name: " + module.default.name);
}

test().then(() => { }, (e) => { print("FAIL: " + e + "\n" + e.stack); $vm.abort(); });
