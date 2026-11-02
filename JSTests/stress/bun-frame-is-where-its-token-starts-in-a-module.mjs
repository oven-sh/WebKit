// The case of bun-frame-is-where-its-token-starts.js that only a module can hold: `import.meta` ends in a name, so a
// call of it is reported where `meta` starts.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function frameOf(first, run) {
    try {
        run();
    } catch (error) {
        for (const frame of String(error.stack).split("\n")) {
            const match = /^f@.*:(\d+):(\d+)$/.exec(frame);
            if (match)
                return `${match[1] - first}:${match[2]} ${error.message}`;
        }
        throw new Error(`no frame of f in ${error.stack}`);
    }
    throw new Error("nothing was thrown");
}

let first;

first = new Error().line; shouldBe(frameOf(first, function f() {
    import.meta(1);
}), "1:12 import.meta is not a function. (In 'import.meta(1)', 'import.meta' is an instance of Object)", "a call of `import.meta`");

first = new Error().line; shouldBe(frameOf(first, function f() {
    import.
        meta
        (1);
}), "2:9 import.\n        meta\n         is not a function. (In 'import.\n        meta\n        (1)', 'import.\n        meta\n        ' is an instance of Object)", "a call of `import.meta` whose `meta` is on a line of its own");
