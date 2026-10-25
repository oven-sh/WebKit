//@ requireOptions("--useTemporal=1", "--useDollarVM=1")
// Option bags read by Temporal.Duration.prototype.round, duration-like bags (Duration.from, Duration add/subtract)
// and since/until difference options must stay observably correct after a call site has been warmed up with plain
// objects: prototype getters, shape changes, cross-realm objects, Proxies, own getters and prototype-only keys.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error(`${msg}: expected ${expected} but got ${actual}`);
}

function shouldThrow(fn, errorType, msg) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        if (!(e instanceof errorType))
            throw new Error(`${msg}: expected ${errorType.name} but got ${e}`);
    }
    if (!threw)
        throw new Error(`${msg}: expected ${errorType.name} to be thrown`);
}

const iterations = testLoopCount;

const duration = new Temporal.Duration(0, 0, 0, 0, 25, 31, 45);
const start = new Temporal.PlainDate(2020, 1, 31);
const end = new Temporal.PlainDate(2021, 3, 1);

function roundDuration(options) { return duration.round(options).toString(); }
noInline(roundDuration);
function durationFrom(bag) { return Temporal.Duration.from(bag).toString(); }
noInline(durationFrom);
function addDuration(bag) { return duration.add(bag).toString(); }
noInline(addDuration);
function subtractDuration(bag) { return duration.subtract(bag).toString(); }
noInline(subtractDuration);
function dateUntil(options) { return start.until(end, options).toString(); }
noInline(dateUntil);
function dateSince(options) { return start.since(end, options).toString(); }
noInline(dateSince);

function warmUp() {
    for (let i = 0; i < iterations; i++) {
        shouldBe(roundDuration({ smallestUnit: "hour" }), "PT26H", "warm round");
        shouldBe(roundDuration({ smallestUnit: "hour", largestUnit: "day" }), "P1DT2H", "warm round largestUnit");
        shouldBe(durationFrom({ days: 1, hours: 2 }), "P1DT2H", "warm from");
        shouldBe(addDuration({ hours: 1 }), "PT26H31M45S", "warm add");
        shouldBe(subtractDuration({ minutes: 31 }), "PT25H45S", "warm subtract");
        shouldBe(dateUntil({ largestUnit: "month" }), "P13M1D", "warm until");
        shouldBe(dateSince({ largestUnit: "month" }), "-P13M1D", "warm since");
    }
}
warmUp();

// (a) A getter for an option key installed on Object.prototype after warm-up.
{
    const log = [];
    Object.defineProperty(Object.prototype, "roundingMode", {
        configurable: true,
        get() { log.push("roundingMode"); return "floor"; }
    });
    Object.defineProperty(Object.prototype, "years", {
        configurable: true,
        get() { log.push("years"); return 2; }
    });
    for (let i = 0; i < iterations; i++) {
        log.length = 0;
        shouldBe(roundDuration({ smallestUnit: "hour" }), "PT25H", "proto roundingMode round");
        shouldBe(dateUntil({ largestUnit: "month", smallestUnit: "month" }), "P13M", "proto roundingMode until");
        shouldBe(dateSince({ largestUnit: "month", smallestUnit: "month" }), "-P14M", "proto roundingMode since");
        shouldBe(durationFrom({ days: 1 }), "P2Y1D", "proto years from");
        shouldBe(log.join(), "roundingMode,roundingMode,roundingMode,years", "proto log");
    }
    delete Object.prototype.roundingMode;
    delete Object.prototype.years;
    warmUp();
}

// (b) Shape changes: added/deleted keys and object values converted with toString/valueOf.
{
    for (let i = 0; i < iterations; i++) {
        const added = { smallestUnit: "hour" };
        added.roundingMode = "ceil";
        shouldBe(roundDuration(added), "PT26H", "added key");
        added.roundingMode = "trunc";
        shouldBe(roundDuration(added), "PT25H", "changed value");
        delete added.roundingMode;
        shouldBe(roundDuration(added), "PT26H", "deleted key");

        const deleted = { smallestUnit: "hour", largestUnit: "day" };
        delete deleted.largestUnit;
        shouldBe(roundDuration(deleted), "PT26H", "deleted largestUnit");

        const log = [];
        const unit = { toString() { log.push("toString"); return "minute"; }, valueOf() { log.push("valueOf"); return "hour"; } };
        shouldBe(roundDuration({ smallestUnit: unit }), "PT25H32M", "toString unit");
        shouldBe(dateUntil({ largestUnit: { toString() { log.push("largestUnit"); return "year"; } } }), "P1Y1M1D", "toString largestUnit");
        const increment = { toString() { log.push("inc toString"); return "1"; }, valueOf() { log.push("inc valueOf"); return 2; } };
        shouldBe(roundDuration({ smallestUnit: "hour", roundingIncrement: increment }), "PT26H", "valueOf increment");
        const hours = { valueOf() { log.push("hours valueOf"); return 3; }, toString() { log.push("hours toString"); return "4"; } };
        shouldBe(durationFrom({ hours }), "PT3H", "valueOf hours");
        shouldBe(addDuration({ hours }), "PT28H31M45S", "valueOf hours add");
        shouldBe(log.join(), "toString,largestUnit,inc valueOf,hours valueOf,hours valueOf", "conversion log");

        const bag = { days: 1, hours: 2 };
        bag.minutes = 3;
        shouldBe(durationFrom(bag), "P1DT2H3M", "added duration key");
        delete bag.days;
        shouldBe(durationFrom(bag), "PT2H3M", "deleted duration key");

        // Converting an object value runs user code that can change the keys read after it.
        const roundOptions = { smallestUnit: "day", roundingMode: "floor" };
        roundOptions.largestUnit = { toString() { roundOptions.smallestUnit = "minute"; roundOptions.roundingMode = "ceil"; roundOptions.roundingIncrement = 5; return "hour"; } };
        shouldBe(roundDuration(roundOptions), "PT25H35M", "toString changes later round options");
        const untilOptions = { smallestUnit: "day", roundingMode: "trunc" };
        untilOptions.largestUnit = { toString() { untilOptions.smallestUnit = "month"; untilOptions.roundingMode = "expand"; return "year"; } };
        shouldBe(dateUntil(untilOptions), "P1Y2M", "toString changes later until options");
        const mutatingBag = { hours: 2, seconds: 1 };
        mutatingBag.days = { valueOf() { mutatingBag.hours = 5; mutatingBag.minutes = 9; delete mutatingBag.seconds; return 1; } };
        shouldBe(durationFrom(mutatingBag), "P1DT5H9M", "valueOf changes later duration fields");
        let getterCalled = false;
        const accessorOptions = { smallestUnit: "day" };
        accessorOptions.largestUnit = { toString() { Object.defineProperty(accessorOptions, "smallestUnit", { get() { getterCalled = true; return "minute"; } }); return "hour"; } };
        shouldBe(roundDuration(accessorOptions), "PT25H32M", "toString turns a later option into a getter");
        shouldBe(getterCalled, true, "getter installed by toString called");
        const middleOptions = { largestUnit: "hour", roundingIncrement: 1, roundingMode: "floor", smallestUnit: "hour" };
        middleOptions.roundingIncrement = { valueOf() { middleOptions.roundingMode = "ceil"; middleOptions.smallestUnit = "minute"; return 5; } };
        shouldBe(roundDuration(middleOptions), "PT25H35M", "valueOf in a middle option changes later round options");
        const middleBag = { days: 1, hours: 2, minutes: 3, seconds: 4 };
        middleBag.hours = { valueOf() { middleBag.minutes = 50; delete middleBag.seconds; return 7; } };
        shouldBe(durationFrom(middleBag), "P1DT7H50M", "valueOf in a middle field changes later duration fields");
    }
    // Once only: repeatedly adding and deleting an Object.prototype key would turn it into a dictionary, which the
    // shape cache refuses, and the later sections would no longer reach the cached path.
    const protoOptions = { smallestUnit: "hour" };
    protoOptions.largestUnit = { toString() { Object.prototype.roundingMode = "floor"; return "hour"; } };
    shouldBe(roundDuration(protoOptions), "PT25H", "toString adds a later option to Object.prototype");
    delete Object.prototype.roundingMode;
    warmUp();
}

// (c) Option bags and duration-like bags from another realm.
{
    const other = createGlobalObject();
    const roundOptions = other.eval("({ smallestUnit: 'hour', roundingMode: 'floor' })");
    const untilOptions = other.eval("({ largestUnit: 'year' })");
    const bag = other.eval("({ days: 2, minutes: 5 })");
    const protoOptions = createGlobalObject().eval("Object.prototype.smallestUnit = 'minute'; ({})");
    for (let i = 0; i < iterations; i++) {
        shouldBe(roundDuration(roundOptions), "PT25H", "realm round");
        shouldBe(roundDuration(protoOptions), "PT25H32M", "realm proto round");
        shouldBe(dateUntil(untilOptions), "P1Y1M1D", "realm until");
        shouldBe(dateSince(untilOptions), "-P1Y1M1D", "realm since");
        shouldBe(durationFrom(bag), "P2DT5M", "realm from");
        shouldBe(addDuration(bag), "P3DT1H36M45S", "realm add");
        shouldBe(subtractDuration(bag), "-PT22H33M15S", "realm subtract");
    }
    shouldBe(Object.prototype.smallestUnit, undefined, "own realm unaffected");
    warmUp();
}

// (d) Proxies: exact order of get traps.
{
    function loggingProxy(target, log) {
        return new Proxy(target, {
            get(t, key, receiver) { log.push(String(key)); return Reflect.get(t, key, receiver); },
            has(t, key) { log.push("has " + String(key)); return Reflect.has(t, key); },
        });
    }
    for (let i = 0; i < iterations; i++) {
        const log = [];
        shouldBe(roundDuration(loggingProxy({ smallestUnit: "hour", largestUnit: "day", roundingMode: "floor" }, log)), "P1DT1H", "proxy round");
        shouldBe(log.join(), "largestUnit,relativeTo,roundingIncrement,roundingMode,smallestUnit", "proxy round log");

        log.length = 0;
        shouldBe(dateUntil(loggingProxy({ largestUnit: "month", smallestUnit: "day" }, log)), "P13M1D", "proxy until");
        shouldBe(log.join(), "largestUnit,roundingIncrement,roundingMode,smallestUnit", "proxy until log");

        log.length = 0;
        shouldBe(dateSince(loggingProxy({}, log)), "-P395D", "proxy since");
        shouldBe(log.join(), "largestUnit,roundingIncrement,roundingMode,smallestUnit", "proxy since log");

        log.length = 0;
        shouldBe(durationFrom(loggingProxy({ hours: 1, years: 1 }, log)), "P1YT1H", "proxy from");
        shouldBe(log.join(), "days,hours,microseconds,milliseconds,minutes,months,nanoseconds,seconds,weeks,years", "proxy from log");

        log.length = 0;
        shouldBe(addDuration(loggingProxy({ seconds: 15 }, log)), "PT25H32M", "proxy add");
        shouldBe(log.join(), "days,hours,microseconds,milliseconds,minutes,months,nanoseconds,seconds,weeks,years", "proxy add log");
    }
    warmUp();
}

// (e) Getters on the options object itself and their call order.
{
    for (let i = 0; i < iterations; i++) {
        const log = [];
        const options = {
            get smallestUnit() { log.push("get smallestUnit"); return "hour"; },
            get roundingMode() { log.push("get roundingMode"); return "ceil"; },
            get roundingIncrement() { log.push("get roundingIncrement"); return { valueOf() { log.push("valueOf roundingIncrement"); return 2; } }; },
            get relativeTo() { log.push("get relativeTo"); return undefined; },
            get largestUnit() { log.push("get largestUnit"); return { toString() { log.push("toString largestUnit"); return "hour"; } }; },
        };
        shouldBe(roundDuration(options), "PT26H", "getter round");
        shouldBe(log.join(), "get largestUnit,toString largestUnit,get relativeTo,get roundingIncrement,valueOf roundingIncrement,get roundingMode,get smallestUnit", "getter round log");

        log.length = 0;
        const diffOptions = {
            get smallestUnit() { log.push("get smallestUnit"); return "month"; },
            get roundingMode() { log.push("get roundingMode"); return "trunc"; },
            get roundingIncrement() { log.push("get roundingIncrement"); return 1; },
            get largestUnit() { log.push("get largestUnit"); return "year"; },
        };
        shouldBe(dateUntil(diffOptions), "P1Y1M", "getter until");
        shouldBe(log.join(), "get largestUnit,get roundingIncrement,get roundingMode,get smallestUnit", "getter until log");

        log.length = 0;
        const bag = {};
        for (const key of ["years", "weeks", "seconds", "nanoseconds", "months", "minutes", "milliseconds", "microseconds", "hours", "days"]) {
            Object.defineProperty(bag, key, {
                enumerable: true,
                get() { log.push("get " + key); return { valueOf() { log.push("valueOf " + key); return 1; } }; }
            });
        }
        shouldBe(durationFrom(bag), "P1Y1M1W1DT1H1M1.001001001S", "getter from");
        shouldBe(log.join(), "get days,valueOf days,get hours,valueOf hours,get microseconds,valueOf microseconds,get milliseconds,valueOf milliseconds,get minutes,valueOf minutes,get months,valueOf months,get nanoseconds,valueOf nanoseconds,get seconds,valueOf seconds,get weeks,valueOf weeks,get years,valueOf years", "getter from log");

        // An option getter that throws stops reading at that key.
        log.length = 0;
        const throwing = {
            get largestUnit() { log.push("largestUnit"); return undefined; },
            get roundingIncrement() { log.push("roundingIncrement"); throw new TypeError("boom"); },
            get smallestUnit() { log.push("smallestUnit"); return "day"; },
        };
        shouldThrow(() => dateUntil(throwing), TypeError, "throwing getter");
        shouldBe(log.join(), "largestUnit,roundingIncrement", "throwing getter log");
    }
    warmUp();
}

// (f) Keys present only on the prototype of the options object.
{
    const roundProto = { smallestUnit: "hour", roundingMode: "floor" };
    const diffProto = { largestUnit: "year" };
    const bagProto = { days: 3 };
    for (let i = 0; i < iterations; i++) {
        shouldBe(roundDuration(Object.create(roundProto)), "PT25H", "proto-only round");
        const shadowed = Object.create(roundProto);
        shadowed.roundingMode = "ceil";
        shouldBe(roundDuration(shadowed), "PT26H", "shadowed round");
        shouldBe(dateUntil(Object.create(diffProto)), "P1Y1M1D", "proto-only until");
        shouldBe(dateSince(Object.create(diffProto)), "-P1Y1M1D", "proto-only since");
        shouldBe(durationFrom(Object.create(bagProto)), "P3D", "proto-only from");
        shouldBe(addDuration(Object.create(bagProto)), "P4DT1H31M45S", "proto-only add");
    }
    warmUp();
}

// Invalid values in duration-like bags and options.
{
    for (let i = 0; i < iterations; i++) {
        shouldThrow(() => durationFrom({ hours: 1.5 }), RangeError, "non-integral hours");
        shouldThrow(() => durationFrom({ days: Infinity }), RangeError, "Infinity days");
        shouldThrow(() => durationFrom({ minutes: -Infinity }), RangeError, "-Infinity minutes");
        shouldThrow(() => durationFrom({ seconds: NaN }), RangeError, "NaN seconds");
        shouldThrow(() => addDuration({ hours: 0.25 }), RangeError, "non-integral add");
        shouldThrow(() => subtractDuration({ weeks: Infinity }), RangeError, "Infinity subtract");
        shouldThrow(() => durationFrom({ days: 1, hours: -1 }), RangeError, "mixed signs");
        shouldThrow(() => durationFrom({}), TypeError, "empty bag");
        shouldThrow(() => roundDuration({ smallestUnit: "hour", roundingIncrement: Infinity }), RangeError, "Infinity increment");
        shouldThrow(() => roundDuration({ smallestUnit: "hour", roundingIncrement: 0 }), RangeError, "zero increment");
        shouldThrow(() => roundDuration({ smallestUnit: "hour", roundingMode: "sideways" }), RangeError, "bad rounding mode");
        shouldThrow(() => roundDuration({ smallestUnit: "fortnight" }), RangeError, "bad unit");
        shouldThrow(() => roundDuration({}), RangeError, "no units");
        shouldThrow(() => dateUntil({ largestUnit: "hour" }), RangeError, "time largestUnit");
        shouldThrow(() => dateUntil({ roundingIncrement: NaN }), RangeError, "NaN increment");
        shouldBe(roundDuration({ smallestUnit: "hour", roundingIncrement: 1.9 }), "PT26H", "truncated increment");
    }
    warmUp();
}

// Primitive option values that are not strings or numbers, read on the warmed-up path.
{
    for (let i = 0; i < iterations; i++) {
        shouldThrow(() => roundDuration({ largestUnit: Symbol() }), TypeError, "Symbol largestUnit");
        shouldThrow(() => roundDuration({ smallestUnit: "hour", roundingIncrement: 1n }), TypeError, "BigInt increment");
        shouldBe(roundDuration({ smallestUnit: "hour", roundingIncrement: "2" }), "PT26H", "string increment");
        shouldThrow(() => roundDuration({ smallestUnit: "hour", roundingMode: true }), RangeError, "boolean rounding mode");
        shouldBe(durationFrom({ hours: true }), "PT1H", "boolean field");
        shouldBe(durationFrom({ hours: "3" }), "PT3H", "string field");
        shouldThrow(() => durationFrom({ hours: 1n }), TypeError, "BigInt field");
        shouldThrow(() => dateUntil({ largestUnit: null }), RangeError, "null largestUnit");
        shouldThrow(() => dateUntil({ smallestUnit: 5 }), RangeError, "number smallestUnit");
    }
    warmUp();
}

// A string relativeTo, alone and with a smallestUnit getter on Object.prototype.
{
    for (let i = 0; i < iterations; i++)
        shouldBe(roundDuration({ smallestUnit: "day", relativeTo: "2020-01-01" }), "P1D", "string relativeTo");
    const log = [];
    Object.defineProperty(Object.prototype, "smallestUnit", {
        configurable: true,
        get() { log.push("smallestUnit"); return "day"; }
    });
    shouldBe(roundDuration({ relativeTo: "2020-01-01" }), "P1D", "prototype smallestUnit");
    shouldBe(log.join(), "smallestUnit", "prototype smallestUnit read once");
    log.length = 0;
    shouldThrow(() => roundDuration({ relativeTo: "not a date" }), RangeError, "invalid relativeTo");
    shouldBe(log.join(), "", "smallestUnit not read after relativeTo throws");
    delete Object.prototype.smallestUnit;
    warmUp();
}

// Shapes that die between calls.
{
    for (let round = 0; round < 10; round++) {
        for (let i = 0; i < 100; i++) {
            const options = { smallestUnit: "hour" };
            options["key" + round] = i;
            shouldBe(roundDuration(options), "PT26H", "fresh shape");
        }
        fullGC();
        shouldBe(roundDuration({ smallestUnit: "hour", largestUnit: "day" }), "P1DT2H", "after GC");
        shouldBe(durationFrom({ days: 1, hours: 2 }), "P1DT2H", "from after GC");
    }
}

// Dictionary objects, and data properties that are not writable or not enumerable.
{
    for (let i = 0; i < 200; i++)
        Object.prototype["key" + i] = i;
    for (let i = 0; i < 200; i++)
        delete Object.prototype["key" + i];
    shouldBe(roundDuration({ smallestUnit: "hour" }), "PT26H", "dictionary Object.prototype");
    Object.prototype.largestUnit = "day";
    shouldBe(roundDuration({ smallestUnit: "hour" }), "P1DT2H", "key added to dictionary Object.prototype");
    delete Object.prototype.largestUnit;

    const options = $vm.toCacheableDictionary({ smallestUnit: "hour" });
    shouldBe(roundDuration(options), "PT26H", "dictionary options");
    options.largestUnit = "day";
    shouldBe(roundDuration(options), "P1DT2H", "key added to dictionary options");
    const log = [];
    Object.defineProperty(options, "roundingMode", {
        configurable: true,
        get() { log.push("roundingMode"); return "ceil"; }
    });
    shouldBe(roundDuration(options), "P1DT2H", "getter added to dictionary options");
    shouldBe(log.join(), "roundingMode", "getter on dictionary options called");

    const bag = $vm.toCacheableDictionary({ hours: 1 });
    shouldBe(durationFrom(bag), "PT1H", "dictionary duration-like");
    bag.minutes = 2;
    shouldBe(durationFrom(bag), "PT1H2M", "key added to dictionary duration-like");

    shouldBe(roundDuration(Object.defineProperty({}, "smallestUnit", { value: "hour" })), "PT26H", "non-writable non-enumerable option");
    shouldBe(durationFrom(Object.defineProperty({}, "hours", { value: 2 })), "PT2H", "non-enumerable field");
    warmUp();
}
