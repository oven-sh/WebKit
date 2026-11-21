var createBuiltin = $vm.createBuiltin;

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ': expected ' + String(expected) + ' but got ' + String(actual));
}

// globalThis is a JSGlobalProxy. It answers an own property lookup with the slot of its target, the global object,
// so the offset in that slot is not an offset of the proxy's structure.

// With a count of 2 and the default thresholds, the function that reads is still in LLInt at the end.
function testGlobalThis(count) {
    var builtin = createBuiltin(`(function (obj) {
        return @getByIdDirect(obj, "hello");
    })`);
    noInline(builtin);

    function read(expected, what) {
        for (var i = 0; i < count; ++i)
            shouldBe(builtin(globalThis), expected, what);
    }

    globalThis.hello = 42;
    read(42, 'data property');

    globalThis.hello = 43;
    read(43, 'stored again');

    delete globalThis.hello;
    read(undefined, 'deleted');

    // The prototype chain of the global object is not read.
    Object.prototype.hello = 'inherited';
    shouldBe(globalThis.hello, 'inherited', 'ordinary read');
    read(undefined, 'inherited');
    delete Object.prototype.hello;

    globalThis.hello = 44;
    read(44, 'added again');

    var receiver;
    Object.defineProperty(globalThis, 'hello', { get() { receiver = this; return 45; }, configurable: true });
    for (var i = 0; i < count; ++i) {
        receiver = undefined;
        shouldBe(builtin(globalThis), 45, 'getter');
        shouldBe(receiver, globalThis, 'receiver of the getter');
    }

    Object.defineProperty(globalThis, 'hello', { value: 46, writable: true, configurable: true });
    read(46, 'data property after the getter');

    delete globalThis.hello;
    read(undefined, 'deleted at the end');
}
testGlobalThis(2);
testGlobalThis(testLoopCount);

// One site reads an object and the proxy in turn.
(function () {
    var builtin = createBuiltin(`(function (obj) {
        return @getByIdDirect(obj, "world");
    })`);
    noInline(builtin);

    var object = { world: 1 };
    globalThis.world = 2;
    for (var i = 0; i < testLoopCount; ++i) {
        shouldBe(builtin(object), 1, 'object');
        shouldBe(builtin(globalThis), 2, 'proxy');
    }
    delete globalThis.world;
    shouldBe(builtin(globalThis), undefined, 'proxy after the delete');
    shouldBe(builtin(object), 1, 'object after the delete');
}());

// A global object of another realm, read through a proxy and as it is. The global object holds its own properties.
(function () {
    var builtin = createBuiltin(`(function (obj) {
        return @getByIdDirect(obj, "hello");
    })`);
    noInline(builtin);

    var otherGlobalObject = $vm.createGlobalObject();
    var otherProxy = $vm.createGlobalProxy(otherGlobalObject);
    otherGlobalObject.hello = 47;
    shouldBe(builtin(otherProxy), 47, 'proxy of another realm, first read');
    shouldBe(builtin(otherProxy), 47, 'proxy of another realm, second read');
    for (var i = 0; i < testLoopCount; ++i) {
        shouldBe(builtin(otherProxy), 47, 'proxy of another realm');
        shouldBe(builtin(otherGlobalObject), 47, 'global object of another realm');
    }
    otherGlobalObject.hello = 48;
    shouldBe(builtin(otherProxy), 48, 'proxy of another realm after a store');
    shouldBe(builtin(otherGlobalObject), 48, 'global object of another realm after a store');
    shouldBe(builtin(globalThis), undefined, 'this realm has no such property');
}());

// A proxy with a property of its own: a private field is defined on the object that the base constructor returns.
(function () {
    var builtin = createBuiltin(`(function (obj) {
        return @getByIdDirect(obj, "stamped");
    })`);
    noInline(builtin);

    class Stamp extends (function (object) { return object; }) {
        #field = 49;
        static field(object) { return object.#field; }
    }
    var proxy = $vm.createGlobalProxy($vm.createGlobalObject());
    new Stamp(proxy);
    shouldBe(Stamp.field(proxy), 49, 'private field of the proxy');

    proxy.stamped = 50;
    for (var i = 0; i < testLoopCount; ++i)
        shouldBe(builtin(proxy), 50, 'proxy with a private field');
    shouldBe(Stamp.field(proxy), 49, 'private field of the proxy at the end');
}());

// A property under a private name that a builtin defines through the proxy: read, define, read, read.
(function () {
    var get = createBuiltin(`(function (obj) {
        return @getByIdDirectPrivate(obj, "next");
    })`);
    var put = createBuiltin(`(function (obj, value) {
        @putByIdDirectPrivate(obj, "next", value);
    })`);
    noInline(get);
    noInline(put);

    var proxy = $vm.createGlobalProxy($vm.createGlobalObject());
    shouldBe(get(proxy), undefined, 'private name before the definition');
    put(proxy, 51);
    shouldBe(get(proxy), 51, 'private name, first read');
    shouldBe(get(proxy), 51, 'private name, second read');
    for (var i = 0; i < testLoopCount; ++i)
        shouldBe(get(proxy), 51, 'private name');
    put(proxy, 52);
    shouldBe(get(proxy), 52, 'private name defined again');
}());

// A global variable is an own property of the global object that has no offset in its structure.
var declared = 53;
(function () {
    var builtin = createBuiltin(`(function (obj) {
        return @getByIdDirect(obj, "declared");
    })`);
    noInline(builtin);

    for (var i = 0; i < testLoopCount; ++i)
        shouldBe(builtin(globalThis), 53, 'global variable');
    declared = 54;
    shouldBe(builtin(globalThis), 54, 'global variable stored again');
}());
