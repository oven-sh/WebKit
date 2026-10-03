// Arrays with a run-time length of 0, 1 and 2, from a rest parameter and from new Array(length): never grown, grown by
// one push, grown by five pushes, and grown by one unshift. An array that optimized code allocates inline has room for
// its length and no more, so the first push or unshift after it pays for the growth.

function rest(...values) {
    return values;
}
noInline(rest);

function restThenPush(...values) {
    values.push(0);
    return values;
}
noInline(restThenPush);

function restThenFivePushes(...values) {
    for (let i = 0; i < 5; ++i)
        values.push(i);
    return values;
}
noInline(restThenFivePushes);

function restThenUnshift(first, ...values) {
    values.unshift(first);
    return values;
}
noInline(restThenUnshift);

function make(length) {
    return new Array(length);
}
noInline(make);

function makeThenPush(length) {
    const array = new Array(length);
    array.push(0);
    return array;
}
noInline(makeThenPush);

function makeThenFivePushes(length) {
    const array = new Array(length);
    for (let i = 0; i < 5; ++i)
        array.push(i);
    return array;
}
noInline(makeThenFivePushes);

const iterations = 30000;
let totalLength = 0;
for (let i = 0; i < iterations; ++i) {
    totalLength += rest().length + rest(i).length + rest(i, i).length;
    totalLength += restThenPush().length + restThenPush(i).length + restThenPush(i, i).length;
    totalLength += restThenFivePushes().length + restThenFivePushes(i).length + restThenFivePushes(i, i).length;
    totalLength += restThenUnshift(i).length + restThenUnshift(i, i).length + restThenUnshift(i, i, i).length;
    totalLength += make(0).length + make(1).length + make(2).length;
    totalLength += makeThenPush(0).length + makeThenPush(1).length + makeThenPush(2).length;
    totalLength += makeThenFivePushes(0).length + makeThenFivePushes(1).length + makeThenFivePushes(2).length;
}

if (totalLength !== 60 * iterations)
    throw new Error("bad total length: " + totalLength);
