function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected: ${String(expected)}`);
}

// Nothing captures a parameter, so `arguments` is a DirectArguments object.
function direct(a, b)
{
    return arguments;
}

// A closure captures the parameters, so `arguments` is a ScopedArguments object.
function scoped(a, b)
{
    (function () { return a + b; })();
    return arguments;
}

// A direct eval can capture the parameters, so `arguments` is a ScopedArguments object here too.
function scopedByEval(a, b)
{
    eval("");
    return arguments;
}

function count()
{
    return arguments.length;
}

function lengthOf(args)
{
    return args.length;
}
noInline(lengthOf);

function apply(args)
{
    return count.apply(null, args);
}
noInline(apply);

function spread(args)
{
    return count(...args);
}
noInline(spread);

// Each of these makes `length`, `callee` and `Symbol.iterator` own properties of the object. None of them stores a length.
let overrides = [
    function deleteCallee(args) { delete args.callee; },
    function deleteIterator(args) { delete args[Symbol.iterator]; },
    function putCallee(args) { args.callee = 1; },
    function putIterator(args) { args[Symbol.iterator] = Array.prototype.values; },
    function defineCallee(args) { Object.defineProperty(args, "callee", { value: 1 }); },
    function defineIterator(args) { Object.defineProperty(args, Symbol.iterator, { enumerable: false }); },
    function defineLengthWithoutValue(args) { Object.defineProperty(args, "length", { enumerable: false }); },
    function reflectSetWithOtherReceiver(args) { Reflect.set(args, "callee", 1, { }); },
    function putOnHeir(args) { Object.create(args).callee = 1; },
    function freeze(args) { Object.freeze(args); },
    function seal(args) { Object.seal(args); },
];

// Fewer arguments than the two parameters, as many, and more.
let argumentLists = [[], [1], [1, 2], [1, 2, 3], [1, 2, 3, 4, 5]];

function testOverride(create, values, override)
{
    let args = create(...values);
    shouldBe(lengthOf(args), values.length);

    override(args);

    shouldBe(args.length, values.length);
    shouldBe(lengthOf(args), values.length);
    shouldBe(Object.getOwnPropertyDescriptor(args, "length").value, values.length);
    shouldBe(apply(args), values.length);
    shouldBe(Reflect.apply(count, null, args), values.length);
    shouldBe(Array.prototype.slice.call(args).join(), values.join());
    shouldBe(Array.prototype.lastIndexOf.call(args, values.length), values.length - 1);
    if (Symbol.iterator in args) {
        shouldBe(spread(args), values.length);
        shouldBe(Array.from(args).join(), values.join());
    }
}

// The program decides the length before the override, or overrides twice.
function testOrder(create, values)
{
    let stored = create(...values);
    stored.length = 7;
    delete stored.callee;
    shouldBe(lengthOf(stored), 7);
    shouldBe(apply(stored), 7);

    let deleted = create(...values);
    shouldBe(delete deleted.length, true);
    delete deleted.callee;
    shouldBe("length" in deleted, false);
    shouldBe(lengthOf(deleted), undefined);
    shouldBe(apply(deleted), 0);

    let closed = create(...values);
    Object.preventExtensions(closed);
    shouldBe(lengthOf(closed), values.length);
    delete closed.callee;
    shouldBe(lengthOf(closed), values.length);
    shouldBe(apply(closed), values.length);

    let twice = create(...values);
    delete twice.callee;
    delete twice[Symbol.iterator];
    shouldBe(lengthOf(twice), values.length);
    twice.length = 1;
    shouldBe(lengthOf(twice), 1);
    shouldBe(apply(twice), 1);
}

for (let create of [direct, scoped, scopedByEval]) {
    for (let values of argumentLists) {
        for (let override of overrides)
            testOverride(create, values, override);
        testOrder(create, values);
    }
}

// The fast paths of `length` read the argument count of an object that is not overridden. An overridden object takes the slow path.
let pristine = scoped(1, 2, 3);
let more = scoped(1, 2, 3);
delete more.callee;
let fewer = scoped(1);
delete fewer.callee;
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(lengthOf(pristine), 3);
    shouldBe(lengthOf(more), 3);
    shouldBe(lengthOf(fewer), 1);
    shouldBe(apply(more), 3);
}

// The function overrides its own arguments object and reads the length.
function lengthAfterOverride(a, b)
{
    (function () { return a + b; })();
    delete arguments.callee;
    return arguments.length;
}
noInline(lengthAfterOverride);

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(lengthAfterOverride(1), 1);
    shouldBe(lengthAfterOverride(1, 2, 3), 3);
}
