//@ skip if not $jitTests
//@ runDefault("--useConcurrentJIT=0")

// An arrow function that calls super() first reads |this| of its derived constructor from the scope, to see that no
// super() came before. The value is empty each time, and a value profile does not record the empty value. The DFG takes
// such a load for a load of the empty value, not for code that did not run yet: its code for the arrow function stays.

function shouldBe(actual, expected, name)
{
    if (actual !== expected)
        throw new Error(`${name}: expected ${String(expected)} but got ${String(actual)}`);
}

class Base {
    constructor()
    {
        this.own = 1;
    }
}

let initialize;
class Derived extends Base {
    constructor()
    {
        initialize = () => { super(); };
        noInline(initialize);
        initialize();
    }
}
noInline(Derived);

// The shape of a helper that a compiler makes for a lowered class.
let initializeAndUse;
class DerivedWithHelper extends Base {
    constructor()
    {
        initializeAndUse = (...args) => { super(...args); this.field = 2; return this; };
        noInline(initializeAndUse);
        initializeAndUse();
    }
}
noInline(DerivedWithHelper);

// The check for a second super() still throws in the optimized code.
let second;
class DerivedTwice extends Base {
    constructor(again)
    {
        second = () => { super(); };
        noInline(second);
        second();
        if (again)
            second();
    }
}
noInline(DerivedTwice);

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(new Derived().own, 1, "Derived");
    shouldBe(new DerivedWithHelper().field, 2, "DerivedWithHelper");
    shouldBe(new DerivedTwice(false).own, 1, "DerivedTwice");
}

for (let i = 0; i < 10; ++i) {
    let error = null;
    try {
        new DerivedTwice(true);
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof ReferenceError && error.message, "'super()' can't be called more than once in a constructor.", "a second super()");
}

for (const arrowFunction of [initialize, initializeAndUse, second]) {
    const compiles = numberOfDFGCompiles(arrowFunction);
    if (compiles > 1)
        throw new Error(`the DFG compiled ${arrowFunction.name} ${compiles} times`);
}
