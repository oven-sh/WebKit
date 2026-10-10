//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
(function () {
    const keep = [];
    for (let i = 0; i < 6000; i++) {
        const filled = new Array(300).fill(7);
        if (!(i % 3))
            keep.push(filled);
    }
    fullGC();
    for (let i = 0; i < 100; i++)
        Array.prototype["extra" + i] = i;

    function length() { return Array.prototype.length; }
    function countByLength() {
        let count = 0;
        const prototype = Array.prototype;
        for (let i = 0; i < prototype.length && count < 10; i++)
            count++;
        return count;
    }
    function countAbsentElements() {
        let count = 0;
        const prototype = Array.prototype;
        for (let i = 0; i < 4; i++) {
            if (prototype[i] === undefined)
                count++;
        }
        return count;
    }
    function element(i) { return Array.prototype[i]; }
    function has(i) { return i in Array.prototype; }
    function isArray() { return Array.isArray(Array.prototype); }
    function iterate() {
        let count = 0;
        for (const value of Array.prototype)
            count++;
        return count;
    }
    function destructure() {
        const [first, second] = Array.prototype;
        return String(first) + String(second);
    }
    function spread() { return [...Array.prototype].length; }
    function spreadInCall() { return Math.max(0, ...Array.prototype); }
    function apply() { return Math.max.apply(null, Array.prototype); }
    function from() { return Array.from(Array.prototype).length; }
    function indexOf() { return Array.prototype.indexOf(7); }
    function includes() { return Array.prototype.includes(7); }
    function at() { return Array.prototype.at(0); }
    function slice() { return Array.prototype.slice().length; }
    function concat() { return Array.prototype.concat([1]).length; }
    function join() { return Array.prototype.join("-"); }
    function forEach() {
        let count = 0;
        Array.prototype.forEach(() => { count++; });
        return count;
    }
    function map() { return Array.prototype.map((value) => value).length; }
    function some() { return Array.prototype.some((value) => value === 7); }
    function reduce() { return Array.prototype.reduce((count) => count + 1, 0); }
    function pop() { return Array.prototype.pop(); }

    const expected = [
        [length, 0], [countByLength, 0], [countAbsentElements, 4], [isArray, true], [iterate, 0], [destructure, "undefinedundefined"],
        [spread, 0], [spreadInCall, 0], [apply, -Infinity], [from, 0], [indexOf, -1], [includes, false], [at, undefined], [slice, 0],
        [concat, 1], [join, ""], [forEach, 0], [map, 0], [some, false], [reduce, 0], [pop, undefined], [length, 0],
    ];
    for (let round = 0; round < 20; round++) {
        for (const [f, result] of expected)
            check(f(), result, f.name + " of Array.prototype, which has storage for properties and none for elements");
        for (const i of [0, 1, -1, 255, 1.5, "0"]) {
            check(element(i), undefined, "element " + i);
            check(has(i), false, i + " in Array.prototype");
        }
    }
    check(keep.length, 2000, "the arrays that keep the blocks in use");
})();
