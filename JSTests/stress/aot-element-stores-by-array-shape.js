//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function stores(a, index, value) { a[index] = value; }
function defines(first, second, third) { return [first, , second, third]; }
function definesIntegers(i) { return [i | 0, , i | 0, (i + 1) | 0]; }
function definesIntegerThenDouble(i) { return [i | 0, , i + 0.5]; }
function makesSized(size) { return new Array(size); }
function makesEmpty() { return []; }

const countsOperations = typeof aotOperationCount === "function" && isAOTCompiled(stores) && (aotRemarks("stores") || []).includes("calls:PutByVal") && aotOperationCount("operationAOTPutByVal") !== null;
function operationsDuring(name, f) {
    if (!countsOperations) {
        f();
        return -1;
    }
    const before = aotOperationCount(name);
    f();
    return aotOperationCount(name) - before;
}
function checkOperations(actual, atLeast, atMost, what) {
    if (actual >= 0 && (actual < atLeast || actual > atMost))
        throw new Error(what + ": " + actual + " calls of the operation, not " + atLeast + " to " + atMost);
}
const rounds = 1000;
const kept = [];

{
    const integers = [1, 2, 3, 4];
    stores(integers, 0, 5);
    checkOperations(operationsDuring("operationAOTPutByVal", () => {
        for (let i = 0; i < rounds; i++)
            stores(integers, i & 3, i);
    }), 0, 0, "integers stored into an array of integers");
    check(integers.join(), "996,997,998,999", "integers stored into an array of integers");
}
{
    const anything = [{}, 2, "three", 4];
    checkOperations(operationsDuring("operationAOTPutByVal", () => {
        for (let i = 0; i < rounds; i++)
            stores(anything, i & 3, i & 1 ? i : anything);
    }), 0, 0, "values stored into an array of anything");
    check(anything[3], 999, "values stored into an array of anything");
}
{
    const integers = [];
    integers.push(1);
    checkOperations(operationsDuring("operationAOTPutByVal", () => {
        stores(integers, 1, 2);
        stores(integers, 2, 3);
    }), 0, 0, "integers stored right after the end, within the capacity");
    check(integers.join() + " " + integers.length, "1,2,3 3", "integers stored right after the end");
}
{
    const integers = [1, 2, 3, 4];
    stores(integers, 0, 5);
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(integers, 1, 0.5)), 1, 1, "a double stored into an array of integers");
    check(integers.join(), "5,0.5,3,4", "a double stored into an array of integers");
    stores(integers, 2, 7);
    check(integers.join(), "5,0.5,7,4", "an integer stored into what became an array of doubles");
}
{
    const integers = [1, 2, 3, 4];
    stores(integers, 0, 5);
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(integers, 1, "text")), 1, 1, "a string stored into an array of integers");
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(integers, 2, 6)), 0, 0, "an integer stored into what became an array of anything");
    check(integers.join(), "5,text,6,4", "a string stored into an array of integers");
}
{
    const doubles = [0.5, 1.5, 2.5, 3.5];
    stores(doubles, 0, 4.5);
    checkOperations(operationsDuring("operationAOTPutByVal", () => {
        for (let i = 0; i < rounds; i++)
            stores(doubles, i & 3, i & 1 ? i + 0.5 : i);
    }), 0, 0, "doubles and integers stored into an array of doubles");
    check(doubles.join(), "996,997.5,998,999.5", "doubles and integers stored into an array of doubles");
    stores(doubles, 1, -0);
    check(doubles[1], -0, "negative zero stored into an array of doubles");
    stores(doubles, 1, Infinity);
    check(doubles[1], Infinity, "Infinity stored into an array of doubles");
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(doubles, 2, NaN)), 1, 1, "NaN stored into an array of doubles");
    check(doubles.join(), "996,Infinity,NaN,999.5", "NaN stored into an array of doubles");
    const others = [0.5, 1.5];
    stores(others, 0, 2.5);
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(others, 1, "text")), 1, 1, "a string stored into an array of doubles");
    check(others.join(), "2.5,text", "a string stored into an array of doubles");
    const holey = [0.5, , 2.5];
    stores(holey, 1, 1.5);
    check(holey.join() + " " + (1 in holey), "0.5,1.5,2.5 true", "a double stored into a hole");
    const growing = [0.5];
    growing.push(1.5);
    for (let i = 2; i < 40; i++)
        stores(growing, i, i + 0.5);
    check(growing.length + " " + growing[39] + " " + growing[2], "40 39.5 2.5", "doubles stored right after the end");
}
{
    const constant = [1, 2, 3, 4];
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(constant, 0, 9)), 1, 1, "the first store into a literal of constants");
    check(constant.join() + " " + [1, 2, 3, 4].join(), "9,2,3,4 1,2,3,4", "the first store into a literal of constants");
}
{
    const integers = [1, 2, 3, 4];
    stores(integers, 0, 5);
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(integers, 1000, 1)), 1, 1, "an integer stored far past the end");
    check(integers.length, 1001, "an integer stored far past the end");
    checkOperations(operationsDuring("operationAOTPutByVal", () => stores(integers, -1, 1)), 1, 1, "an integer stored at -1");
    check(integers[-1], 1, "an integer stored at -1");
}
{
    const frozen = Object.freeze([1, 2]);
    stores(frozen, 0, 9);
    check(frozen.join(), "1,2", "an integer stored into a frozen array");
    const object = { 0: 1, 1: 2 };
    stores(object, 0, 9);
    check(object[0], 9, "an integer stored into an object with elements");
}
{
    checkOperations(operationsDuring("operationAOTPutByValDirect", () => {
        for (let i = 0; i < rounds; i++)
            kept[i & 7] = defines(i, i + 1, i + 2);
    }), 0, 20, "literals of integers with a hole");
    check(kept[7].join() + " " + (1 in kept[7]), "999,,1000,1001 false", "a literal of integers with a hole");
    check(defines(1, "two", 0.5).join(), "1,,two,0.5", "a literal that starts with an integer");
    checkOperations(operationsDuring("operationAOTPutByValDirect", () => {
        for (let i = 0; i < rounds; i++)
            kept[i & 7] = definesIntegers(i);
    }), 0, 20, "literals of proven integers with a hole");
    check(kept[7].join() + " " + (1 in kept[7]), "999,,999,1000 false", "a literal of proven integers with a hole");
    check(definesIntegerThenDouble(3).join(), "3,,3.5", "a literal of a proven integer and a double");
}
{
    checkOperations(operationsDuring("operationAOTNewArrayWithSize", () => {
        for (let i = 0; i < rounds; i++)
            kept[i & 7] = makesSized(i & 7);
    }), 0, 50, "arrays made with a size");
    check(kept[7].length, 7, "an array made with a size");
    checkOperations(operationsDuring("operationAOTNewArrayWithSize", () => makesSized("7")), 1, 1, "an array made of one string");
    const sized = makesSized(4);
    checkOperations(operationsDuring("operationAOTPutByVal", () => {
        for (let i = 0; i < 4; i++)
            stores(sized, i, i);
    }), 0, 0, "integers stored into an array made with a size");
    check(sized.join(), "0,1,2,3", "integers stored into an array made with a size");
}
{
    checkOperations(operationsDuring("operationAOTNewArray", () => {
        for (let i = 0; i < 20 * rounds; i++)
            kept[i & 7] = makesEmpty();
    }), 0, 400, "empty arrays");
    check(kept[0].length, 0, "an empty array");
}
