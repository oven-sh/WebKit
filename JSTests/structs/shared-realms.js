// Shared structs and SharedArrays read through several global objects of one VM ($.createRealm() and the jsc shell's
// createGlobalObject()). Every global object has its own wrapper per block and keeps its identities; the wrappers of
// everything that is read from an object are created in the global object of that object, so nothing is shared
// between global objects except the blocks themselves.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function deadlocks(fn) { try { fn(); } catch (e) { return e instanceof TypeError; } return false; }

shared struct Leaf { n; }
shared struct Node { value; next; mutex; }

const leaf = new Leaf();
leaf.n = 7;
const node = new Node();
node.value = "main";
node.next = leaf;
node.mutex = new Atomics.Mutex();
const array = new SharedArray(2);
array[0] = node;
array[1] = "x";

function check(name, realm) {
    const g = realm.global;
    assert(g !== globalThis && g.SharedArray !== SharedArray, name + ": a different global object");

    // The same block is another wrapper in another global object, and the same wrapper every time in it.
    const handles = sharedStructHandle(node, 2);
    const other = g.sharedStructFromHandle(handles);
    assert(g.sharedStructFromHandle(handles) === other, name + ": one wrapper per block in a global object");
    assert(other !== node, name + ": not the wrapper of the main global object");
    assert(Object.getPrototypeOf(other) === null && !Object.isExtensible(other) && Object.keys(other).join() === "value,next,mutex", name + ": still a shared struct");
    assert(other.value === "main", name + ": reads the shared value");

    // What is read from an object of a global object is made in that global object, and cached by it.
    const nextOther = other.next;
    assert(nextOther === other.next && nextOther !== leaf && nextOther.n === 7, name + ": a nested struct is the realm's wrapper");
    assert(node.next === leaf && node.next === node.next, name + ": the main global object keeps its identities");
    const read = realm.evalScript("(function (s) { return s.next; })");
    assert(read(other) === nextOther, name + ": code of the realm reading the realm's object");
    assert(read(node) === leaf, name + ": code of the realm reading a main object gets the main wrapper");
    assert(realm.evalScript("(function (o) { return Object.getPrototypeOf(o) === null; })")(nextOther), name + ": null prototype in the realm");
    assert(Atomics.load(other, "next") === nextOther && Atomics.load(other, "next") === other.next, name + ": Atomics on the realm's object");
    assert(g.Atomics.load(node, "next") === leaf, name + ": the realm's Atomics on a main object");

    // Writes are seen through every wrapper.
    other.value = "realm";
    assert(node.value === "realm" && read(node) === leaf, name + ": a write through the realm's wrapper");
    node.value = "main";
    assert(other.value === "main", name + ": a write through the main wrapper");
    let failed = false;
    try { other.value = {}; } catch (e) { failed = e.name === "TypeError"; } // made in the realm of the struct
    assert(failed && other.value === "main", name + ": a value that cannot be shared is refused");

    // Storing the realm's wrapper stores the block: it reads back as the main wrapper.
    const holder = new Node();
    holder.next = nextOther;
    assert(holder.next === leaf, name + ": a wrapper of another global object stores the block");
    assert(Reflect.canBeShared(other) && g.Reflect.canBeShared(node) && g.Reflect.canBeShared(leaf), name + ": shareable across realms");

    // A mutex is one object per state in each global object, with the realm's own prototype.
    const otherMutex = other.mutex;
    assert(otherMutex === other.mutex && otherMutex === Atomics.load(other, "mutex"), name + ": identity of a mutex in the realm");
    assert(otherMutex !== node.mutex, name + ": another object than the main mutex");
    assert(Object.getPrototypeOf(otherMutex) === null, name + ": a mutex has no prototype in any realm");
    assert(Object.getPrototypeOf(node.mutex) === null && node.mutex === node.mutex, name + ": the main mutex is unchanged");
    {
        using token = Atomics.Mutex.lock(otherMutex);
        assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(node.mutex, 0)), name + ": one state, locked through the realm's object");
    }
    const again = Atomics.Mutex.lockIfAvailable(node.mutex, 0);
    assert(again !== null, name + ": unlocked");
    again.unlock();

    // The same for a SharedArray, which can hold the struct.
    const otherArray = g.sharedStructFromHandle(sharedArrayHandle(array));
    assert(otherArray !== array && otherArray.length === 2 && otherArray[1] === "x", name + ": the array in the realm");
    assert(otherArray[0] === other && otherArray[0] === otherArray[0] && array[0] === node, name + ": an element is the realm's wrapper of the block");
    assert(Atomics.load(otherArray, "0") === other, name + ": Atomics on the realm's array");
    assert(Object.getPrototypeOf(Reflect.construct(SharedArray, [1], g.Object)) === null, name + ": SharedArray does not look at new.target");
    assert(new g.SharedArray(2).length === 2, name + ": the realm's own SharedArray");

    // Hot code that sees objects of both global objects keeps its identities after it is compiled.
    function probe(s) { return s.next; }
    for (let i = 0; i < 20000; ++i) {
        const fromMain = probe(node);
        const fromRealm = probe(other);
        if (fromMain !== leaf || fromRealm !== nextOther)
            throw new Error(name + ": identity lost in hot code at iteration " + i);
    }
}

// A unique symbol is one symbol in every global object of a VM: stored from one, it reads back as itself in another.
function checkSymbols(name, realm) {
    const g = realm.global;
    shared struct Cell { v; }
    const cell = new Cell();
    const other = g.sharedStructFromHandle(sharedStructHandle(cell, 1));
    const mine = Symbol("from main");
    cell.v = mine;
    assert(other.v === mine && other.v === other.v, name + ": a symbol stored in the main realm reads back as itself in another");
    assert(Atomics.compareExchange(cell, "v", other.v, null) === mine && cell.v === null, name + ": compareExchange with the symbol read in another realm");
    const theirs = realm.evalScript("Symbol('from the realm')");
    other.v = theirs;
    assert(cell.v === theirs && cell.v.description === "from the realm", name + ": a symbol stored in another realm reads back as itself in the main one");
    assert(g.Atomics.compareExchange(other, "v", theirs, undefined) === theirs, name + ": compareExchange in the realm");
}

// Reading a field that holds a mutex or a condition runs no user code and cannot throw, whatever the realm's Atomics is.
function checkNoUserCode(name, realm) {
    const g = realm.global;
    shared struct Holder { m; c; }
    const holder = new Holder();
    holder.m = new Atomics.Mutex();
    holder.c = new Atomics.Condition();
    let ran = false;
    Object.defineProperty(g, "Atomics", { get() { ran = true; throw new Error("user code ran"); }, configurable: true });
    const other = g.sharedStructFromHandle(sharedStructHandle(holder, 1));
    const m = other.m, c = other.c;
    assert(!ran && Object.getPrototypeOf(m) === null && Object.getPrototypeOf(c) === null && m === other.m, name + ": a read ran no user code");
}

checkSymbols("createRealm", $.createRealm());
checkNoUserCode("createRealm", $.createRealm());
check("createRealm", $.createRealm());
check("createGlobalObject", createGlobalObject().$);
