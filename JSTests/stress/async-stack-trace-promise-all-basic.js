//@ requireOptions("--useAsyncStackTrace=1")

const source = "async-stack-trace-promise-all-basic.js";

function nop() {}

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
  async function fine() { }
  async function thrower() { await fine(); throw new Error('error'); }
  async function run() { await Promise.all([fine(), thrower()]); }

  for (let i = 0; i < testLoopCount; i++) {
       shouldThrowAsync(
           async function test() {
               await run();
           }, Error, "error",
           [
               ["thrower", "65:50"], // Bun: upstream has 65:59, where the token ends
               ["async run", "66:40"], // Bun: upstream has 66:43, where the token ends
               ["async test", "71:22"], // Bun: upstream has 71:25, where the token ends
               ["drainMicrotasks", "[native code]"],
               ["shouldThrowAsync", "19:5"], // Bun: upstream has 19:20, where the token ends
               ["global code", "69:8"] // Bun: upstream has 69:24, where the token ends
           ],
       );
       drainMicrotasks();
   }
}

{
  async function task1() { await nop(); }
  async function task2() {
    await nop();



    throw new Error('task2 error');
  }
  async function task3() { await nop(); }
  async function runner() { await Promise.all([task1(), task2(), task3()]); }

  for (let i = 0; i < testLoopCount; i++) {
       shouldThrowAsync(
           async function test() {
               await runner();
           }, Error, "task2 error",
           [
               ["task2", "93:11"], // Bun: upstream has 93:20, where the token ends
               ["async runner", "96:43"], // Bun: upstream has 96:46, where the token ends
               ["async test", "101:22"], // Bun: upstream has 101:28, where the token ends
               ["drainMicrotasks", "[native code]"],
               ["shouldThrowAsync", "19:5"], // Bun: upstream has 19:20, where the token ends
               ["global code", "99:8"] // Bun: upstream has 99:24, where the token ends
           ],
       );
       drainMicrotasks();
   }
}

{
  async function task1() {}

  async function task2() {
    await foo();
  }
  async function foo() {
    await bar();
  }
  async function bar() {
    await 1;
    await Promise.all([task1(), baz()]);
  }
  async function baz() {
    await 2;
    throw new Error('task2 error');
  }

  async function task3() {}
  async function runner() { await Promise.all([task1(), task2(), task3()]); }

  for (let i = 0; i < testLoopCount; i++) {
       shouldThrowAsync(
           async function test() {
               await runner();
           }, Error, "task2 error",
           [
               ["baz", "131:11"], // Bun: upstream has 131:20, where the token ends
               ["async bar", "127:19"], // Bun: upstream has 127:22, where the token ends
               ["async foo", "123:11"], // Bun: upstream has 123:14, where the token ends
               ["async task2", "120:11"], // Bun: upstream has 120:14, where the token ends
               ["async runner", "135:43"], // Bun: upstream has 135:46, where the token ends
               ["async test", "140:22"], // Bun: upstream has 140:28, where the token ends
               ["drainMicrotasks", "[native code]"],
               ["shouldThrowAsync", "19:5"], // Bun: upstream has 19:20, where the token ends
               ["global code", "138:8"] // Bun: upstream has 138:24, where the token ends
           ],
       );
       drainMicrotasks();
   }
}
