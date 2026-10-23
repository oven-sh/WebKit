//@ runDefault("--useConcurrentJIT=0")

// A cacheable dictionary structure belongs to one object. Optimized code can still watch one of its
// properties for replacement (an Equivalence condition). A replace store that any tier caches for that
// property must fire the watch, and a property whose replace was cached before the watch must refuse it.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual + ', expected: ' + expected);
}
noInline(shouldBe);

// More than Structure::s_maxTransitionLength (128) added properties: the object gets a cacheable
// dictionary structure of its own.
function makeProto() {
    let proto = {};
    for (let i = 0; i < 200; ++i)
        proto['p' + i] = i;
    proto.f = function () { return 1; };
    return proto;
}

function store(object, key, value) {
    object[key] = value;
}
noInline(store);

// The watch comes first: call() is optimized while proto.f is the first function.
{
    let proto = makeProto();
    let object = Object.create(proto);
    function call(o) { return o.f(); }
    noInline(call);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(call(object), 1);
    for (let i = 0; i < 200; ++i)
        store(proto, 'p' + i, -i);
    store(proto, 'f', function () { return 2; });
    shouldBe(call(object), 2);
    shouldBe(proto.p199, -199);
}

// The cached store comes first: every property of proto, f included, has a cached replace before
// call() is optimized. The watch on f is refused, so call() loads f each time.
{
    let proto = makeProto();
    let object = Object.create(proto);
    function call(o) { return o.f(); }
    noInline(call);
    for (let i = 0; i < 200; ++i)
        store(proto, 'p' + i, -i);
    store(proto, 'f', function () { return 1; });
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(call(object), 1);
    store(proto, 'f', function () { return 2; });
    shouldBe(call(object), 2);
}

// Monomorphic put_by_id replace on a dictionary object, the Repatch and LLInt paths.
{
    let proto = makeProto();
    let object = Object.create(proto);
    function call(o) { return o.f(); }
    noInline(call);
    function replaceF(o, value) { o.f = value; }
    noInline(replaceF);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(call(object), 1);
    replaceF(proto, function () { return 3; });
    shouldBe(call(object), 3);
    for (let i = 0; i < testLoopCount; ++i)
        replaceF(proto, function () { return 4; });
    shouldBe(call(object), 4);
}
