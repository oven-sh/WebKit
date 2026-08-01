// A shorthand property inside an arrow function should not make the enclosing
// ordinary function capture its `arguments` object in the activation.
//
// parseProperty used to call setInnerArrowFunctionUsesEval() unconditionally for
// shorthand properties when currentScope() was an arrow function. That marked
// the arrow as "uses eval", so the enclosing function computed
// m_needsArguments = true and stored `arguments` in its JSLexicalEnvironment.
// Closures returned from such a function then retained every call argument.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error(msg + ": expected " + expected + " but got " + actual);
}

function make(opts) {
    var host = {};
    var worker = (resolvedTypeReferenceDirective) => ({ resolvedTypeReferenceDirective });
    return { worker, inner };
    function inner() { return host; }
}

var refs = [];
var cur = make({ previous: null, payload: null });
for (var i = 0; i < 64; i++) {
    var opts = { previous: cur, payload: {} };
    refs.push(new WeakRef(opts));
    cur = make(opts);
}

gc();
gc();

var alive = 0;
for (var i = 0; i < refs.length; i++) {
    if (refs[i].deref())
        alive++;
}

// Without the fix, every `opts` object is reachable via the closure's captured
// `arguments`, so `alive` is 64. After the fix only the last one or two can
// still be live.
if (alive > 4)
    throw new Error("closure retained arguments: " + alive + "/64 options objects alive after GC");

// Removing the unconditional setInnerArrowFunctionUsesEval() must not break the
// observable semantics of `eval` or `arguments` inside arrows.
(function() {
    "use strict";
    function outer(a, b) {
        var g = () => ({ eval, args: arguments.length });
        return g();
    }
    var r = outer(1, 2, 3);
    shouldBe(typeof r.eval, "function", "eval shorthand");
    shouldBe(r.args, 3, "arguments.length in arrow");
})();

(function() {
    function outer(a, b, c) {
        var g = () => ({ arguments });
        return g();
    }
    var r = outer(1, 2, 3);
    shouldBe(r.arguments.length, 3, "arguments shorthand in arrow");
    shouldBe(r.arguments[2], 3, "arguments shorthand in arrow [2]");
})();
