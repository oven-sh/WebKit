//@ requireOptions("--useAsyncStackTrace=1", "-m")

function shouldBe(a, b) {
  if (a !== b)
    throw new Error(`Expected ${b} but got ${a}`);
}

async function foo(x) {
  await bar(x);
}
async function bar(x) {
  await x;
  throw new Error("FOO");
}

try {
  await foo();
} catch (e) {
  const lines = e.stack.split("\n");
  shouldBe(lines.length, 3); // Bun: upstream expects 2. The module that waits at the top-level await has a frame.
  shouldBe(lines[0].slice(0, 4), "bar@");
  shouldBe(lines[1].slice(0, 10), "async foo@");
  shouldBe(lines[2].slice(0, 18), "async module code@");
}
