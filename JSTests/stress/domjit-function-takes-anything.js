function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`expected ${String(expected)}, got ${String(actual)}`);
}

const object = $vm.createDOMJITFunctionObject();

function call(object, first, second) { return object.takesAnything(first, second); }
noInline(call);

function callWithNumbers(object, i) { return object.takesAnything(i + 1, i + 0.5); }
noInline(callWithNumbers);

function seesWhatTheCallChanges(object, box) {
    const before = box.value;
    object.takesAnything({ toString() { ++box.value; return ""; } }, null);
    return box.value - before;
}
noInline(seesWhatTheCallChanges);

function catches(object, i) {
    try {
        return object.takesAnything(i & 1 ? Symbol.iterator : "", i)[1];
    } catch (e) {
        return e instanceof TypeError ? -i : NaN;
    }
}
noInline(catches);

const values = [1, 1.5, "a", null, undefined, true, {}, [], 1n, -0];
for (let i = 0; i < testLoopCount; ++i) {
    const [first, second] = [values[i % values.length], values[(i >> 2) % values.length]];
    const result = call(object, first, second);
    shouldBe(result[0], String(first));
    shouldBe(result[1], second);
    shouldBe(callWithNumbers(object, i).join(), `${i + 1},${i + 0.5}`);
    shouldBe(seesWhatTheCallChanges(object, { value: i }), 1);
    shouldBe(catches(object, i), i & 1 ? -i : i);
}

// The result is made in the realm of the function.
const other = createGlobalObject();
const ofOther = other.eval("$vm.createDOMJITFunctionObject()");
function callOther(object) { return object.takesAnything(1, 2); }
noInline(callOther);
function callEither(object) { return object.takesAnything(1, 2); }
noInline(callEither);
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(callOther(ofOther) instanceof other.Array, true);
    shouldBe(callEither(i & 1 ? ofOther : object) instanceof (i & 1 ? other.Array : Array), true);
}
