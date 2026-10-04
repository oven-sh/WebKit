//@ runDefault("--compileMainScriptAheadOfTime=1", "--aotFactsPath=/tmp/jsc-aot-facts.txt")

(function () {
    "use strict";

    let total = 0;
    function makesPoint(a, b) { return { x: a, y: b }; }
    function readsX(point) { return point.x; }
    function storesZ(point, value) { point.z = value; return point; }
    function storesQuotedKey(point, value) { point["quoted key"] = value; return point; }
    function storesByKey(object, key, value) { object[key] = value; return object; }
    function addsToTotal(value) { total += value; return total; }
    function listsKeys(object) { return Object.keys(object); }
    function Pair(first, second) { this.first = first; this.second = second; }
    function storesItself(object) { object.self = object; return object; }

    const point = makesPoint(1, 2);
    if (readsX(point) !== 1 || storesZ(point, 3).z !== 3 || storesQuotedKey(point, 4)["quoted key"] !== 4 || storesByKey(point, "w", 5).w !== 5)
        throw new Error("wrong property");
    if (addsToTotal(2) !== 2 || addsToTotal(3) !== 5)
        throw new Error("wrong total");
    if (listsKeys(point).join() !== "x,y,z,quoted key,w")
        throw new Error("wrong keys");
    if (new Pair(1, 2).second !== 2)
        throw new Error("wrong pair");
    const loop = storesItself({ });
    if (loop.self !== loop)
        throw new Error("wrong loop");

    if (!aotRemarks("makesPoint"))
        return;

    const facts = readFile("/tmp/jsc-aot-facts.txt");
    writeFile("/tmp/jsc-aot-facts.txt", "");
    function has(pattern) {
        if (!pattern.test(facts))
            throw new Error("the facts have no line like " + pattern);
    }
    function hasNot(pattern) {
        if (pattern.test(facts))
            throw new Error("the facts have a line like " + pattern);
    }

    for (const line of facts.split("\n")) {
        if (line && !/^(F(\t[^\t]*){8}|B(\t[^\t]*){4}|N(\t[^\t]*){5})$/.test(line))
            throw new Error("a line of the facts is broken: " + line);
    }

    has(/^F\t\d+\t\d+\t\d+\t\d+\t\d+\t\d+\t\d+\tcall$/m);
    has(/^F\t\d+\t\d+\t\d+\t\d+\t\d+\t\d+\t\d+\tconstruct$/m);
    has(/^B\t\d+\t\t\t \d+$/m);
    has(/^B\t\d+\t\t \d+\t$/m);
    has(/^N\t\d+\tArgument\t\t \S+\t$/m);
    has(/^N\t\d+\top_new_object\t\d+\t name=x store=\d+ name=y store=\d+\t \S+:\d+ \S+:\d+$/m);
    has(/^N\t\d+\top_create_this\t\d+\t name=first name=second store=\d+:0 store=\d+:1\t/m);
    has(/^N\t\d+\top_get_by_id\t\d+\t name=x\t \S+:\d+$/m);
    has(/^N\t\d+\top_put_by_id\t\d+\t name=z\t \S+:\d+ \S+:\d+$/m);
    has(/^N\t\d+\top_put_by_id\t\d+\t name=quoted\\u0020key\t/m);
    has(/^N\t\d+\top_put_by_val\t\d+\t\t \S+:\d+ \S+:\d+ \S+:\d+$/m);
    has(/^N\t\d+\top_put_by_id\t\d+\t name=self\t (\S+:\d+) \1$/m);
    has(/^N\t\d+\top_new_func\t\d+\t function=\d+:\d+:\d+:\d+\t/m);
    has(/^N\t\d+\top_call\t\d+\t callee=\d+/m);
    has(/^N\t\d+\top_construct\t\d+\t callee=\d+/m);
    has(/^N\t\d+\tIntrinsic\t\t \S*\.keys\t$/m);
    has(/^N\t\d+\top_ret\t\d+\t\t \S+:\d+$/m);

    const read = /^N\t\d+\top_get_from_scope\t\d+\t name=total variable=(\d+:\d+)\t/m.exec(facts);
    const written = /^N\t\d+\top_put_to_scope\t\d+\t name=total variable=(\d+:\d+)\t/m.exec(facts);
    if (!read || !written || read[1] !== written[1])
        throw new Error("the read and the store of one variable do not name the same variable");

    hasNot(/ name=z direct/);
    hasNot(/ name=neverMentioned/);
    hasNot(/^N\t\d+\top_put_by_val\t\d+\t name=/m);
})();
