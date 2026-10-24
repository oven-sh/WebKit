//@ runDefault("--verifyGC=true", "--useConcurrentGC=false")
//@ runDefault("--verifyGC=true", "--collectContinuously=true")
//@ runDefault("--verifyGC=true", "--useGenerationalGC=false", "--sweepSynchronously=true")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const objectCount = 6000;
const countBeforeStopAllocating = 1000;
const arrayLength = 160;
const stringRadix = 36;

function createObjectMaker(fieldCount) {
    const fields = [];
    for (let i = 0; i < fieldCount; ++i)
        fields.push(`field${i}: id`);
    const lastField = `field${fieldCount - 1}`;
    return {
        create: new Function("id", `return { ${fields.join(", ")} };`),
        readFirst: object => object.field0,
        readLast: object => object[lastField],
    };
}

const makers = [1, 3, 6, 9, 14, 22, 40].map(createObjectMaker);
makers.push({
    create: id => new Array(arrayLength).fill(id),
    readFirst: array => array[0],
    readLast: array => array[arrayLength - 1],
});

function readStringId(string) {
    return parseInt(string, stringRadix) - objectCount;
}

makers.push({
    create: id => (id + objectCount).toString(stringRadix),
    readFirst: readStringId,
    readLast: readStringId,
});

const retentionPatterns = [
    () => true,
    () => false,
    id => id % 2 === 0,
    id => id % 3 === 0,
    id => id % 12 < 7,
    id => id === 0,
    id => id === objectCount - 1,
];

function appendObjects(maker, objects, count) {
    for (let i = 0; i < count; ++i)
        objects.push(maker.create(objects.length));
}

function checkObjects(maker, objects) {
    for (let id = 0; id < objects.length; ++id) {
        const object = objects[id];
        if (!object)
            continue;
        shouldBe(maker.readFirst(object), id);
        shouldBe(maker.readLast(object), id);
    }
}

function testSweep(maker, isRetained) {
    const objects = [];
    appendObjects(maker, objects, objectCount);
    for (let id = 0; id < objectCount; ++id) {
        if (!isRetained(id))
            objects[id] = null;
    }
    fullGC();

    appendObjects(maker, objects, countBeforeStopAllocating);
    edenGC();
    appendObjects(maker, objects, objectCount - countBeforeStopAllocating);
    checkObjects(maker, objects);

    edenGC();
    edenGC();
    checkObjects(maker, objects);
}

for (const maker of makers) {
    for (const isRetained of retentionPatterns)
        testSweep(maker, isRetained);
}
