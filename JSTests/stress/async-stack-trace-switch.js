//@ requireOptions("--useAsyncStackTrace=1")

const source = "async-stack-trace-switch.js";

function shouldThrowAsync(run, errorType, message, stackFunctions) {
    let actual;
    var hadError = false;
    run().then(
        function (value) {
            actual = value;
        },
        function (error) {
            hadError = true;
            actual = error;
        },
    );
    drainMicrotasks();
    if (!hadError) {
        throw new Error("Expected " + run + "() to throw " + errorType.name + ", but did not throw.");
    }
    if (!(actual instanceof errorType)) {
        throw new Error("Expected " + run + "() to throw " + errorType.name + ", but threw '" + actual + "'");
    }
    if (message !== void 0 && actual.message !== message) {
        throw new Error("Expected " + run + "() to throw '" + message + "', but threw '" + actual.message + "'");
    }

    const stackTrace = actual.stack;
    if (!stackTrace) {
        throw new Error("Expected error to have stack trace, but it was undefined");
    }

    const stackLines = stackTrace.split('\n').filter(line => line.trim());

    for (let i = 0; i < stackFunctions.length; i++) {
        const [expectedFunction, expectedLocation] = stackFunctions[i];
        const isNativeCode = expectedLocation === "[native code]" 
        const stackLine = stackLines[i];

        let found = false;

        if (isNativeCode) {
            if (stackLine === `${expectedFunction}@[native code]`)
                found = true;
        } else {
            if (stackLine === `${expectedFunction}@${source}:${expectedLocation}`)
                found = true;
            if (stackLine === `${expectedFunction}@${source}`)
                found = true;
        }

        if (!found) {
            throw new Error(
                `Expected stack trace to contain '${expectedFunction}' at '${expectedLocation}', but got '${stackLine}'` +
                `\nActual stack trace:\n${stackTrace}\n`
            );
        }
    }
}

{
    async function foo(value) {
        switch (value) {
            case 1:
            case 2:
            case 3:
                return "foo";
        }
        await bar(value);
    }

    async function bar(value) {
        switch (value) {
            case 1:
            case 2:
            case 3:
                return "bar";
        }

        switch (value) {
            case 4:
            case 5:
                return "bar";
        }

        await baz(value);
    }

    async function baz(value) {
        await value;

        throw new Error("error from baz");
    }

    for (let i = 0; i < testLoopCount; i++) {
        shouldThrowAsync(
            async function test() {
                await foo(9);
            }, Error, "error from baz",
            [
                ["baz", "92:15"], // Bun: upstream has 92:24, where the token ends
                ["async bar", "86:15"], // Bun: upstream has 86:18, where the token ends
                ["async foo", "69:15"], // Bun: upstream has 69:18, where the token ends
                ["async test", "98:23"], // Bun: upstream has 98:26, where the token ends
                ["drainMicrotasks", "[native code]"],
                ["shouldThrowAsync", "17:5"], // Bun: upstream has 17:20, where the token ends
                ["global code", "96:9"] // Bun: upstream has 96:25, where the token ends
            ],
        );
        drainMicrotasks();
    }
}
