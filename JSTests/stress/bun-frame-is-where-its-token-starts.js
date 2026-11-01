// A frame is reported where the token that it belongs to starts, which is also where V8 reports it: a name where the
// name starts, a spread at its `...`, `new X(args)` at the `new`, and a call whose callee ends in a name at that name.
// Upstream reports these four where the token ends. The text behind that position belongs to the next token, so a
// source map that maps tokens cannot say which of the two a position means.
//
// A read of a member is at its `.` or `[`, and a call of anything else is behind its callee, as upstream has them.
//
// Each case is a function `f` of this file. What is compared is the line of its frame, counted from the line `f`
// starts on, the column, and the message: the text that a message quotes does not move with the position.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function boom() { throw new Error("boom"); }
class Thrower { constructor() { boom(); } }
const object = { boom, Thrower, nothing: undefined, self() { return this; }, get getter() { return boom(); }, tag() { return boom(); } };
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

// A name.

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

first = new Error().line; shouldBe(frameOf(first, function f() {
    "use strict";
    missing = 1;
}), "2:5 missing is not defined", "a name that is assigned to");

// A spread.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(...
        1);
}), "1:9 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread argument");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use([0, ...
        1]);
}), "1:13 Spread syntax requires ...iterable[Symbol.iterator] to be a function", "a spread element");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use({ a: 0, ...
        { get x() { return boom(); } } });
}), "1:17 boom", "a spread property");

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
    use(object.nothing());
}), "1:16 object.nothing is not a function. (In 'object.nothing()', 'object.nothing' is undefined)", "a call of a member that is no function");

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

// As upstream: a member at its `.` or `[`, a call behind a callee that does not end in a name, and a template at
// its backquote.

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing
        .x);
}), "2:9 undefined is not an object (evaluating 'object.nothing\n        .x')", "a read of a member");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object.nothing
        [0]);
}), "2:9 undefined is not an object (evaluating 'object.nothing\n        [0]')", "a read of an index");

first = new Error().line; shouldBe(frameOf(first, function f() {
    use(object
        .getter);
}), "2:9 boom", "a getter");

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
