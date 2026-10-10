//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function Grows(flags) { this.flags = flags; }
function extend(o, i) {
    o.id = i;
    o.symbol = "s";
    o.objectFlags = 2;
    o.members = undefined;
    o.properties = undefined;
    o.callSignatures = undefined;
    o.constructSignatures = undefined;
    o.indexInfos = undefined;
    return o;
}
function describe(o) { return Object.keys(o).join() + ":" + o.flags + "," + o.id + "," + o.symbol + "," + o.objectFlags + "," + o.indexInfos; }
function StaysSmall(a) { this.a = a; }
function GrowsALot(a) { this.a = a; }
noInline(extend);

if (isAOTCompiled(Grows)) {
    const first = new Grows(0);
    const capacityAtFirst = $vm.inlineCapacity(first);
    check(capacityAtFirst < 9, true, "the first object has room for little");
    const all = [];
    for (let i = 0; i < 400; i++)
        all.push(extend(new Grows(i), i));
    const capacityLater = $vm.inlineCapacity(new Grows(0));
    check(capacityLater >= 9, true, "later objects have room for what they are given: " + capacityLater);
    check(capacityLater <= 18, true, "but not for much more than is looked at: " + capacityLater);
    for (let i = 0; i < 400; i++)
        check(describe(all[i]), "flags,id,symbol,objectFlags,members,properties,callSignatures,constructSignatures,indexInfos:" + i + "," + i + ",s,2,undefined", "object " + i);
    check($vm.inlineCapacity(first), capacityAtFirst, "an object that exists keeps its size");

    const small = $vm.inlineCapacity(new StaysSmall(1));
    for (let i = 0; i < 400; i++)
        check(new StaysSmall(i).a, i, "an object that gets nothing more");
    check($vm.inlineCapacity(new StaysSmall(1)), small, "a constructor whose objects get nothing more makes them as before");

    for (let i = 0; i < 100; i++) {
        const o = new GrowsALot(i);
        for (let j = 0; j < 40; j++)
            o["p" + j] = j;
        o.q0 = 0; o.q1 = 1; o.q2 = 2; o.q3 = 3; o.q4 = 4; o.q5 = 5; o.q6 = 6; o.q7 = 7; o.q8 = 8; o.q9 = 9;
        o.r0 = 0; o.r1 = 1; o.r2 = 2; o.r3 = 3; o.r4 = 4; o.r5 = 5; o.r6 = 6; o.r7 = 7; o.r8 = 8; o.r9 = 9;
        check(o.a + o.p39 + o.q9 + o.r9, i + 39 + 18, "an object that gets a lot");
    }
    check($vm.inlineCapacity(new GrowsALot(1)) <= 18, true, "there is a limit, rounded up to a size that objects come in");

    fullGC();
    for (let i = 0; i < 100; i++)
        check(describe(extend(new Grows(i), i)), "flags,id,symbol,objectFlags,members,properties,callSignatures,constructSignatures,indexInfos:" + i + "," + i + ",s,2,undefined", "after a collection");

    Grows.prototype = { inherited: 1 };
    for (let i = 0; i < 100; i++) {
        const o = extend(new Grows(i), i);
        check(o.inherited + o.id, 1 + i, "after the prototype is replaced");
    }

    class Base { constructor(x) { this.x = x; } }
    class Derived extends Base { constructor(x) { super(x); this.y = x + 1; } }
    for (let i = 0; i < 200; i++) {
        const o = extend(new Derived(i), i);
        check(o.x + o.y + o.id, 3 * i + 1, "a derived class");
        check(o instanceof Derived, true, "an instance of the derived class");
    }
}
