// A generator or async function is a wrapper function (takes the parameters) and a body function (runs later). A
// closure that the body creates must keep alive only the parameters the body refers to. Before, the wrapper put
// every parameter in its activation, so such a closure kept every parameter, and a chain of calls
// `current = await refresh(current)` kept every value ever passed.

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

function churn(depth) {
    if (depth <= 0)
        return 0;
    let sum = 0;
    const objects = [];
    for (let i = 0; i < 200; ++i)
        objects.push({ a: i, b: [i] });
    for (let i = 0; i < objects.length; ++i)
        sum += objects[i].a;
    return sum + churn(depth - 1);
}

// Runs allocate() in its own frame, then collects with that frame gone.
function collect(allocate) {
    let allocated = allocate();
    for (let round = 0; round < 3; ++round) {
        churn(3);
        $.clearKeptObjects();
        $.gc();
    }
    return allocated;
}

// Runs a generator to its return value, or settles a promise. Returns the closure the body returned.
function finish(result) {
    let closure;
    if (typeof result.next === "function") {
        result.next();
        const step = result.next();
        if (typeof step.then === "function") {
            step.then(s => { closure = s.value; });
            drainMicrotasks();
        } else
            closure = step.value;
    } else {
        result.then(value => { closure = value; });
        drainMicrotasks();
    }
    return closure;
}

// Each case calls a function whose body never reads `previous` and returns a closure over the body's own data.
const unused = {
    "generator": payload => (function* (previous) {
        yield 0;
        const data = [1, 2, 3];
        return () => data.length;
    })(payload),
    "generator method": payload => ({ *method(previous) { yield 0; const data = [1, 2, 3]; return () => data.length; } }).method(payload),
    "async function": payload => (async function (previous) {
        await 0;
        const data = [1, 2, 3];
        return () => data.length;
    })(payload),
    "async arrow": payload => (async (previous) => {
        await 0;
        const data = [1, 2, 3];
        return () => data.length;
    })(payload),
    "async method": payload => ({ async method(previous) { await 0; const data = [1, 2, 3]; return () => data.length; } }).method(payload),
    "async generator": payload => (async function* (previous) {
        yield 0;
        const data = [1, 2, 3];
        return () => data.length;
    })(payload),
    "generator, only the first parameter is read": payload => (function* (first, previous) {
        yield first;
        const data = [1, 2, 3];
        return () => data.length + first;
    })(1, payload),
    "async function, a destructured parameter": payload => (async function ({ previous }) {
        await 0;
        const data = [1, 2, 3];
        return () => data.length;
    })({ previous: payload }),
    "async function, a parameter with a default": payload => (async function (previous = null) {
        await 0;
        const data = [1, 2, 3];
        return () => data.length;
    })(payload),
    "generator, a rest parameter": payload => (function* (...previous) {
        yield 0;
        const data = [1, 2, 3];
        return () => data.length;
    })(payload),
};

for (const [name, call] of Object.entries(unused)) {
    const { ref, closure } = collect(() => {
        let payload = { name };
        const ref = new WeakRef(payload);
        const closure = finish(call(payload));
        payload = null;
        return { ref, closure };
    });
    assert(ref.deref() === undefined, `${name}: the closure kept an unused parameter alive`);
    assert(closure() >= 3, `${name}: the closure still reads its own data`);
}

// The reported shape: each call takes the previous closure and returns a new one.
{
    async function refresh(previous) {
        await 0;
        const data = new Array(1000).fill(1);
        return () => data.length;
    }
    const { firstRef, current } = collect(() => {
        let current = null;
        let firstRef;
        for (let i = 0; i < 50; ++i) {
            current = finish(refresh(current));
            if (i === 0)
                firstRef = new WeakRef(current);
        }
        return { firstRef, current };
    });
    assert(firstRef.deref() === undefined, "a chain of async calls kept every earlier closure alive");
    assert(current() === 1000, "the last closure still works");
}

// A parameter the body refers to stays alive: a read, a write, a nested function, a `var` of the same name.
{
    const used = {
        "read after await": async function (previous) { await 0; return () => previous; },
        "read before await": async function (previous) { const p = previous; await 0; return () => p; },
        "nested function": function* (previous) { yield 0; return function () { return previous; }; },
        "var of the same name": function* (previous) { yield 0; var previous; return () => previous; },
        "var of the same name, assigned": async function (previous) { await 0; var previous = previous; return () => previous; },
    };
    for (const [name, make] of Object.entries(used)) {
        const { ref, closure } = collect(() => {
            let payload = { name };
            const ref = new WeakRef(payload);
            const closure = finish(make(payload));
            payload = null;
            return { ref, closure };
        });
        assert(ref.deref() === closure(), `${name}: the closure must keep the parameter it reads`);
    }
}

// A `var` that has the name of a parameter is that parameter (regress-223533).
{
    function* g1(foo) {
        var foo;
        yield foo;
        foo = 2;
        var foo;
        yield foo;
    }
    const it = g1(1);
    assert(it.next().value === 1, "var foo keeps the argument");
    assert(it.next().value === 2, "var foo and the parameter are one binding");

    async function a1(foo, bar) {
        await 0;
        var bar;
        return [foo, bar];
    }
    let result;
    a1(1, 2).then(v => { result = v; });
    drainMicrotasks();
    assert(result[0] === 1 && result[1] === 2, "var bar keeps the argument in an async function");

    function* g2(...rest) {
        yield 0;
        var rest;
        return rest.length;
    }
    const it2 = g2(1, 2, 3);
    it2.next();
    assert(it2.next().value === 3, "var rest keeps a rest parameter");

    async function* ag([a], { b } = { b: 5 }) {
        yield 0;
        var a, b;
        yield a + b;
    }
    let sum;
    const agIt = ag([4]);
    agIt.next().then(() => agIt.next()).then(s => { sum = s.value; });
    drainMicrotasks();
    assert(sum === 9, "var of destructured parameters keeps their values");

    function* withArguments(a) {
        var a;
        yield a;
        a = 3;
        yield arguments[0];
    }
    const it3 = withArguments(1);
    assert(it3.next().value === 1, "var a keeps the argument with a sloppy arguments object");
    assert(it3.next().value === 3, "the sloppy arguments object is mapped to the parameter");
}

// `arguments` in a sloppy body reads the parameters through the wrapper.
{
    function* g(a, b) {
        yield arguments.length;
        yield arguments[1];
        b = 7;
        yield arguments[1];
    }
    const it = g(1, 2);
    assert(it.next().value === 2, "arguments.length");
    assert(it.next().value === 2, "arguments[1]");
    assert(it.next().value === 7, "a sloppy arguments object is mapped to the parameter");

    async function strictArgs(a, b) {
        "use strict";
        await 0;
        b = 7;
        return arguments[1];
    }
    let v;
    strictArgs(1, 2).then(x => { v = x; });
    drainMicrotasks();
    assert(v === 2, "a strict arguments object is not mapped");
}

// A function declaration that has the name of a parameter is a binding of the body.
{
    function* g(a) {
        yield typeof a;
        function a() { }
    }
    assert(g(1).next().value === "function", "the function declaration wins in the body");

    function* h(a) {
        { function a() { } }
        yield a;
    }
    assert(h(1).next().value === 1, "a block function of a parameter's name is not hoisted over the parameter");
}

// A default parameter expression can refer to another parameter through a closure.
{
    async function d(a, f = () => a) {
        await 0;
        a = 2;
        return f();
    }
    let v;
    d(1).then(x => { v = x; });
    drainMicrotasks();
    assert(v === 2, "a closure in a default expression sees the parameter");
}
