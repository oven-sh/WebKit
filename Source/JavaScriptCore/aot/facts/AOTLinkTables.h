/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCodeRole.h"
#include "AOTEffects.h"
#include "AOTFamilies.h"
#include "AOTLinkCoroutineTables.h"
#include "AOTLinkKnownEffects.h"
#include "AOTLinkOrderOfScopes.h"
#include "AOTMechanisms.h"
#include "AOTTestRuns.h"
#include "AOTThinSummary.h"
#include "AOTUnionTaggedMakers.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// What Link leaves for the plan and the walk. Flat tables, made once, then read by any thread. A default entry claims nothing.
// Every TypeID lies in the program's table.

struct LinkedParameter {
    // Null: nothing is expected. What the source declares, else what was seen to arrive beside `unknown`. A guess.
    TypeID expected;
    // The source declares `expected`. A guess from a declaration is opened anywhere, a weaker one only in a loop.
    bool expectedIsDeclared { false };
    // Null: nothing is proven. Of a function whose callers are not all known it stays null.
    // It is written for captures, `this`, the callee, new.target and the positional parameters. Of the rest parameter only what the compiler established, of the hidden rest and the argument count nothing.
    TypeID proven;
    bool escapes { true };
    // A PROOF, in Facts::linked() alone. Every caller is known, and all that arrives beside primitives is an object of one of the engine's classes out of sites whose objects have not left the program,
    // and on which no reached code lays a name, defines anything by a descriptor or puts another object above. So each has the names that its class gives it and no other, for as long as it lives.
    bool staysAsItsClassMakesIt { false };
};

struct LinkedFunction {
    bool isReached { true };
    bool everyCallerIsKnown { false };
    // Every call that may enter it is listed in LinkTables::calls: it has this function as its one callee. Only then may the walk's arrivals stand for its callers.
    // Never without everyCallerIsKnown: the calls of unseen code are listed nowhere. Never of an accessor, an initializer or the body of a module: no call stands for what enters those.
    bool everyCallHasOneCallee { false };
    bool resultIsFresh { false };
    // A call in tail position is a jump, and whoever is jumped to returns in place of the function. An entry by value returns a boxed value, and comes back for a raw one to box it.
    // True: in no ring of such calls that the function stands in does one go through an entry by value, and every member may return in a form of its own. Only then may it do so: else each turn of the ring would leave a frame.
    bool ringsOfTailCallsAreListed { false };
    // In one realm: a Worker and a second realm run everything again, each over statics of its own. The body of a module that the loader enters once,
    // or a function that one call alone may enter, which stands outside of loops in a function of which the same holds.
    bool runsAtMostOnce { false };
    // Somebody in its own cycle may name it, be it the function itself. Then not all that callers leave is there when it is walked.
    bool isNamedInItsCycle { true };
    // One of the calls that are listed (LinkTables::calls) enters it and stands in a loop of its caller: its text repeats as the text of that loop does. No proof rests on it.
    bool isCalledInLoop { false };
    // Its closure alone lies under a name in some object or class, or is one of the lost, in one of the two readings of Link. A guess goes by either.
    bool liesUnderNameOrIsLost { false };
    // Such a function, reached and with a body: whoever holds the object or the value may come to know the function, behind a guess too, and nobody lists who.
    // NO REFERRER STANDS FOR THOSE. Every unit of the function whose code may name another function is in the queue of Compile from the start (unitsNamedOutsideCode()), and a mention of what is there from the start brings nothing.
    // Never with everyCallHasOneCallee: only the calls that are listed enter such a function, and each of them is a referrer.
    bool isInQueueFromStart { false };
    // All that it names is in the queue from the start, or is what every reached function names (LinkedGroup::Kind::Anybody). So its code brings no unit, in whoever's code a copy of it stands.
    bool namesOnlyWhatEveryFunctionLeadsTo { false };
    // Of a function that a construct call hands `this` (Function::takesThisFromConstruction()): whoever enters it constructs, so `this` is an object that was made for that very entry and that nothing else has yet.
    // Never without everyCallerIsKnown. `F()`, `F.call(o)` and `Base.call(this)` end it. new.target may be a class or a function below.
    bool everyEntryIsConstruction { false };
    // Reached code stores into a closure of the function, defines or deletes a property of one, copies into one, freezes one or gives it another prototype: the subject holds the closures of this function and of no other.
    // AN EXPECTATION AND NO PROOF. False says nothing: what is done through a lost value or by unseen code is not in it. Only what costs time and bytes, and never an answer, may go by it.
    bool closuresAreChanged { false };
    // A PROOF, in Facts::linked() alone. Every caller is known, so no closure of the function has left the program, and no reached code stores into one, defines or deletes a property of one, copies into one, freezes one or lays one below something else.
    // So each has the names that a function object of its kind is made with, and no other. Never of a constructor. The default claims nothing.
    bool closuresStayAsMade { false };
    // A PROOF, in Facts::linked() alone. Every caller is known, so no closure of the function has left the program, and reached code stores into one under `prototype` alone: it defines and deletes no property of one, copies into none,
    // freezes none and lays none below something else. So beside `prototype` each has the names that a function object of its kind is made with, and stands where it was made. Of constructor functions too. The default claims nothing.
    bool closuresGetNoNameButPrototype { false };
    // As above, but for stores by name, each of which has its row in LinkTables::namesOnFunctions: a name without a row is on no closure of the function.
    bool closuresGetOnlyNamesThatAreListed { false };
    // A PROOF, in Facts::linked() alone. Every caller is known, and no reached code stores, defines or deletes under `prototype` of a closure of the function, under a key that nobody knows, by a descriptor or by a copy.
    // So `prototype` of each closure holds the object that the closure was made with. The default claims nothing.
    bool prototypePropertyStaysAsMade { false };
    // Functions that may name one another, straight or through others, have one number (LinkTables::referrers). Alone: a number of its own.
    // The default puts every function into one cycle: nothing is ordered then, and no arrival is used.
    uint32_t cycle { 0 };
    // Of the cycle. Whoever may name a function from outside its cycle has a lower rank.
    uint32_t rank { 0 };
    // Null: nothing is proven.
    TypeID result;
    TypeID thrown;
    // Its own and those of all it may call. A default Effects is every effect.
    Effects effectsOnReturn;
    Effects effectsOnThrow;
    // Into LinkTables::parameters.
    uint32_t firstParameter { 0 };
    uint32_t numberOfParameters { 0 };
    // ThinSummary::weightOfOperations of a body without loops that calls nobody, or only such bodies, three levels down: then with theirs. notLeaf: anything else. NO PROOF: the walk decides by it what it copies.
    static constexpr uint32_t notLeaf = UINT32_MAX;
    uint32_t weightAsLeaf { notLeaf };
};

// A field of a layout or of an environment, a variable, a capture.
struct LinkedCell {
    // Null: nothing is proven. What a variable or a field of an environment holds before its first store, Primitive::Absent where it starts empty, is in it, unless the compiler established that no read comes first.
    // Of a field of any other layout it is the join of what the stores bring and does not cover a read in front of the first: that is what isFilled is for. `never`: no store brings anything.
    TypeID proven;
    bool everyWriterIsKnown { false };
    bool isWrittenAfterConstruction { true };
    // Of a variable that is born as a function: some reached body stores into it, or somebody out of sight may. Of any other variable it says nothing.
    bool isStoredInto { true };
    // Of a variable: `const ansi = /\u001b\[[0-9;]*m/g`. A PROOF, in Facts::linked() alone, as LinkedParameter::staysAsItsClassMakesIt says of a parameter: all the objects that arrive there are of one of the engine's classes and stay as that class makes them.
    bool staysAsItsClassMakesIt { false };
    // Of a field of a record or of an instance. Every construction of the layout defines the field before the object can be seen from outside that construction, and no object of the layout loses a property or gets an accessor.
    // So whoever has such an object from anywhere but from the construction that he is part of himself finds the property in it, as a value of the object's own. A body that is making the object goes by what it has stored so far.
    bool isFilled { false };
    // And no body that is making such an object reads the name where it may not have defined it yet. So every read by name in the program finds the property.
    // Both of a variable: the body of its module stores into it in front of every read, so what it held at first is not in `proven`.
    bool isFilledAtEveryRead { false };
    // Of a field of a record or of an instance. NO PROOF, IN EITHER READING: every construction in sight has defined the field when it is through with the object, whoever has seen the object in between and whatever may take the field away later.
    // It is what the mark of the cell may answer for: an object is born with the mark, loses it where its maker is through and every such field lies in its slot, and gets it back where one is emptied. AOTLinkBirthsInlines.h.
    bool isOfBirth { false };
    // Of a field of a record: `{ get x() { .. } }`. Code in sight lays an accessor there whose function it names, and nothing in sight gives the field a value: no allocation and no constant data lists it,
    // no store of a maker and no definition by name may create it, nothing is copied into an object of the layout, no descriptor is laid there. A store into an object that is made runs the accessor.
    // A GUESS AT HOW THE OBJECTS ARE MADE, and no proof of what they hold.
    bool isFilledByAccessorsAlone { false };
    // Of a field of a record: A PROOF, in Facts::linked() alone. Beside isFilledByAccessorsAlone: every construction of the layout is in sight and has laid an accessor under the name before the object can be seen from outside it,
    // no body that is making such an object reads the name earlier, no object of the layout loses a property, and nothing else is ever defined on one by a descriptor.
    // So whoever has such an object finds an accessor of the object's own under the name. WHERE it lies the run time decides, object by object: in the slot, or beside it (Object::tryDefineAccessorsOfLiteral()).
    // Which functions: LinkedAccessorsOfField.
    bool holdsAccessorFromBirth { false };
    // Of a field of a record or of an instance: `this.parent = undefined`. Whatever makes an object of the layout gives the field the constant undefined, and nothing else. A GUESS at what the objects hold when they are made.
    bool isOnlyGivenUndefinedByMakers { false };
    // Of a field that the layout lists: `this.exports = undefined` in the constructor. Some maker gives it the constant undefined, the construction itself gives it nothing else, and no second constructor (WriterOfLinkLayouts::appendFields()) does.
    // A GUESS at which objects come to hold anything there, which decides where the slot lies and nothing else: SlotPlan lays it out of line where the written type lets the field be left out.
    bool isLeftUndefinedByMakers { false };
    // Of a field that the layout lists: `this.socket = null`. A store in sight behind the construction brings the constant undefined or null, or the makers bring that and nothing else.
    // A GUESS at what the objects hold, which decides whether the mark of the cell answers for the value of the field: PlannedLayout::holdsRawValueFromBirth().
    bool isGivenUndefinedOrNullInSight { false };
    // Of a field of a record of which objects are made: `interface Message { text: string; isMeta?: boolean; .. }   { text }`. No literal in sight lists it and no maker goes on to store it: it comes by a later store, by a copy,
    // out of a text that the run time parses, or never. A GUESS of the same kind, with the same use.
    bool isGivenByNoMaker { false };
    // Of a field that Link appended: `Object.defineProperty(this, "id", { value: id })`. Every definition in sight that creates the property is one by a descriptor in sight, and all of them create it in this one form.
    // A GUESS AT HOW THE OBJECTS ARE MADE, and no proof of what they hold: the record of the layout says the form, and a property that is created in another form lies beside its slot.
    OptionSet<FieldForm> formThatDefinitionsCreate;
};

enum class LayoutChange : uint8_t {
    // Any loss of extensibility, or of `writable` on a property.
    IsFrozen = 1 << 0,
    GainsProperty = 1 << 1,
    LosesProperty = 1 << 2,
    // The link of the object, or a store, definition or deletion on an object above it. What compiled code stores above an instance under the name of a method shows only in the LinkedMember of that name.
    ChangesPrototype = 1 << 3,
    // Any definition by a descriptor.
    GetsAccessor = 1 << 4,
};

// LinkTables::appendedKeys, but for those that the plan lays in the cell: appendedFieldsThatTypeOfThisRequires. A slot of the widest form takes a granule of the Extension, so the plan has room for all of them
// where no listed field has left the cell. Those come first: placeAppendedFieldsForSlotPlan().
inline constexpr uint32_t maxAppendedFieldsOutOfLine = maxExtensionGranules;

struct LinkedLayout {
    // The nearest. Null: none.
    FamilyID family;
    bool isAllocated { true };
    // Some object of the layout may come into the hands of code that is not compiled code of the program. Ask Facts::linked(): in Facts::held() it stays as it is here.
    // True: whatever is said of the layout, its fields and what lies above it is at most held. False: it is proven, and no door is ever passed.
    bool reachesUnseenCode { true };
    // What may happen to some object of the layout after it is made.
    OptionSet<LayoutChange> possibleChanges { LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor };
    // `{ raw: 0, get value() { .. } }`: possibleChanges says GetsAccessor for the sake of LinkFlow::layoutsWithKnownAccessors alone: literals in sight lay accessors whose functions they name, under names that the layout lists.
    // Nothing else is ever defined on an object of the layout by a descriptor, so every other field stays what it was made as.
    bool isRedefinedByLiteralsAlone { false };
    // Whatever the program has laid above an object of the layout is a LinkedMember of its class or of a class above: none of them has a member under a computed key or a base that is no class of the program.
    bool listsEveryMemberAbove { false };
    // The same of its class object and the class objects above that.
    bool listsEveryStaticMember { false };
    // False: from this layout up its row of written bases none is of a constructor function. findMember() does not go up such a row for a name under which no class has a member.
    bool hasConstructorFunctionInRow { true };
    // `counts[word] = 1`: reached code lays a property on what it knows as an object of the layout, under a key that is no name. A guess: what is stored through a lost object or by unseen code does not show.
    bool isFilledUnderComputedKeys { false };
    // False is A PROOF, in Facts::linked() alone and only beside !reachesUnseenCode: nothing ever sees in which order an object of the layout got its keys. No for-in, no copy, no row of a built-in that runs OwnKeys or OwnValues,
    // and no inspection comes to one. The default claims nothing.
    bool keysMayBeListed { true };
    // Whatever stands below the prototype object of the class is an object of the layout, or of a member that the tag of its maker picks. So `x instanceof C` asks for those shapes. Ask Facts::linked(): in Facts::held() it stays as it is here.
    bool onlyOwnObjectsStandBelowPrototype { false };
    // Of the layout of a constructor function that is made once. A PROOF, in Facts::linked() alone: writePrototypeObjectsThatAreSettled().
    // Nobody ever stores `prototype` of the function (LinkedFunction::prototypePropertyStaysAsMade), so every object of the layout, and of a layout below it, is made below ONE object: the one that the function was made with.
    // Nothing lies on that object but what LinkTables::namesOnPrototypes lists, no object of the layout gets a name of its own that its fields do not list, another prototype or an accessor, and none has left.
    bool prototypeObjectIsSettledAheadOfEveryObject { false };
    // `class Socket extends NetSocket { }`, of a class that is written without a base: closures of the constructor of this one class, or of this one constructor function, are all that arrives as its heritage.
    // What the record of the class expects above it (ClassFacts::prototypeBase): the definition looks at what it is handed.
    LayoutID heritage;
    // Of a record, in Facts::held(): the one function in whose body objects of the layout are made. Null: none in sight, or several.
    FunctionID onlyFunctionThatMakesObjects;
};

// A method, getter or setter of a class.
struct LinkedMember {
    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MethodKind kind { MethodKind::Method };
    uint32_t index { 0 };
    // Null: computed.
    PropertyKeyID key;
    // The one function whose closure the member holds in every run. Null: it may have been replaced, or reached code never defines it.
    FunctionID function;
    bool everyWriterIsKnown { false };
    // What the evaluation of the class lays there, as it is written. Null: no closure that the definition itself makes.
    FunctionID written;
};

// What the program lays under a name on the object that a constructor function is made with under `prototype`: LinkFlow::storedOnPrototypeOf(). It may not lie there yet when a lookup comes by.
struct LinkedNameOnPrototype {
    LayoutID layout;
    // Null: a descriptor defines a key that nobody knows.
    PropertyKeyID key;
    // Closures of this one function are all that is ever stored there. Null: nobody can tell.
    FunctionID function;
    // Closures of this one function are all that code in sight stores there, whoever else has the object in hand, and no descriptor defines the name. NO PROOF: what the record of the class expects.
    FunctionID expected;
    // `Object.defineProperties(F.prototype, { size: { get() { .. }, set(value) { .. } } })`: one descriptor in sight is laid under the name, nothing is stored there, and as it is written it holds closures of these functions and no value.
    // NO PROOF either.
    FunctionID expectedGetter;
    FunctionID expectedSetter;
};

// What the program lays under a name on a function of its own: LinkFlow::laidOnClosuresOf(). PROOFS, in Facts::linked() alone.
struct LinkedNameOnFunction {
    FunctionID holder;
    PropertyKeyID key;
    // Closures of this one function are all that is ever stored there, and every caller of the holder is known. Null: nobody can tell.
    FunctionID function;
};

struct LinkTables;
// `F.prototype.m = function () { }`: what the record of a constructor function lists, behind what is there. AOTLinkPrototypes.cpp.
// THE RECORD EXPECTS, IT PROVES NOTHING: when the first object is made, the run time looks at the prototype object, and begins the guard of a name where a closure of the function lies under it (ClassTable::armMemberGuards()).
// So it lists LinkedNameOnPrototype::expected. Whoever reads under the name is among the referrers of that function: WriterOfLink::indexFunctionsThatAreHeld().
void appendMembersAssignedToPrototype(const Program&, const LinkTables&, LayoutID, Vector<ClassMember>&);

// What a lookup under a name comes to above the objects of a layout, or on its class object: LinkTables::findMember().
struct MemberFound {
    enum class Kind : uint8_t {
        // Nobody can tell.
        Unknown,
        // No class of the row has a member under the name, so the lookup goes on above the classes of the program.
        Nothing,
        Method,
        Accessor,
    };

    Kind kind { Kind::Unknown };
    // The one function whose closure lies there in every run.
    FunctionID method;
    // Null: the accessor lacks that half.
    FunctionID getter;
    FunctionID setter;
};

// What many name together. In the graph of referrers it is one node between those who name it and its members: as many edges as there are of both, where a list for each member would have as many as their product.
struct LinkedGroup {
    enum class Kind : uint8_t {
        // The code of the library behind a built-in, and what has no body: the walk names them where no summary shows it. Every reached function names the group.
        Anybody,
        // `subject`: a FamilyID. The constructors of its layouts.
        Constructors,
        // `subject`: its place among such groups. What a shared call of LinkFlow lists.
        SharedCall,
    };

    Kind kind { Kind::Anybody };
    uint32_t subject { 0 };
    // As LinkedFunction has them.
    uint32_t cycle { 0 };
    uint32_t rank { 0 };
};

struct LinkedCall {
    FunctionID caller;
    ValueID site;
    // The one function that the call enters, if it enters any: what is called may be no function, and the call of that throws.
    FunctionID callee;
    // SummaryCall::isInTailPosition.
    bool isInTailPosition { false };
    // In one run of the caller it may enter the callee again: it stands in a loop, or a protocol makes it, which may ask as often as it likes.
    bool mayComeAgain { true };

    friend constexpr auto operator<=>(const LinkedCall&, const LinkedCall&) = default;
};

// `table.handler(x)`, off objects of one layout of the program that lists the name as a field: the call was seen to find ONE built-in there, beside at most one function of the program.
// NO PROOF, and in Facts::held() alone: whoever goes by it tests what is called.
struct LinkedCallThatFindsBuiltin {
    FunctionID caller;
    ValueID site;
    BuiltinID builtin;
    // Null: none.
    FunctionID callee;

    friend constexpr auto operator<=>(const LinkedCallThatFindsBuiltin&, const LinkedCallThatFindsBuiltin&) = default;
};

// The order in which LinkFlow::run() takes the nodes that have climbed, and the observers that were put where something was held by then. THE TABLES MAY NOT HANG ON IT.
// A build takes the newest first. The others are for the tests alone: the commands `closure` and `link` in tests/README.md.
enum class OrderOfWorklist : uint8_t { NewestFirst, OldestFirst, Scattered };

// What Link takes unseen code to do to an object of the program, a prototype object or a class object that it gets hold of.
// Anything: from then on the fields and members hold anything, all five changes are possible, and the objects count as unseen. What the tables say then is PROVEN and needs no test: Facts::linked().
// OnlyReadsAndCalls: an escape flows outward only. What the tables say then is NO PROOF. It is the best guess at what no store ever breaks, and it is HELD: Facts::held().
enum class WhatUnseenCodeDoes : uint8_t { Anything, OnlyReadsAndCalls };

// `o[k]`, where all that k can be in any run is the one symbol of a key.
struct LinkedAccess {
    FunctionID function;
    ValueID site;
    PropertyKeyID key;

    friend constexpr auto operator<=>(const LinkedAccess&, const LinkedAccess&) = default;
};

// `flags.verbose`, where all that arrives at `flags` are objects of the engine's classes on whose allocation sites descriptors lay closures of one function as getters: under that name where a name is written, else under names that nobody knows.
// `log.enabled`, where all that arrives at `log` are closures of one function on which a descriptor lays the getter.
// NO PROOF, IN EITHER READING: the name may find something else, or nothing. Whoever calls the function has looked the name up and has tested what it found.
struct LinkedGetterAtRead {
    FunctionID function;
    ValueID site;
    FunctionID getter;

    friend constexpr auto operator<=>(const LinkedGetterAtRead&, const LinkedGetterAtRead&) = default;
};

// `nodes[i]`: all that the flow has seen come out of a read under a computed key, with undefined for a place that holds nothing. A PROOF, in Facts::linked() alone: it is what Link itself goes on with behind the read.
// It says nothing of how the read is done: the receiver may have left the walk's sight, and the operation may test its own guess.
struct LinkedResultOfRead {
    FunctionID function;
    ValueID site;
    TypeID proven;
};

// `prices.get(name)`, `for (const [name, price] of prices)`: all that arrives at the receiver of the operation is a Map, a WeakMap or a Set of one family of sites that has not escaped: what the flow has seen go into its slots.
// A PROOF, in Facts::linked() alone, as LinkedResultOfRead is: it is what Link itself goes on with behind the operation. Of a Set `value` is the element, and `key` is null. Null: nothing is said.
struct LinkedContentsAtOperation {
    FunctionID function;
    ValueID site;
    TypeID key;
    TypeID value;
};

// `nodes.push(x)`, `nodes[i]`, `nodes.length`: all that arrives at the receiver of the operation is of one of the engine's classes and stays as that class makes it, as LinkedParameter::staysAsItsClassMakesIt says of a parameter.
// A PROOF, in Facts::linked() alone. It holds whatever has run in front of the operation. No view of a buffer is listed: another thread may lengthen one.
struct LinkedReceiverAsMade {
    FunctionID function;
    ValueID site;

    friend constexpr auto operator<=>(const LinkedReceiverAsMade&, const LinkedReceiverAsMade&) = default;
};

struct LinkedCatch {
    FunctionID function;
    BlockID handler;
    // A GUESS IN BOTH READINGS: what was seen to arrive at the handler, beside whatever nobody knows. As LinkedParameter::expected is written. An error of the engine's own, a host function and a rejection bring what nobody lists.
    TypeID caught;
};

// `{ get total() { .. }, set total(v) { .. } }`: the one function that literals in sight lay as the getter, and as the setter, of the field. Null: none, or several. NO PROOF, IN EITHER READING: whoever calls one has tested what lies in the slot.
// BUT WHERE getterIsProven | setterIsProven SAYS SO, in Facts::linked() alone: LinkedCell::holdsAccessorFromBirth, and every construction lays a closure of that one function as this half.
// So an access under the name off an object of the layout is a call of the function with the object as `this`, and whatever lies in a filled slot has a closure of it as that half.
struct LinkedAccessorsOfField {
    FieldReference field;
    FunctionID getter;
    FunctionID setter;

    bool getterIsProven { false };
    bool setterIsProven { false };
    friend constexpr auto operator<=>(const LinkedAccessorsOfField&, const LinkedAccessorsOfField&) = default;
};

// How large the flow was that filled the tables.
struct SizeOfLinkFlow {
    uint64_t nodes { 0 };
    uint64_t edges { 0 };
    uint64_t observers { 0 };
    uint64_t nodesTakenOffWorklist { 0 };
    uint64_t observersTold { 0 };
    uint64_t rounds { 0 };
};

// An operation of the program writes on what the flow says is that built-in and nothing else. For the report of the writes that a lock turns away.
struct WriteFollowedToBuiltin {
    FunctionID function;
    ValueID site;
    BuiltinID holder;
    // Of a copy and of a map of descriptors: the names of the objects that the flow brings as the source. knowsNamesBrought: these are all that any of them has.
    Vector<PropertyKeyID> namesBrought { };
    bool knowsNamesBrought { false };
};

// The lookups of reached code that the objects of the program do not answer: each may go on to an object of the engine or of the host.
// What is reached went by them (AOTBuiltinsInReach.h), and Facts::builtinUses() does.
struct LookupsPastProgramObjects {
    // By key.
    BitVector namesRead;
    BitVector namesCalled;
    // Of the two: those that text of the program spells. A name that the text of the library alone spells finds no function whose call or read is an event.
    BitVector namesSpelledByProgram;
    // By built-in: the flow has found the entry itself, and follows the object from there.
    BitVector builtinsRead;
    BitVector builtinsCalled;
    // The flow has lost the object: it has reached unseen code, has met another value at a node that says `anything` from then on, or was asked for a member under a key that nobody knows.
    BitVector builtinsLetGo;
    // A call that Link sees goes by no row, so nobody says what the code behind the built-in is passed.
    BitVector builtinsCalledOutOfSight;
    // By built-in, as the table lists it: a member that a program may replace, which Link has taken for what the table says lies there: BuiltinsInReach::takeForConstant().
    // MUST BE TRUE OF THE PROGRAM THAT RUNS: each is locked at the start (Facts::builtinMembersLockedAtStartUp()), with every mechanism withheld too: what is reached hangs on it.
    BitVector membersTakenForConstants;
    // THE WRITES THAT THE FLOW HAS FOLLOWED TO BUILT-INS: a store, a definition, a deletion or a copy, of an operation or of a row. Which members stay open goes by these: WritesOnBuiltinsInProgramFacts.
    // By built-in, as the table lists it: the subject is the holder, and the write names the member.
    BitVector membersWritten;
    // Ascending by function and site. LinkFlow::takeWritesFollowedToBuiltins().
    Vector<WriteFollowedToBuiltin> writesFollowedToBuiltins;
    // From here on by the entry that stands for the object. The subject is the object, and nobody can read the key.
    BitVector holdersWrittenUnderAnyName;
    // The subject is the object, whatever the name: `process.env.TZ = zone`. The table lists no member there, so membersWritten says nothing of it.
    BitVector holdersWritten;
    // LinkType::holdsLostBuiltins.
    BitVector lostBuiltins;
    BitVector builtinsAtUnseen;
    // Code of the program passes the built-in an object that was written with the option under which its row runs unseen code: `Bun.serve({ app })`.
    BitVector builtinsPassedOptionThatRunsCode;
    // By class: an instance has reached unseen code.
    BitVector classesWithInstancesAtUnseen;
    // By key. Through a value that holds lost built-ins.
    BitVector namesReadThroughLostBuiltins;
    BitVector namesWrittenThroughLostBuiltins;
    // Code of the program writes under the name through a value that nobody knows: `unknown.now = f`.
    BitVector namesWrittenThroughUnseenByProgram;
    BitVector namesWrittenThroughUnseenByLibrary;
    bool writesUnderAnyNameThroughUnseenByProgram { false };
    bool writesUnderAnyNameThroughLostBuiltins { false };
    // Object.getPrototypeOf() and its like are handed such a value.
    bool climbsFromLostBuiltins { false };
    bool climbsFromUnseenObjects { false };
    // From then on a value that nobody knows may be one of them.
    bool lostBuiltinsReachUnseen { false };
    bool readsUnderKeysNobodyKnows { false };
    // An object has reached unseen code. Until then nobody but reached code looks up the name of a protocol or a symbol.
    bool nativeCodeMayHoldObjects { false };
};

// How many accesses by name of reached code go through a value that Link knows as an object of the family. A count for the plan, which lays the fields that are used most at one offset. It proves nothing.
struct LinkedUseThroughFamily {
    FamilyID family;
    PropertyKeyID key;
    uint32_t reads { 0 };
    uint32_t stores { 0 };
};

// Fields, each named by the layout that declares it and its key.
class FieldsOfNumbers {
public:
    void add(LayoutID declarer, PropertyKeyID key) { m_fields.add(wordOf(declarer, key)); }
    bool contains(LayoutID declarer, PropertyKeyID key) const { return m_fields.contains(wordOf(declarer, key)); }
    // The text names a field by what is written of the receiver: the layout itself, the one that it is declared as, or one of the types that its objects are written as. AOTEnforcedFields.cpp.
    bool names(const Program&, LayoutID, PropertyKeyID) const;

private:
    static uint64_t wordOf(LayoutID declarer, PropertyKeyID key) { return (static_cast<uint64_t>(declarer.index()) + 1) << 32 | key.index(); }

    UncheckedKeyHashSet<uint64_t> m_fields;
};

struct LinkTables {
    Vector<LinkedFunction> functions;
    Vector<LinkedParameter> parameters;
    // Ascending by caller and site. A call that is not listed may enter anything. A site may be that of an operation that is no call and looks a function up on an operand.
    // A site at which the summary has several calls is not listed.
    Vector<LinkedCall> calls;
    // Ascending by caller and site. A site at which the summary has several such calls is not listed.
    Vector<LinkedCallThatFindsBuiltin> callsThatFindBuiltins;
    // By function: it makes one of them. Nearly every function says no here.
    BitVector callersThatFindBuiltins;
    // By function, and one more: where the calls that it makes begin in `calls`.
    Vector<uint32_t> firstCallOfCaller;
    // Ascending by function and site. The accesses under computed keys that are accesses under one key: the walk builds them by that name, and whatever lies under the name has the function among its referrers.
    Vector<LinkedAccess> accessesUnderOneKey;
    // Ascending by function and site. In Facts::held() alone.
    Vector<LinkedGetterAtRead> gettersExpectedAtReads;
    // Ascending by function. Empty: nothing is known of any coroutine.
    Vector<LinkedCoroutine> coroutines;
    // Null: nothing is known. AOTLinkCoroutines.cpp.
    const LinkedCoroutine* findCoroutine(FunctionID) const;
    // Ascending by function and site. In Facts::linked(): a proof; a read that is not listed may give anything. In Facts::held(): what was seen to come out of the read, an expectation.
    Vector<LinkedResultOfRead> resultsOfReadsByKey;
    Vector<LinkedContentsAtOperation> contentsAtOperations;
    // Ascending. In Facts::linked() alone. The site of a read, by name or by key, or of a call by name: the read of the method has no place of its own.
    Vector<LinkedReceiverAsMade> receiversThatStayAsMade;
    // Ascending. In Facts::linked() alone. The site of an operation that iterates over an operand by the protocol, an IteratorOpen or an AppendSpread: all that arrives there to be iterated over stays as its class makes it.
    // Nothing is said of the iterator and of what its steps answer with, which the protocol looks at under the same site.
    Vector<LinkedReceiverAsMade> iterablesThatStayAsMade;
    // In Facts::linked() alone, laid there behind both readings: fieldsReadAsIntegersIn(), fieldsStoredWhatIsNoInt32In(), keysStoredWhatIsNoInt32In(). Empty: nobody has looked.
    FieldsOfNumbers fieldsReadAsIntegers;
    FieldsOfNumbers fieldsReadForArithmetic;
    FieldsOfNumbers fieldsStoredWhatIsNoInt32;
    // Some store in sight brings a variable of which `variables` or `fields` proves numbers without fractions: what is counted up and down.
    FieldsOfNumbers fieldsStoredCounts;
    BitVector keysStoredWhatIsNoInt32;
    // Ascending by field.
    Vector<LinkedAccessorsOfField> accessorsExpectedInFields;
    // By key: some row above stands under it. The walk says no by this bit in front of every other question.
    BitVector keysOfAccessorsExpectedInFields;
    // By key: some class declares a static field under it. accessStaticFieldInVariable() says no by this bit in front of every other question.
    BitVector keysOfStaticFields;
    // What TestRunsInLink has gathered: link() is where the flow ends. Both ascend, each entry once. Empty outside a test run. They claim nothing: openNamesFoundByLink() and finishFactsOfTestRun() take them.
    Vector<NameAddedByTests> namesAddedByTests;
    Vector<VariableID> exportsSpiedOnThatWireLeavesShut;
    // Whose objects a function writes, what it does given its arguments, what a call of several functions does. In Facts::linked() alone: PROOFS. Empty lists claim nothing.
    LinkKnownEffects known;
    // Ascending by function and handler. A handler that is not listed: nothing was seen to arrive. Empty claims nothing.
    Vector<LinkedCatch> catches;
    // WHO MAY NAME A FUNCTION: call it, make a closure of it, evaluate the class it belongs to, and the rest of the list above UnitQueue in driver/AOTCompileQueue.h.
    // A graph. Its nodes are the functions, under their own numbers, and behind them the groups: groups[k] is node functions.size() + k. By node: the nodes that name it, ascending. Offsets into referrers, one more than there are nodes.
    // The code of a function may mention a unit of whatever the function leads to, from node to node. IT HAS NO MORE EDGES THAN LINK'S FLOW HAS: nothing in it is a product of two sets.
    // IN Facts::linked() IT IS THE GRAPH OF BOTH READINGS, one laid over the other: code tests for a function that Facts::held() names, and mentions it then. The cycles and ranks below come from that.
    Vector<LinkedGroup> groups;
    Vector<uint32_t> firstReferrer;
    Vector<FunctionID> referrers;
    // THE ORDER OF CYCLES, which is all that the queue of Compile reads of the graph. A cycle may hold groups alone.
    // By cycle: the rank, and the other cycles that hold a referrer of a member, each once, ascending: when all of them are done and none has named a function of the cycle, nobody ever will.
    // Offsets into cyclesAwaited, one more than there are cycles.
    Vector<uint32_t> rankOfCycle;
    Vector<uint32_t> firstCycleAwaited;
    Vector<uint32_t> cyclesAwaited;
    // The same the other way round: by cycle, the cycles that wait for it, ascending. listCyclesNamed() makes them of the two above.
    Vector<uint32_t> firstCycleNamed;
    Vector<uint32_t> cyclesNamed;
    // By layout, then by field: offsets into fields, one more than there are layouts.
    Vector<uint32_t> firstFieldOfLayout;
    Vector<LinkedCell> fields;
    // A LAYOUT HAS A FIELD FOR EVERY NAME THAT THE PROGRAM MAY STORE ON AN OBJECT OF IT. By layout: the names that it does not list itself, in the order in which the program is written to store them.
    // The field of the name at `place` is FieldReference { layout, number of listed fields + place }. The plan gives each a slot, which is empty when the object is made. No construction fills it.
    // A list claims nothing: a name that is missing is an extra property, and its stores show as LayoutChange::GainsProperty. At most maxAppendedFieldsOutOfLine a layout beside those that lie in the cell, and with the listed ones no more than a key order counts.
    // Offsets into appendedKeys and appendedFields, one more than there are layouts. Empty: no layout has any.
    Vector<uint32_t> firstAppendedFieldOfLayout;
    Vector<PropertyKeyID> appendedKeys;
    // THE NAMES THAT held() ALONE APPENDS, in linked()'s tables alone: computeFacts() lays them here once both flows have ended (layKeysThatHeldAloneAppendsOver()). Offsets, one more than there are layouts. Empty: none.
    // The plan numbers them BEHIND appendedKeysOf(): place = appendedKeysOf().size() + the index here. So no place of appendedKeys moves and nobody waits for the longer flow.
    // A SLOT IS A DECISION OF THE LAYOUT AND NO PROOF: these tables have no cell for such a field (findCellOfField(): null), so nothing is claimed of it, and a store into it stands behind its shape test like any other.
    Vector<uint32_t> firstKeyAppendedByHeldAloneOfLayout;
    Vector<PropertyKeyID> keysAppendedByHeldAlone;
    // By place in keysAppendedByHeldAlone: held().appendedFieldsThatTypeOfThisRequires has the name.
    BitVector keysAppendedByHeldAloneThatLieInCell;
    Vector<LinkedCell> appendedFields;
    // By place in appendedKeys. `interface ObjectType extends Type { objectFlags: number }`: some type that is written of the objects says that whoever is one has the property. A guess.
    BitVector appendedFieldsThatSomeWrittenTypeRequires;
    // `interface Type { id: number }   function Type(this: Type) { }`: every type that is written of `this` and lists the name says so. Whoever has an object made fills it. A guess.
    // Or a second constructor stores it: a body that stores on what was constructed for it and returns that (`createObjectType()` behind `createType()` behind `new Type()`).
    BitVector appendedFieldsThatTypeOfThisRequires;
    // Of each layout, among the appended names that lie out of line: the one that the program names most often. Where the cell has a word left over in its size class the plan lays it there. A guess, which decides where the slot lies and nothing else.
    BitVector appendedFieldsNamedMostOften;
    // WHAT MAKERS STORE. `const made = new F(..); made.a = ..; made.b = ..`: by layout, the appended names that some body stores one behind the other on what a call or an allocation of its own has given it.
    // A guess at the order in which objects get their keys: plan/AOTKeyOrders.cpp. Offsets, one more than there are layouts and than there are runs. Empty: no layout has any.
    Vector<uint32_t> firstRunOfMakersOfLayout;
    Vector<uint32_t> firstKeyOfRunOfMakers;
    Vector<PropertyKeyID> keysOfRunsOfMakers;
    // `const node = createBaseDeclaration(kind) as PropertyAccessExpression; node.expression = ..; node.name = ..`: a run of stores on what a row of calls has handed on from the constructor of a tagged maker (Program::taggedFamilies).
    // The body, its first store, the names that the whole row stores, the innermost body first, and the makers that the row leads to. A guess: plan/AOTKeyOrders.cpp reads in the body which members are written of the object.
    struct RunBehindTaggedMakers {
        FunctionID body;
        ValueID firstStore;
        uint32_t firstKey { 0 };
        uint32_t numberOfKeys { 0 };
        uint32_t firstMaker { 0 };
        uint32_t numberOfMakers { 0 };
    };
    Vector<RunBehindTaggedMakers> runsBehindTaggedMakers;
    Vector<PropertyKeyID> keysOfRunsBehindTaggedMakers;
    Vector<LayoutID> makersOfRunsBehindTaggedMakers;
    Vector<LinkedCell> variables;
    // A PROOF, in Facts::linked() alone: empty in the other reading.
    OrderOfScopes orderOfScopes;
    // By function, then by capture: offsets into captures, one more than there are functions.
    Vector<uint32_t> firstCaptureOfFunction;
    Vector<LinkedCell> captures;
    Vector<LinkedLayout> layouts;
    // Ascending by class, side and index. Facts::membersOf() is made of these.
    Vector<LinkedMember> members;
    // Places in `members`, ascending by class, side, key and index: indexMembersByKey(). A class may have thousands of members, and each lookup under a name asks for one.
    Vector<uint32_t> membersByKey;
    // By side: the keys under which some class has a member: indexMembersByKey(). Most names that are looked for above an object are names of fields.
    std::array<BitVector, 2> keysOfMembers;
    // Ascending by layout and key, the null key last.
    Vector<LinkedNameOnPrototype> namesOnPrototypes;
    // Ascending by constructor: those of LinkFlow::taggedMakers whose constructor is reached. In Facts::linked() alone.
    Vector<TaggedMaker> taggedMakers;
    // Ascending by family and key. In Facts::linked() alone.
    Vector<LinkedUseThroughFamily> usesThroughFamilies;
    SizeOfLinkFlow sizeOfFlow;
    LookupsPastProgramObjects lookups;
    // JavaScript that the build has not seen: text that is evaluated, code from disk.
    bool unseenCodeCanRun { true };

    const LinkedFunction& function(FunctionID id) const LIFETIME_BOUND { return functions[id.index()]; }
    std::span<const LinkedParameter> parametersOf(FunctionID id) const LIFETIME_BOUND { return parameters.span().subspan(function(id).firstParameter, function(id).numberOfParameters); }
    // Of a function or of a group.
    std::span<const FunctionID> referrersOf(FunctionID node) const LIFETIME_BOUND { return referrers.span().subspan(firstReferrer[node.index()], firstReferrer[node.index() + 1] - firstReferrer[node.index()]); }
    uint32_t numberOfNodesOfReferrers() const { return static_cast<uint32_t>(functions.size() + groups.size()); }
    bool isGroup(FunctionID node) const { return node.index() >= functions.size(); }
    const LinkedGroup& group(FunctionID node) const LIFETIME_BOUND { return groups[node.index() - functions.size()]; }
    std::span<const uint32_t> cyclesAwaitedBy(uint32_t cycle) const LIFETIME_BOUND { return cyclesAwaited.span().subspan(firstCycleAwaited[cycle], firstCycleAwaited[cycle + 1] - firstCycleAwaited[cycle]); }
    std::span<const uint32_t> cyclesNamedBy(uint32_t cycle) const LIFETIME_BOUND { return cyclesNamed.span().subspan(firstCycleNamed[cycle], firstCycleNamed[cycle + 1] - firstCycleNamed[cycle]); }
    void listCyclesNamed();
    // From node to node of `referrers`. With the tables at their defaults every function leads to every other.
    bool cycleLeadsTo(uint32_t referrer, uint32_t named) const;
    bool leadsTo(FunctionID referrer, FunctionID function) const { return cycleLeadsTo(functions[referrer.index()].cycle, functions[function.index()].cycle); }
    // UnitQueue brings such a unit whenever it is mentioned, and no cycle waits for it. So its code may name only what is in the queue from the start, wherever its function leads.
    bool standsOutsideCycles(CodeUnit unit) const { return namesOnlyUnitsOfItsFunction(unit.role) && functions[unit.function.index()].isInQueueFromStart; }
    bool leadsTo(CodeUnit referrer, FunctionID function) const { return leadsTo(referrer.function, function) && (!standsOutsideCycles(referrer) || functions[function.index()].isInQueueFromStart); }
    // THE WALK ASKS BEFORE IT TAKES A VALUE FOR A FUNCTION: before it calls the function straight, and before it tests for its closure. A guess may expect what no door has brought: what held() has somewhere, a layout that is known by its names alone.
    // False: code of `referrer` that mentioned the function could bring a unit for which nobody has waited. The call goes by the value, and nothing is tested.
    bool mayBeMentionedBy(CodeUnit referrer, FunctionID function) const { return functions[function.index()].isInQueueFromStart || leadsTo(referrer, function); }
    const LinkedCell& field(FieldReference field) const LIFETIME_BOUND { return fields[firstFieldOfLayout[field.layout.index()] + field.field]; }
    std::span<const PropertyKeyID> appendedKeysOf(LayoutID id) const LIFETIME_BOUND
    {
        size_t layout = id.index();
        if (layout + 1 >= firstAppendedFieldOfLayout.size())
            return { };
        return appendedKeys.span().subspan(firstAppendedFieldOfLayout[layout], firstAppendedFieldOfLayout[layout + 1] - firstAppendedFieldOfLayout[layout]);
    }
    std::span<const PropertyKeyID> keysAppendedByHeldAloneOf(LayoutID id) const LIFETIME_BOUND
    {
        size_t layout = id.index();
        if (layout + 1 >= firstKeyAppendedByHeldAloneOfLayout.size())
            return { };
        return keysAppendedByHeldAlone.span().subspan(firstKeyAppendedByHeldAloneOfLayout[layout], firstKeyAppendedByHeldAloneOfLayout[layout + 1] - firstKeyAppendedByHeldAloneOfLayout[layout]);
    }
    // As the plan numbers the fields behind those that the layout lists: PlannedLayout::appendedKeys.
    uint32_t numberOfAppendedKeysOfPlan(LayoutID id) const { return static_cast<uint32_t>(appendedKeysOf(id).size() + keysAppendedByHeldAloneOf(id).size()); }
    Vector<PropertyKeyID, 16> appendedKeysOfPlan(LayoutID id) const
    {
        Vector<PropertyKeyID, 16> keys;
        keys.append(appendedKeysOf(id));
        keys.append(keysAppendedByHeldAloneOf(id));
        return keys;
    }
    // Of a field that the layout lists or that Link appended. Null: the tables have no word of it.
    const LinkedCell* findCellOfField(FieldReference field) const LIFETIME_BOUND
    {
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size())
            return nullptr;
        uint32_t numberListed = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout];
        if (field.field < numberListed)
            return &fields[firstFieldOfLayout[layout] + field.field];
        uint32_t place = field.field - numberListed;
        return place < appendedKeysOf(field.layout).size() ? &appendedFields[firstAppendedFieldOfLayout[layout] + place] : nullptr;
    }
    // Into appendedKeys. Nothing: the layout lists the field itself, or nobody does.
    std::optional<size_t> findPlaceOfAppendedField(FieldReference field) const
    {
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size())
            return std::nullopt;
        uint32_t numberListed = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout];
        if (field.field < numberListed || field.field - numberListed >= appendedKeysOf(field.layout).size())
            return std::nullopt;
        return firstAppendedFieldOfLayout[layout] + (field.field - numberListed);
    }
    bool isAppendedFieldRequiredBySomeWrittenType(FieldReference field) const
    {
        std::optional<size_t> place = findPlaceOfAppendedField(field);
        return place && appendedFieldsThatSomeWrittenTypeRequires.get(*place);
    }
    bool isAppendedFieldNamedMostOften(FieldReference field) const
    {
        std::optional<size_t> place = findPlaceOfAppendedField(field);
        return place && appendedFieldsNamedMostOften.get(*place);
    }
    bool isAppendedFieldRequiredByTypeOfThis(FieldReference field) const
    {
        std::optional<size_t> place = findPlaceOfAppendedField(field);
        if (place)
            return appendedFieldsThatTypeOfThisRequires.get(*place);
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size() || layout + 1 >= firstKeyAppendedByHeldAloneOfLayout.size())
            return false;
        size_t numberInFront = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout] + appendedKeysOf(field.layout).size();
        return field.field >= numberInFront && field.field - numberInFront < keysAppendedByHeldAloneOf(field.layout).size() && keysAppendedByHeldAloneThatLieInCell.get(firstKeyAppendedByHeldAloneOfLayout[layout] + (field.field - numberInFront));
    }
    void forEachRunOfMakers(LayoutID id, NOESCAPE const Invocable<void(std::span<const PropertyKeyID>)> auto& functor) const
    {
        size_t layout = id.index();
        if (layout + 1 >= firstRunOfMakersOfLayout.size())
            return;
        for (uint32_t run = firstRunOfMakersOfLayout[layout]; run < firstRunOfMakersOfLayout[layout + 1]; ++run)
            functor(keysOfRunsOfMakers.span().subspan(firstKeyOfRunOfMakers[run], firstKeyOfRunOfMakers[run + 1] - firstKeyOfRunOfMakers[run]));
    }
    const LinkedCell& variable(VariableID id) const LIFETIME_BOUND { return variables[id.index()]; }
    const LinkedCell& capture(FunctionID id, uint32_t capture) const LIFETIME_BOUND { return captures[firstCaptureOfFunction[id.index()] + capture]; }
    const LinkedLayout& layout(LayoutID id) const LIFETIME_BOUND { return layouts[id.index()]; }
    // Null: the call may enter anything. AOTLink.cpp.
    FunctionID calleeAt(FunctionID caller, ValueID site) const;
    // Null: none is listed. AOTLink.cpp.
    const LinkedCallThatFindsBuiltin* findCallThatFindsBuiltin(FunctionID caller, ValueID site) const LIFETIME_BOUND;
    // Null: the access goes by the value of its key. AOTLink.cpp.
    PropertyKeyID keyAt(FunctionID, ValueID site) const;
    // Null: nothing is proven. AOTLink.cpp.
    TypeID resultOfReadByKey(FunctionID, ValueID site) const;
    const LinkedContentsAtOperation* contentsAtOperation(FunctionID, ValueID site) const;
    bool receiverStaysAsMadeAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(receiversThatStayAsMade, LinkedReceiverAsMade { function, site }); }
    bool iterableStaysAsMadeAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(iterablesThatStayAsMade, LinkedReceiverAsMade { function, site }); }
    // Null: nothing is expected. AOTLinkKeyOrders.cpp.
    const LinkedAccessorsOfField* findAccessorsExpectedIn(FieldReference) const LIFETIME_BOUND;
    // Null: nothing is expected. AOTLinkExceptions.cpp.
    TypeID caughtAt(FunctionID, BlockID handler) const;
    // Behind every change of `members`. AOTLink.cpp.
    void indexMembersByKey();
    // Places in `members`, ascending. AOTLink.cpp.
    std::span<const uint32_t> membersUnder(LayoutID classLayout, ClassSide, PropertyKeyID) const LIFETIME_BOUND;
    // As LinkFlow::findMembers() goes: the members of the nearest class that has any under the key, from the layout's own class upward. AOTLink.cpp.
    // Above a constructor function nothing is found for sure: a statement lays each name on its prototype object, and the link above that object. So there the answer is Nothing or Unknown.
    // It speaks of the prototype objects and the class objects alone. What may stand in front of them on an object of the layout, and whether the object still stands below them, its fields and LinkedLayout::possibleChanges say.
    MemberFound findMember(const Program&, LayoutID, ClassSide, PropertyKeyID) const;
    // Where findMember() cannot tell because a constructor function stands in the row: the functions whose closures are all that a lookup under the key may come to on the prototype objects above the objects of the layout.
    // It may come to none of them too, and then goes on above the classes. False: nobody can tell. AOTLink.cpp.
    bool findFunctionsLaidAbove(const Program&, LayoutID, PropertyKeyID, Vector<FunctionID, 4>&) const;
    // Ascending by holder and key.
    Vector<LinkedNameOnFunction> namesOnFunctions;
    const LinkedNameOnFunction* findNameOnFunction(FunctionID holder, PropertyKeyID) const LIFETIME_BOUND;
    // No closure of the function has the name for itself, and each stands where it was made.
    bool closuresLackName(FunctionID, PropertyKeyID) const;
};

// AOTLink.cpp. An operation in the text of the function may name a property by the symbol.
// The library keeps some fields under symbols of its own in a form of its own, which only its own text finds by name. Whoever else looks under such a symbol does so with a key that is a value,
// and the run time first calls what the library has registered for the holders of the field: PropertyCaches::prepareHoldersBeforeLookupsUnder().
bool isNameInTextOf(const Program&, PropertyKeyID symbol, FunctionID);
// AOTLink.cpp. A module of the library makes it.
bool isSymbolOfLibrary(const Program&, PropertyKeyID);

// AOTLink.cpp. Sized for the program, every entry at its default: what Link leaves with every mechanism withheld.
LinkTables conservativeLinkTables(const Program&);
LinkTables conservativeLinkTables(const Program&, const ParallelLoops&);

// AOTLink.cpp. By function, by the records alone, reached or not. In one realm.
BitVector bodiesThatLoaderEntersAtMostOnce(const Program&);

// AOTLink.cpp. By the record of the function alone: see LinkedFunction::ringsOfTailCallsAreListed too. False: every unit of it returns boxed values.
// Whoever constructs with a function asks whether the body has handed back an object, what a coroutine returns goes into its promise or its generator, and the loader enters the body of a module itself.
bool mayReturnInFormOfItsOwn(const Function&);
// AOTLink.cpp. No row of calls of the built-in returns what a callback gives, so nothing that it calls stands in its place: a call of it comes back. False claims nothing.
// Link's rings, the plan's ends of tail calls and the walk ask it: `return Reflect.apply(f, t, list)` is a jump to `f`.
bool callsOfBuiltinComeBack(const Program&, BuiltinID);

// AOTLink.cpp. What the language defines on the holder when a class is evaluated, whatever stands above it: `constructor` on the prototype object; `prototype`, `length` and `name` on the class object.
// No record lists it. So a lookup under such a name ends at the first class of the row: at a member that redefines the name there, or at that property. What a class further up declares is never come to.
bool classHolderIsMadeWithName(const Program&, ClassSide, PropertyKeyID);

// The flow that fills these tables: AOTLinkFlow.h.

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
