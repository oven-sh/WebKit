//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=400", "--maxPerThreadStackUsage=4294967295")

// The stack check of an FTL function must cover the frame that a direct tail
// call builds for its callee. The callee does check the stack, but only once
// its arguments are in place, and the LLInt prologue calls into C++ before it.
//
// The callee's frame here is 192KB, more than the zone that is reserved below
// the stack limit, so at the limit a frame that nothing checked reaches past
// the end of the stack. With no cap on the stack usage the limit is that zone
// above the end of the stack, however large the agent's stack is.

"use strict";

function agent() {
    "use strict";

    function count() { return arguments.length; }
    noInline(count);

    const chunk = Array.from({ length: 1000 }, (_, i) => i % 11);
    const spreads = 24;
    const f = new Function("count", `"use strict"; return function many(x, rare) { if (rare) { const a = [${chunk.join(",")}]; return count(${Array(spreads).fill("...a").join(",")}); } return x + 1; }`)(count);
    noInline(f);

    // Once below the FTL, so that the call site has a profile.
    if (f(3, true) !== chunk.length * spreads)
        throw new Error("wrong count before the FTL");
    for (let i = 0; i < testLoopCount; i++)
        f(i, false);

    // Down to the stack limit, then the call from each frame on the way back
    // up until there is room for it. The frames above catch whatever is thrown
    // here, so a failure is kept for later.
    let overflows = 0;
    let done = false;
    let failure = null;
    function recurse() {
        try {
            recurse();
        } catch { }
        if (done)
            return;
        try {
            if (f(3, true) !== chunk.length * spreads)
                failure = "wrong count from the FTL";
            done = true;
        } catch (e) {
            if (!(e instanceof RangeError))
                failure = "unexpected " + e;
            overflows++;
        }
    }
    noInline(recurse);
    recurse();

    if (failure)
        throw new Error(failure);
    if (!overflows)
        throw new Error("the call never ran out of stack");
    if (!done)
        throw new Error("the call never had room");
}

$.agent.start(`
    try {
        (${agent})();
        $.agent.report("ok");
    } catch (e) {
        $.agent.report(String(e));
    }
`);

let report;
while (!(report = $.agent.getReport()))
    $.agent.sleep(1);
if (report !== "ok")
    throw new Error(report);
