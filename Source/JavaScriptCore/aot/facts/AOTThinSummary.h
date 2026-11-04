/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTEffects.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTOpcode.h"
#include "AOTThreads.h"
#include "AOTType.h"
#include <memory>
#include <optional>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class Diagnostics;
class ParallelLoops;

namespace Typed {

class Program;

// What a function does, as seen from outside it. Nothing here speaks of a local: the scan eliminates them.
// A summary is a small graph: nodes, edges between them, and four kinds of records that Link can only resolve once it knows what arrives at a node:
// calls, accesses, operators, and changes of what an object is. Beside those: which members each class is given.

inline constexpr uint32_t noSummaryNode = UINT32_MAX;

// WHAT AN OBJECT OF ONE OF THE ENGINE'S CLASSES HOLDS LIES IN SLOTS: the elements of a list, the keys and values of a map, the value of a promise, an internal field, what a row of the table of built-ins lays under a name on a record that it makes.
// A slot is a number that means the same in every class.
class SlotOfContents {
public:
    constexpr SlotOfContents() = default;
    constexpr SlotOfContents(ContentsSlot slot)
        : m_number(static_cast<uint32_t>(slot))
    {
    }

    // What lies on the object under names that nobody knows: a store under a computed key, unseen code.
    static constexpr SlotOfContents underUnknownNames() { return behindEnumerators(UnderUnknownNames); }
    // Whatever lies on the object under a name. Every slot under a name flows into it, and so does the one above.
    static constexpr SlotOfContents underAnyName() { return behindEnumerators(UnderAnyName); }
    // Of the object that a coroutine hands its caller, and of no other: what the body yields, and what `return` hands over in its text. They speak of the body, whoever has the object: no family of sites has a node for them (slotSpeaksOfBodyOfCoroutine()).
    static constexpr SlotOfContents yieldedAlone() { return behindEnumerators(YieldedAlone); }
    static constexpr SlotOfContents returnedAlone() { return behindEnumerators(ReturnedAlone); }
    static constexpr SlotOfContents internalField(uint32_t index) { return fromNumber(firstInternalField + index); }
    // A slot of the table that is neither of the three nor an internal field.
    static constexpr SlotOfContents stateOf(BuiltinSlotID slot) { return fromNumber(firstStateSlot + slot.index()); }
    static constexpr SlotOfContents under(PropertyKeyID key) { return fromNumber(nameBit | key.index()); }
    static constexpr SlotOfContents fromNumber(uint32_t number)
    {
        SlotOfContents slot;
        slot.m_number = number;
        return slot;
    }

    constexpr uint32_t number() const { return m_number; }
    constexpr explicit operator bool() const { return m_number != UINT32_MAX; }
    constexpr bool isUnderName() const { return !!*this && (m_number & nameBit); }
    constexpr PropertyKeyID key() const { return PropertyKeyID(m_number & ~nameBit); }
    constexpr std::optional<ContentsSlot> contentsSlot() const { return m_number < numberOfEnumerators<ContentsSlot> ? std::optional(static_cast<ContentsSlot>(m_number)) : std::nullopt; }
    constexpr std::optional<uint32_t> indexOfInternalField() const { return m_number >= firstInternalField && m_number < firstStateSlot ? std::optional(m_number - firstInternalField) : std::nullopt; }

    friend constexpr bool operator==(SlotOfContents, SlotOfContents) = default;

private:
    // The slots that no enumerator of ContentsSlot names are numbered behind the enumerators, in this order. Whoever adds one adds it here, so the count below knows of it.
    enum SlotBehindEnumerators : uint32_t { UnderUnknownNames, UnderAnyName, YieldedAlone, ReturnedAlone, numberOfSlotsBehindEnumerators };
    static constexpr SlotOfContents behindEnumerators(SlotBehindEnumerators slot) { return fromNumber(numberOfEnumerators<ContentsSlot> + slot); }
    static constexpr uint32_t firstInternalField = 16;
    static_assert(numberOfEnumerators<ContentsSlot> + numberOfSlotsBehindEnumerators <= firstInternalField);
    static constexpr uint32_t firstStateSlot = 1u << 16;
    static constexpr uint32_t nameBit = 1u << 31;

    // UINT32_MAX: none.
    uint32_t m_number { UINT32_MAX };
};

struct SummaryNode {
    enum class Kind : uint8_t {
        Unknown, // Node 0 of every summary: any value at all, and whatever flows into it reaches code that nobody has seen.

        // ---- Seen from outside the function. Link has ONE node for each of these in the whole program.
        Parameter, // first: the index among the ENTRY parameters of this function (captures, this and the callee included, as Function::numberOfEntryParameters() counts)
        Result, // first: the index among the results
        Thrown,
        Capture, // first: the index among the captures. second: 0: of this function. Else FunctionID + 1: of the closure of that function which is made here.
        Variable, // first: VariableID
        Field, // first: LayoutID, second: the field. An environment is a layout. DATA FIELDS ONLY: the layout of a class does not list its methods.
        Member, // first: LayoutID of the class, second: the index that DefineMethod gives the member, with the top bit set for ClassSide::Static. What the prototype, or the class object, holds under that member's key.
        Contents, // first: the ClassInstance node of this summary, second: SlotOfContents::number(). What the objects that are made there hold in the slot.

        // ---- Sources: where values are made.
        Constant, // first: TypeID of the program's table. Primitives, built-ins. second: WrittenUnit::bits() where an allocation or constant data lists it as the value of a field. 0: nobody says which value it is.
        Allocation, // first: LayoutID. Exactly that layout. Of a namespace object: the one object of the module is named here.
        ClassInstance, // first: BuiltinClassID, second: which of the places in this body. An object of one of the engine's classes is made here: a list, a map, a promise, an arguments object. EACH IS AN ALLOCATION SITE: Link numbers them once for the program.
        Closure, // first: FunctionID. A class object is the closure of its constructor.
        Builtin, // first: BuiltinID, the canonical one. The object itself, as the realm has it from the start, whatever its holder has under the name by now.

        // ---- Local to the summary.
        Join, // What its incoming edges bring. Wherever a call, an access or a change needs one node for a value that has several origins.
        ResultOfCall, // first: the index in ThinSummary::calls, second: the index among its results
        ResultOfAccess, // first: the index in ThinSummary::accesses
        ResultOfOperator, // first: the index in ThinSummary::operators
        ResultOfConversion, // first: the index in ThinSummary::calls of the first call of a conversion to a primitive, whose calls stand in a row. What any of them answers.
    };

    Kind kind { Kind::Unknown };
    uint32_t first { 0 };
    uint32_t second { 0 };

    friend constexpr bool operator==(const SummaryNode&, const SummaryNode&) = default;
};

// `{ kind: "num", value }`: which text, integer or boolean is written as the value of a field. Link's flow knows kinds of primitives alone; WriterOfLinkLayouts reads this off the edge into the field.
class WrittenUnit {
public:
    enum class Kind : uint8_t { None, Text, Integer, Boolean };

    constexpr WrittenUnit() = default;
    static constexpr WrittenUnit text(StringID text) { return text.index() < payloadLimit ? WrittenUnit(Kind::Text, text.index()) : WrittenUnit(); }
    static constexpr WrittenUnit integer(int64_t value) { return value >= -static_cast<int64_t>(payloadLimit / 2) && value < static_cast<int64_t>(payloadLimit / 2) ? WrittenUnit(Kind::Integer, static_cast<uint32_t>(value + payloadLimit / 2)) : WrittenUnit(); }
    static constexpr WrittenUnit boolean(bool value) { return WrittenUnit(Kind::Boolean, value); }
    static constexpr WrittenUnit fromBits(uint32_t bits)
    {
        WrittenUnit unit;
        unit.m_bits = bits;
        return unit;
    }

    constexpr uint32_t bits() const { return m_bits; }
    constexpr explicit operator bool() const { return !!m_bits; }
    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> 30); }
    constexpr StringID text() const { return StringID(m_bits & (payloadLimit - 1)); }
    constexpr int64_t integer() const { return static_cast<int64_t>(m_bits & (payloadLimit - 1)) - payloadLimit / 2; }
    constexpr bool boolean() const { return m_bits & 1; }

    friend constexpr bool operator==(WrittenUnit, WrittenUnit) = default;

private:
    static constexpr uint32_t payloadLimit = 1u << 30;

    constexpr WrittenUnit(Kind kind, uint32_t payload)
        : m_bits(static_cast<uint32_t>(kind) << 30 | payload)
    {
    }

    uint32_t m_bits { 0 };
};

// What is at `from` may arrive at `to`.
struct SummaryEdge {
    uint32_t from { 0 };
    uint32_t to { 0 };
    // Null: all of it. Else only what is of this type: a test that the program wrote stands between.
    TypeID passes;
    // Of an edge into node 0: the operation runs the value, or code that it finds on the value, there and then. ThinSummary::ownEffects leaves that code out.
    // Any other edge into node 0 only lets the value go: what runs it later is unseen code, which some other record lets run.
    bool runsCodeOfValue { false };
};

// A PROTOCOL IS A CALL BY NAME. Where an operation converts an operand to a primitive, awaits it or iterates over it, the scan leaves a call by Name for each key that the protocol looks up,
// with what the key is looked up on as receiver, at the site of the operation. So one site may have several calls, and the calls need not stand in the order of their sites.
// Where only an object answers to the protocol (ToPrimitive, Thenable) the receiver is a Join that only objects pass into.

struct SummaryCall {
    enum class Callee : uint8_t {
        Binding, // `function`. `arguments` are its ENTRY parameters in order, captures, this and the callee included. `receiver` is noSummaryNode.
        Name, // `key` looked up on what is at `receiver`. `arguments` are positional.
        Value, // what is at `subject`. `arguments` are positional.
        Builtin, // `builtin`. `arguments` are positional. What it does with them, what it gives and whether that is fresh: its row in the table of built-ins.
    };

    ValueID site;
    Callee callee { Callee::Value };
    bool isConstruction { false };
    bool isInLoop { false };
    // A ReturnCall names it: the function returns what the call gives, at once, and the call may be a jump.
    bool isInTailPosition { false };
    // The arguments come in a list whose length is not known: `arguments` holds the one node of what is read out of the list.
    bool passesList { false };
    // By Name: a protocol looks the key up, and no call is written at the site. By Builtin: the walk enters the code behind the built-in for a protocol, and what that does to the values of the program is laid down beside the call.
    bool isMadeByProtocol { false };
    // What the call answers goes into one IteratorOpen, synchronous and no `yield*`, and nowhere else: no name is looked up on it, nothing keeps it, no block takes it.
    // And no iterator of the body is sent a value, thrown into or returned through, but those that a `yield*` has opened. False says nothing.
    bool resultIsOnlyIterated { false };
    // `table.get(keyOf(a, b))`: what the call gives is the first argument of a call by a name that Map.prototype or Set.prototype has. A guess at what pays: it claims nothing.
    bool resultIsReadAsKey { false };
    // No way leads from the operation to a Return of the body: whoever gets an answer from the function has not come by it. False claims nothing. Never of a call that a protocol makes.
    bool liesOnNoWayToReturn { false };
    FunctionID function;
    // By Value too, where the value is the function of a native binding. Such a call goes on as one of a value that nobody knows: Link asks the marks of the entry.
    BuiltinID builtin;
    // By Builtin: keyOfSymbolThatCallMakes(). Outside of loops the call gives the one symbol that the image holds under the key, and no other.
    PropertyKeyID key;
    uint32_t subject { noSummaryNode };
    // noSummaryNode: none is passed, so undefined arrives. Node 0: anything may.
    uint32_t receiver { noSummaryNode };
    // Of a construction. noSummaryNode: the callee itself.
    uint32_t newTarget { noSummaryNode };
    // By Binding: a Join that holds what has passed a test of the program and is an argument of the call. While nothing lies there no run makes the call, and Link does not enter the callee. noSummaryNode: the call waits for nothing.
    uint32_t waitsFor { noSummaryNode };
    // The node that takes what the call throws: SummaryHandler::caught of the handler that the block of `site` names. noSummaryNode: the Thrown node of the function.
    uint32_t thrownTo { noSummaryNode };
    // Into ThinSummary::arguments.
    uint32_t firstArgument { 0 };
    uint32_t numberOfArguments { 0 };
};

// A read or a store on an object that the body did not make itself, or made and let go.
// Where the body made the object and still has it alone, the scan lays a plain edge to or from the Field node in place of this.
// AN ACCESS TO CONTENTS (`slot`) looks no key up and runs nobody's code. Only an object of one of the engine's classes answers to it. Into a list that the operation itself makes, the scan lays a plain edge to the Contents node.
struct SummaryAccess {
    enum class Kind : uint8_t {
        Read, // `value` is the ResultOfAccess node
        Store, // PropSet: it may run a setter, and it may add the property
        Define, // PropDefine, a field initializer: it never runs a setter
        Has, // `in`, hasOwn: it reads no value
    };

    ValueID site;
    Kind kind { Kind::Read };
    // The object at `subject` is one that this body is making: what an allocation of the body gives, what `super()` answers in the constructor of a derived class, `this` of an initializer of fields.
    // Of a store: it stands in front of the first point at which the body lets that object leave. Handing it to an initializer of fields is no such point.
    // Of a read: the body has not defined the key on every way to the read, so the read may come in front of the definition that fills the field. So too of `this` of a plain function, which `new` may have handed it.
    bool isPartOfConstruction { false };
    // Of such a read: the object is what `super()` has answered. The constructors above are through with it, so it has what they fill.
    bool subjectIsAnsweredBySuperCall { false };
    // Of a store under `prototype`: what is stored is an object that the body has made for this store: beside it the body only stores into the object under names, and those stores stand in the summary as stores into what a read of this `prototype` gives.
    // So whoever follows that `prototype` by itself need not follow the object: `F.prototype = { .. }`, `F.prototype = Object.create(above)`.
    bool storesObjectMadeForIt { false };
    // Of a read under `prototype`: no code reads here. The scan has written it at the operation that makes such an object, and what it gives stands for that object.
    bool standsForObjectMadeAtSite { false };
    // As SummaryCall::liesOnNoWayToReturn.
    bool liesOnNoWayToReturn { false };
    uint32_t subject { 0 };
    // Of `super.x`, which looks the key up from `subject` upward: what an accessor that the lookup finds gets as `this`. noSummaryNode: the subject itself.
    uint32_t receiver { noSummaryNode };
    // Null: the key is computed, and `keyNode` is where it comes from. While all that lies there is one symbol that is made once, the access is one under the key of that symbol: LinkFlow::keyOf().
    PropertyKeyID key;
    uint32_t keyNode { noSummaryNode };
    // Of an access under a computed key: `emitter["on" + kind]`. The text that the key begins with in every run. Null: it may begin with anything.
    StringID beginningOfKey;
    uint32_t value { noSummaryNode };
    // Set: the access is to contents. `key` is null.
    SlotOfContents slot;
    // Of a read of the value of a promise, where the operation settles what it is handed (await, the resolution of a promise): what is no promise arrives here as it is.
    // Then `keyNode` is what `then` finds on those: where that may be called, the function hands on what nobody sees, so the read gives anything.
    uint32_t whatHoldsNothing { noSummaryNode };
    // Of such a read: `subject` is the ResultOfCall node of a call, and the operation stands right behind that call: nothing runs between the two.
    bool standsRightBehindCall { false };
    // As SummaryCall::thrownTo.
    uint32_t thrownTo { noSummaryNode };
    // Of a definition by a descriptor that a map holds: `Object.defineProperties(object, { key: { get() { .. }, set(value) { .. } } })`. It is written in the body, holds no value, and under `get` and `set` closures of these.
    // WHAT THE RECORD OF A CLASS EXPECTS ON ITS PROTOTYPE OBJECT (LinkedNameOnPrototype::expectedGetter), and nothing else.
    bool laysAccessorAsWritten { false };
    FunctionID writtenGetter;
    FunctionID writtenSetter;
    // The layout of the map, where it has one: whoever reads the name off an object of that layout has this descriptor in hand.
    LayoutID mapOfWrittenAccessor;
    // `{ key: accessors.key }`: the descriptor is not written here. Where it comes from.
    uint32_t descriptorFromElsewhere { noSummaryNode };
    // The definition that stands for `Object.defineProperty(subject, key, descriptor)`: ScanOfBody::defineByDescriptor().
    bool isByDescriptor { false };
    // Of such a definition: the descriptor is in sight (SummaryChange::descriptorIsInSight) and says `enumerable`, `configurable` and `writable` by constants.
    // The form of the property that it creates where the object lacks the name. Nothing: nobody says.
    std::optional<OptionSet<FieldForm>> formCreated;

    bool isToContents() const { return !!slot; }
    // `o.name = value`, and a definition that says what it lays: node 0 is unseen code, which stands for what a descriptor out of sight holds.
    bool laysValueInSightUnderName() const { return !isToContents() && (kind == Kind::Store || (kind == Kind::Define && value && value != noSummaryNode)); }
};

// An operation with an OperatorForm whose result may be a number: which primitives it gives hangs on which arrive at its operands. resultTypeOfOperator() of ir/AOTResultRules.h says which, for Link as for the walk.
// Nothing at an operand: nothing at the result. A record is left only where something outside the body comes to hold the result.
struct SummaryOperator {
    static constexpr uint32_t mostOperands = 2;

    ValueID site;
    Opcode opcode { Opcode::Add };
    // The ResultOfOperator node. Of ToThis a Join: the objects among its operand arrive there by an edge, and what stands in the place of the primitives by Link's word.
    uint32_t result { 0 };
    // By the schema's operand index. noSummaryNode: the operator has no such operand.
    uint32_t operands[mostOperands] { noSummaryNode, noSummaryNode };
    // The ResultOfConversion node of the operand: an object among it is converted, and the operator goes on with the primitives that lie here. noSummaryNode: the body shows that no object comes.
    uint32_t converted[mostOperands] { noSummaryNode, noSummaryNode };
};

// DefineMethod. The layout of a class lists its data fields only, so these records are the one place that says which methods, getters and setters a class has.
// The scan also lays the edge from `value` into the Member node.
struct SummaryMember {
    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MethodKind kind { MethodKind::Method };
    uint32_t index { 0 };
    // Null: the key is computed. Then no lookup by name on that class, or on a class below it, can be sure to miss.
    PropertyKeyID key;
    uint32_t value { 0 };
};

// Something other than a store that changes what an object is. Found by the operand uses of the rows and of the table of built-ins
// (deletesByKey, definesName with a descriptor, changesIntegrity, changesPrototype, becomesPrototype, assignsFrom), never by the text of a name.
struct SummaryChange {
    enum class Kind : uint8_t {
        Freezes, // freeze, seal, preventExtensions
        Deletes,
        DefinesByDescriptor, // it may lay an accessor or change attributes. `other`: the descriptor.
        ChangesPrototype, // of `subject`. `other`: the new prototype.
        BecomesPrototype, // `subject` becomes the prototype of something that is made here, or of `other`
        CopiesInto, // Object.assign, spread: every own property of `other` is stored into `subject`
    };

    ValueID site;
    Kind kind { Kind::Freezes };
    uint32_t subject { 0 };
    // Null: every key, or a computed one.
    PropertyKeyID key;
    uint32_t other { noSummaryNode };
    // Of DefinesByDescriptor: the operation names the function that it lays as a getter or as a setter (`{ get x() { .. } }`), and `other` is that function in place of a descriptor.
    enum class Lays : uint8_t { WhatDescriptorSays, Getter, Setter };
    Lays lays { Lays::WhatDescriptorSays };
    // `Object.defineProperty(object, key, { get: .. })`: the descriptor is written in the body and is handed over as it is written: what it holds under `get`. A GUESS AT WHO ANSWERS A READ. The descriptor escapes like any other.
    uint32_t getter { noSummaryNode };
    // As SummaryCall::liesOnNoWayToReturn.
    bool liesOnNoWayToReturn { false };
    // CopiesInto: each property is defined on `subject`, so no setter on it or above it takes a value: `{ ...other }`. False: it is assigned, or nobody says.
    bool definesWhatItCopies { false };
    // `Object.defineProperty(subject, "text", ..)`: the key is written, and it is no key of the program: no operation, layout, class or row names it. `key` is null, and it is the key of no field and of no member.
    bool isUnderNameThatNobodyElseNames { false };
    // `Object.defineProperty(subject, "name", { get: .., enumerable: true })`. A PROOF: the descriptor is made in the body, nobody but the body has had it when the call takes it, and the key is written.
    // The call reads six names off the descriptor and keeps nothing of it, so neither the descriptor nor the subject is handed to anybody: the change is of that one key.
    // What the descriptor holds under `value`, `get` and `set` the scan has handed to unseen code itself: ScanOfBody::layDescriptorsInSight().
    bool descriptorIsInSight { false };
    // Then: what it holds under `get` and under `set`. The engine runs either with the object as `this`, at no call that is written. noSummaryNode: the descriptor does not name it.
    uint32_t getterInSight { noSummaryNode };
    uint32_t setterInSight { noSummaryNode };
    // Then: it names `value` once, neither `get` nor `set`, says `enumerable` and `configurable` by constants and `writable: true`, the allocation fills it and it leaves at the call alone. THE DEFINITION UNDER THE NAME BRINGS THE VALUE ITSELF (SummaryAccess::value of the Define at the site),
    // and the scan has not handed it to unseen code: `valueInSight`: LINK DOES, wherever it does not take the definition for a store: observeDefinitionInLinkLayouts().
    bool laysValueInSight { false };
    uint32_t valueInSight { noSummaryNode };
};

// ThinSummary::halvesOfAccessorsNotLaid.
struct HalfOfAccessorInSummary {
    FieldReference field;
    bool isGetter { false };

    friend constexpr auto operator<=>(const HalfOfAccessorInSummary&, const HalfOfAccessorInSummary&) = default;
};

// WHICH FIELDS AN OBJECT HAS WHEN IT IS FIRST SEEN FROM OUTSIDE ITS CONSTRUCTION. An object leaves a body where it is returned, stored, captured, thrown, handed to a call or to a parameter of a block that several edges enter.
// A key is filled where the body defines it on every way from where it gets the object to every point at which the object leaves.
// An allocation under exactly one layout is a whole construction: ThinSummary::fieldsLeftEmpty lists what it does not fill. The object of a class is made by several bodies, each of which leaves one of these.
struct SummaryConstruction {
    enum class Object : uint8_t {
        Allocated, // By the constructor of a base class, under the prototype of new.target: the layout of the class or one below.
        AnsweredBySuperCall, // In the constructor of a derived class.
        This, // Of an initializer of fields, and of a plain function, which `new` hands the object.
    };

    struct Key {
        PropertyKeyID key;
        bool isFilled { false };
        // A read or an assignment names it on the object: above the object it may find an accessor, which gets the object as `this`.
        bool isLookedUp { false };
    };

    // An initializer of fields that is handed the object. That is no point at which the object leaves.
    struct Part {
        FunctionID function;
        // It has run on every way to every point at which the object leaves.
        bool runsOnEveryWay { false };
        // For the mark of the cell, no proof: on every way to every point at which the body is through with the object. AOTScanBirthsInlines.h.
        bool runsOnEveryWayToEnd { false };
    };

    Object object { Object::Allocated };
    // It leaves in some other way than as what the body returns.
    bool isSeenFromOutside { false };
    // Into ThinSummary::keysOfConstructions, ascending by key.
    uint32_t firstKey { 0 };
    uint32_t numberOfKeys { 0 };
    // For the mark of the cell, no proof. Into ThinSummary::keysFilledAtEnds, in no order: what the body has defined, or its allocation lists, on every way to every point at which it is through with the object, whoever has seen the object in between.
    uint32_t firstKeyFilledAtEnd { 0 };
    uint32_t numberOfKeysFilledAtEnd { 0 };
    // Into ThinSummary::partsOfConstructions, in the order in which they run.
    uint32_t firstPart { 0 };
    uint32_t numberOfParts { 0 };
};

enum class SummaryTrait : uint16_t {
    HasLoops = 1 << 0,
    Suspends = 1 << 1,
    UsesArguments = 1 << 2,
    HasDirectEval = 1 << 3,
    ListsKeys = 1 << 4,
    MayThrow = 1 << 5,
    // An operation of the body itself does. What a row of a built-in does under a key of its call is not meant.
    ReadsUnderComputedKey = 1 << 6,
    StoresUnderComputedKey = 1 << 7,
};

// `function make() { start(); const limit = read(); return run; function start() { .. } function run(n) { return n > limit; } }`: of the body that makes a scope, for its slots with a dead zone. Of some the scan
// cannot say by itself that nobody sees them empty: the scope has left in front of the store (AOTScanEnvironmentsInlines.h). Link finds from where each function over the scope may be entered: findOrderOfScopes().
// A block is named by its number in the order of dominators, in which all that a block stands above follows it in a row.
struct SummaryOrderOfScope {
    // A thing is a function, by its number; a slot of the scope, by its field and slotBit; or whoever is over the scope.
    static constexpr uint32_t slotBit = 1u << 31;
    static constexpr uint32_t anyone = UINT32_MAX;
    // From the point on the thing is in other hands than the body's: a closure of the function, or what was read out of the slot, is used for more than to be captured or laid into a slot of the scope; the function is called by binding.
    struct Entry {
        uint32_t thing { anyone };
        uint32_t block { 0 };
        uint32_t position { 0 };
    };
    // The first store into the slot. lastBlockBelow == aboveAll: the scan has found by itself that nobody sees the slot empty. It lets nothing empty arrive at the field then, which shows only where Link knows what else arrives:
    // of a slot that holds what unknown code made, the walk has the written type alone, and that admits the empty word.
    static constexpr uint32_t aboveAll = UINT32_MAX;
    struct Store {
        uint32_t field { 0 };
        uint32_t block { 0 };
        uint32_t lastBlockBelow { 0 };
        uint32_t position { 0 };
    };
    // Whoever has the holder has what is held: a closure of the function captures it, or it lies in the slot.
    struct Hold {
        uint32_t holder { anyone };
        uint32_t held { anyone };
    };

    LayoutID layout;
    // The scan says nothing of this scope. It says so, so that what another place says which makes a scope of the layout is not taken for all of them.
    bool isGivenUp { false };
    Vector<Entry> entries;
    Vector<Store> stores;
    Vector<Hold> holds;
};

// ThinSummary::otherNamesOfObjectsBeingMade.
struct OtherNameOfObjectBeingMade {
    ValueID parameter;
    ValueID object;
};

// `number`: of a variable or a function. `position`: a place in the stream of the values of the body, in halves: twice the number of an operation and one.
// The end of a block, with its terminator: twice the number behind its last operation. It lies in front of whatever the stream holds next, which has that number.
struct SummaryPlaceInBody {
    uint32_t number { 0 };
    uint32_t position { 0 };

    friend constexpr auto operator<=>(const SummaryPlaceInBody&, const SummaryPlaceInBody&) = default;
};

// `const pattern = /x/`, `const table = { a: 1 }`, `const helper = () => 1` in the body of a module that the loader enters once, outside of every loop.
// `made` makes an object out of nothing but the text of the program: a closure that captures nothing, the object of a literal, constant data. Nobody can tell when it is made.
// Behind casts, `store` alone takes it, a PutModuleVariable of a variable of the module.
struct SummaryDefinition {
    VariableID variable;
    ValueID made;
    ValueID store;
};

struct SummaryHandler {
    BlockID handler;
    // A Join. By edges: the operands of the Throws and Rethrows under the handler, and node 0, for what the engine throws by itself. By Link: what the calls and accesses under it throw.
    // CatchValue and CatchException of the block give what lies here.
    uint32_t caught { 0 };
};

// `JSON.parse(text) as Config`, `const config: Config = JSON.parse(text)`: what the call at `site` gives is parsed by the type. facts/AOTLinkStringParsers.h.
struct SummaryTypeParsed {
    ValueID site;
    TypeID type;
};

// There is no list of what escapes: a value escapes by an edge into node 0.
struct ThinSummary {
    Vector<SummaryNode> nodes;
    Vector<SummaryEdge> edges;
    // `if (node.kind === "num")`: the names under which the body reads what it then tells the holders apart by: NarrowByField. Each once.
    Vector<PropertyKeyID> keysOfTestedFields;
    Vector<SummaryCall> calls;
    Vector<uint32_t> arguments;
    Vector<SummaryAccess> accesses;
    Vector<SummaryOperator> operators;
    Vector<SummaryChange> changes;
    Vector<SummaryMember> members;
    Vector<SummaryConstruction> constructions;
    Vector<SummaryConstruction::Key> keysOfConstructions;
    Vector<SummaryConstruction::Part> partsOfConstructions;
    // Sorted, without repeats. Of a layout that is no record and no instance nothing is listed: no field of such a layout counts as filled.
    Vector<FieldReference> fieldsLeftEmpty;
    // For the mark of the cell, no proof. Of allocations under exactly one layout: WHERE THE END SAYS OTHERWISE THAN THE LEAVINGS. The body is through with such an object at its FinishObject and where it returns it. Both sorted, without repeats, and nearly always empty.
    // Among fieldsLeftEmpty, and defined at every end.
    Vector<FieldReference> fieldsFilledAtEndAlone;
    // Not among fieldsLeftEmpty for the sake of that object, and not defined at some end: `const o = { a }; o.b = 1; use(o)`.
    Vector<FieldReference> fieldsEmptyAtEndAlone;
    // `{ get total() { .. } }`, of allocations under exactly one layout that have left: under the name of the field the body has laid a half of an accessor on the object, and THIS half, as a closure that the body made for it,
    // did not lie there at some point at which the object left. A literal that lays a getter alone lists the setter. Sorted, without repeats.
    // A name that the object has left without is among fieldsLeftEmpty, and a value under the name shows as an edge or as a definition: of neither is anything said here.
    Vector<HalfOfAccessorInSummary> halvesOfAccessorsNotLaid;
    Vector<PropertyKeyID> keysFilledAtEnds;
    // What the allocations of the body list, as written, one allocation behind the other: a layout, a number of fields, and that many fields. forEachListOfAllocation().
    // An object lists its keys in the order in which it got them, and an allocation creates only a row of fields whose order the image holds, so the plan has to hear of each: plan/AOTKeyOrders.cpp.
    // A list that begins the order in which a layout without a base declares its fields is left out: the plan has that order anyway.
    // BEHIND WHAT AN ALLOCATION LISTS STAND THE MEMBERS THAT THE LITERAL DEFINES BEHIND IT, in their turn: `{ style: s, onDone: f }` lists nothing and defines two. A name that the layout does not list has no number yet
    // (Link appends it): its key stands there with keyInListOfAllocation set, and the plan asks SlotPlan::findField().
    static constexpr uint32_t keyInListOfAllocation = 1u << 31;
    Vector<uint32_t> fieldsListedByAllocations;
    Vector<LayoutID> classesEvaluated;
    // The allocations of environments in the blocks that the scan has entered, ascending: plan/AOTEnvironmentsHandedBack.cpp looks at what the body does with each.
    Vector<ValueID> environmentsMade;
    // Of those, the ones that the body hands to a function which it enters itself.
    Vector<ValueID> environmentsHandedToFunctionsEntered;
    Vector<SummaryOrderOfScope> ordersOfScopes;
    // The objects among the operands on which an operation of the body runs Protocol::OwnKeys or OwnValues: somebody sees in which order they got their keys. Sorted, without repeats.
    Vector<uint32_t> nodesWhoseKeysAreListed;
    // Ascending by `made`.
    Vector<SummaryDefinition> definitions;
    // WHATEVER THE FIELDS OF THE BODY'S OPERATIONS AND TERMINATORS NAME, by BodyValue::forEachReference(), so no operation and no field is left out: a function itself . the constructor of a layout . what a variable is born as .
    // the body of a module that is the target . each function whose closures a type names, that of an expected result too. Sorted, without repeats.
    // Code that is made for the body may mention a unit of each, and of nothing else that only the body says: LinkTables::referrers.
    Vector<FunctionID> functionsNamed;
    // No run enters them: the constants of the body decide the terminators that lead there (walk/AOTFoldingOfConstants.h). Ascending. The scan has not looked into them, so Link has heard of nothing that they alone name,
    // and the walk, which asks the same rules, builds nothing for them. Empty where Folding is withheld.
    Vector<BlockID> blocksNeverEntered;
    // The parameters of blocks under which the scan has gone on following an object that the body is making: ONE EDGE THAT A RUN CAN TAKE enters the block, however many are written. `object`: the value that the object is followed by.
    // The walk asks nobody else which parameter is such a name (AOTWalkTypedAccesses.cpp): of an object under it, what Link says to those outside its construction does not hold.
    Vector<OtherNameOfObjectBeingMade> otherNamesOfObjectsBeingMade;
    // The handler blocks that a block which the scan has entered names. Ascending by handler.
    Vector<SummaryHandler> handlers;
    // The modules that operations of those blocks name as their target. Sorted, without repeats. Code asks for each, if only code that never runs, so none of them is in the program for the host's sake.
    Vector<ModuleID> modulesNamedInBlocksNeverEntered;
    // An operation names the module as its target and gives a value that no node follows: a promise of the namespace object, what the module exports, a namespace object whose names the build does not know. Whoever has that reads the exports by name.
    Vector<ModuleID> modulesHandedOut;
    // Both of the body of a module without a table of exports, which the loader enters once. Each ascending by number, which stands once.
    // `var kState = Symbol("kState"); function stateOf(o) { return o[kState] }`: whoever can call stateOf() got it from the body behind the store, so it never finds what the variable held at first. Link says so: findVariablesStoredFirst().
    // The variables of the module that the body stores into on every way, in front of its own first read of them: the first such store.
    Vector<SummaryPlaceInBody> firstStoresOfBody;
    // The functions that the body names: the first operation that does more with the function than make a closure of it. In front of that nobody can call it.
    Vector<SummaryPlaceInBody> firstUsesOfFunctions;
    // Sorted, without repeats. The modules other than the function's own of which an operation names a variable or a function.
    Vector<ModuleID> otherModulesNamed;
    // `require` of the module gives a value that lies at the node, a Join, beside the unknown: what the module answers in the common run. Link lays it there: it is the result of another body, or a variable.
    struct AnswerOfRequire {
        ModuleID module;
        uint32_t node { 0 };
    };
    Vector<AnswerOfRequire> answersOfRequire;
    // WHAT IS HELD ALONE (Facts::held()): Link lays these where an escape flows outward only. The value of a public module of the library goes to unseen code, so beside each of them stands the unknown and nothing is ever proven of it.
    // `import events from "node:events"`: the Builtin that names the object of a compiled module gives what the body of the module has returned. As answersOfRequire.
    Vector<AnswerOfRequire> objectsOfCompiledModules;
    // `import { EventEmitter } from "node:events"`, in the summary of the body of the module: the loader reads the name off what the body has returned, into the variable. `read`: the ResultOfAccess node.
    struct NameThatLoaderTakes {
        uint32_t read { 0 };
        VariableID variable;
    };
    Vector<NameThatLoaderTakes> namesThatLoaderTakes;
    // Ascending by site. Of a call that may enter a built-in whose row has a typed result: the type of the record that the call names, or what the users of the result ask for.
    // Whether the call enters such a row shows in Link. Behind appendParsersOfTypesThatScanHasFound() a site stands once.
    Vector<SummaryTypeParsed> typesParsedAtCalls;
    // An operation walks up the chain of what lies at `subject`, or lists its keys. On a proxy either runs a trap, and ownEffects leaves that out: someInspectionMayRunCode() of AOTLinkUnionEffects.cpp.
    struct Inspection {
        enum class Kind : uint8_t { WalksChain, ListsKeys };

        ValueID site;
        Kind kind { Kind::WalksChain };
        uint32_t subject { 0 };
    };
    Vector<Inspection> inspections;

    void forEachListOfAllocation(NOESCAPE const Invocable<void(LayoutID, std::span<const uint32_t>)> auto& functor) const
    {
        std::span<const uint32_t> rest = fieldsListedByAllocations.span();
        while (!rest.empty()) {
            functor(LayoutID(rest[0]), rest.subspan(2, rest[1]));
            rest = rest.subspan(2 + rest[1]);
        }
    }
    // Of the body of a module alone, ascending: the operations whose part of ownEffects may run code that nobody knows. Whoever follows the order in which a program starts asks where that is.
    Vector<ValueID> sitesThatRunUnknownCode;
    // The calls of a conversion to a primitive are left once for each value, at the first operation in the order of the blocks that converts it. That need not be the first that runs: the code behind a loop may be numbered in front of the rest of a turn.
    // The other operations that convert the value, and the first of its calls. Whoever follows the order in which a program starts enters the calls at each.
    struct FurtherConversion {
        ValueID site;
        uint32_t firstCall { 0 };
    };
    Vector<FurtherConversion> furtherConversions;
    // The names that the operations of the body spell, drawn from the records above, and those of the stores that became edges. Each sorted, without repeats. Link reads none of them: AOTFactsOfProgram.cpp does.
    Vector<PropertyKeyID> namesRead;
    Vector<PropertyKeyID> namesCalled;
    Vector<PropertyKeyID> namesStored;
    // What the body itself may do, its calls and accesses left out. Link adds what those may run.
    // A default Effects is every effect: the scan starts from Effects::none().
    Effects ownEffects;
    // ---- AOTScanKnownEffectsInlines.h fills what follows, AOTLinkKnownEffectsInlines.h reads it. ownEffects LEAVES OUT what an access under a written key does: its record, or the call by name that stands for it, is Link's to judge.
    struct ParameterThatIsResult {
        uint32_t result { 0 };
        uint32_t entryParameter { 0 };
        // As SummaryEdge::passes.
        TypeID passes;
    };
    // What most bodies lack lies apart, behind one word: there are 293,000 summaries.
    class KnownEffectsApart {
    public:
        struct Lists {
            // What of ownEffects.writes is written by operations that lie on some way to a Return. Empty: all of it. Else one entry.
            Vector<LocationMasks> ownWritesOnWaysToReturn;
            // `(a, b) => a.rank - b.rank`: the declared bounds, united, of the operators that do less where plain primitives alone arrive at their operands. ownEffects leaves them out. Empty: there is none. Else one entry.
            Vector<Effects> ownEffectsOfOperators;
            // The nodes that the operands of those operators come from. Ascending, without repeats. A number, a boolean, undefined or null that is written is left out.
            Vector<uint32_t> operandsOfOperatorsLeftApart;
            // `function id(x) { return x }`: the results all of whose sources are entry parameters, an entry for each edge. Ascending by result and parameter. Of no coroutine, and of no body that has an arguments object or evaluates text.
            Vector<ParameterThatIsResult> parametersThatAreResults;
        };

        KnownEffectsApart() = default;
        KnownEffectsApart(KnownEffectsApart&&) = default;
        KnownEffectsApart& operator=(KnownEffectsApart&&) = default;
        KnownEffectsApart(const KnownEffectsApart& other)
            : m_lists(other.m_lists ? makeUniqueWithoutFastMallocCheck<Lists>(*other.m_lists) : nullptr)
        {
        }
        KnownEffectsApart& operator=(const KnownEffectsApart& other)
        {
            m_lists = other.m_lists ? makeUniqueWithoutFastMallocCheck<Lists>(*other.m_lists) : nullptr;
            return *this;
        }

        // Whoever fills a list reserves what it takes: a list that grows by itself begins with room for sixteen.
        Lists& ensureLists()
        {
            if (!m_lists)
                m_lists = makeUniqueWithoutFastMallocCheck<Lists>();
            return *m_lists;
        }
        // Null: the list is empty.
        const LocationMasks* ownWritesOnWaysToReturn() const LIFETIME_BOUND { return m_lists && !m_lists->ownWritesOnWaysToReturn.isEmpty() ? &m_lists->ownWritesOnWaysToReturn[0] : nullptr; }
        const Effects* ownEffectsOfOperators() const LIFETIME_BOUND { return m_lists && !m_lists->ownEffectsOfOperators.isEmpty() ? &m_lists->ownEffectsOfOperators[0] : nullptr; }
        std::span<const uint32_t> operandsOfOperatorsLeftApart() const LIFETIME_BOUND { return m_lists ? m_lists->operandsOfOperatorsLeftApart.span() : std::span<const uint32_t>(); }
        std::span<const ParameterThatIsResult> parametersThatAreResults() const LIFETIME_BOUND { return m_lists ? m_lists->parametersThatAreResults.span() : std::span<const ParameterThatIsResult>(); }

    private:
        std::unique_ptr<Lists> m_lists;
    };
    KnownEffectsApart knownEffectsApart;
    // bitOfLayoutInKnownEffects() of each layout in an object of which an operation without a record writes: an environment. objectsOfAnyLayout: a class object, a prototype object, a list that is only made longer, whatever nobody can name.
    uint64_t layoutsOfOwnWrites { 0 };
    // One of those operators joins two texts, and what comes out may be longer than a string can be.
    bool operatorsLeftApartMayJoinTexts { false };
    // What a callee has written when it throws may be written when the body returns.
    bool someBlockHasHandler { false };
    // SummaryCall::resultIsReadAsKey of one of `calls`.
    bool someResultIsReadAsKey { false };
    OptionSet<SummaryTrait> traits;
    // By entry parameter, up to 64: a call by value in a loop of the body calls it, behind casts and the program's own tests.
    uint64_t entryParametersCalledInLoops { 0 };
    uint32_t numberOfOperations { 0 };
    // What a copy of the body pays for: its operations but for constants, conversions to a boolean and reads of module variables, which fold or are one load; and one for every four cases of a switch. No proof: bytes and time go by it.
    uint32_t weightOfOperations { 0 };
    // AOTScanStrings.cpp. The written texts that an edge hands to a parameter of a block, that a Return hands back, and the cases of a SwitchString. Sorted, without repeats. The plan gives each a number: computeLiteralStrings().
    Vector<StringID> textsThatMeetOthers;
    // Text of the program alone. The written texts under which the table has a function whose call or read is an event: `emitter.rawListeners("constructor")`. Whoever is handed one may read under it. Sorted, without repeats.
    Vector<PropertyKeyID> textsThatNameEvents;
    // `const k = "kind"; node[k]`: the accesses under computed keys that the constants of the body make accesses under one name. The records above have them under that name. Ascending by site.
    struct AccessUnderConstantKey {
        ValueID site;
        PropertyKeyID key;
    };
    Vector<AccessUnderConstantKey> accessesUnderConstantKeys;
    // The modules whose path an operation of the body hands to the program or to the loader. Sorted, without repeats.
    Vector<ModuleID> modulesWhosePathIsRead;
    // A name that is made when the program runs may be that of any file. True too of a body that the scan has not read to its end.
    bool mayReadPathOfAnyModule { true };

    bool has(SummaryTrait trait) const { return traits.contains(trait); }
};

class ThinSummaries {
public:
    explicit ThinSummaries(size_t numberOfFunctions)
        : m_summaries(numberOfFunctions)
    {
    }

    // What takes the time is the first touch of each page, so every thread touches some.
    ThinSummaries(size_t numberOfFunctions, const ParallelLoops& loops)
    {
        m_summaries.reserveInitialCapacity(numberOfFunctions);
        std::span<uint8_t> storage = asMutableByteSpan(unsafeMakeSpan(m_summaries.mutableSpan().data(), numberOfFunctions));
        forEachRangeOfIndicesInParallel(loops, numberOfFunctions, [&](size_t first, size_t end) {
            zeroSpan(storage.subspan(first * sizeof(ThinSummary), (end - first) * sizeof(ThinSummary)));
        });
        m_summaries.grow(numberOfFunctions);
    }

    const ThinSummary& at(FunctionID id) const LIFETIME_BOUND { return m_summaries[id.index()]; }
    ThinSummary& at(FunctionID id) LIFETIME_BOUND { return m_summaries[id.index()]; }
    size_t size() const { return m_summaries.size(); }
    // On one thread, for a function without a body that the program has got behind the scan.
    void append(ThinSummary&& summary) { m_summaries.append(WTF::move(summary)); }

    // By StringID. Of the texts that are the name of a field which nothing stores (FieldFlags::IsNeverWritten): those that a String operation of some body or a node of the constant data spells, reached or not.
    // `const letters = ["a", "b"]; for (const name of letters) made[name] = 1`: such a text may come to be a key.
    const BitVector& textsSpelledThatNameFieldsNeverWritten() const LIFETIME_BOUND { return m_textsSpelledThatNameFieldsNeverWritten; }
    void setTextsSpelledThatNameFieldsNeverWritten(BitVector&& texts) { m_textsSpelledThatNameFieldsNeverWritten = WTF::move(texts); }

private:
    Vector<ThinSummary> m_summaries;
    BitVector m_textsSpelledThatNameFieldsNeverWritten;
};

// AOTScan.cpp. The first of the two visits of a function. By function, in parallel. It reads each body once and looks at no other function.
// With Mechanism::Folding withheld the walk keeps both sides of every branch, so the scan looks into every block.
ThinSummaries scanBodies(const Program&, Diagnostics&, const ParallelLoops&, Mechanisms = allMechanisms);
ThinSummary scanBody(const Program&, Diagnostics&, FunctionID, Mechanisms = allMechanisms);
// On one thread: a summary for each function that the program has got since the summaries were made, by ONE scan: what a scan sets up goes by the size of the program.
void scanBodiesAppendedToProgram(const Program&, Diagnostics&, ThinSummaries&, Mechanisms);

// AOTScan.cpp. `Symbol("text")` as it is written in the body of a module that the loader enters once, with a number whose key the program holds. Outside of loops it runs once: keyOfSymbolMadeOnceBy().
// `function make() { const tag = Symbol(); return class { [tag] = 1 } }` makes a name in each run, and the key of the field stands for them all: null.
// The scan and every unit that holds the call answer the same, or two symbols would go by one key: it goes by the records of the program and by the body. `definitionOf`: of a value of that body.
PropertyKeyID keyOfSymbolThatCallMakes(const Program&, FunctionID, const BodyValue& call, const ScopedLambda<BodyValue(ValueID)>& definitionOf);
// AOTScan.cpp. The symbol that the image holds under the key stands in the place of the call: Link types the answer by that and the walk puts it there. Both ask here, of the scan's record of the call:
// which edges lead back the scan has decided, by those that a run can take. Null: the call makes a symbol whenever it runs.
PropertyKeyID keyOfSymbolMadeOnceBy(const Program&, const SummaryCall&);
// AOTScan.cpp. A RequireModule of `target` that is written in `referrer` answers with what the body of `target` has returned, and with nothing else.
bool requireAnswersWithResultOfBody(const Program&, ModuleID referrer, ModuleID target);
// `function F() { this.x = .. }`: the layout of what `new F` makes, whose class object the function object is. ThinSummary::classesEvaluated lists it where a closure of F is made. Null: the function is no such function.
LayoutID classOfConstructorFunction(const Program&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
