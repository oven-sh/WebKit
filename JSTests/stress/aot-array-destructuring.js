//@ runDefault("--compileMainScriptAheadOfTime=1")
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
function oneOfApplies(f, ...patterns) {
    let remarks = remarksOf(f);
    if (remarks && !remarks.some(remark => patterns.some(pattern => matches(remark, pattern))))
        throw new Error("none of " + patterns.join(", ") + " applies to " + f.name + ": " + remarks.join(" "));
}
function swap(a, b) { [a, b] = [b, a]; return a + "," + b; }
function rotate(a, b, c) { [a, b, c] = [b, c, a]; return a + "," + b + "," + c; }
function fewer(a, b) { [a, b] = [1]; return a + "," + b; }
function more() { let log = []; let [a] = [log.push("x"), log.push("y")]; return a + ":" + log.join(); }
function defaults() { let [a = 1, b = 2, c = 3] = [undefined, null]; return a + "," + b + "," + c; }
function elision() { let [, b, , d] = [1, 2, 3, 4]; return b + "," + d; }
function members(o) { [o.x, o.y] = [o.y, o.x]; return o.x + "," + o.y; }
function order() { let log = []; let o = { set a(v) { log.push("a=" + v); }, set b(v) { log.push("b=" + v); } }; [o.a, o.b] = [(log.push("1"), 1), (log.push("2"), 2)]; return log.join(); }
function result(a, b) { let r = ([a, b] = [b, a]); return r.join() + ":" + a + "," + b; }
function two(t) { const [q, r] = t; return q + "," + r; }
function three(t) { let [a, b = "d", c] = t; return a + "," + b + "," + c; }
function skip(t) { let [, second] = t; return second; }
function nested(t) { let [[a, b], c] = t; return a + "," + b + "," + c; }
function rest(t) { let [a, ...others] = t; return a + ":" + others.join(); }
function groups(s) { let [, year, month] = s.match(/(\d+)-(\d+)/); return year + "/" + month; }
function execGroups(s) { let [all, optional] = /x(y)?/.exec(s); return all + ":" + optional; }
function indices(s) { let [all, one] = /a(b)/d.exec(s); return all + ":" + one; }
class Custom extends Array { *[Symbol.iterator]() { yield "custom"; yield "iterator"; } }
for (let round = 0; round < 3; ++round) {
    check(swap(1, 2), "2,1", "swap");
    check(rotate(1, 2, 3), "2,3,1", "rotate");
    check(fewer(8, 9), "1,undefined", "fewer values than targets");
    check(more(), "1:x,y", "more values than targets");
    check(defaults(), "1,null,3", "defaults");
    check(elision(), "2,4", "elisions");
    check(members({ x: 1, y: 2 }), "2,1", "members");
    check(order(), "1,2,a=1,b=2", "order of evaluation");
    check(result(1, 2), "2,1:2,1", "the value of the assignment");
    check(two([3, 4]), "3,4", "an array");
    check(two([3]), "3,undefined", "a short array");
    check(two([]), "undefined,undefined", "an empty array");
    check(two([3, 4, 5]), "3,4", "a long array");
    check(two([1.5, 2.5]), "1.5,2.5", "doubles");
    check(two([, 4]), "undefined,4", "a hole");
    check(two(["a", {}.x]), "a,undefined", "undefined in it");
    check(three([1, undefined, 3]), "1,d,3", "a default");
    check(skip([1, 2]), 2, "an elision");
    check(nested([[1, 2], 3]), "1,2,3", "nested");
    check(rest([1, 2, 3]), "1:2,3", "rest");
    check(two("xy"), "x,y", "a string");
    check(two(new Set([7, 8])), "7,8", "a set");
    check(two(new Custom(1, 2)), "custom,iterator", "a subclass with its own iterator");
    let own = [1, 2];
    own[Symbol.iterator] = function* () { yield "own"; };
    check(two(own), "own,undefined", "an array with its own iterator");
    let inherits = [1, 2];
    Object.setPrototypeOf(inherits, { *[Symbol.iterator]() { yield "inherited"; } });
    check(two(inherits), "inherited,undefined", "an array with another prototype");
    check((function () { return two(arguments); })(5, 6), "5,6", "arguments");
    check(two(new Uint8Array([9, 10])), "9,10", "a typed array");
    check(groups("on 2026-10"), "2026/10", "the result of match");
    check(execGroups("xy"), "xy:y", "the result of exec");
    check(execGroups("x"), "x:undefined", "a group that did not take part");
    check(indices("ab"), "ab:b", "the result of exec with indices");
    let threw = false;
    try { two(null); } catch (e) { threw = e instanceof TypeError; }
    check(threw, true, "null");
    threw = false;
    try { two({}); } catch (e) { threw = e instanceof TypeError; }
    check(threw, true, "not iterable");
}
{
    let prototype = Object.getPrototypeOf([][Symbol.iterator]());
    let log = [];
    let method = function () { log.push("return, then " + this.next().value); return {}; };
    let isImmutable = Object.getOwnPropertyDescriptor(prototype, "next").writable === false;
    if (isImmutable) {
        check(prototype.return, undefined, "array iterators have no return method");
        check(Reflect.set(prototype, "return", method), false, "and cannot be given one");
        check(Reflect.defineProperty(prototype, "return", { value: method }), false, "nor by defining it");
        Iterator.prototype.return = method;
    } else
        prototype.return = method;
    let closed = text => isImmutable ? "" : text;
    class Counting extends Iterator { count = 0; next() { return { done: false, value: ++this.count }; } }
    for (let round = 0; round < 2; ++round) {
        log.length = 0;
        check(two([1]), "1,undefined", "a short array, with a return method");
        check(log.join(), "", "an iterator that ran out is not closed");
        check(two([1, 2]), "1,2", "an array of the same length, with a return method");
        check(log.join(), closed("return, then undefined"), "it is closed");
        log.length = 0;
        check(two([1, 2, 3]), "1,2", "a long array, with a return method");
        check(log.join(), closed("return, then 3"), "it is closed where it stopped");
        log.length = 0;
        check(skip([1, 2, 3]), 2, "an elision, with a return method");
        check(log.join(), closed("return, then 3"), "an elision counts");
        log.length = 0;
        check(swap(1, 2), "2,1", "an array literal, with a return method");
        check(log.join(), closed("return, then undefined"), "the iterator of a literal is closed too");
        log.length = 0;
        if (isImmutable) {
            check(two(new Counting), "1,2", "another iterator, with a return method");
            check(log.join(), "return, then 3", "other iterators are closed");
        }
    }
    delete (isImmutable ? Iterator.prototype : prototype).return;
    log.length = 0;
    check(two([1, 2, 3]), "1,2", "the return method is gone");
    check(log.join(), "", "nothing is called");
}

function spreads(x) { let [a, b] = [...x]; return a + "," + b; }
function hasHole(x) { let [a, b] = [, x]; return a + "," + b; }
function takesRest(x) { let [a, ...b] = [x, 1, 2]; return a + ":" + b.join(); }
function fromCall(x) { let [a, b] = Array.of(x, 1); return a + "," + b; }
function nestedLiteral(x) { let [[a], b] = [[x], 1]; return a + "," + b; }
function breaks(t) { for (let x of t) { if (x > 1) return x; } return 0; }
check(spreads([1, 2]), "1,2", "a literal with a spread");
check(hasHole(1), "undefined,1", "a literal with a hole");
check(takesRest(0), "0:1,2", "a literal, and a rest element");
check(fromCall(0), "0,1", "the result of a call");
check(nestedLiteral(0), "0,1", "a literal in a literal");
check(breaks([1, 2, 3]), 2, "leaving a loop early");
for (let f of [swap, rotate, fewer, defaults, elision, members])
    doesNotApply(f, "opens-iterator", "advances-iterator", "calls:operationAOTNewArray", "array-born-from-registers");
for (let f of [result, spreads, takesRest, fromCall, two, three, skip, nested, rest, groups])
    applies(f, "opens-iterator", "advances-iterator");
for (let f of [result, takesRest])
    oneOfApplies(f, "calls:operationAOTNewArray", "array-born-from-registers");
for (let f of [two, three, skip, nested, rest, result, breaks])
    doesNotApply(f, "calls:IteratorCloseCheck");
