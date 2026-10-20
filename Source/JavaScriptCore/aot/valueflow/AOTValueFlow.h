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
// A member of the universe: a kind of primitive, the unknown object, an allocation site, the closure of a function, a built-in, a constant key.
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
    PrivateField,
    Slot,
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
    // Not places in an object but what a protocol gives: only lookup() takes them.
    // What opening an iteration over the set gives, and the values that such an iteration hands out. The payload is the IteratorKind.
    OpenedIterators,
    IteratedValues,
    // What the methods of the iterators in the set answer. The payload is the ResumeMode.
    IteratorResults,
    AwaitedValue,
    // No value: the statement records that the member under a key, or every member, of the built-in holders in the set is written, or that every member is read.
    MemberWritten,
    EveryMemberWritten,
    EveryMemberRead,
};

// A store under a name of the first kind may be what a read under a name of the second kind finds although the two names differ: the read takes whatever lies under any name of that sort,
// or the store does not know its name. Names of one kind are the same only if their payloads are.
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
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::Slot:
        return written == ValueFlowNameKind::InternalFields;
    case ValueFlowNameKind::InternalFields:
        return written == ValueFlowNameKind::Slot;
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
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

// The value is mask & (the node's set | constant). An operand without a node holds the constant alone.
struct ValueFlowOperand {
    ValueFlowNodeID node;
    ValueSetID constant;
    ValueSetID mask;
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

// One member of the universe stands for objects that nobody follows. The mask of a type that lists every object inside it does not admit that member, yet any of the objects it lists may be one that the member stands for.
struct ValueFlowStandIn {
    ValueFlowObjectID member;
    // The objects that such masks list.
    ValueSetID covered;
};

// Where what belongs to one body starts in the lists of all bodies; it ends where the next body's starts.
struct ValueFlowBodyRange {
    uint32_t statement { 0 };
    // The statements in front of this one only look at their operands and may run before the function is reached.
    uint32_t statementLeavingBody { 0 };
    // From here on the statements of bodies that were copied into call sites of this one.
    uint32_t copiedStatement { 0 };
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
    // Of a call that has a copy of this function's body for itself: where this function is called, the operands go into the copy and the result comes out of it.
    FunctionID copiedCallee;
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
    // Arguments past the grouped ones, and what is spread: they may land at any parameter.
    ValueFlowNodeID otherArguments;
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;

    ValueFlowNodeID argument(unsigned index) const { return ValueFlowNodeID(firstArgument.index() + index); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
