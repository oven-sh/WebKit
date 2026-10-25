//@ runDefault

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function flat(string) {
    return string.split("").join("");
}

// substring() of a rope may return a fiber, and resolving a short cut may narrow it: cut a flat string, then copy the cut with join().
function flat16Bit(string) {
    return [flat(String.fromCharCode(0x100) + string).substring(1), ""].join("");
}

const widths = [
    { name: "8-bit", is8Bit: true, firstCodeUnit: 0x61, flatten: flat },
    { name: "16-bit", is8Bit: false, firstCodeUnit: 0x430, flatten: flat },
    { name: "Latin-1 in 16-bit", is8Bit: false, firstCodeUnit: 0x61, flatten: flat16Bit },
];

function concatenate(fibers) {
    return fibers.reduce((left, right) => left + right);
}

function cut(base, start, length) {
    const string = base.substring(start, start + length);
    shouldBe(isRope(string), true, "isRope of a substring");
    return string;
}

const flatString = { name: "flat string", isRope: false, create: ({ width, text }) => width.flatten(text) };
const leftLeaningRope = { name: "left-leaning rope", isRope: true, create: ({ fibers }) => concatenate(fibers) };
const rightLeaningRope = { name: "right-leaning rope", isRope: true, create: ({ fibers }) => fibers.reduceRight((right, left) => left + right) };
const threeFiberRope = { name: "three-fiber rope", isRope: true, create: ({ fibers: [first, second, ...rest] }) => `${first}${second}${concatenate(rest)}` };
const substringRope = { name: "substring rope", isRope: true, create: ({ base, text }) => cut(base, 1, text.length) };
const ropeOfSubstringRopes = {
    name: "rope of substring ropes",
    isRope: true,
    create: ({ base, fibers }) => concatenate(fibers.map((fiber, index) => cut(base, 1 + index * fiber.length, fiber.length))),
};
const deepRepresentations = [flatString, leftLeaningRope, rightLeaningRope];
const allRepresentations = [...deepRepresentations, threeFiberRope, substringRope, ropeOfSubstringRopes];

function createOperand(representation, input) {
    const string = representation.create(input);
    shouldBe(isRope(string), representation.isRope, `isRope of ${representation.name} of ${input.name}`);
    shouldBe(is8BitString(string), input.width.is8Bit, `is8BitString of ${representation.name} of ${input.name}`);
    return string;
}

function createFiber(width, codeUnits) {
    return width.flatten(String.fromCharCode(...codeUnits));
}

function createFibers(width, fiberCount, fiberLength) {
    const fibers = [];
    for (let i = 0; i < fiberCount; ++i)
        fibers.push(createFiber(width, Array.from({ length: fiberLength }, (_, j) => width.firstCodeUnit + (i * fiberLength + j) % 26)));
    return fibers;
}

function createInput(width, fibers, name) {
    const text = fibers.join("");
    return { name: `${name} (${width.name})`, width, fibers, text, base: width.flatten(`<${text}>`) };
}

function differentCodeUnit(codeUnit) {
    const lowByte = codeUnit & 0xff;
    return lowByte === codeUnit ? codeUnit ^ 1 : lowByte;
}

function replaceCodeUnit({ width, fibers }, index) {
    const fiberIndex = Math.floor(index / fibers[0].length);
    const offset = index % fibers[0].length;
    const codeUnits = Array.from(fibers[fiberIndex], character => character.charCodeAt(0));
    codeUnits[offset] = differentCodeUnit(codeUnits[offset]);
    const replacedFibers = fibers.slice();
    replacedFibers[fiberIndex] = createFiber(width, codeUnits);
    return createInput(width, replacedFibers, `${fibers.length} fibers, different at ${index}`);
}

function equals(left, right) {
    return left === right;
}
noInline(equals);

function shouldHaveText(string, text, message) {
    shouldBe(string.length, text.length, message);
    for (let i = 0; i < text.length; ++i)
        shouldBe(string.charCodeAt(i), text.charCodeAt(i), message);
}

function checkEquality(leftRepresentation, leftInput, rightRepresentation, rightInput) {
    const message = `${leftRepresentation.name} of ${leftInput.name} and ${rightRepresentation.name} of ${rightInput.name}`;
    const expected = leftInput.text === rightInput.text;
    const hasSameLength = leftInput.text.length === rightInput.text.length;
    const left = createOperand(leftRepresentation, leftInput);
    const right = createOperand(rightRepresentation, rightInput);

    shouldBe(equals(left, right), expected, message);
    for (const [string, representation, input] of [[left, leftRepresentation, leftInput], [right, rightRepresentation, rightInput]]) {
        if (!hasSameLength)
            shouldBe(isRope(string), representation.isRope, message);
        if (input.width.is8Bit)
            shouldBe(is8BitString(string), true, message);
        shouldHaveText(string, input.text, message);
    }

    const map = new Map([[left, true]]);
    shouldBe(map.has(leftInput.text), true, message);
    shouldBe(map.has(right), expected, message);
    shouldBe(equals(left, right), expected, message);
    shouldBe(equals(right, left), expected, message);
}

function checkAllPairs(representations, leftInputs, rightInputs) {
    for (const leftRepresentation of representations) {
        for (const rightRepresentation of representations) {
            for (const leftInput of leftInputs) {
                for (const rightInput of rightInputs)
                    checkEquality(leftRepresentation, leftInput, rightRepresentation, rightInput);
            }
        }
    }
}

const fiberLength = 3;

function testFiberCount(fiberCount, representations) {
    const boundary = Math.floor(fiberCount / 2) * fiberLength;
    const replacedIndices = [0, boundary - 1, boundary, boundary + 1, fiberCount * fiberLength - 1];
    for (const leftWidth of widths) {
        const leftInput = createInput(leftWidth, createFibers(leftWidth, fiberCount, fiberLength), `${fiberCount} fibers`);
        for (const rightWidth of widths) {
            const rightInput = createInput(rightWidth, createFibers(rightWidth, fiberCount, fiberLength), `${fiberCount} fibers`);
            const longerInput = createInput(rightWidth, createFibers(rightWidth, fiberCount, fiberLength + 1), `${fiberCount} longer fibers`);
            checkAllPairs(representations, [leftInput], [rightInput, longerInput, ...replacedIndices.map(index => replaceCodeUnit(rightInput, index))]);
        }
    }
}

function testSharedFibers(width) {
    const [first, second] = createFibers(width, 2, fiberLength);
    const inputs = [[first, first, first], [first, second, first], [first, first, second]].map((fibers, index) => createInput(width, fibers, `shared fibers ${index}`));
    checkAllPairs(allRepresentations, inputs, inputs);
}

function testHigherTiers(width) {
    const input = createInput(width, createFibers(width, 5, fiberLength), "repeated");
    const replacedInput = replaceCodeUnit(input, fiberLength);
    for (let i = 0; i < 1e4; ++i) {
        shouldBe(createOperand(leftLeaningRope, input) === input.text, true, input.name);
        shouldBe(createOperand(leftLeaningRope, replacedInput) === input.text, false, replacedInput.name);
    }
}

const fixedStackFiberLimit = 23;
for (const fiberCount of [5, fixedStackFiberLimit, fixedStackFiberLimit + 1, fixedStackFiberLimit + 2])
    testFiberCount(fiberCount, allRepresentations);
testFiberCount(10000, deepRepresentations);

for (const width of widths) {
    testSharedFibers(width);
    testHigherTiers(width);
}
