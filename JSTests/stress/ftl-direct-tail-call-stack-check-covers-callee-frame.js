//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=400")

// The stack check of an FTL function must cover the frame that a direct tail
// call builds for its callee. The callee does check the stack, but only once
// its arguments are in place, and the LLInt prologue calls into C++ before it.
//
// The callee's frame here is 192KB, more than the zone that is reserved below
// the stack limit. At the limit of a small stack (an agent's is 512KB on
// Darwin) a frame that nothing checked reaches past the end of the stack.

"use strict";

function agent() {
    "use strict";

    function count() { return arguments.length; }
    noInline(count);

    const chunk = Array.from({ length: 1000 }, (_, i) => i % 11);
    const spreads = 24;
    const f = new Function("count", `"use strict"; return function many(x, rare) { if (rare) { const a = [${chunk.join(",")}]; return count(${Array(spreads).fill("...a").join(",")}); } return x + 1; }`)(count);
    noInline(f);

    let report = "ok";

    // Once below the FTL, so that the call site has a profile.
    if (f(3, true) !== chunk.length * spreads)
        report = "wrong count before the FTL";
    for (let i = 0; i < testLoopCount; i++)
        f(i, false);

    // Down to the stack limit, then the call from each frame on the way back
    // up until there is room for it.
    let overflows = 0;
    let done = false;
    function recurse() {
        try {
            recurse();
        } catch { }
        if (done)
            return;
        try {
            if (f(3, true) !== chunk.length * spreads)
                report = "wrong count from the FTL";
            done = true;
        } catch (e) {
            if (!(e instanceof RangeError))
                report = "unexpected " + e;
            overflows++;
        }
    }
    noInline(recurse);
    recurse();

    if (!overflows)
        report = "the call never ran out of stack";
    if (!done)
        report = "the call never had room";
    $.agent.report(report);
}

$.agent.start(`(${agent})()`);

let report;
while (!(report = $.agent.getReport()))
    $.agent.sleep(1);
if (report !== "ok")
    throw new Error(report);
