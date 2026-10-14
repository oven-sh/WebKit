//@ requireOptions("--useDollarVM=1")
// Bun: the ahead-of-time compiler knows the engine's built-ins by a table (Source/JavaScriptCore/aot/builtins/*.def). Every entry
// has to be found in the engine where the table says, be implemented the way the table says, and a constant has to have the
// table's value. After an upstream sync each line printed here names an entry of the table to mend.
if (typeof $vm.aotTypedBuiltinMismatches === "function") {
    const mismatches = $vm.aotTypedBuiltinMismatches();
    if (mismatches.length)
        throw new Error(mismatches.length + " entries of the table of built-ins do not agree with the engine:\n" + mismatches.join("\n"));
}
