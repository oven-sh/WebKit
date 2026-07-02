function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// An array whose prototype was replaced can reach a toJSON through the chain, and
// SerializeJSONProperty finds it with GetV. The fast stringifier only knows about the
// global Array.prototype, so it has to check the array's own prototype.
{
    let prototype = { __proto__: Array.prototype, toJSON() { return 'replaced'; } };
    let array = [1, 2, 3];
    Object.setPrototypeOf(array, prototype);
    shouldBe(JSON.stringify(array), '"replaced"');
    shouldBe(JSON.stringify({ array }), '{"array":"replaced"}');
    shouldBe(JSON.stringify([array]), '["replaced"]');
    shouldBe(JSON.stringify(array, null, 2), '"replaced"');
}

{
    let prototype = Object.create(Array.prototype);
    Object.defineProperty(prototype, 'toJSON', { value() { return this.length; }, enumerable: false });
    let array = [1, 2];
    Object.setPrototypeOf(array, prototype);
    shouldBe(JSON.stringify(array), '2');
}

// A plain array keeps taking the fast path.
{
    shouldBe(JSON.stringify([1, 2, 3]), '[1,2,3]');
    shouldBe(JSON.stringify(Object.setPrototypeOf([1, 2, 3], Array.prototype)), '[1,2,3]');
    let arrayWithProperty = [1];
    arrayWithProperty.extra = 2;
    shouldBe(JSON.stringify(arrayWithProperty), '[1]');
}

// An array with no prototype has no toJSON to find.
{
    let array = [1, 2];
    Object.setPrototypeOf(array, null);
    shouldBe(JSON.stringify(array), '[1,2]');
}

// An own toJSON still wins over the prototype's.
{
    let prototype = { __proto__: Array.prototype, toJSON() { return 'prototype'; } };
    let array = [1];
    Object.setPrototypeOf(array, prototype);
    Object.defineProperty(array, 'toJSON', { value() { return 'own'; }, enumerable: false });
    shouldBe(JSON.stringify(array), '"own"');
}

// The fast path must agree with the general stringifier, which an identity callable
// replacer forces us onto.
{
    let prototype = { __proto__: Array.prototype, toJSON() { return { length: this.length }; } };
    let values = [
        Object.setPrototypeOf([1, 2, 3], prototype),
        Object.setPrototypeOf(['string'], prototype),
        [Object.setPrototypeOf([1], prototype)],
        { nested: Object.setPrototypeOf([1], prototype) },
        Object.setPrototypeOf([], prototype),
    ];
    for (let value of values) {
        for (let space of [undefined, 2, '\t']) {
            shouldBe(JSON.stringify(value, null, space), JSON.stringify(value, (key, x) => x, space));
        }
    }
}
