//@ runDefault
//@ runDefault("--useJIT=0")
//@ runDefault("--useJIT=0", "--collectContinuously=1")
//@ runDefault("--useJIT=0", "--useLLIntUnsetCaching=0", "--useLLIntPrototypeCacheRearming=0", "--useLLIntStringLengthFastPath=0", "--missCountForLLIntTierUp=0")
//@ runDefault("--useConcurrentJIT=0", "--missCountForLLIntTierUp=2", "--thresholdForJITSoon=10")

// Random reads of properties through get_by_id sites, between random changes to the objects and to their
// prototype chains. Each read is compared with a walk of the chain that uses no get_by_id site for the name.
// What a site has cached (an own property, a value of the chain, or that there is no property) must never
// change what the read returns.

let seed = 0x2f6e2b1;
function random(n) {
    seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff;
    return (seed >>> 8) % n;
}

const names = ["alpha", "beta", "gamma", "delta", "length", "epsilon"];

// Several sites for each name. Functions of the same source share their code, so each has its own source.
const sites = [];
for (const name of names) {
    for (let i = 0; i < 4; ++i)
        sites.push({ name, get: new Function("o", "return o." + name + "; // " + sites.length) });
}

function reference(receiver, name) {
    let object = Object(receiver);
    while (object !== null) {
        const descriptor = Object.getOwnPropertyDescriptor(object, name);
        if (descriptor) {
            if ("value" in descriptor)
                return descriptor.value;
            return descriptor.get ? descriptor.get.call(receiver) : undefined;
        }
        object = Object.getPrototypeOf(object);
    }
    return undefined;
}

function isOnChainOf(object, candidate) {
    for (let o = candidate; o !== null; o = Object.getPrototypeOf(o)) {
        if (o === object)
            return true;
    }
    return false;
}

let made = 0;
function makeObject() {
    let object;
    switch (random(6)) {
    case 0:
        object = { };
        break;
    case 1:
        object = Object.create(null);
        break;
    case 2:
        object = [made, made + 1];
        break;
    case 3:
        object = function () { };
        break;
    case 4:
        object = new (class Made { constructor() { this.own = made; } });
        break;
    default:
        object = { made };
        break;
    }
    made++;
    return object;
}

const pool = [];
for (let i = 0; i < 24; ++i)
    pool.push(makeObject());
const primitives = ["text", "a longer text " + made, 7, 1.5, true, Symbol("s"), 10n, "ro" + "pe".repeat(20)];
const shared = [Object.prototype, Array.prototype, Function.prototype, String.prototype, Number.prototype];
const hasVM = typeof $vm === "object";

function change(step) {
    const object = pool[random(pool.length)];
    const name = names[random(names.length)];
    switch (random(12)) {
    case 0:
    case 1:
    case 2:
        if (name !== "length" || !(Array.isArray(object) || typeof object === "function"))
            object[name] = step;
        break;
    case 3:
    case 4:
        delete object[name];
        break;
    case 5: {
        const proto = random(4) ? pool[random(pool.length)] : null;
        if (proto === null || !isOnChainOf(object, proto)) {
            if (Object.isExtensible(object))
                Object.setPrototypeOf(object, proto);
        }
        break;
    }
    case 6: {
        const descriptor = Object.getOwnPropertyDescriptor(object, name);
        if (!descriptor || descriptor.configurable)
            Object.defineProperty(object, name, { get() { return "getter " + name; }, configurable: true, enumerable: true });
        break;
    }
    case 7: {
        // A property of a prototype that many objects share, and away with it a few steps later.
        const target = shared[random(shared.length)];
        if (name !== "length" && !Object.getOwnPropertyDescriptor(target, name))
            Object.defineProperty(target, name, { value: "shared " + step, configurable: true, writable: true, enumerable: false });
        break;
    }
    case 8:
        for (const target of shared) {
            if (name !== "length")
                delete target[name];
        }
        break;
    case 9:
        pool[random(pool.length)] = makeObject();
        break;
    case 10:
        if (hasVM && !random(4)) {
            if (random(2))
                $vm.toCacheableDictionary(object);
            else
                $vm.toUncacheableDictionary(object);
        }
        break;
    default:
        if (!random(40))
            gc();
        break;
    }
}

const steps = 60000;
for (let step = 0; step < steps; ++step) {
    if (!random(5))
        change(step);
    const site = sites[random(sites.length)];
    const receiver = random(8) ? pool[random(pool.length)] : primitives[random(primitives.length)];
    // Runs of the same receiver, so that the caches get made and used.
    const repeats = 1 + random(4);
    for (let i = 0; i < repeats; ++i) {
        const expected = reference(receiver, site.name);
        const actual = site.get(receiver);
        if (actual !== expected)
            throw new Error("step " + step + ", repeat " + i + ": ." + site.name + " of " + (typeof receiver) + " returned " + String(actual) + ", expected " + String(expected));
    }
}

for (const target of shared) {
    for (const name of names) {
        if (name !== "length")
            delete target[name];
    }
}
