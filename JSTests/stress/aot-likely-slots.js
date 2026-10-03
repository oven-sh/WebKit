//@ runDefault("--compileMainScriptAheadOfTime=1")
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
function usesStubs(f) { return remarksOf(f)?.some(remark => matches(remark, "calls:GetById")); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
function repeat(f) {
    for (let i = 0; i < 100; i++)
        f(i);
}

function Node(kind, pos, end) {
    this.pos = pos;
    this.end = end;
    this.kind = kind;
    this.id = 0;
    this.flags = 1;
    this.parent = null;
}
function Token(kind, pos, end) {
    this.pos = pos;
    this.end = end;
    this.kind = kind;
    this.id = 0;
    this.flags = 2;
}
function Sym(flags, name) {
    this.flags = flags;
    this.escapedName = name;
    this.parent = null;
}

function kindOf(o) { return o.kind; }
function flagsOf(o) { return o.flags; }
function parentOf(o) { return o.parent; }
function escapedNameOf(o) { return o.escapedName; }
function neverWritten(o) { return o.nobodyWritesThis; }
function addedLater(o) { return o.addedLater; }
function inLiteralOnly(o) { return o.inLiteralOnly; }
for (const f of [kindOf, flagsOf, parentOf, escapedNameOf, neverWritten, addedLater, inLiteralOnly])
    noInline(f);

if (usesStubs(neverWritten)) {
    applies(kindOf, "looks-in-likely-slots:kind");
    applies(flagsOf, "looks-in-likely-slots:flags");
    applies(parentOf, "looks-in-likely-slots:parent");
    applies(escapedNameOf, "looks-in-likely-slots:escapedName");
    applies(inLiteralOnly, "looks-in-likely-slots:inLiteralOnly");
}
doesNotApply(neverWritten, "looks-in-likely-slots");
doesNotApply(addedLater, "looks-in-likely-slots");

repeat(i => {
    check(kindOf(new Node(i, 0, 1)), i, "the kind of a node");
    check(kindOf(new Token(i + 1, 0, 1)), i + 1, "the kind of a token");
    check(flagsOf(new Node(i, 0, 1)), 1, "the flags of a node");
    check(flagsOf(new Token(i, 0, 1)), 2, "the flags of a token");
    check(flagsOf(new Sym(i, "s")), i, "the flags of a symbol, in another slot");
    check(parentOf(new Node(i, 0, 1)), null, "the parent of a node");
    check(parentOf(new Sym(i, "s")), null, "the parent of a symbol");
    check(escapedNameOf(new Sym(i, "s" + i)), "s" + i, "the name of a symbol");
    check(inLiteralOnly({ first: 1, inLiteralOnly: i }), i, "a property of a literal");
    check(neverWritten(new Node(i, 0, 1)), undefined, "a property that nothing writes");
    const extended = new Node(i, 0, 1);
    extended.addedLater = i;
    check(addedLater(extended), i, "a property that is added later");
    check(kindOf(extended), i, "the kind of a node that got another property");
});

repeat(i => {
    check(kindOf({ kind: i }), i, "in the first slot");
    check(kindOf({ a: 0, kind: i }), i, "in the second slot");
    check(kindOf({ a: 0, b: 0, kind: i }), i, "in the slot where constructors put it, in a literal");
    check(kindOf({ a: 0, b: 0, c: 0, kind: i }), i, "in the fourth slot");
    check(kindOf({ pos: 0, end: 1, other: i }), undefined, "another name in that slot");
    check(kindOf({}), undefined, "an empty object");
    check(kindOf(Object.create({ kind: i })), i, "inherited");
    check(kindOf(Object.create(new Node(i, 0, 1))), i, "inherited from a node");
    check(kindOf(5), undefined, "a number");
    check(kindOf("text"), undefined, "a string");
    check(kindOf(true), undefined, "a boolean");
    check(kindOf(Symbol()), undefined, "a symbol");
    check(kindOf([1, 2, 3]), undefined, "an array");
    check(kindOf(function () { }), undefined, "a function");
    check(kindOf(new Map), undefined, "a map");
    let threw = 0;
    for (const nothing of [undefined, null]) {
        try {
            kindOf(nothing);
        } catch (error) {
            threw += error instanceof TypeError;
        }
    }
    check(threw, 2, "undefined and null throw");
});

{
    let calls = 0;
    const withGetter = new Node(1, 0, 1);
    Object.defineProperty(withGetter, "kind", { get() { return ++calls; }, configurable: true });
    repeat(i => check(kindOf(withGetter), i + 1, "the property becomes an accessor"));
    Object.defineProperty(withGetter, "kind", { value: "again", writable: true, enumerable: true, configurable: true });
    repeat(() => check(kindOf(withGetter), "again", "the property becomes a value again"));

    const readOnly = new Node(2, 0, 1);
    Object.defineProperty(readOnly, "kind", { writable: false });
    repeat(() => check(kindOf(readOnly), 2, "the property becomes read-only"));
    const hidden = new Node(3, 0, 1);
    Object.defineProperty(hidden, "kind", { enumerable: false });
    repeat(() => check(kindOf(hidden), 3, "the property stops being enumerable"));
    repeat(() => check(kindOf(Object.freeze(new Node(4, 0, 1))), 4, "a frozen node"));
    repeat(() => check(kindOf(Object.seal(new Node(5, 0, 1))), 5, "a sealed node"));

    const deleted = new Node(6, 0, 1);
    repeat(() => check(kindOf(deleted), 6, "before the property is deleted"));
    delete deleted.kind;
    repeat(() => check(kindOf(deleted), undefined, "after the property is deleted"));
    deleted.kind = 7;
    repeat(() => check(kindOf(deleted), 7, "after the property is added again"));

    const earlierDeleted = new Node(8, 0, 1);
    delete earlierDeleted.pos;
    repeat(() => check(kindOf(earlierDeleted), 8, "after an earlier property is deleted"));
    earlierDeleted.pos = 9;
    repeat(() => check(kindOf(earlierDeleted), 8, "after the earlier property is added again"));

    const replaced = new Node(10, 0, 1);
    delete replaced.kind;
    replaced.somethingElse = "not the kind";
    repeat(() => check(kindOf(replaced), undefined, "after another property is added where it was deleted"));

    const dictionary = new Node(11, 0, 1);
    for (let i = 0; i < 200; i++)
        dictionary["p" + i] = i;
    for (let i = 0; i < 190; i++)
        delete dictionary["p" + i];
    repeat(() => check(kindOf(dictionary), 11, "a dictionary"));
    delete dictionary.kind;
    repeat(() => check(kindOf(dictionary), undefined, "a dictionary that lost the property"));

    let gets = 0;
    const proxy = new Proxy(new Node(12, 0, 1), { get(target, key) { gets++; return target[key]; } });
    repeat(() => check(kindOf(proxy), 12, "a proxy"));
    check(gets, 100, "calls of the trap");

    const changed = new Node(13, 0, 1);
    repeat(i => {
        changed.kind = i;
        check(kindOf(changed), i, "a value that changes");
    });
    changed.kind = "text";
    check(kindOf(changed), "text", "a value of another type");
    changed.kind = { nested: true };
    check(kindOf(changed).nested, true, "an object");
}

{
    const prototype = { kind: "inherited" };
    function Derived() { this.pos = 0; this.end = 1; }
    Derived.prototype = prototype;
    repeat(() => check(kindOf(new Derived), "inherited", "a constructor that does not write it"));
    const shadowing = new Derived;
    shadowing.kind = "own";
    repeat(() => check(kindOf(shadowing), "own", "an own property in that very slot that shadows"));
    delete shadowing.kind;
    repeat(() => check(kindOf(shadowing), "inherited", "no longer shadowed"));
}

for (let round = 0; round < 4; round++) {
    const kept = [];
    repeat(i => kept.push(new Node(i, round, 1), new Token(i, round, 1), new Sym(i, "s")));
    fullGC();
    for (let i = 0; i < 50; i++)
        ({ ["unrelated" + round + "_" + i]: i, kind: "wrong" });
    repeat(i => {
        check(kindOf(kept[3 * i]), i, "a node after a collection");
        check(kindOf(kept[3 * i + 1]), i, "a token after a collection");
        check(flagsOf(kept[3 * i + 2]), i, "a symbol after a collection");
    });
}
