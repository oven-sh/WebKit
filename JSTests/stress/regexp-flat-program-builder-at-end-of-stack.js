//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1", "--maxPerThreadStackUsage=100000000")

const depth = 62;
let made = 0;
let matched = 0;
function dive() {
    try {
        dive();
    } catch { }
    if (matched < 3) {
        new RegExp("(?:x|".repeat(depth) + "a" + made++ + ")*y".repeat(depth)).test("xay");
        matched++;
    }
}
dive();
