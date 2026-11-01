//@ requireOptions("--useAsyncStackTrace=1")

const source = "async-stack-trace-promise-allSettled-basic.js";

function nop() {}

function shouldBe(a, b) {
  if (a !== b)
    throw new Error(`Expected ${a} to be ${b}`);
}

function testStack(error, stackFunctions) {
  const stackTrace = error.stack;
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

function unwrap(promise) {
  let result;
  promise.then(value => result = value);
  drainMicrotasks();
  return result;
}

{
  async function fine() { }
  async function thrower() { await fine(); throw new Error('error'); }
  async function run() {
    return await Promise.allSettled([thrower()]);
  }
  
  for (let i = 0; i < testLoopCount; i++) {
    const results = unwrap(run());
    shouldBe(results.length, 1);
    shouldBe(results[0].status, "rejected");
    testStack(results[0].reason, [
      ["thrower", "53:50"], // Bun: upstream has 53:59, where the token ends
      ["async run", "55:26"], // Bun: upstream has 55:36, where the token ends
      ["drainMicrotasks", "[native code]"],
      ["unwrap", "47:3"], // Bun: upstream has 47:18, where the token ends
      ["global code", "59:21"] // Bun: upstream has 59:27, where the token ends
    ]);
  }
}

{
  async function fine() { }
  async function task1() {
    
    await nop();
  
    throw new Error("error from task1");
  }
  async function task2() {
    await nop();
  
  
  
    throw new Error('error from task2');
  }
  async function task3() { await 1; throw new Error("error from task3"); }
  async function run() {
    return await Promise.allSettled([fine(), task1(), fine(), task2(), task3()]);
  }
  
  for (let i = 0; i < testLoopCount; i++) {
    const results = unwrap(run());
    shouldBe(results.length, 5);
    shouldBe(results[0].status, "fulfilled");
    shouldBe(results[1].status, "rejected");
    shouldBe(results[2].status, "fulfilled");
    shouldBe(results[3].status, "rejected");
    shouldBe(results[4].status, "rejected");
    testStack(results[1].reason, [
      ["task1", "78:11"], // Bun: upstream has 78:20, where the token ends
      ["async run", "89:26"], // Bun: upstream has 89:36, where the token ends
      ["drainMicrotasks", "[native code]"],
      ["unwrap", "47:3"], // Bun: upstream has 47:18, where the token ends
      ["global code", "93:21"] // Bun: upstream has 93:27, where the token ends
    ]);
    testStack(results[3].reason, [
      ["task2", "85:11"], // Bun: upstream has 85:20, where the token ends
      ["async run", "89:26"], // Bun: upstream has 89:36, where the token ends
      ["drainMicrotasks", "[native code]"],
      ["unwrap", "47:3"], // Bun: upstream has 47:18, where the token ends
      ["global code", "93:21"] // Bun: upstream has 93:27, where the token ends
    ]);
    testStack(results[4].reason, [
      ["task3", "87:43"], // Bun: upstream has 87:52, where the token ends
      ["async run", "89:26"], // Bun: upstream has 89:36, where the token ends
      ["drainMicrotasks", "[native code]"],
      ["unwrap", "47:3"], // Bun: upstream has 47:18, where the token ends
      ["global code", "93:21"] // Bun: upstream has 93:27, where the token ends
    ]);
  }
}
