function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

// SerializeJSONProperty looks up toJSON with GetV, so an own toJSON is called
// regardless of its enumerability. The fast stringifier skips DontEnum
// properties, so it has to check for toJSON before doing that.
function withNonEnumerableToJSON(object, toJSON)
{
    Object.defineProperty(object, 'toJSON', { value: toJSON, enumerable: false, writable: false, configurable: false });
    return object;
}

{
    let object = withNonEnumerableToJSON({ uri: 'u', cid: 'c', text: 't' }, function () { return { uri: this.uri, cid: this.cid }; });
    Object.freeze(object);
    shouldBe(JSON.stringify(object), '{"uri":"u","cid":"c"}');
    shouldBe(JSON.stringify({ record: object }), '{"record":{"uri":"u","cid":"c"}}');
    shouldBe(JSON.stringify([object]), '[{"uri":"u","cid":"c"}]');
    shouldBe(JSON.stringify(object, null, 2), '{\n  "uri": "u",\n  "cid": "c"\n}');
}

{
    shouldBe(JSON.stringify(withNonEnumerableToJSON({ a: 1 }, function () { return 'replaced'; })), '"replaced"');
    shouldBe(JSON.stringify(withNonEnumerableToJSON({ a: 1 }, function () { return undefined; })), undefined);
    shouldBe(JSON.stringify(withNonEnumerableToJSON({ a: 1 }, function (key) { return key; })), '""');
    shouldBe(JSON.stringify({ key: withNonEnumerableToJSON({ a: 1 }, function (key) { return key; }) }), '{"key":"key"}');
}

// The fast stringifier bails out on the DynamicBuffer path too.
{
    let array = [];
    for (let i = 0; i < 2000; ++i)
        array.push(withNonEnumerableToJSON({ index: i, name: 'name' + i }, function () { return this.index; }));
    let expected = '[' + array.map((_, i) => i).join(',') + ']';
    shouldBe(JSON.stringify(array), expected);
}

// An own toJSON that is not callable is serialized as a normal property.
{
    shouldBe(JSON.stringify(withNonEnumerableToJSON({ a: 1 }, 42)), '{"a":1}');
    shouldBe(JSON.stringify({ a: 1, toJSON: 42 }), '{"a":1,"toJSON":42}');
}

// An enumerable own toJSON keeps working.
{
    shouldBe(JSON.stringify({ a: 1, toJSON() { return 'enumerable'; } }), '"enumerable"');
    shouldBe(JSON.stringify(Object.freeze({ a: 1, toJSON() { return 'frozen'; } })), '"frozen"');
}

// The fast path must agree with the general stringifier, which an identity
// callable replacer forces us onto.
{
    let values = [
        withNonEnumerableToJSON({ a: 1 }, function () { return { b: 2 }; }),
        withNonEnumerableToJSON({ a: 1 }, function () { return 'string'; }),
        withNonEnumerableToJSON({ a: 1 }, 'not callable'),
        { nested: withNonEnumerableToJSON({ a: 1 }, function () { return [1, 2]; }) },
        [withNonEnumerableToJSON({ a: 1 }, function () { return null; })],
        withNonEnumerableToJSON({ 'キー': '値' }, function () { return this['キー']; }),
    ];
    for (let value of values) {
        for (let space of [undefined, 2, '\t']) {
            shouldBe(JSON.stringify(value, null, space), JSON.stringify(value, (key, x) => x, space));
        }
    }
}
