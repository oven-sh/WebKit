//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error(`${msg}: expected ${expected} but got ${actual}`);
}

// Options bags are read through a cache keyed on their structure and Object.prototype's.
const duration = new Temporal.Duration(0, 0, 0, 0, 25, 31, 45);
function round(options) { return duration.round(options).toString(); }
function from(bag) { return Temporal.Duration.from(bag).toString(); }
function until(options) { return new Temporal.PlainDate(2020, 1, 31).until(new Temporal.PlainDate(2021, 3, 1), options).toString(); }

function warm() {
    shouldBe(round({ smallestUnit: "hour" }), "PT26H", "warm round");
    shouldBe(from({ days: 1, hours: 2 }), "P1DT2H", "warm from");
    shouldBe(until({ largestUnit: "month" }), "P13M1D", "warm until");
}
warm();
warm();

// A getter for an option key installed on Object.prototype.
{
    const log = [];
    Object.defineProperty(Object.prototype, "roundingMode", { configurable: true, get() { log.push("roundingMode"); return "floor"; } });
    Object.defineProperty(Object.prototype, "years", { configurable: true, get() { log.push("years"); return 2; } });
    shouldBe(round({ smallestUnit: "hour" }), "PT25H", "Object.prototype roundingMode");
    shouldBe(from({ days: 1 }), "P2Y1D", "Object.prototype years");
    shouldBe(log.join(), "roundingMode,years", "Object.prototype getters called");
    delete Object.prototype.roundingMode;
    delete Object.prototype.years;
    warm();
}

// Keys added to and deleted from the options object, and keys only on its prototype.
{
    const options = { smallestUnit: "hour" };
    options.roundingMode = "floor";
    shouldBe(round(options), "PT25H", "added key");
    delete options.roundingMode;
    shouldBe(round(options), "PT26H", "deleted key");
    shouldBe(round({ smallestUnit: "hour", get roundingMode() { return "floor"; } }), "PT25H", "own getter");
    shouldBe(round(Object.create({ smallestUnit: "hour", roundingMode: "floor" })), "PT25H", "keys on the prototype");
    const bag = { days: 1, hours: 2 };
    bag.minutes = 3;
    shouldBe(from(bag), "P1DT2H3M", "added duration field");
}

// Converting an object value runs user code that can change the options read after it.
{
    const options = { smallestUnit: "day", roundingMode: "floor" };
    options.largestUnit = { toString() { options.smallestUnit = "minute"; options.roundingMode = "ceil"; options.roundingIncrement = 5; return "hour"; } };
    shouldBe(round(options), "PT25H35M", "toString changes later options");
    const bag = { hours: 2, seconds: 1 };
    bag.days = { valueOf() { bag.hours = 5; bag.minutes = 9; delete bag.seconds; return 1; } };
    shouldBe(from(bag), "P1DT5H9M", "valueOf changes later duration fields");
}

// Options from another realm, whose Object.prototype has a key.
{
    const options = createGlobalObject().eval("Object.prototype.smallestUnit = 'minute'; ({})");
    shouldBe(round(options), "PT25H32M", "another realm's Object.prototype");
    shouldBe(until(createGlobalObject().eval("({ largestUnit: 'year' })")), "P1Y1M1D", "another realm's options");
}

// Calls remain correct across full GC.
for (let i = 0; i < 3; i++) {
    shouldBe(round({ smallestUnit: "hour", ["key" + i]: i }), "PT26H", `fresh shape ${i}`);
    fullGC();
    shouldBe(round({ smallestUnit: "hour", largestUnit: "day" }), "P1DT2H", `after GC ${i}`);
}

// Dictionaries take the uncached path. Object.prototype stays a dictionary, so this comes last.
{
    const options = $vm.toCacheableDictionary({ smallestUnit: "hour" });
    shouldBe(round(options), "PT26H", "dictionary options");
    options.largestUnit = "day";
    shouldBe(round(options), "P1DT2H", "key added to dictionary options");
    $vm.toCacheableDictionary(Object.prototype);
    shouldBe(round({ smallestUnit: "hour" }), "PT26H", "dictionary Object.prototype");
    Object.prototype.largestUnit = "day";
    shouldBe(round({ smallestUnit: "hour" }), "P1DT2H", "key added to dictionary Object.prototype");
}
