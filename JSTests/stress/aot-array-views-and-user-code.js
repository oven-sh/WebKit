//@ runDefault("--compileMainScriptAheadOfTime=1")
(function () {
    function check(actual, expected, what) {
        if (!Object.is(actual, expected))
            throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
    }

    function countsSevens(a) {
        let n = 0;
        for (let i = 0; i < a.length; i++) {
            if (a[i] === 7)
                n++;
        }
        return n;
    }
    check(countsSevens([7, 1, 7, 2, 7]), 3, "a plain array");
    check(countsSevens([]), 0, "an empty array");
    check(countsSevens([7, , 7]), 2, "an array with a hole");
    check(countsSevens([7.5, 7, 1.5]), 1, "an array of doubles");
    check(countsSevens(["7", 7, {}, null]), 1, "an array of anything");
    check(countsSevens([, , , , ,]), 0, "an array of holes");

    function countsSevensFromMinusOne(a) {
        let n = 0;
        for (let i = -1; i < a.length; i++) {
            if (a[i] === 7)
                n++;
        }
        return n;
    }
    check(countsSevensFromMinusOne([1, 2, 3, 4]), 0, "the key -1 is absent");
    check(countsSevensFromMinusOne([7, 7]), 2, "the key -1 is absent, the others count");
    {
        const grows = [1, 2, 3, 4];
        Object.defineProperty(grows, "-1", { get() { for (let k = 0; k < 100; k++) this.push(7); return 0; } });
        check(countsSevensFromMinusOne(grows), 100, "a getter for -1 lengthens the array");
        const moves = [7, 7, 7, 7];
        Object.defineProperty(moves, "-1", {
            get() {
                for (let k = 0; k < 100; k++)
                    this.push(0);
                this[0] = this[1] = this[2] = this[3] = 0;
                return 0;
            }
        });
        check(countsSevensFromMinusOne(moves), 0, "a getter for -1 moves the elements and changes them");
        const shrinks = [];
        for (let k = 0; k < 10000; k++)
            shrinks.push(7);
        let litter = [];
        Object.defineProperty(shrinks, "-1", {
            get() {
                this.length = 0;
                fullGC();
                for (let k = 0; k < 200; k++)
                    litter.push(new Array(100).fill(k));
                return 7;
            }
        });
        check(countsSevensFromMinusOne(shrinks), 1, "a getter for -1 empties the array, and its storage is collected");
        const becomesDoubles = [7, 7, 7];
        Object.defineProperty(becomesDoubles, "-1", { get() { this.push(0.5); return 0; } });
        check(countsSevensFromMinusOne(becomesDoubles), 3, "a getter for -1 turns the elements into doubles");
        const becomesSparse = [7, 7, 7];
        Object.defineProperty(becomesSparse, "-1", { get() { this[100000] = 7; this.length = 4; this[1] = 0; return 0; } });
        check(countsSevensFromMinusOne(becomesSparse), 2, "a getter for -1 makes the array sparse");
        const throws = [7, 7];
        Object.defineProperty(throws, "-1", { get() { this.push(7); throw new RangeError("from the getter"); } });
        let message = "nothing";
        try { countsSevensFromMinusOne(throws); } catch (error) { message = error.message; }
        check(message, "from the getter", "a getter for -1 throws");
        check(throws.length, 3, "after it has lengthened the array");
    }

    function ownGetterShortens() {
        const a = [1, 2, 3, 4];
        Object.defineProperty(a, 1, { get() { this.length = 2; return 0; }, configurable: true });
        let count = 0;
        for (let i = 0; i < a.length; i++) {
            if (a[i] === 100)
                return -1;
            count++;
        }
        return count;
    }
    check(ownGetterShortens(), 2, "a getter for an element shortens the array");

    function ownGetterLengthens() {
        const a = [1, 2, 3];
        Object.defineProperty(a, 0, { get() { if (this.length < 6) this.push(7); return 0; }, configurable: true });
        let count = 0;
        for (let i = 0; i < a.length; i++) {
            if (a[i] === 100)
                return -1;
            count++;
        }
        return count;
    }
    check(ownGetterLengthens(), 4, "a getter for an element lengthens the array");

    function proxyLengthens() {
        const a = [1, , 1];
        let fired = false;
        Object.setPrototypeOf(a, new Proxy(Array.prototype, {
            get(target, key, receiver) {
                if (key === "1" && !fired) { fired = true; for (let j = 0; j < 100; j++) receiver.push(1); }
                return Reflect.get(target, key, receiver);
            }
        }));
        let n = 0;
        for (let i = 0; i < a.length; i++) { if (a[i] === 1) n++; }
        return n;
    }
    check(proxyLengthens(), 102, "a proxy in the prototype chain lengthens the array when a hole is read");

    function proxyMovesElements() {
        const a = [1, , 1, 1];
        let fired = false;
        Object.setPrototypeOf(a, new Proxy(Array.prototype, {
            get(target, key, receiver) {
                if (key === "1" && !fired) { fired = true; for (let j = 0; j < 100; j++) receiver.push(0); receiver[2] = 5; receiver[3] = 5; receiver.length = 4; }
                return Reflect.get(target, key, receiver);
            }
        }));
        let n = 0;
        for (let i = 0; i < a.length; i++) { if (a[i] === 1) n++; }
        return n;
    }
    check(proxyMovesElements(), 1, "a proxy in the prototype chain moves the elements and changes them");

    function proxySeesReadsPastTheEnd() {
        const a = [1, 1];
        let keys = [];
        Object.setPrototypeOf(a, new Proxy(Array.prototype, {
            get(target, key, receiver) {
                if (typeof key === "string" && key !== "length")
                    keys.push(key);
                if (key === "2")
                    receiver.push(1, 1);
                return Reflect.get(target, key, receiver);
            }
        }));
        let n = 0;
        for (let i = 0; i < a.length + 1 && i < 10; i++) { if (a[i] === 1) n++; }
        return n + ":" + keys.join();
    }
    check(proxySeesReadsPastTheEnd(), "3:2,push,4", "a proxy in the prototype chain lengthens the array when it is read past its end");

    function readsFractions(a) {
        let n = 0;
        for (let i = 0; i < a.length; i++) {
            if (a[i] === 7)
                n++;
            if (a[i + 0.5] === 7)
                n += 100;
        }
        return n;
    }
    check(readsFractions([7, 7]), 2, "keys with a fraction are absent");
    {
        const a = [7, 1];
        Object.defineProperty(a, "0.5", { get() { this.push(7, 7); this[1] = 7; return 7; } });
        check(readsFractions(a), 104, "a getter for 0.5 lengthens the array");
    }

    function readsBeyondIndices(a) {
        let n = 0;
        for (let i = 0; i < a.length; i++) {
            if (a[i] === 7)
                n++;
            if (a[i + 4294967296] === 7)
                n += 100;
        }
        return n;
    }
    check(readsBeyondIndices([7, 7]), 2, "keys beyond the indices are absent");
    {
        const a = [7, 1];
        Object.defineProperty(a, "4294967296", { get() { this.push(7, 7); this[1] = 7; return 7; } });
        check(readsBeyondIndices(a), 104, "a getter for 4294967296 lengthens the array");
    }

    function readsTwoArrays(a, b) {
        let n = 0;
        for (let i = -1; i < a.length; i++) {
            if (a[i] === 7)
                n++;
            for (let j = 0; j < b.length; j++) {
                if (b[j] === 7)
                    n += 10;
            }
        }
        return n;
    }
    check(readsTwoArrays([7], [7, 7]), 41, "two arrays");
    {
        const a = [7], b = [7, 7];
        Object.defineProperty(a, "-1", { get() { for (let k = 0; k < 50; k++) b.push(0); b[0] = 0; b.push(7); return 0; } });
        check(readsTwoArrays(a, b), 41, "a getter of one array changes the other");
    }

    function innerReadChangesOuterArray(a, b) {
        let n = 0;
        for (let i = 0; i < a.length; i++) {
            if (a[i] === 7)
                n++;
            for (let j = -1; j < b.length; j++) {
                if (b[j] === 7)
                    n += 100;
            }
        }
        return n;
    }
    check(innerReadChangesOuterArray([7, 7], [7]), 202, "nested loops");
    {
        const a = [7, 1], b = [1];
        let fired = false;
        Object.defineProperty(b, "-1", { get() { if (!fired) { fired = true; for (let k = 0; k < 60; k++) a.push(7); a[1] = 7; } return 0; } });
        check(innerReadChangesOuterArray(a, b), 62, "a getter run in the inner loop changes the array of the outer loop");
    }

    function readsRows(rows) {
        let n = 0;
        for (let i = 0; i < rows.length; i++) {
            let row = rows[i];
            for (let j = -1; j < row.length; j++) {
                if (row[j] === 7)
                    n++;
            }
        }
        return n;
    }
    check(readsRows([[7, 7], [7], []]), 3, "rows");
    {
        const rows = [[7], [1]];
        Object.defineProperty(rows[0], "-1", { get() { rows.push([7, 7, 7]); rows[1].push(7); this.push(7); return 0; } });
        check(readsRows(rows), 6, "a getter of a row changes the rows");
    }

    function storesInLoop(a) {
        for (let i = 0; i < a.length; i++)
            a[i] = a[i] + 1;
        return a.join();
    }
    check(storesInLoop([1, 2, 3]), "2,3,4", "a loop that stores");

    function callsInLoop(a, f) {
        let n = 0;
        for (let i = 0; i < a.length; i++) {
            f(a);
            if (a[i] === 7)
                n++;
        }
        return n;
    }
    check(callsInLoop([7, 7], a => { if (a.length < 4) a.push(7); }), 4, "a loop that calls");

    function inheritedGetterLengthens() {
        const a = [1, , 1];
        Object.defineProperty(Array.prototype, 1, { get() { if (this.length < 10) this.push(1); return 0; }, configurable: true });
        let n = 0;
        for (let i = 0; i < a.length; i++) { if (a[i] === 1) n++; }
        delete Array.prototype[1];
        return n;
    }
    check(inheritedGetterLengthens(), 3, "a getter for an element on Array.prototype lengthens the array");

    {
        const b = [7, , 7];
        let once = true;
        Object.defineProperty(Array.prototype, 1, { get() { if (once) { once = false; b.push(7, 7); } return 0; }, configurable: true });
        check(countsSevens(b), 4, "the same with an array that was made before");
        delete Array.prototype[1];
    }
    check(countsSevens([7, , 7]), 2, "arrays after Array.prototype had an accessor");

    if (aotRemarks("countsSevens")) {
        const view = "array-view", reloads = "reloads-array-view";
        for (let name of ["countsSevens", "countsSevensFromMinusOne", "ownGetterShortens", "ownGetterLengthens", "proxyLengthens", "proxyMovesElements", "proxySeesReadsPastTheEnd", "readsFractions", "readsBeyondIndices", "readsTwoArrays", "innerReadChangesOuterArray", "inheritedGetterLengthens"]) {
            for (let remark of [view, reloads]) {
                if (!aotRemarks(name).includes(remark))
                    throw new Error(remark + " does not apply to " + name + ": " + aotRemarks(name).join(" "));
            }
        }
        for (let name of ["storesInLoop", "callsInLoop", "check"]) {
            for (let remark of [view, reloads]) {
                if (aotRemarks(name).includes(remark))
                    throw new Error(remark + " applies to " + name);
            }
        }
    }
})();
