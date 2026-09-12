//@ runDefault("--scribbleFreeCells=1", "--useZombieMode=1", "--collectContinuously=1", "--sweepSynchronously=1")

// A JSCellButterfly with no elements is a 16 byte cell, and JSCellButterfly::toButterfly() is the cell
// plus 16 bytes. So a Butterfly* to it points at the end of the cell, and it can be the only reference.
// Object.keys() and its siblings make a copy-on-write JSArray from the cached names butterfly of the
// Structure. The JIT fast paths, and CommonSlowPaths::allocateNewArrayBuffer() behind them, allocate
// that JSArray with only toButterfly() in hand, after the last use of the object. When the object was
// the only owner of its Structure, that allocation can collect with the end pointer as the only
// reference to the cached butterfly. The conservative scan has to take it for a reference to the cell.
// This is the one JSCellButterfly size that keeps the past-the-end rules of
// ConservativeRoots::genericAddPointer().
//
// Each object below has its own Structure. The third call is the cache hit. With zombie mode a
// collected butterfly reads back as garbage, or the shell crashes.

function check(array, what, i)
{
    if (!Array.isArray(array) || array.length !== 0)
        throw new Error(`${what} at ${i}: bad array length ${array && array.length}`);
}

function symbolsOfFreshObject(i)
{
    const object = { };
    object["k" + i] = i;
    Object.getOwnPropertySymbols(object);
    Object.getOwnPropertySymbols(object);
    return Object.getOwnPropertySymbols(object);
}
noInline(symbolsOfFreshObject);

function keysOfFreshObject(i)
{
    const object = Object.create({ ["p" + i]: 1 });
    Object.keys(object);
    Object.keys(object);
    return Object.keys(object);
}
noInline(keysOfFreshObject);

function ownKeysOfFreshObject(i)
{
    const object = Object.create({ ["q" + i]: 1 });
    Reflect.ownKeys(object);
    Reflect.ownKeys(object);
    return Reflect.ownKeys(object);
}
noInline(ownKeysOfFreshObject);

let kept = [];
for (let i = 0; i < 150000; ++i) {
    kept.push(symbolsOfFreshObject(i), keysOfFreshObject(i), ownKeysOfFreshObject(i));
    if (kept.length > 300) {
        for (const array of kept)
            check(array, "kept array", i);
        kept = [];
    }
}
