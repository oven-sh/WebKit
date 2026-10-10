//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}

(function () {
    function visited(a) {
        const seen = [];
        a.forEach(function (value, index) { seen.push(index + ":" + value); });
        return seen.join(" ");
    }
    function mapped(a) { return Array.prototype.join.call(a.map(function (value) { return value + "!"; })); }
    function filtered(a) { return Array.prototype.join.call(a.filter(function (value) { return value !== "b"; })); }
    function hasInherited(a) { return a.some(function (value) { return value === "inherited"; }); }
    function lacksInherited(a) { return a.every(function (value) { return value !== "inherited"; }); }
    function concatenated(a) { return a.reduce(function (all, value) { return all + value; }, ">"); }
    const all = [visited, mapped, filtered, hasInherited, lacksInherited, concatenated];
    function results(a) { return all.map(f => String(f(a))).join(" | "); }

    const withHole = "0:a 2:c | a!,,c! | a,c | false | true | >ac";
    const withInherited = "0:a 1:inherited 2:c | a!,inherited!,c! | a,inherited,c | true | false | >ainheritedc";

    check(results(["a", , "c"]), withHole, "an original array with a hole");
    check(results(["a", "b", "c"]), "0:a 1:b 2:c | a!,b!,c! | a,c | false | true | >abc", "an original array");
    check(results([]), " |  |  | false | true | >", "an empty array");
    check(results([1.5, , 2.5]), "0:1.5 2:2.5 | 1.5!,,2.5! | 1.5,2.5 | false | true | >1.52.5", "an array of doubles with a hole");
    for (const f of all)
        applies(f, "inlined-builtin");

    const reparented = ["a", , "c"];
    Object.setPrototypeOf(reparented, { __proto__: Array.prototype, 1: "inherited" });
    check(results(reparented), withInherited, "an array with another prototype");

    const twiceRemoved = ["a", , "c"];
    Object.setPrototypeOf(twiceRemoved, { __proto__: { __proto__: Array.prototype, 1: "inherited" } });
    check(results(twiceRemoved), withInherited, "an array whose second prototype has the element");

    class Sub extends Array { }
    Sub.prototype[1] = "inherited";
    const ofSubclass = new Sub(3);
    ofSubclass[0] = "a";
    ofSubclass[2] = "c";
    check(results(ofSubclass), withInherited, "an instance of a subclass");

    const nonEnumerable = ["a", , "c"];
    Object.setPrototypeOf(nonEnumerable, Object.create(Array.prototype, { 1: { value: "inherited" } }));
    check(results(nonEnumerable), withInherited, "an inherited element that is read-only");

    const withGetter = ["a", , "c"];
    Object.setPrototypeOf(withGetter, Object.create(Array.prototype, { 1: { get() { return "inherited"; } } }));
    check(results(withGetter), withInherited, "an inherited accessor");

    const foreign = createGlobalObject().Array("a", "b", "c");
    delete foreign[1];
    Object.setPrototypeOf(foreign, Array.prototype);
    check(results(foreign), withHole, "an array of another realm with this realm's prototype");

    const withOwnProperty = ["a", , "c"];
    withOwnProperty.extra = 1;
    check(results(withOwnProperty), withHole, "an array with a property of its own");

    const shadowing = ["a", , "c"];
    shadowing.forEach = function (callback) { callback("shadowed", 7); };
    check(visited(shadowing), "7:shadowed", "an array with a forEach of its own");

    const arrayLike = { length: 3, 0: "a", 2: "c", __proto__: { 1: "inherited" } };
    for (const name of ["forEach", "map", "filter", "some", "every", "reduce"])
        arrayLike[name] = Array.prototype[name];
    check(results(arrayLike), withInherited, "an object that is no array");

    check(results($vm.createRuntimeArray(1, 2, 3)), "0:1 1:2 2:3 | 1!,2!,3! | 1,2,3 | false | true | >123", "an array that keeps its length and elements elsewhere");

    function trapsSeen(install) {
        const log = [];
        const handler = {
            has(target, key) { log.push("has " + String(key)); return Reflect.has(target, key); },
            get(target, key, receiver) {
                if (typeof key === "string" && key !== "forEach")
                    log.push("get " + key);
                return Reflect.get(target, key, receiver);
            },
        };
        const a = ["a", , "c", , ];
        const proxy = new Proxy({ __proto__: Array.prototype, 3: "behind" }, handler);
        if (install === "before")
            Object.setPrototypeOf(a, proxy);
        a.forEach(function (value, index) {
            log.push("call " + index + " " + value);
            if (install === "during" && !index)
                Object.setPrototypeOf(a, proxy);
        });
        return log.join(", ");
    }
    const traps = "call 0 a, has 1, call 2 c, has 3, get 3, call 3 behind";
    check(trapsSeen("before"), traps, "a proxy on the prototype chain");
    check(trapsSeen("during"), traps, "a proxy put on the prototype chain by the callback");
    applies(trapsSeen, "inlined-builtin");

    function reparentedByCallback(method) {
        const a = ["a", , "c"];
        const seen = [];
        const callback = function (value, index) {
            seen.push(index + ":" + value);
            if (!index)
                Object.setPrototypeOf(a, { __proto__: Array.prototype, 1: "inherited" });
            return method === "every";
        };
        switch (method) {
        case "forEach":
            a.forEach(function (value, index) { return callback(value, index); });
            break;
        case "map":
            a.map(function (value, index) { return callback(value, index); });
            break;
        case "filter":
            a.filter(function (value, index) { return callback(value, index); });
            break;
        case "some":
            a.some(function (value, index) { return callback(value, index); });
            break;
        case "every":
            a.every(function (value, index) { return callback(value, index); });
            break;
        case "reduce":
            a.reduce(function (ignored, value, index) { return callback(value, index); }, 0);
            break;
        }
        return seen.join(" ");
    }
    for (const method of ["forEach", "map", "filter", "some", "every", "reduce"])
        check(reparentedByCallback(method), "0:a 1:inherited 2:c", method + " of an array that its callback gives another prototype");
    applies(reparentedByCallback, "inlined-builtin");

    function truncatedByCallback() {
        const a = ["a", "b", "c"];
        Object.setPrototypeOf(a, Array.prototype);
        const seen = [];
        a.forEach(function (value, index) {
            seen.push(index + ":" + value);
            if (!index) {
                a.length = 1;
                Object.setPrototypeOf(a, { __proto__: Array.prototype, 2: "inherited" });
            }
        });
        return seen.join(" ");
    }
    check(truncatedByCallback(), "0:a 2:inherited", "an element past the end that is inherited");

    function sumsWithoutCalls(a) {
        let sum = 0;
        a.forEach(function (value) { sum += value; });
        return sum;
    }
    check(sumsWithoutCalls([1, , 3]), 4, "a sum over a hole");
    const numbers = [1, , 3];
    Object.setPrototypeOf(numbers, { __proto__: Array.prototype, 1: 2 });
    check(sumsWithoutCalls(numbers), 6, "a sum over an inherited element");
    applies(sumsWithoutCalls, "inlined-builtin");

    function hasTwo(a) { return a.some(function (value) { return value === 2; }); }
    check(hasTwo([1, , 3]), false, "a search over a hole");
    check(hasTwo([1, 2, 3]), true, "a search");
    check(hasTwo(numbers), true, "a search over an inherited element");
    applies(hasTwo, "inlined-builtin", "array-view");

    function elementAddedToArrayPrototype() {
        const a = ["a", , "c"];
        const seen = [];
        a.forEach(function (value, index) {
            seen.push(index + ":" + value);
            if (!index)
                Array.prototype[1] = "inherited";
        });
        delete Array.prototype[1];
        return seen.join(" ");
    }
    check(elementAddedToArrayPrototype(), "0:a 1:inherited 2:c", "an element that the callback adds to Array.prototype");
    check(results(["a", , "c"]), withHole, "an original array with a hole, afterwards");
})();
