//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
let messages = [];
function record(what, promise) { promise.then(() => messages.push(what + ": nothing was thrown"), error => messages.push(what + ": " + error.message)); }

async function opens(iterable) { for await (const element of iterable) { } }
async function advances(iterable) { for await (const element of iterable) { } }
function opensSync(iterable) { for (const element of iterable) { } }

record("the method is a number", opens({ [Symbol.asyncIterator]: 5 }));
record("next is a number", advances({ [Symbol.asyncIterator]() { return { next: 5 }; } }));
record("the sync method is a number", opens({ [Symbol.iterator]: 5 }));
record("no method", opens(5));
record("the iterator is a number", opens({ [Symbol.asyncIterator]() { return 5; } }));
record("the result is a number", advances({ [Symbol.asyncIterator]() { return { next() { return 5; } }; } }));
drainMicrotasks();

check(messages.join("\n"), [
    "the method is a number: 5 is not a function (near '...element of iterable...')",
    "next is a number: 5 is not a function (near '...element of iterable...')",
    "the sync method is a number: iterable should have an iterator symbol",
    "no method: iterable should have an iterator symbol",
    "the iterator is a number: Iterator result interface is not an object.",
    "the result is a number: Iterator result interface is not an object.",
].join("\n"), "messages of for await");

let message;
try {
    opensSync({ [Symbol.iterator]: 5 });
} catch (error) {
    message = error.message;
}
check(message, "{} is not iterable", "message of for of");
