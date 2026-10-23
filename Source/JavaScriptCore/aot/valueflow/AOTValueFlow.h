/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTIR.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// What the parts of the analysis share: the bodies make statements, the rules run them, the solver holds the values.

// An interned, immutable set of members of the universe. Equal sets have equal ids.
using ValueSetID = ID<struct ValueSetIDTag>;
// A member of the universe: a kind of primitive, one of the two placeholders, an allocation site, the closure of a function, a built-in, a constant key.
using ValueFlowObjectID = ID<struct ValueFlowObjectIDTag>;
using ValueFlowNodeID = ID<struct ValueFlowNodeIDTag>;
using ValueFlowStatementID = ID<struct ValueFlowStatementIDTag>;
// One way in which the table of built-ins lets native code look at an operand.
using ValueFlowBehaviorID = ID<struct ValueFlowBehaviorIDTag>;

enum class ValueFlowNameKind : uint8_t {
    Property,
    Getter,
    Setter,
    Elements,
    // What is stored or defined under a key that the analysis does not know.
    UnknownKey,
    GetterUnderUnknownKey,
    SetterUnderUnknownKey,
    // Whatever lies under any name.
    EveryProperty,
    EveryGetter,
    EverySetter,
    PrototypeLink,
    // The objects whose own properties were copied onto this one.
    CopiedFrom,
    // Something, once a member of a built-in holder may have been deleted under the key, or under a key that the analysis does not know.
    DeletedMember,
    DeletedMemberUnderUnknownKey,
    // Something, once a property of an object of the program may have been given another form under the key, or under a key that the analysis does not know.
    RedefinedMember,
    RedefinedMemberUnderUnknownKey,
    PrivateField,
    Slot,
    // A slot that no code reads out of the object, whoever holds the object: what a bound function was made of.
    HiddenSlot,
    // An internal field that is named by its number, which may be any slot of the object.
    InternalFields,
    // What a built-in iterator or a generator hands out step by step, what the body of a generator returns, and what a step hands in.
    SteppedValues,
    ReturnedValue,
    SentValues,
    // Of a container that stands for a built-in iterator over it: the method that would have made the iterator.
    OpenedBy,
    // Of a function that native code has made for the executor of a promise: the promises that a call of it resolves, or rejects.
    ResolvedPromises,
    RejectedPromises,
    // Of an object that a built-in has allocated: the prototype object of each class as whose instance one has allocated it, and the object itself where an allocation names no such class.
    AllocatedAs,
    // Not places in an object but what a protocol gives: only lookup() takes them.
    OpenedIterators,
    IteratedValues,
    // What the methods of the iterators in the set answer. The payload is the ResumeMode.
    IteratorResults,
    AwaitedValue,
    // No value: the statement records that the member under a key, or every member, of the built-in holders in the set is written, or that every member is read.
    MemberWritten,
    EveryMemberWritten,
    EveryMemberRead,
    // Of the array of a rest parameter that its function only spreads into one call: what the callers have passed, by the index at which it lies in the array.
    RestArgument,
    // The names under which a store or a definition has added a property to an object of the program that its layout does not declare: key constants, and the kinds of strings and symbols where nobody knows the name.
    AddedNames,
    // No value, and only lookup() takes it: the statement does what code that nobody compiles does with what it finds above the escaped instances in the set.
    FoundThroughEscapedInstances,
};

// A store under a name of the first kind may be what a read under a name of the second kind finds although the two names differ: the read takes whatever lies under any name of that sort, or the store does not know its name.
constexpr bool mayBeTheSameName(ValueFlowNameKind written, ValueFlowNameKind read)
{
    switch (read) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
        return written == ValueFlowNameKind::UnknownKey;
    case ValueFlowNameKind::Getter:
        return written == ValueFlowNameKind::GetterUnderUnknownKey;
    case ValueFlowNameKind::Setter:
        return written == ValueFlowNameKind::SetterUnderUnknownKey;
    case ValueFlowNameKind::EveryProperty:
        return written == ValueFlowNameKind::Property || written == ValueFlowNameKind::Elements || written == ValueFlowNameKind::UnknownKey || written == ValueFlowNameKind::Slot
            || written == ValueFlowNameKind::InternalFields || written == ValueFlowNameKind::SteppedValues || written == ValueFlowNameKind::ReturnedValue;
    case ValueFlowNameKind::EveryGetter:
        return written == ValueFlowNameKind::Getter || written == ValueFlowNameKind::GetterUnderUnknownKey;
    case ValueFlowNameKind::EverySetter:
        return written == ValueFlowNameKind::Setter || written == ValueFlowNameKind::SetterUnderUnknownKey;
    case ValueFlowNameKind::DeletedMember:
        return written == ValueFlowNameKind::DeletedMemberUnderUnknownKey;
    case ValueFlowNameKind::RedefinedMember:
        return written == ValueFlowNameKind::RedefinedMemberUnderUnknownKey;
    case ValueFlowNameKind::Slot:
        return written == ValueFlowNameKind::InternalFields;
    case ValueFlowNameKind::InternalFields:
        return written == ValueFlowNameKind::Slot;
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return false;
    }
    return false;
}

// What a read or a store through a set of objects names.
struct ValueFlowName {
    ValueFlowNameKind kind { ValueFlowNameKind::Property };
    uint32_t payload { 0 };

    static constexpr ValueFlowName of(ValueFlowNameKind kind) { return { kind, 0 }; }
    static constexpr ValueFlowName property(PropertyKeyID key) { return { ValueFlowNameKind::Property, key.index() }; }
    static constexpr ValueFlowName getter(PropertyKeyID key) { return { ValueFlowNameKind::Getter, key.index() }; }
    static constexpr ValueFlowName setter(PropertyKeyID key) { return { ValueFlowNameKind::Setter, key.index() }; }

    friend constexpr bool operator==(const ValueFlowName&, const ValueFlowName&) = default;
    friend constexpr auto operator<=>(const ValueFlowName&, const ValueFlowName&) = default;
};

// Hidden slots that are no slots of the table, whose numbers count up from zero.
inline constexpr ValueFlowName keyOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xffffffff };
inline constexpr ValueFlowName valueOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xfffffffe };
inline constexpr ValueFlowName sourcesOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xfffffffd };
// Of a Map: the pairs out of which its constructor has taken its keys and values.
inline constexpr ValueFlowName entriesPairsOfMap { ValueFlowNameKind::HiddenSlot, 0xfffffffc };

// What a mask does to the two placeholders, and to an object that it does not list.
enum class ValueFlowMaskKind : uint8_t {
    // A set that a rule asks for. Whatever it does not list is left out.
    Plain,
    // A test at run time stands behind it: the arm of a type test, an operation that goes on only with what passes its test.
    Checked,
    // The type of a position or of a value with no test in front of it.
    Declared,
    // The type that a value has while the checks in front of it pass: of a Check, and of what is typed by the result of one.
    Expected,
};

// Two members of the universe stand for objects that no set lists one by one.
struct ValueFlowPlaceholders {
    // An object or a function that compiled code has allocated and that the analysis has stopped following.
    ValueFlowObjectID untracked;
    // Anything: a value from code that nobody compiles or from an unsound edge, and any object that was handed to such code.
    ValueFlowObjectID unknown;
    // What the families of types are made of.
    ValueSetID objectsWithLayoutOfProgram;
    ValueSetID closures;
};

// Where the unknown object comes into being. Whoever brings it into being names one of these.
enum class ValueFlowUnknownSource : uint8_t {
    // An overload of the table without a declaration, a native binding, a property of a host object that the table does not declare.
    UndescribedNativeResult,
    // Evaluated text, a native add-on.
    UnseenCode,
    UncheckedConversionOfUntracked,
    // JSON.parse without a typed parser, a file outside the image.
    DataFromOutside,
    // A stack overflow, the TypeError of a heap operation: what `catch (e)` sees.
    ThrownByRunTime,
    ParameterOfEscapedFunction,
    ContentsOfEscapedObject,
    // What a callee answers that nobody knows.
    ResultOfUnknownCallee,
    // The construction of a plain function without a layout, the prototype object of such a function, the raw strings of a template.
    ObjectWithoutLayout,
    // An overload whose result type nothing proves: beside what is declared.
    DeclaredNativeResult,
};

// The value is what the mask lets through of (the node's set | constant). An operand without a node holds the constant alone.
struct ValueFlowOperand {
    ValueFlowNodeID node;
    ValueSetID constant;
    ValueSetID mask;
    ValueFlowMaskKind maskKind { ValueFlowMaskKind::Plain };
};

struct ValueFlowEdge {
    ValueFlowNodeID from;
    ValueFlowNodeID to;

    friend constexpr bool operator==(const ValueFlowEdge&, const ValueFlowEdge&) = default;
    friend constexpr auto operator<=>(const ValueFlowEdge&, const ValueFlowEdge&) = default;
};

struct ValueFlowNodeSet {
    ValueFlowNodeID node;
    ValueSetID set;
};

struct ValueFlowNodeMask {
    ValueFlowNodeID node;
    ValueSetID mask;
    ValueFlowMaskKind kind { ValueFlowMaskKind::Plain };
    // What takes the place of a placeholder. None where a placeholder that the mask admits passes as itself.
    ValueSetID familyObjects;
};

// Where what belongs to one body starts in the lists of all bodies; it ends where the next body's starts.
struct ValueFlowBodyRange {
    uint32_t statement { 0 };
    // The statements in front of this one only look at their operands and may run before the function is reached.
    uint32_t statementLeavingBody { 0 };
    uint32_t edgeOnceReached { 0 };
    uint32_t seedOnceReached { 0 };
    uint32_t functionEntered { 0 };
};

enum class ValueFlowStatementKind : uint8_t {
    // An operation of the IR. Its operands are the operation's, in the operation's order.
    Operation,
    // The statements below are made by rules, once per key.
    Lookup,
    CallGroup,
    Behavior,
    Escape,
    // Made by the bodies. What evaluating a module does beside running its body: the statement lies in that body.
    ModuleEvaluation,
    // What goes between the body of an async function or a generator and the promise or the generator that its callers get: the statement lies in that body.
    Coroutine,
    // In a function that only tests its arguments, and in the typed body that it calls.
    EntryChecks,
    // In the host's body, one of each.
    HostRoots,
    UsedBuiltins,
    UnseenCode,
    ModulesLoadedByName,
};

struct ValueFlowStatement {
    ValueFlowStatementKind kind { ValueFlowStatementKind::Operation };
    Opcode opcode { };
    FunctionID function;
    ValueID site;
    uint32_t firstOperand { 0 };
    uint32_t operandCount { 0 };
    // None where nobody reads it.
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;
    // Where the sites of the operation start in the list of the universe.
    uint32_t firstSite { UINT32_MAX };
};

enum class ValueFlowCallKind : uint8_t {
    Call,
    Construct,
    // An accessor that a read or a store has found. A getter takes no argument, a setter one.
    Get,
    Set,
    // A call that no operation of the program spells out: native code calls a callback, runs a protocol or goes through a bound function. A function is entered exactly as by Call and Construct.
    CallByNativeCode,
    ConstructByNativeCode,
    // A call that native code makes where no call of the program is under way, with operands that a record of the table lists. No statement of the program leads to what is entered, so it is no seen code for that.
    CallByHost,
};

constexpr bool isConstruction(ValueFlowCallKind kind)
{
    return kind == ValueFlowCallKind::Construct || kind == ValueFlowCallKind::ConstructByNativeCode;
}

inline constexpr unsigned numberOfGroupedArguments = 8;

// What every call of one set of callees shares. A call site has one edge per operand into it and one out of it.
struct ValueFlowCallGroup {
    ValueFlowNodeID thisValue;
    ValueFlowNodeID newTarget;
    ValueFlowNodeID firstArgument;
    // Arguments whose position nobody knows, as what is spread: they may land at any parameter.
    ValueFlowNodeID otherArguments;
    // Arguments at the positions past the grouped ones: they land at no parameter in front of those positions.
    ValueFlowNodeID laterArguments;
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;

    ValueFlowNodeID argument(unsigned index) const { return ValueFlowNodeID(firstArgument.index() + index); }
    ValueFlowNodeID argumentAt(unsigned position) const { return position < numberOfGroupedArguments ? argument(position) : laterArguments; }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
