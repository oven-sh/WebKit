//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-jit §5.5 Transition, history §48 (tenth round). A base-class
// constructor that adds many properties is reached through several subclasses,
// so its put sites are megamorphic in the profile; once it is inlined into a
// caller that knows the class being constructed, the optimizing tiers know the
// exact structure at every put and fold each one into an inline transition
// (PutByOffset + PutStructure, with an allocated or reallocated out-of-line
// storage for the puts past the inline capacity). Flag on those folds used to
// be refused outright; they are admitted now under the parser's rules - the
// structures' thread-local sets watched, a CheckTransitionOwner before the
// store, an InvalidationPoint before a (re)allocating install - so the folded
// code must (1) build correct objects on the thread that owns them, (2) be
// retired, not run, once another thread transitions objects of the same
// structures (the sets fire under a stop and jettison it), after which the
// generic path builds the objects, and (3) never lose or misplace a property
// in either phase. The test checks every property of every object built, on the
// main thread alone, then on the main thread and three others at once (which
// fires the sets mid-run), then on the main thread again.
load("../harness.js", "caller relative");

class Leg {
    constructor(fix, n) {
        this.previous = undefined; this.next = undefined; this.fix = fix; this.location = n;
        this.course = n + 1; this.distance = n + 2; this.trueAirspeed = n + 3; this.windDirection = n + 4;
        this.windSpeed = n + 5; this.heading = n + 6; this.estGS = n + 7; this.startFlightTiming = false;
        this.stopFlightTiming = true; this.engineConfig = 3; this.fuelFlow = n + 8; this.distanceRemaining = n + 9;
        this.estimatedTimeEnroute = undefined; this.estTimeRemaining = n + 10; this.estFuel = n + 11;
    }
}
class RallyLeg extends Leg { constructor(a, b) { super(a, b); this.rally = b + 12; } }
class StartLeg extends RallyLeg { }
class TimingLeg extends RallyLeg { }
class RallyLegWithoutFix extends RallyLeg { }
class TaxiLeg extends RallyLegWithoutFix { }
class RunupLeg extends RallyLegWithoutFix { }
class TakeoffLeg extends RallyLegWithoutFix { }
class ClimbLeg extends RallyLegWithoutFix { }
class PatternLeg extends RallyLegWithoutFix { }
class TurnLeg extends RallyLegWithoutFix { }
const ctors = [Leg, RallyLeg, StartLeg, TimingLeg, RallyLegWithoutFix, TaxiLeg, RunupLeg, TakeoffLeg, ClimbLeg, PatternLeg, TurnLeg];

function check(l, C, n, who) {
    const ok = l.previous === undefined && l.next === undefined && l.fix === who && l.location === n
        && l.course === n + 1 && l.distance === n + 2 && l.trueAirspeed === n + 3 && l.windDirection === n + 4
        && l.windSpeed === n + 5 && l.heading === n + 6 && l.estGS === n + 7 && l.startFlightTiming === false
        && l.stopFlightTiming === true && l.engineConfig === 3 && l.fuelFlow === n + 8 && l.distanceRemaining === n + 9
        && l.estimatedTimeEnroute === undefined && l.estTimeRemaining === n + 10 && l.estFuel === n + 11
        && (C === Leg ? !("rally" in l) : l.rally === n + 12)
        && Object.keys(l).length === (C === Leg ? 19 : 20) && l.constructor === C;
    if (!ok)
        throw new Error(who + ": bad " + C.name + " #" + n + ": " + JSON.stringify(l));
}

function build(who, count, keepEvery) {
    const kept = [];
    for (let i = 0; i < count; ++i) {
        const C = ctors[i % ctors.length];
        const l = new C(who, i);
        check(l, C, i, who);
        if (i % keepEvery === 0)
            kept.push(l);
    }
    // Re-check the kept ones after the loop (their structures may have been
    // transitioned by other threads' fires meanwhile; the objects must not change).
    for (const l of kept)
        check(l, l.constructor, l.location, who);
    return kept.length;
}

// (1) Main thread alone: the folded code runs (owner, sets valid).
build("main-1", 60000, 97);

// (2) Four threads at once: the first foreign transition of each structure fires
// its sets (jettisoning the folded code) and everyone continues on the generic
// path; a thread also adds a property of its own to some objects, transitioning
// shared structures further.
const threads = [];
for (let t = 0; t < 3; ++t) {
    threads.push(new Thread(() => {
        const who = "thread-" + t;
        let n = 0;
        for (let round = 0; round < 3; ++round)
            n += build(who, 20000, 89);
        return n;
    }));
}
build("main-2", 60000, 83);
for (const t of threads)
    t.join();

// (3) Main thread again, after the fires: recompiled code, generic transitions.
build("main-3", 30000, 79);
