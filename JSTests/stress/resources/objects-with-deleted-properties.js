const plainObjectCreators = {
    firstPropertyDeleted() {
        const object = { a: 1, b: 2 };
        delete object.a;
        return object;
    },
    lastPropertyDeleted() {
        const object = { a: 1, b: 2 };
        delete object.b;
        return object;
    },
    middlePropertyDeleted() {
        const object = { a: 1, b: 2, c: 3 };
        delete object.b;
        return object;
    },
    allPropertiesDeleted() {
        const object = { a: 1, b: 2 };
        delete object.a;
        delete object.b;
        return object;
    },
    twoPropertiesDeleted() {
        const object = { a: 1, b: 2, c: 3, d: 4 };
        delete object.a;
        delete object.c;
        return object;
    },
    outOfLineStorage() {
        const object = { };
        for (let i = 0; i < 20; ++i)
            object["p" + i] = i;
        delete object.p3;
        delete object.p17;
        return object;
    },
    dictionary() {
        const object = { x: 0 };
        for (let i = 0; i < 300; ++i)
            object["k" + i] = i;
        for (let i = 0; i < 150; ++i)
            delete object["k" + i];
        return object;
    },
    accessor() {
        const object = { a: 1, get g() { return 1; }, b: 2 };
        delete object.a;
        return object;
    },
    prototypeInUse() {
        const prototype = { a: 1, m() { } };
        Object.create(prototype).w = 1;
        delete prototype.a;
        return prototype;
    },
    symbolDeleted() {
        const symbol = Symbol();
        const object = { [symbol]: 1, b: 2 };
        delete object[symbol];
        return object;
    },
};

const functionAndArrayCreators = {
    function() {
        const object = Object.assign(function () { }, { a: 1, b: 2 });
        delete object.a;
        return object;
    },
    functionNameDeleted() {
        const object = function named() { };
        delete object.name;
        return object;
    },
    array() {
        const object = Object.assign([1, 2, 3], { a: 1, b: 2 });
        delete object.a;
        return object;
    },
};

const possiblyAbsentKeys = ["a", "b", "c", "p3", "k7", "name", "fresh"];
