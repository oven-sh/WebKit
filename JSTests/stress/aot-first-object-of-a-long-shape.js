//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useMiniVMModeWithoutJIT=0", "--verifyGC=1")

(function () {
    function shouldBe(actual, expected, what) {
        if (actual !== expected)
            throw new Error(what + ": " + actual + " instead of " + expected);
    }
    function probe() { return 1; }
    const compiled = typeof isAOTCompiled === "function" && isAOTCompiled(probe);
    const isDictionary = object => describe(object).includes(", Dictionary");
    const structureOf = object => /StructureID: (\d+)/.exec(describe(object))[1];

    function fill(object, prefix, count) {
        for (let i = 0; i < count; ++i)
            object[prefix + i] = i;
        return object;
    }
    function namesOf(prefix, count) {
        const names = [];
        for (let i = 0; i < count; ++i)
            names.push(prefix + i);
        return names.join();
    }

    const first = fill({}, "a", 30);
    const second = fill({}, "a", 30);
    const third = fill({}, "a", 30);
    shouldBe(isDictionary(first), compiled, "the first object of a long shape leaves no transitions");
    shouldBe(isDictionary(second), false, "the second object of a long shape");
    shouldBe(isDictionary(third), false, "the third object of a long shape");
    shouldBe(structureOf(second), structureOf(third), "later objects of a long shape share their structure");
    for (const object of [first, second, third]) {
        shouldBe(Object.keys(object).join(), namesOf("a", 30), "order of keys");
        shouldBe(object.a0 + object.a8 + object.a29, 37, "values");
        shouldBe(JSON.stringify(Object.getOwnPropertyDescriptor(object, "a9")), '{"value":9,"writable":true,"enumerable":true,"configurable":true}', "attributes");
        let visited = "";
        for (const name in object)
            visited += name;
        shouldBe(visited, namesOf("a", 30).replaceAll(",", ""), "enumeration");
        shouldBe(Object.keys({ ...object }).join(), namesOf("a", 30), "spread");
    }

    shouldBe(isDictionary(fill({}, "b", 8)), false, "a short shape");
    shouldBe(isDictionary(fill({}, "b", 8)), false, "a short shape again");

    function read(object) { return object.a3 + object.a29; }
    for (let i = 0; i < 100; ++i)
        shouldBe(read(first), 32, "reads of the first object");
    shouldBe(isDictionary(first), false, "an object that is read does not stay a dictionary");
    first.later = 1;
    first.evenLater = 2;
    shouldBe(isDictionary(first), false, "and does not become one again");
    for (let i = 0; i < 100; ++i)
        shouldBe(read(first), 32, "reads after more properties");
    shouldBe(Object.keys(first).join(), namesOf("a", 30) + ",later,evenLater", "order of keys after more properties");
    delete first.a4;
    shouldBe("a4" in first, false, "a deleted property");
    shouldBe(first.a5, 5, "the property after a deleted one");

    function exportAll(target, all) {
        for (const name in all)
            Object.defineProperty(target, name, { get: all[name], enumerable: true, configurable: true });
        return target;
    }
    function getters(prefix, count) {
        const all = {};
        for (let i = 0; i < count; ++i)
            all[prefix + i] = () => i;
        return all;
    }
    const exported = exportAll({}, getters("e", 20));
    shouldBe(isDictionary(exported), compiled, "an object that gets many accessors");
    shouldBe(Object.keys(exported).join(), namesOf("e", 20), "order of accessors");
    function readExported(object) { return object.e1 + object.e19; }
    for (let i = 0; i < 100; ++i)
        shouldBe(readExported(exported), 20, "accessors");
    shouldBe(isDictionary(exported), false, "an object with accessors that is read does not stay a dictionary");

    class Wide {
        constructor(x) {
            this.f0 = x; this.f1 = x; this.f2 = x; this.f3 = x; this.f4 = x; this.f5 = x;
            this.f6 = x; this.f7 = x; this.f8 = x; this.f9 = x; this.f10 = x; this.f11 = x;
        }
        sum() { return this.f0 + this.f11; }
    }
    const instances = [];
    for (let i = 0; i < 50; ++i)
        instances.push(new Wide(i));
    for (let i = 0; i < 50; ++i) {
        shouldBe(instances[i].sum(), 2 * i, "fields of an instance");
        shouldBe(Object.keys(instances[i]).join(), namesOf("f", 12), "order of fields");
    }
    shouldBe(isDictionary(instances[2]), false, "the third instance of a class with many fields");
    shouldBe(structureOf(instances[2]), structureOf(instances[49]), "instances share their structure");

    let made = 0;
    function count() { ++made; }
    class ManyMethods {
        constructor(x) {
            count();
            this.first = x;
            this.second = x;
            this.third = x;
            this.fourth = x;
        }
        m0() { return 0; } m1() { return 1; } m2() { return 2; } m3() { return 3; } m4() { return 4; } m5() { return 5; }
        m6() { return 6; } m7() { return 7; } m8() { return 8; } m9() { return 9; } m10() { return 10; } m11() { return this.fourth; }
    }
    const withMethods = [];
    for (let i = 0; i < 50; ++i)
        withMethods.push(new ManyMethods(i));
    shouldBe(made, 50, "constructor calls");
    shouldBe(isDictionary(ManyMethods.prototype), false, "a prototype whose instances are constructed does not stay a dictionary");
    for (let i = 0; i < 50; ++i) {
        shouldBe(withMethods[i].m11() + withMethods[i].m10(), i + 10, "methods of a class with many methods");
        shouldBe(Object.keys(withMethods[i]).join(), "first,second,third,fourth", "fields of a class with many methods");
    }
    shouldBe(structureOf(withMethods[1]), structureOf(withMethods[49]), "instances of a class with many methods share their structure");
    shouldBe(Object.getOwnPropertyNames(ManyMethods.prototype).join(), "constructor,m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11", "order of methods");

    const frozen = Object.freeze(fill({}, "z", 20));
    shouldBe(Object.isFrozen(frozen), true, "a frozen long object");
    shouldBe((() => { "use strict"; try { frozen.z1 = 5; } catch (error) { return error instanceof TypeError; } return false; })(), true, "a store to a frozen long object");

    const prototype = fill({}, "m", 40);
    const inheritor = Object.create(prototype);
    function readInherited(object) { return object.m39; }
    for (let i = 0; i < 100; ++i)
        shouldBe(readInherited(inheritor), 39, "an inherited property");
    prototype.m39 = 40;
    shouldBe(readInherited(inheritor), 40, "an inherited property that changed");
    prototype.added = 1;
    shouldBe(inheritor.added, 1, "a property added to the prototype");
})();
