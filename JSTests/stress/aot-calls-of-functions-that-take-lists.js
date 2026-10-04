//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const isCompiled = !!(aotRemarks("check") || []).length;
function remarksOf(name) {
    let remarks = aotRemarks(name);
    if (isCompiled && !(remarks && remarks.length))
        throw new Error("no remarks for " + name);
    return isCompiled ? remarks : null;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(name, ...patterns) {
    let remarks = remarksOf(name);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + name + ": " + remarks.join(" "));
    }
}
function doesNotApply(name, ...patterns) {
    let remarks = remarksOf(name);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + name + ": " + remarks.join(" "));
    }
}
function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
const usesDataStubs = (remarksOf("readsProperty") || []).includes("calls:GetById");

(function () {
    "use strict";
    function act(mode, name, values) {
        if (mode === "throw")
            throw new Error(name + ":" + values.join());
        if (mode === "stack")
            return new Error().stack.split("\n").map(line => line.split("@")[0]).join();
        return name + ":" + values.join();
    }
    function takesRest(mode, ...rest) { const result = act(mode, "takesRest", rest); return result; }
    function takesArguments(mode) { const result = act(mode, "takesArguments", Array.prototype.slice.call(arguments, 1)); return result; }
    function takesNine(mode, a, b, c, d, e, f, g, h) { const result = act(mode, "takesNine", [a, b, c, d, e, f, g, h]); return result; }
    function takesTwo(mode, a) { const result = act(mode, "takesTwo", [a]); return result; }
    function Rest(mode, ...rest) { this.result = act(mode, "Rest", rest); }
    function Arguments(mode) { this.result = act(mode, "Arguments", Array.prototype.slice.call(arguments, 1)); }
    function Nine(mode, a, b, c, d, e, f, g, h) { this.result = act(mode, "Nine", [a, b, c, d, e, f, g, h]); }
    function Two(mode, a) { this.result = act(mode, "Two", [a]); }
    const holder = { takesRest, takesArguments, takesNine, takesTwo };
    const constructors = { Rest, Arguments, Nine, Two };

    function plainRest(mode) { try { const result = takesRest(mode, 1, 2); return result; } catch (error) { return "caught " + error.message; } }
    function cachedRest(mode) { try { const result = holder.takesRest(mode, 1, 2); return result; } catch (error) { return "caught " + error.message; } }
    function constructsRest(mode) { try { const result = new Rest(mode, 1, 2).result; return result; } catch (error) { return "caught " + error.message; } }
    function cachedConstructsRest(mode) { try { const result = new constructors.Rest(mode, 1, 2).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailRest(mode) { return takesRest(mode, 1, 2); }
    function cachedTailRest(mode) { return holder.takesRest(mode, 1, 2); }
    function spreadsRest(mode) { try { const list = [mode, 1, 2]; const result = takesRest(...list); return result; } catch (error) { return "caught " + error.message; } }
    function cachedSpreadsRest(mode) { try { const list = [mode, 1, 2]; const result = holder.takesRest(...list); return result; } catch (error) { return "caught " + error.message; } }
    function appliesRest(mode) { try { const list = [mode, 1, 2]; const result = takesRest.apply(null, list); return result; } catch (error) { return "caught " + error.message; } }
    function constructSpreadsRest(mode) { try { const list = [mode, 1, 2]; const result = new Rest(...list).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailSpreadsRest(mode) { const list = [mode, 1, 2]; return takesRest(...list); }

    function plainArguments(mode) { try { const result = takesArguments(mode, 1, 2); return result; } catch (error) { return "caught " + error.message; } }
    function cachedArguments(mode) { try { const result = holder.takesArguments(mode, 1, 2); return result; } catch (error) { return "caught " + error.message; } }
    function constructsArguments(mode) { try { const result = new Arguments(mode, 1, 2).result; return result; } catch (error) { return "caught " + error.message; } }
    function cachedConstructsArguments(mode) { try { const result = new constructors.Arguments(mode, 1, 2).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailArguments(mode) { return takesArguments(mode, 1, 2); }
    function cachedTailArguments(mode) { return holder.takesArguments(mode, 1, 2); }
    function spreadsArguments(mode) { try { const list = [mode, 1, 2]; const result = takesArguments(...list); return result; } catch (error) { return "caught " + error.message; } }
    function cachedSpreadsArguments(mode) { try { const list = [mode, 1, 2]; const result = holder.takesArguments(...list); return result; } catch (error) { return "caught " + error.message; } }
    function appliesArguments(mode) { try { const list = [mode, 1, 2]; const result = takesArguments.apply(null, list); return result; } catch (error) { return "caught " + error.message; } }
    function constructSpreadsArguments(mode) { try { const list = [mode, 1, 2]; const result = new Arguments(...list).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailSpreadsArguments(mode) { const list = [mode, 1, 2]; return takesArguments(...list); }

    function plainNine(mode) { try { const result = takesNine(mode, 1, 2, 3, 4, 5, 6, 7, 8); return result; } catch (error) { return "caught " + error.message; } }
    function cachedNine(mode) { try { const result = holder.takesNine(mode, 1, 2, 3, 4, 5, 6, 7, 8); return result; } catch (error) { return "caught " + error.message; } }
    function constructsNine(mode) { try { const result = new Nine(mode, 1, 2, 3, 4, 5, 6, 7, 8).result; return result; } catch (error) { return "caught " + error.message; } }
    function cachedConstructsNine(mode) { try { const result = new constructors.Nine(mode, 1, 2, 3, 4, 5, 6, 7, 8).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailNine(mode) { return takesNine(mode, 1, 2, 3, 4, 5, 6, 7, 8); }
    function cachedTailNine(mode) { return holder.takesNine(mode, 1, 2, 3, 4, 5, 6, 7, 8); }
    function spreadsNine(mode) { try { const list = [mode, 1, 2, 3, 4, 5, 6, 7, 8]; const result = takesNine(...list); return result; } catch (error) { return "caught " + error.message; } }
    function cachedSpreadsNine(mode) { try { const list = [mode, 1, 2, 3, 4, 5, 6, 7, 8]; const result = holder.takesNine(...list); return result; } catch (error) { return "caught " + error.message; } }
    function appliesNine(mode) { try { const list = [mode, 1, 2, 3, 4, 5, 6, 7, 8]; const result = takesNine.apply(null, list); return result; } catch (error) { return "caught " + error.message; } }
    function constructSpreadsNine(mode) { try { const list = [mode, 1, 2, 3, 4, 5, 6, 7, 8]; const result = new Nine(...list).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailSpreadsNine(mode) { const list = [mode, 1, 2, 3, 4, 5, 6, 7, 8]; return takesNine(...list); }

    function plainTwo(mode) { try { const result = takesTwo(mode, 1); return result; } catch (error) { return "caught " + error.message; } }
    function cachedTwo(mode) { try { const result = holder.takesTwo(mode, 1); return result; } catch (error) { return "caught " + error.message; } }
    function constructsTwo(mode) { try { const result = new Two(mode, 1).result; return result; } catch (error) { return "caught " + error.message; } }
    function cachedConstructsTwo(mode) { try { const result = new constructors.Two(mode, 1).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailTwo(mode) { return takesTwo(mode, 1); }
    function cachedTailTwo(mode) { return holder.takesTwo(mode, 1); }
    function spreadsTwo(mode) { try { const list = [mode, 1]; const result = takesTwo(...list); return result; } catch (error) { return "caught " + error.message; } }
    function cachedSpreadsTwo(mode) { try { const list = [mode, 1]; const result = holder.takesTwo(...list); return result; } catch (error) { return "caught " + error.message; } }
    function appliesTwo(mode) { try { const list = [mode, 1]; const result = takesTwo.apply(null, list); return result; } catch (error) { return "caught " + error.message; } }
    function constructSpreadsTwo(mode) { try { const list = [mode, 1]; const result = new Two(...list).result; return result; } catch (error) { return "caught " + error.message; } }
    function tailSpreadsTwo(mode) { const list = [mode, 1]; return takesTwo(...list); }

    function run(caller, name, callee, values, isTail) {
        for (let i = 0; i < 3; i++)
            check(caller("value"), callee + ":" + values, name);
        let thrown;
        try {
            thrown = caller("throw");
        } catch (error) {
            thrown = "escaped " + error.message;
        }
        check(thrown, (isTail ? "escaped " : "caught ") + callee + ":" + values, name + " when the callee throws");
        const frames = caller("stack");
        check(frames.slice(0, frames.indexOf(",run") + 4), ["act", callee].concat(isTail ? [] : [name], ["run"]).join(), "the frames below " + name);
        check(caller("value"), callee + ":" + values, name + " afterwards");
    }
    run(plainRest, "plainRest", "takesRest", "1,2", false);
    run(cachedRest, "cachedRest", "takesRest", "1,2", false);
    run(constructsRest, "constructsRest", "Rest", "1,2", false);
    run(cachedConstructsRest, "cachedConstructsRest", "Rest", "1,2", false);
    run(tailRest, "tailRest", "takesRest", "1,2", true);
    run(cachedTailRest, "cachedTailRest", "takesRest", "1,2", true);
    run(spreadsRest, "spreadsRest", "takesRest", "1,2", false);
    run(cachedSpreadsRest, "cachedSpreadsRest", "takesRest", "1,2", false);
    run(appliesRest, "appliesRest", "takesRest", "1,2", false);
    run(constructSpreadsRest, "constructSpreadsRest", "Rest", "1,2", false);
    run(tailSpreadsRest, "tailSpreadsRest", "takesRest", "1,2", true);
    run(plainArguments, "plainArguments", "takesArguments", "1,2", false);
    run(cachedArguments, "cachedArguments", "takesArguments", "1,2", false);
    run(constructsArguments, "constructsArguments", "Arguments", "1,2", false);
    run(cachedConstructsArguments, "cachedConstructsArguments", "Arguments", "1,2", false);
    run(tailArguments, "tailArguments", "takesArguments", "1,2", true);
    run(cachedTailArguments, "cachedTailArguments", "takesArguments", "1,2", true);
    run(spreadsArguments, "spreadsArguments", "takesArguments", "1,2", false);
    run(cachedSpreadsArguments, "cachedSpreadsArguments", "takesArguments", "1,2", false);
    run(appliesArguments, "appliesArguments", "takesArguments", "1,2", false);
    run(constructSpreadsArguments, "constructSpreadsArguments", "Arguments", "1,2", false);
    run(tailSpreadsArguments, "tailSpreadsArguments", "takesArguments", "1,2", true);
    run(plainNine, "plainNine", "takesNine", "1,2,3,4,5,6,7,8", false);
    run(cachedNine, "cachedNine", "takesNine", "1,2,3,4,5,6,7,8", false);
    run(constructsNine, "constructsNine", "Nine", "1,2,3,4,5,6,7,8", false);
    run(cachedConstructsNine, "cachedConstructsNine", "Nine", "1,2,3,4,5,6,7,8", false);
    run(tailNine, "tailNine", "takesNine", "1,2,3,4,5,6,7,8", true);
    run(cachedTailNine, "cachedTailNine", "takesNine", "1,2,3,4,5,6,7,8", true);
    run(spreadsNine, "spreadsNine", "takesNine", "1,2,3,4,5,6,7,8", false);
    run(cachedSpreadsNine, "cachedSpreadsNine", "takesNine", "1,2,3,4,5,6,7,8", false);
    run(appliesNine, "appliesNine", "takesNine", "1,2,3,4,5,6,7,8", false);
    run(constructSpreadsNine, "constructSpreadsNine", "Nine", "1,2,3,4,5,6,7,8", false);
    run(tailSpreadsNine, "tailSpreadsNine", "takesNine", "1,2,3,4,5,6,7,8", true);
    run(plainTwo, "plainTwo", "takesTwo", "1", false);
    run(cachedTwo, "cachedTwo", "takesTwo", "1", false);
    run(constructsTwo, "constructsTwo", "Two", "1", false);
    run(cachedConstructsTwo, "cachedConstructsTwo", "Two", "1", false);
    run(tailTwo, "tailTwo", "takesTwo", "1", true);
    run(cachedTailTwo, "cachedTailTwo", "takesTwo", "1", true);
    run(spreadsTwo, "spreadsTwo", "takesTwo", "1", false);
    run(cachedSpreadsTwo, "cachedSpreadsTwo", "takesTwo", "1", false);
    run(appliesTwo, "appliesTwo", "takesTwo", "1", false);
    run(constructSpreadsTwo, "constructSpreadsTwo", "Two", "1", false);
    run(tailSpreadsTwo, "tailSpreadsTwo", "takesTwo", "1", true);

    function sumsRest(n, ...rest) { if (!n) return 0; const below = holder.sumsRest(n - 1, ...rest); return below + rest.length; }
    function sumsNine(n, a, b, c, d, e, f, g, h) { if (!n) return 0; const below = holder.sumsNine(n - 1, a, b, c, d, e, f, g, h); return below + h; }
    function SumsRest(n, ...rest) { this.sum = n ? new constructors.SumsRest(n - 1, ...rest).sum + rest.length : 0; }
    holder.sumsRest = sumsRest;
    holder.sumsNine = sumsNine;
    constructors.SumsRest = SumsRest;
    check(sumsRest(2000, 1, 2, 3), 6000, "returns through 2000 calls with rest parameters");
    check(sumsNine(2000, 1, 2, 3, 4, 5, 6, 7, 8), 16000, "returns through 2000 calls with nine parameters");
    check(new SumsRest(2000, 1, 2).sum, 4000, "returns through 2000 constructs with rest parameters");

    function loopsRest(n, ...rest) { if (!n) return rest.length; return holder.loopsRest(n - 1, ...rest); }
    function loopsNine(n, a, b, c, d, e, f, g, h) { if (!n) return h; return holder.loopsNine(n - 1, a, b, c, d, e, f, g, h); }
    function loopsArguments(n) { if (!n) return arguments.length; return holder.loopsArguments(n - 1, 1, 2); }
    function loopsDirectly(n, ...rest) { if (!n) return rest.length; return loopsDirectly(n - 1, 1, 2, 3); }
    holder.loopsRest = loopsRest;
    holder.loopsNine = loopsNine;
    holder.loopsArguments = loopsArguments;
    check(loopsRest(300000, 1, 2, 3), 3, "300000 tail calls with a spread");
    check(loopsNine(300000, 1, 2, 3, 4, 5, 6, 7, 8), 8, "300000 tail calls with nine parameters");
    check(loopsArguments(300000, 1, 2), 3, "300000 tail calls of a function that reads arguments");
    check(loopsDirectly(300000), 3, "300000 tail calls of a known function with rest parameters");

    function overflowsRest(n, ...rest) { const below = holder.overflowsRest(n + 1, 1, 2); return below + 1; }
    function overflowsSpread(n, ...rest) { const below = holder.overflowsSpread(n + 1, ...rest); return below + 1; }
    function overflowsNine(n, a, b, c, d, e, f, g, h) { const below = holder.overflowsNine(n + 1, a, b, c, d, e, f, g, h); return below + 1; }
    function overflowsApply(n) { const below = holder.overflowsApply.apply(null, [n + 1, 1, 2]); return below + arguments.length; }
    function OverflowsRest(n, ...rest) { this.below = new constructors.OverflowsRest(n + 1, ...rest); }
    holder.overflowsRest = overflowsRest;
    holder.overflowsSpread = overflowsSpread;
    holder.overflowsNine = overflowsNine;
    holder.overflowsApply = overflowsApply;
    constructors.OverflowsRest = OverflowsRest;
    function overflowOf(f) {
        try {
            f();
        } catch (error) {
            return error instanceof RangeError ? "RangeError" : String(error);
        }
        return "nothing";
    }
    for (let i = 0; i < 2; i++) {
        check(overflowOf(() => overflowsRest(0, 1, 2)), "RangeError", "recursion through rest parameters");
        check(overflowOf(() => overflowsSpread(0, 1, 2)), "RangeError", "recursion through a spread");
        check(overflowOf(() => overflowsNine(0, 1, 2, 3, 4, 5, 6, 7, 8)), "RangeError", "recursion through nine parameters");
        check(overflowOf(() => overflowsApply(0)), "RangeError", "recursion through apply");
        check(overflowOf(() => new OverflowsRest(0, 1, 2)), "RangeError", "recursion through constructs");
        check(cachedRest("value"), "takesRest:1,2", "a call after the stack overflowed");
        check(spreadsNine("value"), "takesNine:1,2,3,4,5,6,7,8", "a spread after the stack overflowed");
    }
})();

(function () {
    function mapped(a, b) { a = 5; arguments[1] = 6; return a + "," + b + "," + arguments[0] + "," + arguments[1] + "," + arguments.length; }
    const holder = { mapped };
    function plainMapped() { const result = mapped(1, 2, 3); return result; }
    function cachedMapped() { const result = holder.mapped(1, 2, 3); return result; }
    function spreadsMapped() { const list = [1, 2, 3]; const result = holder.mapped(...list); return result; }
    function appliesMapped() { const result = mapped.apply(null, [1, 2, 3]); return result; }
    for (let i = 0; i < 3; i++) {
        check(plainMapped(), "5,6,5,6,3", "mapped arguments of a known function");
        check(cachedMapped(), "5,6,5,6,3", "mapped arguments through a cached call");
        check(spreadsMapped(), "5,6,5,6,3", "mapped arguments through a spread");
        check(appliesMapped(), "5,6,5,6,3", "mapped arguments through apply");
    }
})();

applies("takesRest", "takes-argument-list");
applies("Rest", "takes-argument-list");
applies("takesArguments", "takes-argument-list");
applies("Arguments", "takes-argument-list");
applies("takesNine", "takes-argument-list");
applies("Nine", "takes-argument-list");
applies("mapped", "takes-argument-list");
doesNotApply("takesTwo", "takes-argument-list");
doesNotApply("Two", "takes-argument-list");
(usesDataStubs ? applies : doesNotApply)("cachedRest", "cached-call");
(usesDataStubs ? applies : doesNotApply)("cachedTailRest", "cached-call");
doesNotApply("spreadsRest", "cached-call", "direct-call");
(usesDataStubs ? applies : doesNotApply)("cachedArguments", "cached-call");
(usesDataStubs ? applies : doesNotApply)("cachedTailArguments", "cached-call");
doesNotApply("spreadsArguments", "cached-call", "direct-call");
(usesDataStubs ? applies : doesNotApply)("cachedTwo", "cached-call");
(usesDataStubs ? applies : doesNotApply)("cachedTailTwo", "cached-call");
doesNotApply("spreadsTwo", "cached-call", "direct-call");
applies("plainRest", "direct-call:takesRest");
applies("constructsRest", "direct-construct:Rest");
doesNotApply("cachedRest", "direct-call");
doesNotApply("cachedConstructsRest", "direct-construct");
applies("plainArguments", "direct-call:takesArguments");
applies("constructsArguments", "direct-construct:Arguments");
doesNotApply("cachedArguments", "direct-call");
doesNotApply("cachedConstructsArguments", "direct-construct");
applies("plainNine", "direct-call:takesNine");
applies("constructsNine", "direct-construct:Nine");
doesNotApply("cachedNine", "direct-call");
doesNotApply("cachedConstructsNine", "direct-construct");
applies("plainTwo", "direct-call:takesTwo");
applies("constructsTwo", "direct-construct:Two");
doesNotApply("cachedTwo", "direct-call");
doesNotApply("cachedConstructsTwo", "direct-construct");
