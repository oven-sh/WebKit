function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected} but got ${actual}`);
}

function bufferOf(target) {
    return target instanceof ArrayBuffer ? target : target.buffer;
}

// Takes over a block of this size if one was just freed.
function reuseFreedMemory(byteLength) {
    const blocks = [];
    for (let i = 0; i < 8; ++i)
        blocks.push(new Uint8Array(new ArrayBuffer(byteLength)).fill(0xee));
    return blocks;
}

const targets = {
    fast: () => new Uint8Array(100),
    oversize: () => new Uint8Array(4096),
    oversizeFloat64: () => new Float64Array(2000),
    wasteful: () => new Uint8Array(new ArrayBuffer(4096)),
    subarray: () => new Uint8Array(new ArrayBuffer(4096), 16, 100),
    dataView: () => new DataView(new ArrayBuffer(4096), 8),
    buffer: () => new ArrayBuffer(4096),
    external: () => $vm.createExternalArrayBuffer(4096),
    resizable: () => new ArrayBuffer(4096, { maxByteLength: 8192 }),
    lengthTracking: () => new Uint8Array(new ArrayBuffer(4096, { maxByteLength: 8192 })),
    lengthTrackingDataView: () => new DataView(new ArrayBuffer(4096, { maxByteLength: 8192 })),
};

// The source is detached and the new owner has the same bytes, not a copy of them.
const moves = {
    transfer: buffer => buffer.transfer(),
    structuredClone: buffer => structuredClone(buffer, { transfer: [buffer] }),
    transferTwice: buffer => buffer.transfer().transfer(),
};
for (const [targetName, create] of Object.entries(targets)) {
    for (const [moveName, move] of Object.entries(moves)) {
        const what = `${moveName} of ${targetName}`;
        const target = create();
        const { byteLength, byteOffset = 0 } = target;
        const borrow = $vm.borrowBytes(target);
        borrow("fill", 1);

        const source = bufferOf(target);
        const { resizable, byteLength: bufferByteLength } = source;
        const owner = move(source);
        shouldBe(source.detached, true, what);
        shouldBe(source.byteLength, 0, what);
        shouldBe(source.resizable, resizable, what);
        shouldBe(owner.resizable, resizable, what);
        shouldBe(owner.byteLength, bufferByteLength, what);

        const moved = new Uint8Array(owner, byteOffset, byteLength);
        shouldBe(moved.every(byte => byte === 1), true, what);
        borrow("fill", 2);
        shouldBe(moved.every(byte => byte === 2), true, what);
        moved.fill(3);
        shouldBe(borrow("count", 3), byteLength, what);

        shouldBe([target, source, owner].map($vm.isBorrowed).join(), "true,true,false", what);
        borrow("end");
        shouldBe([target, source, owner].map($vm.isBorrowed).join(), "false,false,false", what);
        reuseFreedMemory(bufferByteLength);
        shouldBe(moved.every(byte => byte === 3), true, what);
        // Not left to the collector, which could free it in the middle of one of the counts below.
        transferArrayBuffer(owner);
    }
}

// The bytes outlive their last owner.
const drops = {
    transferToFixedLength0: buffer => buffer.transferToFixedLength(0),
    transferToFixedLengthLonger: buffer => buffer.transferToFixedLength(buffer.byteLength + 1),
    transferArrayBuffer: buffer => transferArrayBuffer(buffer),
    droppedByNewOwner: buffer => transferArrayBuffer(buffer.transfer()),
};
for (const [targetName, create] of Object.entries(targets)) {
    for (const [dropName, drop] of Object.entries(drops)) {
        const what = `${dropName} of ${targetName}`;
        const target = create();
        const { byteLength } = target;
        const first = $vm.borrowBytes(target);
        const second = $vm.borrowBytes(target);
        first("fill", 1);

        const source = bufferOf(target);
        const bufferByteLength = source.byteLength;
        drop(source);
        shouldBe(source.detached, true, what);
        reuseFreedMemory(bufferByteLength);
        shouldBe(first("count", 1), byteLength, what);

        first("end");
        const blocks = reuseFreedMemory(bufferByteLength);
        shouldBe(second("count", 1), byteLength, what);
        second("fill", 2);
        shouldBe(blocks.every(block => block[0] === 0xee && block[16] === 0xee), true, what);
        second("end");
    }
}

// A buffer that gets shorter keeps the pages, whichever buffer the bytes were borrowed from.
const pageSize = 65536;
const shrinks = {
    resize: buffer => (buffer.resize(1), buffer),
    transferShorter: buffer => buffer.transfer(1),
    resizeByNewOwner(buffer) {
        const owner = buffer.transfer().transfer();
        owner.resize(pageSize + 1);
        owner.resize(1);
        return owner;
    },
};
for (const [what, shrink] of Object.entries(shrinks)) {
    const source = new ArrayBuffer(2 * pageSize + 1, { maxByteLength: 4 * pageSize });
    const borrow = $vm.borrowBytes(source);
    borrow("fill", 1);
    const owner = shrink(source);
    shouldBe(owner.byteLength, 1, what);
    shouldBe(borrow("count", 1), 2 * pageSize + 1, what);

    owner.resize(pageSize + 1);
    shouldBe(borrow("count", 0), pageSize, what);
    owner.resize(4 * pageSize);
    shouldBe(new Uint8Array(owner).lastIndexOf(1), 0, what);
    owner.resize(2);
    shouldBe($vm.mappedByteLength(owner), 4 * pageSize, what);
    borrow("end");
    owner.resize(1);
    shouldBe($vm.mappedByteLength(owner), pageSize, what);
}

// They are freed once, as soon as the owner and the borrows are gone.
function freedBy(run) {
    const before = $vm.externalArrayBufferStatistics().freed;
    run();
    return $vm.externalArrayBufferStatistics().freed - before;
}
function* permutations(items) {
    if (items.length < 2)
        return yield items;
    for (const item of items) {
        for (const rest of permutations(items.filter(other => other !== item)))
            yield [item, ...rest];
    }
}
for (const order of permutations(["a", "view", "b", "owner"])) {
    const a = $vm.createExternalArrayBuffer(64);
    const borrowOfA = $vm.borrowBytes(a);
    const borrowOfView = $vm.borrowBytes(new Uint8Array(a));
    const b = a.transfer();
    const borrowOfB = $vm.borrowBytes(b);
    const c = b.transfer();
    const release = {
        a: () => borrowOfA("end"),
        view: () => borrowOfView("end"),
        b: () => borrowOfB("end"),
        owner: () => transferArrayBuffer(c),
    };
    shouldBe(order.map(name => freedBy(release[name])).join(), "0,0,0,1", order);
}
{
    const buffer = $vm.createExternalArrayBuffer(64);
    shouldBe(freedBy(() => $vm.borrowBytes(buffer)("end")), 0, "a borrow without a detach");
    shouldBe(buffer.byteLength, 64, "a borrow without a detach");
    shouldBe(freedBy(() => transferArrayBuffer(buffer)), 1, "a detach after the borrow");
    shouldBe(freedBy(() => $vm.borrowBytes(buffer)("end")), 0, "a borrow of a detached buffer");
}
{
    const source = $vm.createExternalArrayBuffer(64);
    const borrow = $vm.borrowBytes(source);
    const owner = source.transfer();
    shouldBe(freedBy(() => $vm.borrowBytes(source)("end")), 0, "another borrow of the detached source");
    shouldBe(freedBy(() => borrow("end")), 0, "the borrow of the source");
    shouldBe(freedBy(() => $vm.borrowBytes(owner)("end")), 0, "a borrow of the new owner");
    shouldBe(freedBy(() => transferArrayBuffer(owner)), 1, "the new owner");
}
{
    const empty = $vm.createExternalArrayBuffer(0);
    const borrow = $vm.borrowBytes(empty);
    shouldBe(freedBy(() => transferArrayBuffer(empty)), 0, "an empty buffer");
    shouldBe(freedBy(() => borrow("end")), 1, "an empty buffer");
}

// That takes no more stack after many transfers than after one.
function stackUsedToFreeAfter(transfers) {
    let buffer = $vm.createExternalArrayBuffer(8);
    for (let i = 0; i < transfers; ++i) {
        const borrow = $vm.borrowBytes(buffer);
        buffer = buffer.transfer();
        borrow("end");
    }
    shouldBe(freedBy(() => transferArrayBuffer(buffer)), 1, `after ${transfers} transfers`);
    return $vm.externalArrayBufferStatistics().stackUsed;
}
{
    const more = stackUsedToFreeAfter(1000) - stackUsedToFreeAfter(1);
    shouldBe(more < 8000, true, `${more} more bytes of stack`);
}

// A borrow that never ends goes with the buffer. The stack can hold on to a few.
function abandon() {
    const buffer = $vm.createExternalArrayBuffer(64);
    $vm.borrowBytes(buffer);
    transferArrayBuffer(buffer);
}
noInline(abandon);
{
    const freed = freedBy(() => {
        for (let i = 0; i < 40; ++i)
            abandon();
        fullGC();
    });
    shouldBe(freed >= 20, true, `${freed} of 40 abandoned buffers`);
}

// A collection leaves borrowed bytes alone.
{
    const borrows = [];
    for (const create of Object.values(targets)) {
        const target = create();
        const borrow = $vm.borrowBytes(target);
        borrow("fill", 1);
        borrows.push([borrow, target.byteLength]);
        bufferOf(target).transfer();
    }
    edenGC();
    fullGC();
    reuseFreedMemory(100);
    reuseFreedMemory(4096);
    reuseFreedMemory(16000);
    for (const [borrow, byteLength] of borrows) {
        shouldBe(borrow("count", 1), byteLength, "after a collection");
        borrow("end");
    }
}

// A large view does not get an ArrayBuffer for a borrow, and hands over the ones it has when it gets one.
{
    const view = new Uint8Array(4096);
    $vm.borrowBytes(view)("end");
    shouldBe($vm.isBorrowed(view), false, "a view after its borrow");
    const ended = $vm.borrowBytes(view);
    const before = $vm.borrowBytes(view);
    ended("end");
    shouldBe($vm.isBorrowed(view), true, "a borrowed view");
    shouldBe($vm.hasArrayBuffer(view), false, "a borrowed view");

    const buffer = view.buffer;
    shouldBe($vm.hasArrayBuffer(view), true, "after reading buffer");
    shouldBe($vm.isBorrowed(buffer), true, "after reading buffer");
    const after = $vm.borrowBytes(view);
    before("fill", 1);
    transferArrayBuffer(buffer);
    after("end");
    reuseFreedMemory(4096);
    shouldBe(before("count", 1), 4096, "the borrow from before the ArrayBuffer");
    before("end");
    shouldBe($vm.isBorrowed(buffer), false, "after both borrows");
}

// Compiled code allocates small views in cells that abandoned borrows of large ones have left a count in.
{
    function create() {
        return new Uint8Array(8);
    }
    noInline(create);
    for (let i = 0; i < testLoopCount; ++i)
        create();
    fullGC();

    const neighbors = [];
    for (let i = 0; i < 100; ++i) {
        $vm.borrowBytes(new Uint8Array(1001));
        neighbors.push(create());
    }
    fullGC();
    for (let i = 0; i < 300; ++i) {
        const view = create();
        shouldBe($vm.isBorrowed(view), false, "a new view");
        shouldBe($vm.isBorrowed(view.buffer), false, "the buffer of a new view");
    }
}

// Compiled code sees the detach.
{
    const view = new Uint8Array(new ArrayBuffer(4096)).fill(1);
    function read() {
        return view[0];
    }
    noInline(read);
    const borrow = $vm.borrowBytes(view);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(read(), 1, "attached");
    view.buffer.transfer();
    shouldBe(read(), undefined, "detached");
    shouldBe(view.length, 0, "detached");
    borrow("end");
}

if (typeof SharedArrayBuffer !== "undefined") {
    const view = new Uint8Array(new SharedArrayBuffer(4096));
    const borrow = $vm.borrowBytes(view);
    borrow("fill", 1);
    borrow("end");
    shouldBe(view[4095], 1, "shared");
}
