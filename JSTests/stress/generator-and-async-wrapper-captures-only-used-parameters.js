// A generator or async function is a wrapper function (takes the parameters) and a body function (runs later). A
// closure that the body creates must keep alive only the parameters the body refers to. Before, the wrapper put
// every parameter in its activation, so such a closure kept every parameter, and a chain of calls
// `current = await refresh(current)` kept every value ever passed.
//
// Each check allocates many payloads and counts the ones that survive a collection. The retention under test keeps
// all of them. A conservative root (a word of a JIT frame, the values an in-flight DFG plan keeps) can keep one or
// two, so a check does not rest on a single WeakRef.

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

const payloadCount = 32;
function survivors(refs) {
    let alive = 0;
    for (const ref of refs) {
        if (ref.deref() !== undefined)
            ++alive;
    }
    return alive;
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
    const { refs, closures } = collect(() => {
        const refs = [];
        const closures = [];
        for (let i = 0; i < payloadCount; ++i) {
            let payload = { name, i };
            refs.push(new WeakRef(payload));
            closures.push(finish(call(payload)));
            payload = null;
        }
        return { refs, closures };
    });
    const alive = survivors(refs);
    assert(alive <= 2, `${name}: the closures kept ${alive} of ${payloadCount} unused parameters alive`);
    for (const closure of closures)
        assert(closure() >= 3, `${name}: the closure still reads its own data`);
}

// The reported shape: each call takes the previous closure and returns a new one.
{
    async function refresh(previous) {
        await 0;
        const data = new Array(1000).fill(1);
        return () => data.length;
    }
    const { refs, current } = collect(() => {
        let current = null;
        const refs = [];
        for (let i = 0; i < payloadCount; ++i) {
            current = finish(refresh(current));
            if (i < payloadCount - 1)
                refs.push(new WeakRef(current));
        }
        return { refs, current };
    });
    const alive = survivors(refs);
    assert(alive <= 2, `a chain of async calls kept ${alive} of ${payloadCount - 1} earlier closures alive`);
    assert(current() === 1000, "the last closure still works");
}

// A parameter the body refers to is in the wrapper's activation, so a closure from the body keeps it: a read, a
// nested function, a `var` of the same name. This is what the engine does today. A read that no closure needs could
// one day stop keeping the parameter, as V8 does.
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
        assert(ref.deref() === closure(), `${name}: the closure keeps the parameter the body refers to`);
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
