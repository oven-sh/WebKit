//@ requireOptions("--validateICWatchpointLiveness=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=1000")
// An optimized CodeBlock that is jettisoned while one of its frames is live keeps its inline cache stub routines.
// Their structure-transition watchpoints are keyed on the prototype objects the access cases were built for; those
// objects must stay live (or the routine must be dropped) for as long as the watchpoints can fire.
"use strict";

let folded = 1; // constant-folded by the optimizing JIT; writing it jettisons hot()

function hot(o, key, v, callback) {
    o[key] = v + folded + o.onProto;
    if (callback)
        callback();
    return o;
}
noInline(hot);

const base = {};
Object.defineProperty(base, "accessor", { get() { return 1; }, set(v) { }, configurable: true });
let doomedProto = Object.create(base);
doomedProto.onProto = 1;
let twinProto = Object.create(base);
twinProto.onProto = 1;
Object.create(twinProto); // both are prototypes now, so they share a Structure

const otherProto = Object.create({});
otherProto.onProto = 2;

function makeChild(proto, shape) {
    const o = Object.create(proto);
    for (let i = 0; i < shape; i++)
        o["s" + i] = i;
    return o;
}
noInline(makeChild);

function drive(n, proto) {
    for (let i = 0; i < n; i++) {
        hot(makeChild(proto, i & 3), (i & 1) ? "a" : "b", i);
        hot(makeChild(otherProto, i & 3), (i & 1) ? "a" : "b", i);
    }
}
noInline(drive);

drive(3000, doomedProto);
drive(2000, doomedProto); // fills the optimized hot()'s own ICs with cases whose conditions are keyed on doomedProto

function scrub(n) { return n <= 0 ? 0 : 1 + scrub(n - 1); }
noInline(scrub);

hot(makeChild(otherProto, 0), "a", 1, function () {
    folded = 2;         // jettisons the optimized hot() while its frame is on the stack
    doomedProto = null; // last reference
    scrub(3000);
    fullGC();           // --validateICWatchpointLiveness crashes here if a live routine still watches doomedProto
    fullGC();
    twinProto.added = 1; // fires the shared Structure's transition watchpoints
});
