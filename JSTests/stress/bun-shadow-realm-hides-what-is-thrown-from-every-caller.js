//@ requireOptions("--useShadowRealm=1")
// Bun: no object crosses the border of a ShadowRealm: what a function of the realm throws reaches its caller as a TypeError of the caller's realm. The engine did that where the stack walk for a handler
// passes over the frame of the wrapped function. A native caller (sort, replace, a getter, valueOf, toString, a promise job, a bound function) has popped that frame before any walk begins, and was
// handed the inner realm's own object, and through it the realm's Error, its Function and its global object.
const realm = new ShadowRealm();
const throwers = {
    error: realm.evaluate("() => { throw new RangeError('inner'); }"),
    literal: realm.evaluate("() => { throw { secret: 1 }; }"),
    function: realm.evaluate("() => { throw function leaked() { }; }"),
    number: realm.evaluate("() => { throw 7; }"),
    bound: realm.evaluate("(function () { throw new RangeError('bound'); }).bind(null)"),
    proxy: realm.evaluate("new Proxy(function () { }, { apply() { throw new RangeError('trap'); } })"),
};
const callers = {
    direct: thrower => thrower(),
    call: thrower => thrower.call(undefined),
    apply: thrower => thrower.apply(undefined, []),
    reflectApply: thrower => Reflect.apply(thrower, undefined, []),
    bind: thrower => thrower.bind(undefined)(),
    sort: thrower => [2, 1].sort(thrower),
    replaceByRegExp: thrower => "x".replace(/x/, thrower),
    replaceByString: thrower => "x".replace("x", thrower),
    reviver: thrower => JSON.parse("1", thrower),
    getter: thrower => Object.defineProperty({ }, "x", { get: thrower }).x,
    setter: thrower => { Object.defineProperty({ }, "x", { set: thrower }).x = 1; },
    valueOf: thrower => +{ valueOf: thrower },
    toString: thrower => `${{ toString: thrower }}`,
    toPrimitive: thrower => `${{ [Symbol.toPrimitive]: thrower }}`,
    next: thrower => [...{ [Symbol.iterator]: () => ({ next: thrower }) }],
};

function check(caught, title) {
    if (!(caught instanceof TypeError) || Object.getPrototypeOf(caught) !== TypeError.prototype)
        throw new Error(title + ": expected a TypeError of the outer realm but caught " + (caught instanceof Object || caught === null || typeof caught !== "object" && typeof caught !== "function" ? String(caught) : "an object of the inner realm"));
}

for (let i = 0; i < testLoopCount / 100; ++i) {
    for (const how in callers) {
        for (const what in throwers) {
            let hasThrown = false;
            try {
                callers[how](throwers[what]);
            } catch (error) {
                hasThrown = true;
                check(error, how + " of " + what);
            }
            if (!hasThrown)
                throw new Error(how + " of " + what + ": nothing was thrown");
        }
    }
}

const rejections = [];
for (const what in throwers) {
    Promise.resolve(1).then(throwers[what]).catch(error => rejections.push(["then of " + what, error]));
    new Promise(throwers[what]).catch(error => rejections.push(["an executor that is " + what, error]));
}
drainMicrotasks();
if (rejections.length !== 2 * Object.keys(throwers).length)
    throw new Error("expected every promise to be rejected, got " + rejections.length);
for (const [title, error] of rejections)
    check(error, title);

// The other way: what an outer function throws is a TypeError of the inner realm in there.
const judge = realm.evaluate("f => { try { [2, 1].sort(f); return 'nothing was thrown'; } catch (error) { return String(error instanceof TypeError && Object.getPrototypeOf(error) === TypeError.prototype); } }");
if (judge(() => { throw new RangeError("outer"); }) !== "true")
    throw new Error("an outer error was seen in the inner realm as it is");
