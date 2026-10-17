const s1 = (-1).toLocaleString().padEnd(2**31-1, 'aa');
const s2 = (-1).toLocaleString().padEnd(2**31-1, 'aa');
try {
    // Force evaluation of Rope
    var areEqual = s1 == s2; // Bun: upstream: `s1 == s2;`. Here the comparison has an answer, which is looked at below.
} catch (e) {
    exception = e;
}

if (areEqual !== true) // Bun: upstream: `exception != "RangeError: Out of memory"`: it makes both ropes flat to compare them. This fork compares what it cannot make flat piece by piece: no exception.
    throw "FAILED";
