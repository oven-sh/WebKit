//@ requireOptions("--useDollarVM=1", "--useShadowRealm=1", "--useTemporal=1", "--useSharedArrayBuffer=1")
// Bun: the ahead-of-time compiler knows the engine's built-ins by a table (Source/JavaScriptCore/aot/builtins/*.def). Every entry has to be found in the
// engine where the table says and be implemented the way the table says, a constant has to have the table's value, and every name or symbol on Object.prototype
// has to be in the table. After an upstream sync each line printed here names an entry of the table to correct. The options are the ones Bun turns on.
if (typeof $vm.aotBuiltinMismatches === "function") {
    // The shell puts undefined under `console` on its own global object (jsc.cpp), so this one entry is not where the table says. Another realm has it.
    const hiddenByTheShell = "console: is not an object";
    if (typeof console !== "undefined")
        throw new Error("the shell no longer hides console: take hiddenByTheShell out of this test");

    function expectMismatches(expected, when) {
        const actual = $vm.aotBuiltinMismatches().filter(message => message !== hiddenByTheShell);
        if (actual.length !== expected.length || actual.some((message, index) => message !== expected[index]))
            throw new Error(when + ": expected " + expected.length + " mismatches between the table of built-ins and the engine, got " + actual.length + ":\n" + actual.join("\n"));
    }

    expectMismatches([], "in a new realm");

    const otherRealm = $vm.createGlobalObject();
    const mismatchesOfOtherRealm = () => otherRealm.$vm.aotBuiltinMismatches().join("\n");
    if (mismatchesOfOtherRealm() !== "")
        throw new Error("in a realm that is not the shell's: expected no mismatch, got:\n" + mismatchesOfOtherRealm());
    otherRealm.console = undefined;
    if (mismatchesOfOtherRealm() !== hiddenByTheShell)
        throw new Error("with console hidden as the shell hides it: expected '" + hiddenByTheShell + "', got:\n" + mismatchesOfOtherRealm());

    const hypot = Math.hypot;
    Math.hypot = function () { };
    expectMismatches(["Math.hypot: has another kind of callee than the table says"], "with Math.hypot replaced");
    Math.hypot = hypot;

    // Compiled code never calls Math.floor: it is generated in line, so the table says nothing of its callee. What a function in its place has of its own is held against the table.
    const floor = Math.floor;
    Math.floor = function () { };
    expectMismatches(["Math.floor.prototype: is not in the table"], "with Math.floor replaced");
    Math.floor = floor;

    Object.prototype.extra = 1;
    expectMismatches(["Object.prototype.extra: is not in the table"], "with a name added to Object.prototype");
    delete Object.prototype.extra;

    Object.prototype[Symbol.match] = 1;
    expectMismatches(["Object.prototype[Symbol.match]: is not in the table"], "with a symbol added to Object.prototype");
    delete Object.prototype[Symbol.match];

    const at = Object.getOwnPropertyDescriptor(Array.prototype, "at");
    delete Array.prototype.at;
    expectMismatches(["Array.prototype.at: is missing"], "with Array.prototype.at deleted");
    Object.defineProperty(Array.prototype, "at", at);

    expectMismatches([], "with everything put back");
}
