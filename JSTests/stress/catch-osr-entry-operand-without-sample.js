//@ defaultRun; run("eager-catch-liveness", "--useLazyCatchLiveness=false")

// A catch OSR entrypoint must not give the DFG an operand without a prediction. Later phases take SpecNone for code
// that never ran: a double use of such a value became DoubleRep(NotCellNorBigIntUse), which is ToNumber, not a check.

function thrower(shouldThrow) {
  if (shouldThrow) throw new Error("thrown");
}
noInline(thrower);

function doubleArray() {
  const array = [0.5, 1.5];
  array[0] = 2.5;
  return array;
}
noInline(doubleArray);

const oddValues = [null, true, undefined, false];

function check(name, value, result) {
  if (!Object.is(result, value) && result !== String(value))
    throw new Error(name + ": saw " + result + " (" + typeof result + ") in place of " + value);
}

// With Options::useLazyCatchLiveness() these catches run before their value-profile buffer exists and not again
// until the DFG has compiled the function, so the buffer has no sample at all.
const neverSampled = {
  localAfterCatch(value, shouldThrow) {
    const t = value.p;
    try {
      thrower(shouldThrow);
    } catch {}
    return String(t);
  },

  localInFinally(value, shouldThrow) {
    const t = value.p;
    let result;
    try {
      try {
        thrower(shouldThrow);
      } finally {
        result = String(t);
      }
    } catch {}
    return result;
  },

  localInNewArrayInCatch(value, shouldThrow) {
    const t = value.p;
    try {
      thrower(shouldThrow);
    } catch {
      return [t][0];
    }
    return t;
  },

  argumentInNewArrayInCatch(value, shouldThrow, t) {
    try {
      thrower(shouldThrow);
    } catch {
      return [t][0];
    }
    return t;
  },
};

for (const name in neverSampled) {
  const f = neverSampled[name];
  noInline(f);
  f({ p: 0.5 }, true, 0.5);
  for (let i = 0; i < testLoopCount; ++i) f({ p: i + 0.5 }, false, i + 0.5);
  for (const value of oddValues) check(name, value, f({ p: value }, true, value));
}

// Whatever the option says: t is in its TDZ in each sample that reaches the prediction, and an empty bucket is no sample.
function sampledWhileEmpty(skipDeclaration, value, shouldThrow, array) {
  switch (skipDeclaration) {
    case 0:
      let t = value.p;
    case 1:
      try {
        thrower(shouldThrow);
      } catch {
        if (!skipDeclaration) {
          array[0] = t;
          return array[0];
        }
      }
  }
}
noInline(sampledWhileEmpty);

for (let i = 0; i < 10; ++i) sampledWhileEmpty(0, { p: i + 0.5 }, true, doubleArray());
for (let i = 0; i < testLoopCount; ++i) {
  sampledWhileEmpty(0, { p: i + 0.5 }, false, doubleArray());
  if (!(i % 20)) sampledWhileEmpty(1, { p: 0.5 }, true, doubleArray());
}
for (const value of oddValues)
  check("sampledWhileEmpty", value, sampledWhileEmpty(0, { p: value }, true, doubleArray()));
