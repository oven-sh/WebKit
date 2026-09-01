//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// NEGATIVE test for UNGIL K4 §VIII.9 (U-T8b; RE-SCOPE A-t8assert 2026-06-12):
// after the VM's first cross-thread entry, a POST-INIT rewrite of a
// JSGlobalObject's m_globalThis slot (resetPrototype -> setGlobalThis on a
// non-null slot — the global may be published) must still FAIL-STOP in
// ASSERT builds under effective GIL-off. This is the protective half the
// re-scope explicitly preserved: only finishCreation's init write (null
// slot, unpublishable global) was unlocked.
//
// Two arms:
//   Default (corpus) mode — runs the LEGITIMATE arm only: after a thread
//     has entered the VM (cross-thread entry noted), a spawned thread AND
//     the main thread init-create brand-new globals ($vm.createGlobalObject
//     -> finishCreation -> setGlobalThis on a null slot). Under the
//     re-scoped assert this must NOT fail-stop. Prints PASS.
//   Crash arm — invoked with the script argument "crash-arm"
//     (jsc <opts> thisfile.js -- crash-arm). Requires $vm.assertEnabled()
//     and effective GIL-off ($vm.useThreadGIL() === false); otherwise prints
//     a SKIP PASS (the assert is an exact no-op release / GIL-on by
//     contract). When armed: performs the legitimate arm, then calls
//     $vm.resetPrototypeOfGlobalObject(g, {}) — a post-init m_globalThis
//     rewrite after cross-thread entry. EXPECTED OUTCOME: abort (assert
//     fail-stop). Reaching the line after the call is a FAIL (nonzero
//     exit). The corpus runner never passes script arguments, so the crash
//     arm never fires in a corpus run; it is exercised by the pinned
//     §VIII.9 verify:
//       JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 \
//       JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 \
//       JSC_useThreadGILOffUnsafe=1 \
//       WebKitBuild/Debug/bin/jsc --useJSThreads=1 --useDollarVM=1 \
//         JSTests/threads/vmstate/globalthis-postpublication-negative.js \
//         -- crash-arm
//     => must die by signal (SIGABRT), not exit 0, not exit 3.

function fail(msg) {
    print("FAIL: " + msg);
    throw new Error(msg);
}

const scriptArgs = (typeof arguments !== "undefined" && arguments && arguments.length) ? arguments : [];
const crashArm = Array.prototype.indexOf.call(scriptArgs, "crash-arm") >= 0;

if (typeof Thread !== "function" || typeof $vm === "undefined" || typeof $vm.createGlobalObject !== "function") {
    print("PASS (no Thread/$vm — nothing to test in this configuration)");
} else {
    // 1. Note the VM's first cross-thread entry: spawn and join a thread.
    const t = new Thread(function () { return 42; });
    if (t.join() !== 42)
        fail("thread join oracle");

    // 2. LEGITIMATE arm (the re-scope's unlocked path): brand-new globals
    //    AFTER cross-thread entry, on a spawned thread and on main. Each
    //    finishCreation init-writes a null m_globalThis slot — must not
    //    fail-stop.
    const t2 = new Thread(function () {
        const g = $vm.createGlobalObject();
        return (g && typeof g === "object") ? 1 : 0;
    });
    if (t2.join() !== 1)
        fail("spawned-thread createGlobalObject (legitimate init arm)");
    const g = $vm.createGlobalObject();
    if (!g || typeof g !== "object")
        fail("main-thread createGlobalObject (legitimate init arm)");

    if (!crashArm) {
        print("PASS");
    } else if (typeof $vm.resetPrototypeOfGlobalObject !== "function") {
        fail("crash-arm requested but $vm.resetPrototypeOfGlobalObject is missing");
    } else if (!$vm.assertEnabled()) {
        print("PASS (SKIP crash-arm: assert is a contractual no-op in non-ASSERT builds)");
    } else if ($vm.useThreadGIL()) {
        print("PASS (SKIP crash-arm: assert is a contractual no-op under effective GIL-on)");
    } else {
        // 3. NEGATIVE arm: post-init rewrite of g's m_globalThis after
        //    cross-thread entry. resetPrototype with a fresh object always
        //    takes the setGlobalThis path (prototype differs). MUST abort
        //    inside jsThreadsAssertNoPostInitWriteAfterFirstCrossThreadEntry.
        $vm.resetPrototypeOfGlobalObject(g, { negativeArm: true });
        fail("post-publication cross-thread setGlobalThis did NOT fail-stop");
    }
}
