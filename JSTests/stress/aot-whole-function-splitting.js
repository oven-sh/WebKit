//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTWholeFunctionSplitting=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
const split = "split-function";
let log = [];
function note(x) { log.push(x); return x; }
noInline(note);
function logged(f) { log = []; let result; try { result = f(); } catch (error) { result = error.constructor.name; } return log.join("") + "=" + result; }

function readsThree(a, b, c) { note("1"); const x = a.x; note("2"); const y = b.y; note("3"); const z = c.z; note("4"); return x + y + z; }
function writesThree(a, b, c, v) { note("1"); a.x = v; note("2"); b.y = v + 1; note("3"); c.z = v + 2; note("4"); return a.x + b.y + c.z; }
function readsElements(list, i, j) { note("1"); const x = list[i]; note("2"); const y = list[j]; note("3"); return x + "," + y; }
function writesElements(list, i, j, v) { note("1"); list[i] = v; note("2"); list[j] = v + 1; note("3"); return list.length; }
function branches(a, flag) { note("1"); let r = a.x; if (flag) { note("2"); r += a.y; } else { note("3"); r -= a.z; } note("4"); return r + a.w; }
function hasLoop(a, list) { note("1"); let total = a.base; for (let i = 0; i < list.length; i++) total += list[i]; note("2"); return total + a.extra; }
function readsLength(a, b) { note("1"); const x = a.length; note("2"); return x + b.length; }
const splitFunctions = [readsThree, writesThree, readsElements, writesElements, branches, hasLoop, readsLength];
function probe(a) { return a.x; }
const hasDataStubs = !aotRemarks("probe")?.includes("calls:operationAOTGetById");
for (const f of splitFunctions) {
    noInline(f);
    if (hasDataStubs)
        applies(f, split);
}

function oneCheck(a) { return a.x; }
function catches(a, b) { try { return a.x + b.y; } catch { return -1; } }
function* generates(a, b) { yield a.x; yield b.y; }
async function awaits(a, b) { await null; return a.x + b.y; }
for (const f of [oneCheck, catches]) {
    noInline(f);
    doesNotApply(f, split);
}

class P { constructor(x, y, z) { this.x = x; this.y = y; this.z = z; } }
const plain = () => ({ x: 1, y: 2, z: 3, w: 4 });
const other = () => ({ z: 30, y: 20, x: 10, w: 40 });
const getter = () => ({ get x() { note("gx"); return 100; }, get y() { note("gy"); return 200; }, get z() { note("gz"); return 300; }, w: 400 });
const setter = () => { const o = { set x(v) { note("sx"); this._x = v; }, get x() { return this._x; }, set y(v) { note("sy"); this._y = v; }, get y() { return this._y; }, set z(v) { note("sz"); this._z = v; }, get z() { return this._z; } }; return o; };
const proxy = () => new Proxy({ x: 7, y: 8, z: 9 }, { get(t, k) { note("p" + String(k)); return t[k]; }, set(t, k, v) { note("P" + String(k)); t[k] = v; return true; } });
const inherits = () => Object.create({ x: 5, y: 6, z: 7 });
const frozen = () => Object.freeze({ x: 1, y: 2, z: 3 });

for (let round = 0; round < 60; round++) {
    check(logged(() => readsThree(plain(), plain(), plain())), "1234=6", "the same layout three times");
    check(logged(() => readsThree(new P(1, 2, 3), plain(), other())), "1234=33", "three layouts");
    check(logged(() => readsThree(getter(), plain(), plain())), "1gx234=105", "the first check fails");
    check(logged(() => readsThree(plain(), getter(), plain())), "12gy34=204", "the second check fails");
    check(logged(() => readsThree(plain(), plain(), getter())), "123gz4=303", "the third check fails");
    check(logged(() => readsThree(proxy(), proxy(), proxy())), "1px2py3pz4=24", "each is a proxy");
    check(logged(() => readsThree(plain(), null, plain())), "12=TypeError", "the second base is null");
    check(logged(() => readsThree(plain(), plain(), undefined)), "123=TypeError", "the third base is undefined");
    check(logged(() => readsThree(inherits(), {}, 5)), "1234=NaN", "inherited, missing, and a number");
    check(logged(() => readsThree("s", plain(), plain())), "1234=NaN", "a string");

    check(logged(() => writesThree(plain(), plain(), plain(), 1)), "1234=6", "stores to the same layout");
    check(logged(() => writesThree({}, {}, {}, 1)), "1234=6", "stores that add a property");
    check(logged(() => writesThree(plain(), setter(), plain(), 1)), "12sy34=6", "the second store calls a setter");
    check(logged(() => writesThree(setter(), setter(), setter(), 1)), "1sx2sy3sz4=6", "each store calls a setter");
    check(logged(() => writesThree(plain(), proxy(), plain(), 1)), "12Py34py=6", "a store to a proxy");
    check(logged(() => writesThree(plain(), frozen(), plain(), 5)), "1234=14", "a store to a frozen object is ignored");
    check(logged(() => writesThree(plain(), plain(), null, 1)), "123=TypeError", "a store to null");

    check(logged(() => readsElements([1, 2, 3], 0, 2)), "123=1,3", "elements");
    check(logged(() => readsElements([1, 2, 3], 0, 5)), "123=1,undefined", "past the end");
    check(logged(() => readsElements([1, , 3], 1, 2)), "123=undefined,3", "a hole");
    check(logged(() => readsElements([1.5, 2.5], 0, 1)), "123=1.5,2.5", "doubles");
    check(logged(() => readsElements("abc", 0, 2)), "123=a,c", "a string");
    check(logged(() => readsElements({ a: 1, b: 2 }, "a", "b")), "123=1,2", "names");
    check(logged(() => readsElements(new Uint8Array([9, 8]), 0, 1)), "123=9,8", "a typed array");
    check(logged(() => readsElements([1, 2], -1, 0.5)), "123=undefined,undefined", "indices that are none");
    check(logged(() => readsElements(null, 0, 1)), "1=TypeError", "no array");

    check(logged(() => writesElements([1, 2, 3], 0, 2, 7)), "123=3", "stores to elements");
    check(logged(() => writesElements([1, 2, 3], 0, 3, 7)), "123=4", "a store at the end");
    check(logged(() => writesElements([1, 2, 3], 9, 0, 7)), "123=10", "a store past the end");
    check(logged(() => writesElements([1, 2, 3], 0, 1, "s")), "123=3", "stores that change the kind of array");
    check(logged(() => writesElements(Object.freeze([1, 2]), 0, 1, 7)), "123=2", "stores to a frozen array");

    check(logged(() => branches(plain(), true)), "124=7", "one branch");
    check(logged(() => branches(plain(), false)), "134=2", "the other");
    check(logged(() => branches(getter(), true)), "1gx2gy4=700", "a check that fails before the branch");
    check(logged(() => branches(other(), false)), "134=20", "another layout");

    check(logged(() => hasLoop({ base: 1, extra: 2 }, [1, 2, 3])), "12=9", "a loop between two reads");
    check(logged(() => hasLoop({ extra: 2, base: 1 }, [1.5, 2.5])), "12=7", "another layout and doubles");
    check(logged(() => hasLoop({ base: 1, extra: 2 }, [1, "x", 3])), "12=2x32", "a string in the loop");
    check(logged(() => hasLoop({ get base() { note("b"); return 1; }, get extra() { note("e"); return 2; } }, [1])), "1b2e=4", "getters around the loop");

    check(logged(() => readsLength([1, 2], "abc")), "12=5", "an array and a string");
    check(logged(() => readsLength({ length: 7 }, new Uint8Array(3))), "12=10", "an object and a typed array");
    check(logged(() => readsLength(function (a, b) { }, [])), "12=2", "a function");

    check(oneCheck(plain()) + catches(plain(), plain()) + catches(plain(), null), 1 + 3 - 1, "functions that are not split");
    check([...generates(plain(), other())].join(), "1,20", "a generator");
}
let awaited;
awaits({ x: 1 }, { y: 2 }).then(value => { awaited = value; });
drainMicrotasks();
check(awaited, 3, "an async function");
