// Numbers whose encoding is their type, and a model of what arithmetic on them gives. See runtime/TaggedArithmetic.h.

// What a value is, found out by other means than what is being tested.
function kindOf(value)
{
    if (typeof value !== "number")
        return value === undefined ? "none" : "other";
    return describe(value).startsWith("Int32") ? "int" : "float";
}
noInline(kindOf);

function show(value)
{
    let kind = kindOf(value);
    return kind === "int" || kind === "float" ? kind + " " + (Object.is(value, -0) ? "-0" : String(value)) : kind;
}
noInline(show);

const int = value => value | 0;
const float = value => $vm.tagged.toDouble(value);

const ints = [0, 1, -1, 2, 3, 7, -8, 46341, 65536, -65536, 1073741824, 2147483647, -2147483648].map(int);
const floats = [0, -0, 1, -1, 2, 3, 0.5, -1.5, 2147483647, 2147483648, -2147483649, 4294967296, 1e300, -1e300, 5e-324, Infinity, -Infinity, NaN].map(float);
const others = [undefined, null, true, "1", 1n, { valueOf() { throw new Error("not to be asked"); } }, Symbol.iterator];

for (let value of ints) {
    if (kindOf(value) !== "int")
        throw new Error("Not an int: " + describe(value));
}
for (let value of floats) {
    if (kindOf(value) !== "float")
        throw new Error("Not a float: " + describe(value));
}

const plain = { add: (a, b) => a + b, sub: (a, b) => a - b, mul: (a, b) => a * b, div: (a, b) => a / b };

// What show() should say of the result.
function expected(name, left, right)
{
    let leftKind = kindOf(left), rightKind = kindOf(right);
    if ((leftKind !== "int" && leftKind !== "float") || (rightKind !== "int" && rightKind !== "float"))
        return "none";
    let value = plain[name](left, right);
    if (leftKind === "int" && rightKind === "int" && name !== "div") {
        if (name === "mul")
            value = Number(BigInt(left) * BigInt(right));
        return value >= -2147483648 && value <= 2147483647 ? "int " + value : "none";
    }
    return "float " + (Object.is(value, -0) ? "-0" : String(value));
}
noInline(expected);

function check(what, actual, wanted)
{
    let shown = show(actual);
    if (shown !== wanted)
        throw new Error(what + " gave " + shown + " and should give " + wanted);
}
noInline(check);
