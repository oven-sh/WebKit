// A frame is reported where a token starts: a name where the name starts, a member at its name, a spread at the first
// token of its operand, `new X(args)` at the `new`, and a call whose callee ends in a name at that name. Upstream
// reports a name, `new X(args)` and a call where the token ends, a member at its `.` and a spread behind its `...`.
// Where a token ends the next one starts, and neither a `.` nor what stands between a `...` and its operand is a token
// that a source map has: a position there is taken for the token in front of it, which may be on another line.
//
// Section A11 of JSTests/BUN-TEST-DIFFERENCES.md says which of these positions V8 has too.
//
// A read of an index is at its `[`, and a call of anything else is behind its callee, as upstream has them.
//
// Each case is a function `f` of this file. What is compared is the line of its frame, counted from the line `f`
// starts on, the column, and the message: the text that a message quotes does not move with the position.
// bun-frame-is-where-its-token-starts-in-a-module.mjs has the case that only a module can hold.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function boom() { throw new Error("boom"); }
class Thrower { constructor() { boom(); } }
const object = {
    boom, Thrower, nothing: undefined,
    self() { return this; },
    get getter() { return boom(); },
    set setter(value) { boom(); },
    tag() { return boom(); },
    catch() { return boom(); },
};
const thrower = { get x() { return boom(); } };
const counted = { get p() { return 1; }, set p(value) { boom(); } };
const use = (...values) => values.length;

function frameOf(first, run, name = "f") {
    try {
        run();
    } catch (error) {
        for (const frame of String(error.stack).split("\n")) {
            const match = new RegExp(`^${name}@.*:(\\d+):(\\d+)$`).exec(frame);
            if (match)
                return `${match[1] - first}:${match[2]} ${error.message}`;
        }
        throw new Error(`no frame of ${name} in ${error.stack}`);
    }
    throw new Error("nothing was thrown");
}

let first;

// A name that is read.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(
        missing
    );
}), "2:9 missing is not defined", "a name");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(missing);
}), "1:9 missing is not defined", "a name in front of a `)`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    missing();
}), "1:5 missing is not defined", "a name that is called");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(
        late
    );
    let late;
}), "2:9 Cannot access 'late' before initialization.", "a name in its temporal dead zone");

// A name that is written to. None of these names is a local of `f`: it is declared nowhere, or a closure holds it.

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    missing = 1;
}), "2:5 missing is not defined", "a name that is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    ++
        missing;
}), "3:9 missing is not defined", "a name behind `++`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    -- missing;
}), "2:8 missing is not defined", "a name behind `--` and a blank");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    missing++;
}), "2:5 missing is not defined", "a name in front of `++`, in a statement");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    for (;;
        missing--) {
    }
}), "3:9 missing is not defined", "a name in front of `--`, in the head of a loop");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    use(missing++);
}), "2:9 missing is not defined", "a name in front of `++`, in an expression");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    [missing] = [1];
}), "2:6 missing is not defined", "a name in a list that is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    [
        missing
    ] = [1];
}), "3:9 missing is not defined", "a name on a line of its own in a list that is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    [...
        missing
    ] = [1];
}), "3:9 missing is not defined", "a name that takes the rest of a list");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    ({ a:
        missing
    } = { a: 1 });
}), "3:9 missing is not defined", "a name that a property is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    ({
        missing
    } = {});
}), "3:9 missing is not defined", "a name that is a property and what it is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    ({ missing = 1 } = {});
}), "2:8 missing is not defined", "such a name with a default");

first = new Error().line; shouldBe(frameOf(first, function f() {
    ({
        late
    } = {});
    let late;
    return () => late;
}), "2:9 Cannot access 'late' before initialization.", "such a name in its temporal dead zone");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    const constant = 1;
    use(() => constant);
    [
        constant
    ] = [2];
}), "5:9 Attempted to assign to readonly property.", "a constant in a list that is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    for (missing in { a: 1 }) {
    }
}), "2:10 missing is not defined", "a name that `for`-`in` assigns to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    for (
        missing
        in { a: 1 }) {
    }
}), "3:9 missing is not defined", "a name on a line of its own that `for`-`in` assigns to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    for (missing of [1]) {
    }
}), "2:10 missing is not defined", "a name that `for`-`of` assigns to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    for (
        missing
        of [1]) {
    }
}), "3:9 missing is not defined", "a name on a line of its own that `for`-`of` assigns to");

// A member, at its name.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing.x);
}), "1:24 undefined is not an object (evaluating 'object.nothing.x')", "a read of a member");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing
        .x);
}), "2:10 undefined is not an object (evaluating 'object.nothing\n        .x')", "a read of a member on the next line");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing
        .
        x);
}), "3:9 undefined is not an object (evaluating 'object.nothing\n        .\n        x')", "a read of a member whose name is on a line of its own");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object
        .getter);
}), "2:10 boom", "a getter");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object
        ?.getter);
}), "2:11 boom", "a getter behind `?.`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object
        ?.
        getter);
}), "3:9 boom", "a getter on a line of its own behind `?.`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing
        .catch);
}), "2:10 undefined is not an object (evaluating 'object.nothing\n        .catch')", "a read of a member whose name is a reserved word");

first = new Error().line; shouldBe(frameOf(first, function run() {
    class Holder {
        #field = 1;
        static f(other) {
            return other
                .#field;
        }
    }
    Holder.f({});
}), "5:18 Cannot access invalid private field (evaluating 'other\n                .#field')", "a read of a private name");

first = new Error().line; shouldBe(frameOf(first, function run() {
    class Base { static get getter() { return boom(); } }
    class Derived extends Base {
        static f() {
            return super
                .getter;
        }
    }
    Derived.f();
}), "5:18 boom", "a getter of `super`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    object
        .setter = 1;
}), "2:10 boom", "a setter");

first = new Error().line; shouldBe(frameOf(first, function f() {
    object.nothing
        .x = 1;
}), "2:10 undefined is not an object (evaluating 'object.nothing\n        .x = 1')", "a store to a member");

first = new Error().line; shouldBe(frameOf(first, function f() {
    object
        .getter += 1;
}), "2:10 boom", "a getter of a member that is added to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    object
        .getter++;
}), "2:10 boom", "a getter of a member in front of `++`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    ++object
        .getter;
}), "2:10 boom", "a getter of a member behind `++`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing
        .x());
}), "2:10 undefined is not an object (evaluating 'object.nothing\n        .x')", "a read of a member that is called");

first = new Error().line; shouldBe(frameOf(first, function f() {
    for (object
        .setter in { a: 1 }) {
    }
}), "2:10 boom", "a setter that `for`-`in` assigns to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    for (object
        .setter of [1]) {
    }
}), "2:10 boom", "a setter that `for`-`of` assigns to");

// A spread, at the first token of its operand.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(...
        1);
}), "2:9 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(... /* between */ 1);
}), "1:27 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument behind a blank and a comment");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(...1);
}), "1:12 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument behind nothing");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(0, ...
        1);
}), "2:9 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument behind another");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(boom.call(...
        1));
}), "2:9 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument of `call`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(new Thrower(...
        1));
}), "2:9 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument of `new`");

first = new Error().line; shouldBe(frameOf(first, function run() {
    class Derived extends Thrower {
        constructor() {
            super(...
                1);
        }
    }
    new Derived();
}, "Derived"), "4:17 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument of `super`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use([...
        1]);
}), "2:9 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread element");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use([0, ...
        1]);
}), "2:9 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread element behind another");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use([, ...
        1]);
}), "2:9 number is not iterable", "a spread element behind a hole");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use([...
        object
            .nothing]);
}), "2:9 Spread syntax requires ...iterable not be null or undefined", "a spread of a member");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use({ a: 0, ...
        thrower });
}), "2:9 boom", "a spread property");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use({ get a() { return 0; }, ...
        thrower });
}), "2:9 boom", "a spread property behind a getter");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use({ ...
        thrower, get a() { return 0; } });
}), "2:9 boom", "a spread property that an object literal starts with, in front of a getter");

// `new`.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(new
        Thrower(1));
}), "1:9 boom", "new with arguments");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(new Thrower(
        1
    ));
}), "1:9 boom", "new with arguments on other lines");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(new
        object.nothing(1));
}), "1:9 undefined is not a constructor (evaluating 'new\n        object.nothing(1)')", "new of what is no constructor");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(new new (class { constructor() { return Thrower; } })()
        (1));
}), "1:9 boom", "the outer of two `new`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(new
        Thrower);
}), "1:9 boom", "new without arguments, as upstream");

// A call whose callee ends in a name.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(boom());
}), "1:9 boom", "a call of a name");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object
        .self()
        .boom());
}), "3:10 boom", "a call of a member");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object
        ?.boom());
}), "2:11 boom", "a call of a member behind `?.`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing());
}), "1:16 object.nothing is not a function. (In 'object.nothing()', 'object.nothing' is undefined)", "a call of a member that is no function");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.catch());
}), "1:16 boom", "a call of a member whose name is a reserved word");

first = new Error().line; shouldBe(frameOf(first, function run() {
    class Holder {
        #method() { return boom(); }
        f() {
            use(this
                .#method());
        }
    }
    new Holder().f();
}), "5:18 boom", "a call of a private method");

first = new Error().line; shouldBe(frameOf(first, function run() {
    class Base { method() { return boom(); } }
    class Derived extends Base {
        f() {
            use(super
                .method());
        }
    }
    new Derived().f();
}), "5:18 boom", "a call of a method of `super`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(boom.call(null));
}), "1:14 boom", "a call of `call`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(boom.apply(null, []));
}), "1:14 boom", "a call of `apply`");

first = new Error().line; shouldBe(frameOf(first, function run() {
    class Derived extends Thrower {
        constructor() {
            super();
        }
    }
    new Derived();
}, "Derived"), "3:13 boom", "a call of `super`");

// A name that is a keyword elsewhere is a name where it is called.

first = new Error().line; shouldBe(frameOf(first, function f() {
    var let = boom;
    use(let(1));
}), "2:9 boom", "a call of `let`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var let = boom;
    use(l\u0065t(1));
}), "2:9 boom", "a call of `let` that is written with an escape");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var yield = boom;
    use(yield
        (1));
}), "2:9 boom", "a call of `yield`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var await = boom;
    use(await(1));
}), "2:9 boom", "a call of `await`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var async = boom;
    use(async(1));
}), "2:9 boom", "a call of `async`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var of = boom, get = boom, set = boom, static = boom;
    use(of(1), get(1), set(1), static(1));
}), "2:9 boom", "a call of `of`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var of = use, get = boom, set = boom, static = boom;
    use(of(1), get(1), set(1), static(1));
}), "2:16 boom", "a call of `get`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var of = use, get = use, set = boom, static = boom;
    use(of(1), get(1), set(1), static(1));
}), "2:24 boom", "a call of `set`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    var of = use, get = use, set = use, static = boom;
    use(of(1), get(1), set(1), static(1));
}), "2:32 boom", "a call of `static`");

// `new.target` ends in a name too, and so does `import.meta`, which the module has.

first = new Error().line; shouldBe(frameOf(first, function run() {
    function f() {
        use(new.target(1));
    }
    Reflect.construct(f, [], boom);
}), "2:17 boom", "a call of `new.target`");

first = new Error().line; shouldBe(frameOf(first, function run() {
    function f() {
        use(new.
            target
            (1));
    }
    Reflect.construct(f, [], boom);
}), "3:13 boom", "a call of `new.target` whose `target` is on a line of its own");

// As upstream: an index at its `[`, a call behind a callee that ends in no name, a template at its backquote, the store behind a count or a sum, the store of a member that a destructuring assigns to, a `delete` behind its member, the copy that an object literal without a getter or setter starts with.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing
        [0]);
}), "2:9 undefined is not an object (evaluating 'object.nothing\n        [0]')", "a read of an index");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.boom
        ?.());
}), "2:11 boom", "an optional call");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use((object.boom)
        ());
}), "1:22 boom", "a call of a callee in parentheses");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object["boom"]());
}), "1:23 boom", "a call of an index");

first = new Error().line; shouldBe(frameOf(first, function f() {
    (() => {
        boom();
    })();
}), "3:7 boom", "a call of a function expression");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.tag
        `x`);
}), "2:9 boom", "a tagged template");

first = new Error().line; shouldBe(frameOf(first, function f() {
    counted
        .p++;
}), "2:11 boom", "the store of a member in front of `++`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    ++
        counted
            .p;
}), "1:7 boom", "the store of a member behind `++`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    counted
        .p
        += 1;
}), "3:10 boom", "the store of a member that is added to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    [
        object
            .setter
    ] = [1];
}), "1:5 boom", "the store of a member in a list that is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    ({ k:
        object
            .setter
    } = { k: 1 });
}), "3:20 boom", "the store of a member that a property is assigned to");

first = new Error().line; shouldBe(frameOf(first, function f() {
    delete object.nothing
        .x;
}), "2:11 undefined is not an object (evaluating 'delete object.nothing\n        .x')", "a member of nothing that is deleted");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use({ ...
        thrower });
}), "1:9 boom", "a spread that an object literal starts with");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use({ ...
        thrower, a: 0 });
}), "1:9 boom", "a spread that an object literal starts with, in front of a property");
