function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

// Instances of these have a class that overrides getOwnPropertySlot. Map is here for comparison: its instances do not.
const bases = [Error, RegExp, Uint8Array, String, Map];

function make(Base, id) {
    return class extends Base {
        constructor() {
            super();
            this.field = id;
        }
        get getter() { return id + 1000; }
    };
}

function byId(object) { return object.field; }
function getter(object) { return object.getter; }
function missing(object) { return object.absent; }
function byVal(object, key) { return object[key]; }
function inById(object) { return "field" in object; }
function inByIdMissing(object) { return "absent" in object; }
function inByVal(object, key) { return key in object; }
for (const f of [byId, getter, missing, byVal, inById, inByIdMissing, inByVal])
    noInline(f);

// Each site meets one more class at a time, in the end more than an inline cache lists.
for (const Base of bases) {
    const instances = [];
    for (let id = 0; id < 12; ++id) {
        instances.push(new (make(Base, id)));
        for (let i = 0; i < testLoopCount / 100; ++i) {
            for (let j = 0; j <= id; ++j) {
                const object = instances[j];
                shouldBe(byId(object), j);
                shouldBe(getter(object), j + 1000);
                shouldBe(missing(object), undefined);
                shouldBe(byVal(object, "field"), j);
                shouldBe(byVal(object, "absent"), undefined);
                shouldBe(inById(object), true);
                shouldBe(inByIdMissing(object), false);
                shouldBe(inByVal(object, "field"), true);
                shouldBe(inByVal(object, "absent"), false);
            }
        }
    }
}

// What an Error makes on demand is still there at such a site.
function stackOf(object) { return object.stack; }
noInline(stackOf);
for (let id = 0; id < 12; ++id) {
    const C = make(Error, id);
    for (let i = 0; i < testLoopCount / 100; ++i)
        shouldBe(typeof stackOf(new C), "string");
}
