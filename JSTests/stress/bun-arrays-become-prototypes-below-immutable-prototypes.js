// Object.prototype and Array.prototype have immutable properties, and a class prototype with an array below it gets a setter under an
// index. Arrays and objects with elements then become prototypes, by every way of making an heir: each keeps the storage it has, and
// is made longer and shorter, read and stored into, alone and through its heir, and at last put below the setter.

function shouldBe(actual, expected, message)
{
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

const kinds = {
    integers: () => [1, 2, 3],
    fractions: () => [1.5, 2.5, 3.5],
    strings: () => ["a", "b", "c"],
    empty: () => [],
    reserved: () => new Array(3),
    holes: () => [1, , 3],
    record: () => ({ 0: "zero", 1: "one", 2: "two" }),
};

const heirs = {
    setPrototypeOf: prototype => Object.setPrototypeOf({}, prototype),
    create: prototype => Object.create(prototype),
    literal: prototype => ({ __proto__: prototype }),
    constructor: prototype => {
        function Made() { }
        Made.prototype = prototype;
        return new Made;
    },
    construct: prototype => {
        function Target() { }
        Target.prototype = prototype;
        return Reflect.construct(Array, [7, 8], Target);
    },
    array: prototype => Object.setPrototypeOf([7, 8], prototype),
};

$vm.makePropertiesImmutable(Object.prototype);
$vm.makePropertiesImmutable(Array.prototype);

const prototypesFromBefore = [];
for (const kind in kinds) {
    const prototype = kinds[kind]();
    prototypesFromBefore.push({ kind, prototype, heir: Object.create(prototype) });
}

const stored = [];
class Above { }
const below = Object.setPrototypeOf([1, 2], Above.prototype);
Object.defineProperty(Above.prototype, 3, { set(value) { stored.push(value); }, configurable: true });
below[3] = "below";
shouldBe(stored.join(), "below", "the array below the setter");

function exercise(label, prototype, heir)
{
    const first = prototype[0];
    if (!Object.hasOwn(heir, 0))
        shouldBe(heir[0], first, label + " read through the heir");
    if (Array.isArray(prototype)) {
        prototype.length = 100000;
        shouldBe(prototype.length, 100000, label + " longer");
        shouldBe(prototype[0], first, label + " longer, first");
        shouldBe(prototype[99999], undefined, label + " longer, last");
    }
    prototype[99998] = "far";
    shouldBe(heir[99998], "far", label + " far");
    if (Array.isArray(prototype)) {
        prototype.length = 2;
        shouldBe(heir[99998], undefined, label + " shorter");
        shouldBe(prototype.push("pushed"), 3, label + " push");
        shouldBe(heir[2], "pushed", label + " pushed");
    }
    heir[5] = "own";
    shouldBe(prototype[5], undefined, label + " stored into the heir");
    shouldBe(Object.hasOwn(heir, 5), true, label + " stored into the heir");

    Object.setPrototypeOf(prototype, Above.prototype);
    stored.length = 0;
    prototype[3] = "prototype";
    heir[3] = "heir";
    shouldBe(stored.join(), "prototype,heir", label + " below the setter");
    shouldBe(Object.hasOwn(prototype, 3) || Object.hasOwn(heir, 3), false, label + " below the setter");
}

for (const { kind, prototype, heir } of prototypesFromBefore)
    exercise(kind + " from before", prototype, heir);

for (let i = 0; i < 20; ++i) {
    for (const kind in kinds) {
        for (const way in heirs) {
            const prototype = kinds[kind]();
            const storage = $vm.indexingMode(prototype);
            const heir = heirs[way](prototype);
            shouldBe($vm.indexingMode(prototype), storage, kind + " " + way + " storage");
            exercise(kind + " " + way, prototype, heir);
        }
    }
}

const ordinary = [1, 2, 3];
ordinary[3] = 4;
shouldBe(ordinary.length, 4, "an ordinary array");
shouldBe(stored.join(), "prototype,heir", "an ordinary array");
