//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// call-link-record-pin-dying-caller.js — a published CallLinkRecord pins the
// callee CodeBlock it names (Heap::pinRetiredCallLinkRecordCodeBlock). When
// the caller CodeBlock dies, ~CodeBlock destroys the MetadataTable that owns
// its LLInt / baseline DataOnlyCallLinkInfos, and each ~CallLinkInfo releases
// its record's pin; otherwise the callee (and, through its executable graph,
// the callee's whole global object) would be marked forever.
//
// Shape: realm B's body calls a function from realm C enough times for the
// call site to link monomorphically (record pins C's CodeBlock), then both
// realms are dropped. C's global object must become collectable.

if (typeof createGlobalObject !== "function" || typeof $vm === "undefined")
    throw new Error("this test needs the jsc shell with --useDollarVM=1");

const realmCount = 10;

function makeRealms(count) {
    for (let k = 0; k < count; k++) {
        let calleeRealm = createGlobalObject();
        let callee = calleeRealm.Function("x", "return x + 1;");
        let callerRealm = createGlobalObject();
        let body = callerRealm.Function("f", "let s = 0; for (let i = 0; i < 30; i++) s += f(i); return s;");
        if (body(callee) !== 465)
            throw new Error("unexpected call result");
    }
}

const before = $vm.globalObjectCount();
makeRealms(realmCount);

// The pin is released when the sweep destroys the dying caller, after which
// the callee's CodeBlock (and its global) can be collected by a later cycle.
for (let i = 0; i < 12; i++)
    $vm.gc();

const retained = $vm.globalObjectCount() - before;
if (retained > realmCount / 2)
    throw new Error(`callee realms retained after GC: ${retained} of ${realmCount} (records of dead callers still pin their callees)`);
