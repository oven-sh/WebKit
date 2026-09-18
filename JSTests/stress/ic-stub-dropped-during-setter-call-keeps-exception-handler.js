//@ requireOptions("--useConcurrentJIT=false", "--repatchBufferingCountdown=6")

// An inline cache stub that calls a setter from FTL code has an exception handler of its own in the CodeBlock, which
// leads to the catch block. The collection inside the setter resets the inline cache (it holds a buffered case whose
// structure is dead), which drops the stub while its code is still on the stack. The handler has to stay until that
// code has returned: without it the exception thrown by the setter leaves foo past its catch block.
function foo(o, p) {
    var x = 100;
    var result = 101;
    var pf = p.g;
    try {
        x = 102;
        pf++;
        o.f = x + pf;
        o = 104;
        pf++;
        x = 106;
    } catch (e) {
        return {outcome: "exception", values: [o, pf, x]};
    }
    return {outcome: "return", values: [o, pf, x]};
}
noInline(foo);

function make(setter) {
    var o = {};
    o.__defineSetter__("f", setter);
    return o;
}
noInline(make);

function warmup(i) {
    var o = make(function(value) { this._f = value; });
    if (i >= 0)
        o["p" + i] = i;
    foo(o, {g:200});
}
noInline(warmup);

// Many structures while foo is in LLInt and Baseline, so that DFG and FTL compile o.f = ... as a
// put_by_id inline cache instead of speculating on one structure.
for (var i = 0; i < 100; ++i)
    warmup(i);
// One structure from here on, until foo is FTL code whose inline cache holds a stub for it.
for (var i = 0; i < 300000; ++i)
    warmup(-1);

// Buffers a case whose structure has a prototype nothing else references.
function addDoomedCase() {
    var o = Object.create({});
    o.__defineSetter__("f", function(value) { });
    foo(o, {g:200});
}
noInline(addDoomedCase);
addDoomedCase();

function clobber(n) { return n ? clobber(n - 1) + 1 : 0; }
noInline(clobber);
clobber(200);

var o = make(function() {
    gc();
    throw "Error42";
});
var result = foo(o, {g:300});
if (result.outcome != "exception")
    throw "Error: bad outcome " + result.outcome;
