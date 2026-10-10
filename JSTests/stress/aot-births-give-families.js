//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--verifyGC=1", "--scribbleFreeCells=1", "--useZombieMode=1", "--slowPathAllocsBetweenGCs=50")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isOn = (name, otherwise) => options[name] === undefined ? otherwise : !!options[name];
const givesFamilies = isAOTCompiled(check);
const counts = isOn("useAOTOperationCounters", false);
const structureOf = object => /Structure (0x[0-9a-f]+)/.exec(describe(object))[1];
const remark = "born-in-family:";
function familyInRemarksOf(f) {
    const found = [...new Set((aotRemarks(f.name) || []).filter(text => text.startsWith(remark)))];
    check(found.length <= 1, true, f.name + " has one birth");
    return found.length ? Number(found[0].slice(remark.length)) : 0;
}
function familyBornIn(f, object, what) {
    const family = aotFamilyOf(object);
    check(family > 0, givesFamilies, what + " is born with a number");
    check(family, familyInRemarksOf(f), what + " has the number the compiler gave its birth");
    check(aotHasDepartedFamily(family), givesFamilies ? false : null, what + ": nobody has left its family");
    return family;
}
function hasNoFamily(f, object, what) {
    check(aotFamilyOf(object), 0, what + " has no number");
    check(familyInRemarksOf(f), 0, what + ": the compiler gave its birth no number");
}

function makePoint(x, y) { return { pointX: x, pointY: y, pointZ: 0 }; }
function makePointElsewhere(x) { return { pointX: x, pointY: x, pointZ: x }; }
function readPoint(point) { return point.pointX + point.pointY + point.pointZ; }

function makeWithThird(a) { const object = { sharedA: a, sharedB: a }; object.third = a; return object; }
function makeWithFourth(a) { const object = { sharedA: a, sharedB: a }; object.fourth = a; return object; }
function readWithThird(object) { return object.sharedA + object.sharedB + object.third; }
function readWithFourth(object) { return object.sharedA + object.sharedB + object.fourth; }

function makeOnly(a) { return { only: a }; }
function readOnly(object) { return object.only; }

function Particle(mass, velocity) { this.mass = mass; this.velocity = velocity; this.charge = 0; }
function readParticle(particle) { return particle.mass + particle.velocity + particle.charge; }

const familiesWhenLeaked = [];
function leak(object) { familiesWhenLeaked.push(aotFamilyOf(object), Object.keys(object).length); }
function Late(a) { this.lateA = a; this.lateB = a; leak(this); this.lateC = a; }
function readLate(object) { return object.lateA + object.lateB + object.lateC; }

function Reborn(a) { this.rebornA = a; this.rebornB = a; }
function readReborn(object) { return object.rebornA + object.rebornB; }

function makeDoomed(a) { return { doomedA: a, doomedB: a }; }
function readDoomed(object) { return object.doomedA + object.doomedB; }

function Six(a) { this.s1 = a; this.s2 = a; this.s3 = a; this.s4 = a; this.s5 = a; this.s6 = a; }
function readSix(object) { return object.s1 + object.s2 + object.s3 + object.s4 + object.s5 + object.s6; }
function SixAlone(a) { this.t1 = a; this.t2 = a; this.t3 = a; this.t4 = a; this.t5 = a; this.t6 = a; }
function readSixAlone(object) { return object.t1 + object.t2 + object.t3 + object.t4 + object.t5 + object.t6; }

function addsName(object) { object.addedLater = 1; }
function storeByKey(object, names) {
    for (const name of names)
        object[name] = 0;
    return object;
}

const familyOfPoints = familyBornIn(makePoint, makePoint(1, 2), "a literal");
check(familyBornIn(makePointElsewhere, makePointElsewhere(1), "a literal with the same names elsewhere"), familyOfPoints, "both literals are of one family");
check(structureOf(makePoint(1, 2)), structureOf(makePointElsewhere(1)), "both literals have one Structure");
check(readPoint(makePoint(1, 2)), 3, "a literal's values");

const familyWithThird = familyBornIn(makeWithThird, makeWithThird(1), "a literal that gets a third name");
const familyWithFourth = familyBornIn(makeWithFourth, makeWithFourth(1), "a literal that gets a fourth name");
check(familyWithThird !== familyWithFourth, givesFamilies, "literals that give the same names at once and go on differently are of two families");
check(readWithThird(makeWithThird(1)) + readWithFourth(makeWithFourth(2)), 9, "their values");

hasNoFamily(makeOnly, makeOnly(1), "a literal without a known shape");
check(readOnly(makeOnly(5)), 5, "its value");
check(aotFamilyOf(storeByKey({ }, ["pointX", "pointY", "pointZ"])), 0, "an object that got a family's names by key has no number");

const keptParticle = new Particle(1, 2);
const familyOfParticles = familyBornIn(Particle, keptParticle, "an instance");
check(familyOfParticles !== familyOfPoints, givesFamilies, "instances and literals are of two families");
check(readParticle(new Particle(1, 2)), 3, "an instance's values");

const familyOfLate = familyBornIn(Late, new Late(1), "an instance that gets a name behind a call");
check(familiesWhenLeaked[0], familyOfLate, "it has the number as soon as it can be seen");
check(familiesWhenLeaked[1], 2, "it is seen with the names given at once");
check(readLate(new Late(2)), 6, "its values");

const familyOfReborn = familyBornIn(Reborn, new Reborn(1), "an instance whose Structure may die");
for (let i = 0; i < 3; ++i) {
    fullGC();
    check(aotFamilyOf(new Reborn(i)), familyOfReborn, "an instance made after a collection that found no other");
    check(readReborn(new Reborn(i)), 2 * i, "its values");
}

for (const grown of [makePoint(1, 2), new Particle(1, 2)]) {
    const family = aotFamilyOf(grown);
    addsName(grown);
    check(aotFamilyOf(grown), family, "a member that adds a name keeps the number");
    check(aotHasDepartedFamily(family), givesFamilies ? false : null, "adding a name is no departure");
}

const doomed = makeDoomed(1);
const familyOfDoomed = familyBornIn(makeDoomed, doomed, "a literal that will lose a name");
check(readDoomed(doomed), 2, "its values");
delete doomed.doomedA;
check(aotFamilyOf(doomed), 0, "a member that deletes a name loses the number");
check(aotHasDepartedFamily(familyOfDoomed), givesFamilies ? true : null, "and its family knows");
check(aotFamilyOf(makeDoomed(2)), familyOfDoomed, "later births still get the number");
check(aotHasDepartedFamily(familyOfPoints), givesFamilies ? false : null, "no other family is touched");

const early = storeByKey(Object.create(Six.prototype), ["s1", "s2", "s3", "s4", "s5", "s6"]);
const structureBeforeChild = structureOf(early);
early.more = 1;
const six = new Six(1);
check(structureOf(six), structureBeforeChild, "generic code reached the Structure of the instances first");
check(familyInRemarksOf(Six) > 0, givesFamilies, "the compiler gave the construction a number");
check(aotFamilyOf(six), 0, "a Structure that has had a child is refused");
check(readSix(six), 6, "its values");
const familyOfSixAlone = familyBornIn(SixAlone, new SixAlone(1), "an instance of the same form whose Structure nobody reached first");
check(readSixAlone(new SixAlone(1)), 6, "its values");

const rounds = 3000;
for (let i = 0; i < rounds; ++i) {
    check(aotFamilyOf(makePoint(i, i)), familyOfPoints, "every literal");
    check(aotFamilyOf(new Particle(i, i)), familyOfParticles, "every instance");
    check(aotFamilyOf(new Six(i)), 0, "every instance of the refused Structure");
    check(readPoint(makePointElsewhere(i)) + readParticle(new Particle(i, i)), 5 * i, "values");
}

if (counts && givesFamilies) {
    const count = (name, family, detail) => aotOperationCount(name + ":" + family + ":" + detail);
    check(count("Family::given-to", familyOfPoints, "literal"), 1, "one Structure got the number of the points");
    check(count("Family::given-to", familyWithThird, "literal") + count("Family::given-to", familyWithFourth, "literal"), 2, "one Structure each for the two that go on differently");
    check(count("Family::given-to", familyOfParticles, "construction"), 1, "one Structure got the number of the particles");
    check(aotFamilyOf(keptParticle), familyOfParticles, "and the first of them still has it");
    check(count("Family::given-to", familyInRemarksOf(Six), "construction"), 0, "none got the number of the refused");
    check(count("Family::refused-to", familyInRemarksOf(Six), "has-had-child") > 0, true, "the refusal is counted with its reason");
    check(aotOperationCount("Family::refused:has-had-child"), count("Family::refused-to", familyInRemarksOf(Six), "has-had-child"), "nobody else was refused for it");
    check(count("Family::refused-to", familyOfSixAlone, "has-had-child"), 0, "the other was not refused");
    check(count("Family::born-in", familyOfPoints, "with-number") >= rounds, true, "births of points are counted");
    check(count("Family::born-in", familyOfPoints, "without-number"), 0, "all of them with the number");
    check(count("Family::born-in", familyOfParticles, "with-number") >= rounds, true, "births of particles are counted");
    check(count("Family::born-in", familyInRemarksOf(Six), "without-number") >= rounds, true, "births without the number are counted");
    check(count("Family::born-in", familyInRemarksOf(Six), "with-number"), 0, "none of them with it");
    check(count("Family::departed-from", familyOfDoomed, "deletion"), 1, "the departure is counted");
}
