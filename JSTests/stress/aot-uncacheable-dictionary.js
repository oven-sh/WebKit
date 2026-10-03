//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function repeat(f) {
    for (let i = 0; i < 200; i++)
        f(i);
}
function isDictionary(object) { return describe(object).includes("Dictionary"); }
function makeDictionary() {
    const object = { };
    for (let i = 0; i < 100; i++)
        object["p" + i] = i;
    for (let i = 0; i < 50; i++)
        delete object["p" + i];
    return object;
}
function readP95(o) { return o.p95; }
function readP5(o) { return o.p5; }
function readAdded(o) { return o.added; }
for (const f of [readP95, readP5, readAdded])
    noInline(f);

{
    const object = makeDictionary();
    check(describe(object).includes("UncacheableDictionary"), true, "deleting makes an uncacheable dictionary");
    repeat(() => check(readP95(object), 95, "a property of a dictionary"));
    if (isAOTCompiled(readP95))
        check(isDictionary(object), false, "reading flattens a dictionary");
    repeat(() => check(readP95(object), 95, "a property after flattening"));
    repeat(() => check(readP5(object), undefined, "a deleted property after flattening"));
    const names = [];
    for (let i = 50; i < 100; i++) {
        check(object["p" + i], i, "every property after flattening");
        names.push("p" + i);
    }
    check(Object.keys(object).join(), names.join(), "the order after flattening");

    object.added = "added";
    repeat(() => check(readAdded(object), "added", "a property added after flattening"));
    object.p95 = "changed";
    repeat(() => check(readP95(object), "changed", "a property changed after flattening"));

    for (let i = 0; i < 1000 && !isDictionary(object); i++) {
        object["temporary" + i] = i;
        delete object["temporary" + i];
    }
    check(isDictionary(object), true, "deleting again makes a dictionary again");
    repeat(() => check(readP95(object), "changed", "a property of a dictionary that was flattened before"));
    check(isDictionary(object), true, "a dictionary is flattened only once");
    delete object.p95;
    repeat(() => check(readP95(object), undefined, "a property deleted from a dictionary that was flattened before"));
    object.p95 = "again";
    repeat(() => check(readP95(object), "again", "a property added to a dictionary that was flattened before"));
}

{
    const objects = [];
    for (let i = 0; i < 6; i++) {
        objects.push(makeDictionary());
        objects[i].p95 = "object " + i;
    }
    repeat(i => check(readP95(objects[i % 6]), "object " + i % 6, "several dictionaries at one site"));
    for (const object of isAOTCompiled(readP95) ? objects : [])
        check(isDictionary(object), false, "each of several dictionaries is flattened");
}

{
    const prototype = makeDictionary();
    const object = Object.create(prototype);
    repeat(() => check(readP95(object), 95, "a property inherited from a dictionary"));
    prototype.p95 = "changed";
    repeat(() => check(readP95(object), "changed", "a property inherited from a dictionary, changed"));
    delete prototype.p95;
    repeat(() => check(readP95(object), undefined, "a property inherited from a dictionary, deleted"));
}
