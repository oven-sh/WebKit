// Bun keeps a cycle guard on Array#join, Array#toString and Array#toLocaleString: an array that is
// already being joined further up the stack converts to the empty string, the same as V8 and
// SpiderMonkey. Error#toString and RegExp#toString have no guard, so a cycle through them recurses
// until the stack is exhausted and throws a RangeError. A conversion that reuses a receiver that is
// already on the stack without being cyclic must produce its normal result.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrowRangeError(func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error("didn't throw");
    if (!(error instanceof RangeError))
        throw new Error(`expected RangeError but got ${error}`);
}

// A direct self reference renders as the empty string.
{
    let array = [];
    array[0] = array;
    shouldBe(array.join(), "");
    shouldBe(array.toString(), "");
    shouldBe(`${array}`, "");
    shouldBe(array.toLocaleString(), "");
    shouldBe(String(array), "");
    shouldBe(0 ^ array, 0);
    shouldBe(array == "", true);
}

{
    let array = [1];
    array.push(array, 2);
    shouldBe(array.join(), "1,,2");
    shouldBe(array.join("-"), "1--2");
    shouldBe(array.toString(), "1,,2");
    shouldBe(array.toLocaleString(), "1,,2");
}

// Indirect cycles render as the empty string at the point where the cycle closes.
{
    let array = [1, "webkit"];
    array[2] = [3, 4, [5, 6, [array]]];
    shouldBe(array.toString(), "1,webkit,3,4,5,6,");
}

{
    let a = [0];
    let b = [a];
    a.push(b);
    shouldBe(`${a}`, "0,");
    shouldBe(`${b}`, "0,");
}

{
    let array = ["a"];
    array.push({ toString() { return array.join("~"); } });
    shouldBe(array.join("-"), "a-");
}

// A cycle whose fan-out is larger than one must still terminate.
{
    let array = [];
    array[0] = array;
    array[1] = array;
    shouldBe(array.toString(), ",");
}

// The guard applies to array-like receivers of the generic join too.
{
    let object = { length: 2, 0: "x", toString() { return Array.prototype.join.call(object, "-"); } };
    object[1] = object;
    shouldBe(Array.prototype.join.call(object, "-"), "x-");
    shouldBe(Array.prototype.toLocaleString.call(object), "x,");
}

// Error.prototype.toString has no cycle detection.
shouldThrowRangeError(() => {
    let error = new Error;
    error.name = error;
    error.message = error;
    return `${error}`;
});

shouldThrowRangeError(() => {
    let error = new Error;
    error.message = { toString() { return Error.prototype.toString.call(error); } };
    return `${error}`;
});

// Nor does RegExp.prototype.toString.
shouldThrowRangeError(() => {
    let regExp = /a/;
    Object.defineProperty(regExp, "source", { get() { return regExp; } });
    return `${regExp}`;
});

shouldThrowRangeError(() => {
    let regExp = /a/;
    Object.defineProperty(regExp, "flags", { get() { return RegExp.prototype.toString.call(regExp); } });
    return `${regExp}`;
});

// These conversions terminate on their own. Sharing a receiver with a conversion further up the
// stack is not a cycle, so each must return its ordinary result.
{
    let array = [];
    array[0] = { toString() { return Error.prototype.toString.call(array); } };
    shouldBe(array.join(), "Error");
}

{
    let array = [];
    array[0] = { toString() { return RegExp.prototype.toString.call(array); } };
    shouldBe(array.join(), "/undefined/undefined");
}

{
    let object = { length: 2, 0: "x", 1: "y" };
    object.name = { toString() { return Array.prototype.join.call(object, "-"); } };
    shouldBe(Error.prototype.toString.call(object), "x-y");
}

{
    let array = ["a"];
    array[1] = { toString() { return Array.prototype.toLocaleString.call({ length: 1, 0: "b" }); } };
    shouldBe(array.join("-"), "a-b");
}

{
    let error = new Error;
    error.name = "E";
    error.message = { toString() { return Error.prototype.toString.call({ name: "inner", message: "m" }); } };
    shouldBe(`${error}`, "E: inner: m");
}

// A deep but acyclic nesting still converts, and a shared subtree is visited every time it occurs.
// The guard only covers arrays that are still being joined, not arrays that were joined before.
{
    let shared = [1, 2];
    shouldBe([shared, shared].toString(), "1,2,1,2");
    shouldBe([shared, shared].toLocaleString(), "1,2,1,2");
    shouldBe([shared, shared].join("|"), "1,2|1,2");
}

// A guarded conversion leaves no state behind that suppresses later conversions.
{
    let array = [];
    array[0] = array;
    for (let i = 0; i < 2; ++i)
        shouldBe(array.toString(), "");
    shouldBe([1, 2].toString(), "1,2");
    shouldBe(array.join("-"), "");
    shouldBe([1, 2].join("-"), "1-2");
}

// An exception thrown inside a guarded join must unwind the guard too.
{
    let array = [1];
    array.push({ toString() { throw new Error("boom"); } });
    for (let i = 0; i < 2; ++i) {
        let threw = false;
        try {
            array.join();
        } catch (e) {
            threw = e.message === "boom";
        }
        shouldBe(threw, true);
    }
    array.pop();
    shouldBe(array.join(), "1");
}

// Recovering after a stack overflow must leave no state behind either.
{
    let error = new Error;
    error.name = error;
    shouldThrowRangeError(() => `${error}`);
    shouldBe(`${new Error("m")}`, "Error: m");
    shouldBe([1, 2].toString(), "1,2");
}

// The optimizing tiers reach the array conversions through their own paths, so warm them up on an
// acyclic array and then check that a cyclic one renders as the empty string there too.
{
    const convert = a => `${a}`;
    const join = a => a.join("-");
    const joinDefault = a => a.join();
    const toLocale = a => a.toLocaleString();
    noInline(convert);
    noInline(join);
    noInline(joinDefault);
    noInline(toLocale);

    let acyclic = [1, 2, 3];
    let cyclic = [];
    cyclic[0] = cyclic;
    let nested = [1];
    nested.push(nested, 2);

    for (let i = 0; i < 20000; ++i) {
        shouldBe(convert(acyclic), "1,2,3");
        shouldBe(join(acyclic), "1-2-3");
        shouldBe(joinDefault(acyclic), "1,2,3");
        shouldBe(toLocale(acyclic), "1,2,3");
    }

    for (let i = 0; i < 2; ++i) {
        shouldBe(convert(cyclic), "");
        shouldBe(join(cyclic), "");
        shouldBe(joinDefault(cyclic), "");
        shouldBe(toLocale(cyclic), "");
        shouldBe(convert(nested), "1,,2");
        shouldBe(join(nested), "1--2");
        shouldBe(joinDefault(nested), "1,,2");
        shouldBe(toLocale(nested), "1,,2");
    }

    shouldBe(convert(acyclic), "1,2,3");
    shouldBe(join(acyclic), "1-2-3");
    shouldBe(joinDefault(acyclic), "1,2,3");
    shouldBe(toLocale(acyclic), "1,2,3");
}
