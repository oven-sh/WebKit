//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
function thrownBy(f) {
    try {
        f();
    } catch (error) {
        return error.constructor.name + ": " + error.message;
    }
    return "nothing";
}
const folds = "folds-property-of-constant-object", readsSlot = "reads-slot-of-constant-object", isAbsent = "absent-property-is-undefined";
const doesNotAllocate = "does-not-allocate-constant-object";
function identity(x) { return x; }
noInline(identity);

function constants() {
    function made(n) { return { n }; }
    const Kind = { User: "user", Tool: "tool", Count: 2, Half: 0.5, Enabled: true, Nothing: null, Missing: undefined };
    const Made = { first: made(1), second: made(2), third: 3 };
    const Big = { p0: made(0), p1: made(1), p2: made(2), p3: made(3), p4: made(4), p5: made(5), p6: made(6), p7: made(7), p8: made(8), p9: made(9), p10: made(10), p11: made(11), p12: made(12), p13: made(13), p14: made(14), p15: made(15), p16: made(16), p17: made(17), p18: made(18), p19: made(19), p20: made(20), p21: made(21), p22: made(22), p23: made(23), p24: made(24), p25: made(25), p26: made(26), p27: made(27), p28: made(28), p29: made(29), p30: made(30), p31: made(31), p32: made(32), p33: made(33), p34: made(34), p35: made(35), p36: made(36), p37: made(37), p38: made(38), p39: made(39), p40: made(40), p41: made(41), p42: made(42), p43: made(43), p44: made(44), p45: made(45), p46: made(46), p47: made(47), p48: made(48), p49: made(49), p50: made(50), p51: made(51), p52: made(52), p53: made(53), p54: made(54), p55: made(55), p56: made(56), p57: made(57), p58: made(58), p59: made(59), p60: made(60), p61: made(61), p62: made(62), p63: made(63), p64: made(64), p65: made(65), p66: made(66), p67: made(67), p68: made(68), p69: made(69), p70: made(70), p71: made(71), p72: made(72), p73: made(73), p74: made(74), p75: made(75), p76: made(76), p77: made(77), p78: made(78), p79: made(79) };
    function readsString() { return Kind.User; }
    function readsNumber() { return Kind.Count + 1; }
    function readsDouble() { return Kind.Half; }
    function readsBoolean() { return Kind.Enabled; }
    function readsNull() { return Kind.Nothing; }
    function readsUndefined() { return Kind.Missing; }
    function comparesWithString(x) { return x === Kind.Tool ? "tool" : "other"; }
    function masksWithNumber(x) { return x & Kind.Count; }
    function readsAbsentName() { return Kind.absent; }
    function readsInheritedName() { return typeof Kind.toString; }
    function readsSlotInObject() { return Made.second.n; }
    function readsConstantBesideSlots() { return Made.third; }
    function readsSlotOutOfLine() { return Big.p0.n + Big.p63.n + Big.p64.n + Big.p79.n; }
    function testsForNull() { return Kind == null ? "none" : Kind.User; }
    return [readsString, readsNumber, readsDouble, readsBoolean, readsNull, readsUndefined, comparesWithString, masksWithNumber, readsAbsentName, readsInheritedName, readsSlotInObject, readsConstantBesideSlots, readsSlotOutOfLine, testsForNull];
}
{
    const [readsString, readsNumber, readsDouble, readsBoolean, readsNull, readsUndefined, comparesWithString, masksWithNumber, readsAbsentName, readsInheritedName, readsSlotInObject, readsConstantBesideSlots, readsSlotOutOfLine, testsForNull] = constants().map(f => (noInline(f), f));
    for (const f of [readsString, readsNumber, readsDouble, readsBoolean, readsNull, readsUndefined, comparesWithString, masksWithNumber, readsConstantBesideSlots, testsForNull]) {
        applies(f, folds);
        doesNotApply(f, readsSlot);
    }
    applies(readsAbsentName, isAbsent);
    doesNotApply(readsInheritedName, folds, readsSlot, isAbsent);
    applies(readsSlotInObject, readsSlot);
    applies(readsSlotOutOfLine, readsSlot);
    doesNotApply(readsSlotOutOfLine, folds);
    applies(masksWithNumber, "inline-bit-operation");
    for (let i = 0; i < 100; i++) {
        check(readsString(), "user", "a string");
        check(readsNumber(), 3, "a number");
        check(readsDouble(), 0.5, "a double");
        check(readsBoolean(), true, "a boolean");
        check(readsNull(), null, "null");
        check(readsUndefined(), undefined, "undefined");
        check(comparesWithString("tool"), "tool", "a comparison that holds");
        check(comparesWithString("to" + "ol".slice(0, 2)), "tool", "a comparison with a string that was made");
        check(comparesWithString("user"), "other", "a comparison that does not hold");
        check(masksWithNumber(i), i & 2, "a mask");
        check(readsAbsentName(), undefined, "a name that is not there");
        check(readsInheritedName(), "function", "a name that is inherited");
        check(readsSlotInObject(), 2, "a value that is made when the program runs");
        check(readsConstantBesideSlots(), 3, "a constant beside such values");
        check(readsSlotOutOfLine(), 0 + 63 + 64 + 79, "slots in and outside the object");
        check(testsForNull(), "user", "a test for null");
    }
}

function neverAllocated(log) {
    function noted(x) { log.push(x); return x; }
    const AllLiterals = { a: 1, b: "two", c: null };
    const OnlyLiteralsAreRead = { a: 1, unread: noted("evaluated"), b: 2 };
    const AbsentNameIsRead = { a: 1, b: 2 };
    const NeedsSlot = { a: 1, made: noted("made") };
    const InheritedNameIsRead = { a: 1, b: 2 };
    function readsAllLiterals() { return AllLiterals.a + AllLiterals.b + AllLiterals.c; }
    function readsOnlyLiterals() { return OnlyLiteralsAreRead.a + OnlyLiteralsAreRead.b; }
    function readsAbsentNameOfOther() { return AbsentNameIsRead.nothing === undefined ? AbsentNameIsRead.a : 0; }
    function readsSlotToo() { return NeedsSlot.a + NeedsSlot.made; }
    function readsInheritedNameOfOther() { return InheritedNameIsRead.a + typeof InheritedNameIsRead.hasOwnProperty; }
    function testsForNullOfOther() { return AllLiterals == null ? "none" : "some"; }
    return [readsAllLiterals, readsOnlyLiterals, readsAbsentNameOfOther, readsSlotToo, readsInheritedNameOfOther, testsForNullOfOther];
}
{
    const log = [];
    const [readsAllLiterals, readsOnlyLiterals, readsAbsentNameOfOther, readsSlotToo, readsInheritedNameOfOther, testsForNullOfOther] = neverAllocated(log).map(f => (noInline(f), f));
    for (const name of ["AllLiterals", "OnlyLiteralsAreRead", "AbsentNameIsRead"])
        applies(neverAllocated, doesNotAllocate + ":" + name);
    for (const name of ["NeedsSlot", "InheritedNameIsRead"])
        doesNotApply(neverAllocated, doesNotAllocate + ":" + name);
    check(log.join(), "evaluated,made", "what is in a literal is evaluated though the object is not made");
    for (let i = 0; i < 100; i++) {
        check(readsAllLiterals(), "1twonull", "an object of literals");
        check(readsOnlyLiterals(), 3, "an object of which only literals are read");
        check(readsAbsentNameOfOther(), 1, "an object of which a name that is not there is read");
        check(readsSlotToo(), "1made", "an object of which a value that was made is read");
        check(readsInheritedNameOfOther(), "1function", "an object of which an inherited name is read");
        check(testsForNullOfOther(), "some", "a test for null of an object that is not made");
    }
}

function onlyRead() {
    const Indexed = { a: 1, b: 2 };
    function indexes(k) { return Indexed[k]; }
    function readsIndexed() { return Indexed.a; }
    const Tested = { a: 1, b: 2 };
    function tests(k) { return (k in Tested) + ("a" in Tested); }
    function readsTested() { return Tested.a; }
    const Spread = { a: 1, b: 2 };
    function spreads() { return { ...Spread, c: 3 }; }
    function readsSpread() { return Spread.a; }
    const Enumerated = { a: 1, b: 2 };
    function enumerates() { let all = ""; for (const k in Enumerated) all += k + Enumerated[k]; return all; }
    function readsEnumerated() { return Enumerated.a; }
    const Listed = { a: 1, b: 2 };
    function lists() { return Object.keys(Listed).join() + Object.values(Listed).join() + Object.entries(Listed).join() + Object.hasOwn(Listed, "a") + Object.getOwnPropertyNames(Listed).length; }
    function readsListed() { return Listed.a; }
    const Compared = { a: 1, b: 2 };
    function compares(x) { return x === Compared || x !== Compared && typeof Compared; }
    function readsCompared() { return Compared.a; }
    const Truthy = { a: 1, b: 2 };
    function isTruthy() { return Truthy ? !Truthy : "falsy"; }
    function readsTruthy() { return Truthy.a; }
    return { indexes, readsIndexed, tests, readsTested, spreads, readsSpread, enumerates, readsEnumerated, lists, readsListed, compares, readsCompared, isTruthy, readsTruthy };
}
{
    const all = onlyRead();
    for (const name in all)
        noInline(all[name]);
    for (const name of ["readsIndexed", "readsTested", "readsSpread", "readsEnumerated", "readsListed", "readsCompared", "readsTruthy"])
        applies(all[name], folds);
    for (const name of ["Indexed", "Tested", "Spread", "Enumerated", "Listed", "Compared"])
        doesNotApply(onlyRead, doesNotAllocate + ":" + name);
    applies(onlyRead, doesNotAllocate + ":Truthy");
    for (let i = 0; i < 100; i++) {
        check(all.readsIndexed() + all.indexes("b"), 3, "an object that is indexed");
        check(all.indexes("missing"), undefined, "with a name that is not there");
        check(typeof all.indexes("toString"), "function", "or is inherited");
        check(all.tests("a") + all.readsTested(), 3, "an object that is tested with in");
        check(all.readsSpread() + all.spreads().b + all.spreads().c, 6, "an object that is spread");
        check(all.enumerates() + all.readsEnumerated(), "a1b21", "an object that is enumerated");
        check(all.lists() + all.readsListed(), "a,b1,2a,1,b,2true21", "an object whose names and values are listed");
        check(all.compares(1) + all.readsCompared(), "object1", "an object that is compared");
        check(all.isTruthy(), false, "an object that is only tested, and is not made");
        check(all.readsTruthy(), 1, "and read");
    }
    all.spreads().a = 9;
    check(all.readsSpread(), 1, "a copy that is changed is not the object");
}

function storedAfterBranch(flag) {
    const Branchy = { a: 1, chosen: flag ? "yes" : "no", b: "two", either: flag || 5 };
    const BranchyButUnread = { a: 1, chosen: flag ? "yes" : "no", b: 2 };
    function readsBeforeBranch() { return Branchy.a; }
    function readsAfterBranch() { return Branchy.b; }
    function readsChosen() { return Branchy.chosen + Branchy.either; }
    function readsLiteralsOnly() { return BranchyButUnread.a + BranchyButUnread.b; }
    return [readsBeforeBranch, readsAfterBranch, readsChosen, readsLiteralsOnly];
}
{
    const [readsBeforeBranch, readsAfterBranch, readsChosen, readsLiteralsOnly] = storedAfterBranch(false).map(f => (noInline(f), f));
    applies(readsBeforeBranch, folds);
    applies(readsAfterBranch, folds);
    applies(readsChosen, readsSlot);
    doesNotApply(readsChosen, folds);
    applies(readsLiteralsOnly, folds);
    applies(storedAfterBranch, doesNotAllocate + ":BranchyButUnread");
    doesNotApply(storedAfterBranch, doesNotAllocate + ":Branchy");
    const chosenOtherwise = storedAfterBranch(true)[2];
    for (let i = 0; i < 100; i++) {
        check(readsBeforeBranch(), 1, "a property before one whose value is chosen");
        check(readsAfterBranch(), "two", "a property after it");
        check(readsChosen(), "no5", "the chosen values");
        check(chosenOtherwise(), "yestrue", "the other values");
        check(readsLiteralsOnly(), 3, "an object with a chosen value that nothing reads");
    }
}

function sameNameElsewhere() {
    const shared = { a: 1, b: 2 };
    function readsShared() { return shared.a; }
    return readsShared;
}
function looksUpSameName(text) {
    const shared = { a: 3, b: 4 };
    function readsOtherShared() { return shared.a; }
    noInline(readsOtherShared);
    eval(text);
    return readsOtherShared;
}
{
    const readsShared = sameNameElsewhere(), readsOtherShared = looksUpSameName("shared.a = 30");
    noInline(readsShared);
    applies(readsShared, folds);
    doesNotApply(readsOtherShared, folds, readsSlot);
    check(readsShared(), 1, "a variable with the name of one that evaluated code can reach");
    check(readsOtherShared(), 30, "the one that it can reach");
}

async function keptAcrossAwait(x) {
    const made = { a: x }, literals = { a: 1, b: "two" };
    await null;
    return made.a + literals.a + literals.b;
}
function* keptAcrossYield(x) {
    const made = { a: x }, literals = { a: 1, b: "two" };
    yield made.a;
    yield made.a + literals.a + literals.b;
}
async function* keptAcrossBoth(x) {
    const made = { a: x, b: 2 };
    await null;
    yield made.a;
    yield made.b;
}
{
    doesNotApply(keptAcrossAwait, doesNotAllocate);
    doesNotApply(keptAcrossYield, doesNotAllocate);
    doesNotApply(keptAcrossBoth, doesNotAllocate);
    let awaited, both = [];
    keptAcrossAwait(32).then(value => { awaited = value; });
    (async () => { for await (const value of keptAcrossBoth(5)) both.push(value); })();
    drainMicrotasks();
    check(awaited, "33two", "objects that are kept across an await");
    check(both.join(), "5,2", "an object that is kept across an await and a yield");
    check([...keptAcrossYield(7)].join(), "7,8two", "objects that are kept across a yield");
}

function registersInFrame() {
    function made(n) { return { n }; }
    const list = [made(0), made(1), made(2), made(3), made(4), made(5), made(6), made(7), made(8), made(9), made(10), made(11), made(12), made(13), made(14), made(15), made(16), made(17), made(18), made(19), made(20), made(21), made(22), made(23), made(24), made(25), made(26), made(27), made(28), made(29), made(30), made(31), made(32), made(33), made(34), made(35), made(36), made(37), made(38), made(39)];
    const Framed = { a: 1, b: made(2) };
    const FramedLiterals = { a: 5, b: "six" };
    function readsFramed() { return Framed.a + Framed.b.n + FramedLiterals.a + FramedLiterals.b; }
    return [readsFramed, list];
}
function readByHandler(fails) {
    var local, Handled;
    function readsHandled() { return Handled.a; }
    noInline(readsHandled);
    try {
        local = { a: 1, b: 2 };
        Handled = local;
        if (fails)
            throw new Error("fails");
    } catch {
        local.a = 7;
    }
    return readsHandled;
}
{
    const [readsFramed, list] = registersInFrame();
    noInline(readsFramed);
    applies(readsFramed, folds, readsSlot);
    applies(registersInFrame, doesNotAllocate + ":FramedLiterals");
    doesNotApply(registersInFrame, doesNotAllocate + ":Framed");
    check(list.length, 40, "an array whose elements are read from the frame");
    const readsHandled = readByHandler(true);
    doesNotApply(readsHandled, folds, readsSlot);
    for (let i = 0; i < 100; i++) {
        check(readsFramed(), "8six", "objects that are made in registers that are kept in the frame");
        check(readsHandled(), 7, "an object that a handler reads from the frame and changes");
        check(readByHandler(false)(), 1, "and one that it does not");
    }
}

function namedLikeOneThatIsAwaited() {
    const sameName = { a: 1, b: 2 };
    function readsSameName() { return sameName.a; }
    return readsSameName;
}
async function awaitsOneOfThatName(x) {
    let sameName = { a: x, b: 2 };
    const reads = () => sameName;
    await null;
    reads().a++;
    return sameName.a;
}
function readInFunctionWithHandler() {
    const Tried = { a: 1, b: identity(2) };
    function readsTried(fails) {
        let kept = 0;
        try {
            kept = Tried.a + Tried.b;
            if (fails)
                throw new Error("fails");
            kept += Tried.a;
        } catch {
            kept += 10 + Tried.a;
        }
        return kept;
    }
    return readsTried;
}
{
    const readsSameName = namedLikeOneThatIsAwaited(), readsTried = readInFunctionWithHandler();
    noInline(readsSameName);
    noInline(readsTried);
    applies(readsSameName, folds);
    doesNotApply(awaitsOneOfThatName, folds, readsSlot, doesNotAllocate);
    applies(readsTried, folds, readsSlot);
    let awaited;
    awaitsOneOfThatName(5).then(value => { awaited = value; });
    drainMicrotasks();
    check(awaited, 6, "a variable that is read after an await, through a scope that cannot be told");
    for (let i = 0; i < 100; i++) {
        check(readsSameName(), 1, "a variable of the same name elsewhere");
        check(readsTried(false), 4, "a constant object that is read where there is a handler");
        check(readsTried(true), 14, "and in the handler");
    }
}

function perInstantiation(n) {
    function made(x) { return { x }; }
    const Own = { tag: "same", value: made(n) };
    function readsOwn() { return Own.tag + Own.value.x; }
    return readsOwn;
}
{
    const first = perInstantiation(1), second = perInstantiation(2);
    noInline(first);
    applies(first, folds, readsSlot);
    for (let i = 0; i < 100; i++) {
        check(first(), "same1", "the object of the first instantiation");
        check(second(), "same2", "the object of the second");
    }
}

function disqualified() {
    const Passed = { a: 1, b: 2 };
    function passes() { return identity(Passed); }
    function readsPassed() { return Passed.a; }
    const Written = { a: 1, b: 2 };
    function writes(v) { Written.a = v; }
    function readsWritten() { return Written.a; }
    const Called = { a: 1, f() { return this; } };
    function calls() { return Called.f(); }
    function readsCalled() { return Called.a; }
    let Reassigned = { a: 1, b: 2 };
    function reassigns() { Reassigned = { a: 3, b: 4 }; }
    function readsReassigned() { return Reassigned.a; }
    const Deleted = { a: 1, b: 2 };
    function deletes() { delete Deleted.a; }
    function readsDeleted() { return Deleted.a; }
    const Returned = { a: 1, b: 2 };
    function returns() { return Returned; }
    function readsReturned() { return Returned.a; }
    const Copied = { a: 1, b: 2 };
    let copy;
    function copies() { copy = Copied; return copy; }
    function readsCopied() { return Copied.a; }
    const Merged = { a: 1, b: 2 }, Other = { a: 5, b: 6 };
    function merges(which) { return (which ? Merged : Other).a; }
    function readsMerged() { return Merged.a; }
    const Computed = { a: 1, ["b" + 1]: 2 };
    function readsComputed() { return Computed.a; }
    const WithGetter = { a: 1, get b() { return this; } };
    function readsWithGetter() { return WithGetter.a; }
    const Defined = { a: 1, b: 2 };
    function defines() { Object.defineProperty(Defined, "a", { value: 10 }); }
    function readsDefined() { return Defined.a; }
    const DefinedSeveral = { a: 1, b: 2 };
    function definesSeveral() { Object.defineProperties(DefinedSeveral, { a: { get() { return 11; } } }); }
    function readsDefinedSeveral() { return DefinedSeveral.a; }
    const DefinedInCallee = { a: 1, b: 2 };
    function foo(a, props) { Object.defineProperties(a, props); }
    function definesInCallee() { foo(DefinedInCallee, { a: { value: 12 } }); }
    function readsDefinedInCallee() { return DefinedInCallee.a; }
    const Assigned = { a: 1, b: 2 };
    function assigns() { Object.assign(Assigned, { a: 13 }); }
    function readsAssigned() { return Assigned.a; }
    const Reflected = { a: 1, b: 2 };
    function reflects() { Reflect.set(Reflected, "a", 14); }
    function readsReflected() { return Reflected.a; }
    const WithNewPrototype = { a: 1, b: 2 };
    function setsPrototype() { Object.setPrototypeOf(WithNewPrototype, { inherited: 15 }); }
    function readsWithNewPrototype() { return WithNewPrototype.inherited; }
    const WithAssignedProto = { a: 1, b: 2 };
    function assignsProto() { WithAssignedProto.__proto__ = { inherited: 16 }; }
    function readsWithAssignedProto() { return WithAssignedProto.inherited; }
    const Frozen = { a: 1, b: 2 };
    function freezes() { return Object.freeze(Frozen); }
    function readsFrozen() { return Frozen.a; }
    const UsedAsKey = { a: 1, b: 2 };
    function indexesWith(o) { return o[UsedAsKey]; }
    function readsUsedAsKey() { return UsedAsKey.a; }
    const WrittenIndexed = { a: 1, b: 2 };
    function writesIndexed(k, v) { WrittenIndexed[k] = v; }
    function readsWrittenIndexed() { return WrittenIndexed.a; }
    const WithProtoInLiteral = { __proto__: { inherited: 17 }, a: 1, b: 2 };
    function readsWithProtoInLiteral() { return WithProtoInLiteral.a; }
    function readsInheritedFromLiteral() { return WithProtoInLiteral.inherited; }
    let givenAway;
    const Coerced = { a: 1, toString() { givenAway = this; return "a"; } };
    function coerces(o) { return Object.hasOwn(o, Coerced); }
    function changesCoerced() { givenAway.a = 21; }
    function readsCoerced() { return Coerced.a; }
    const NotLiteral = identity({ a: 1, b: 2 });
    function readsNotLiteral() { return NotLiteral.a; }
    return { passes, readsPassed, writes, readsWritten, calls, readsCalled, reassigns, readsReassigned, deletes, readsDeleted, returns, readsReturned, copies, readsCopied,
        defines, readsDefined, definesSeveral, readsDefinedSeveral, definesInCallee, readsDefinedInCallee, assigns, readsAssigned, reflects, readsReflected, setsPrototype, readsWithNewPrototype,
        assignsProto, readsWithAssignedProto, coerces, changesCoerced, readsCoerced, freezes, readsFrozen, indexesWith, readsUsedAsKey, writesIndexed, readsWrittenIndexed, readsWithProtoInLiteral, readsInheritedFromLiteral, merges, readsMerged, readsComputed, readsWithGetter, readsNotLiteral };
}
{
    const all = disqualified();
    for (const name in all) {
        noInline(all[name]);
        doesNotApply(all[name], folds, readsSlot, isAbsent);
    }
    for (let i = 0; i < 100; i++) {
        check(all.readsPassed(), 1, "an object that is passed");
        check(all.readsCalled(), 1, "an object whose method is called");
        check(all.calls().a, 1, "which gets the object as this");
        check(all.readsReturned() + all.returns().b, 3, "an object that is returned");
        check(all.readsCopied() + all.copies().b, 3, "an object that is copied to another variable");
        check(all.readsMerged() + all.merges(false), 6, "an object that meets another");
        check(all.readsComputed(), 1, "a literal with a computed name");
        check(all.readsWithGetter(), 1, "a literal with a getter");
        check(all.readsNotLiteral(), 1, "an object that comes out of a call");
    }
    const changes = [
        ["readsDefined", "defines", 1, 10, "Object.defineProperty"],
        ["readsDefinedSeveral", "definesSeveral", 1, 11, "Object.defineProperties"],
        ["readsDefinedInCallee", "definesInCallee", 1, 12, "Object.defineProperties in a function that is given the object"],
        ["readsAssigned", "assigns", 1, 13, "Object.assign"],
        ["readsReflected", "reflects", 1, 14, "Reflect.set"],
        ["readsWithNewPrototype", "setsPrototype", undefined, 15, "Object.setPrototypeOf"],
        ["readsWithAssignedProto", "assignsProto", undefined, 16, "an assignment to __proto__"],
    ];
    for (const [reads, changesIt, before, after, what] of changes) {
        check(all[reads](), before, "before " + what);
        all[changesIt]();
        check(all[reads](), after, "after " + what);
    }
    check(all.readsCoerced(), 1, "an object that is made a property key of, before");
    check(all.coerces({ a: 0 }), true, "which calls its toString");
    all.changesCoerced();
    check(all.readsCoerced(), 21, "and so gives it away");
    check(all.readsFrozen(), 1, "an object that is frozen later");
    check(Object.isFrozen(all.freezes()), true, "and is given away by that");
    check(all.indexesWith({ "[object Object]": 18 }) + all.readsUsedAsKey(), 19, "an object that is used as a key");
    all.writesIndexed("a", 20);
    check(all.readsWrittenIndexed(), 20, "an object that is written by index");
    check(all.readsWithProtoInLiteral(), 1, "a literal that names its prototype");
    check(all.readsInheritedFromLiteral(), 17, "and inherits from it");
    all.passes().a = 7;
    check(all.readsPassed(), 7, "an object that was passed and then changed");
    check(all.readsWritten(), 1, "before a write");
    all.writes(9);
    check(all.readsWritten(), 9, "after a write");
    check(all.readsReassigned(), 1, "before the variable is assigned again");
    all.reassigns();
    check(all.readsReassigned(), 3, "after it");
    check(all.readsDeleted(), 1, "before a property is deleted");
    all.deletes();
    check(all.readsDeleted(), undefined, "after it");
}

function tooEarly() {
    function readsConstantOfVar() { return Late.a; }
    function readsSlotOfVar() { return Late.made; }
    function readsConstantOfConst() { return Later.a; }
    noInline(readsConstantOfVar);
    noInline(readsSlotOfVar);
    noInline(readsConstantOfConst);
    const early = [thrownBy(readsConstantOfVar), thrownBy(readsSlotOfVar), thrownBy(readsConstantOfConst)];
    function readsOfVarThatIsNotMade() { return LateLiterals.a; }
    function testsVarThatIsNotMade() { return LateLiterals == null; }
    noInline(readsOfVarThatIsNotMade);
    noInline(testsVarThatIsNotMade);
    early.push(thrownBy(readsOfVarThatIsNotMade), testsVarThatIsNotMade());
    var Late = { a: 1, made: identity(2) };
    var LateLiterals = { a: 5, b: 6 };
    const Later = { a: 3, b: 4 };
    early.push(readsOfVarThatIsNotMade(), testsVarThatIsNotMade());
    return [early, readsConstantOfVar, readsSlotOfVar, readsConstantOfConst];
}
{
    const [early, readsConstantOfVar, readsSlotOfVar, readsConstantOfConst] = tooEarly();
    applies(readsConstantOfVar, folds);
    applies(readsSlotOfVar, readsSlot);
    applies(readsConstantOfConst, folds);
    check(early[0], "TypeError: undefined is not an object (evaluating 'Late.a')", "a constant of a var that is not assigned yet");
    check(early[1], "TypeError: undefined is not an object (evaluating 'Late.made')", "a slot of one");
    check(early[2], "ReferenceError: Cannot access 'Later' before initialization.", "a constant of a const that is not initialized yet");
    applies(readsConstantOfConst, "constant-object-is-initialized");
    doesNotApply(readsConstantOfVar, "constant-object-is-initialized");
    doesNotApply(readsSlotOfVar, "constant-object-is-initialized");
    applies(tooEarly, doesNotAllocate + ":LateLiterals", doesNotAllocate + ":Later");
    doesNotApply(tooEarly, doesNotAllocate + ":Late");
    check(early[3], "TypeError: undefined is not an object (evaluating 'LateLiterals.a')", "a constant of a var whose object is never made, before it is assigned");
    check(early[4], true, "which is null or undefined then");
    check(early[5], 5, "and has the constant afterwards");
    check(early[6], false, "and is not null");
    check(readsConstantOfVar(), 1, "the same read later");
    check(readsSlotOfVar(), 2, "the same slot later");
    check(readsConstantOfConst(), 3, "the same const later");
}

function seenByEval(text) {
    const Visible = { a: 1, b: 2 };
    function readsVisible() { return Visible.a; }
    noInline(readsVisible);
    eval(text);
    return readsVisible;
}
{
    const readsVisible = seenByEval("Visible.a = 8");
    doesNotApply(readsVisible, folds, readsSlot);
    check(readsVisible(), 8, "an object that code that is evaluated can reach");
}
