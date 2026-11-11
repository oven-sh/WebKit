// Sites that meet more structures than an inline cache lists, where a class of the objects overrides getOwnPropertySlot.
// This checks what such sites read. It passes too when they are folded to a megamorphic case that cannot serve them:
// the accesses are then slower, not different, and a script cannot see which it was.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function make(Base, id) {
    return class extends Base {
        constructor() {
            super();
            this.field = id;
        }
        get getter() { return id + 1000; }
    };
}

// Fresh code each time, so fresh inline caches, and one site for each key: a site that gives up does not take another with it.
function makeSites() {
    const sites = {
        byId: new Function("object", "return object.field;"),
        getter: new Function("object", "return object.getter;"),
        byIdAbsent: new Function("object", "return object.absent;"),
        byVal: new Function("object", "key", "return object[key];"),
        byValAbsent: new Function("object", "key", "return object[key];"),
        inById: new Function("object", "return 'field' in object;"),
        inByIdAbsent: new Function("object", "return 'absent' in object;"),
        inByVal: new Function("object", "key", "return key in object;"),
        inByValAbsent: new Function("object", "key", "return key in object;"),
    };
    for (const name in sites)
        noInline(sites[name]);
    return sites;
}

// Map is here for comparison: its instances are ordinary objects.
for (const Base of [Error, RegExp, Uint8Array, String, Map]) {
    const sites = makeSites();
    const instances = [];
    for (let id = 0; id < 12; ++id) {
        instances.push(new (make(Base, id)));
        for (let i = 0; i < testLoopCount / 100; ++i) {
            for (let j = 0; j <= id; ++j) {
                const object = instances[j];
                shouldBe(sites.byId(object), j);
                shouldBe(sites.getter(object), j + 1000);
                shouldBe(sites.byIdAbsent(object), undefined);
                shouldBe(sites.byVal(object, "field"), j);
                shouldBe(sites.byValAbsent(object, "absent"), undefined);
                shouldBe(sites.inById(object), true);
                shouldBe(sites.inByIdAbsent(object), false);
                shouldBe(sites.inByVal(object, "field"), true);
                shouldBe(sites.inByValAbsent(object, "absent"), false);
            }
        }
    }
}

// A few such values among many ordinary objects.
{
    const sites = makeSites();
    const values = [];
    for (let id = 0; id < 20; ++id) {
        const object = { };
        object["own" + id] = 0;
        object.field = id;
        values.push([object, id]);
    }
    String.prototype.field = "string";
    Symbol.prototype.field = "symbol";
    Array.prototype.field = "array";
    values.splice(2, 0, ["text", "string"], [Symbol("s"), "symbol"], [Array.prototype, "array"], [Object.assign(new Error, { field: "error" }), "error"]);
    for (let i = 0; i < testLoopCount / 10; ++i) {
        for (const [value, expected] of values) {
            shouldBe(sites.byId(value), expected);
            shouldBe(sites.byVal(value, "field"), expected);
        }
    }
}

// What an Error makes on demand is still there at such a site.
{
    const stackOf = new Function("object", "return object.stack;");
    noInline(stackOf);
    for (let id = 0; id < 12; ++id) {
        const C = make(Error, id);
        for (let i = 0; i < testLoopCount / 100; ++i)
            shouldBe(typeof stackOf(new C), "string");
    }
}
