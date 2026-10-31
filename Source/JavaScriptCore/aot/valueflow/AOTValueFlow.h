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

using ValueSetID = ID<struct ValueSetIDTag>;
using ValueFlowObjectID = ID<struct ValueFlowObjectIDTag>;
using ValueFlowNodeID = ID<struct ValueFlowNodeIDTag>;
using ValueFlowStatementID = ID<struct ValueFlowStatementIDTag>;
using ValueFlowBehaviorID = ID<struct ValueFlowBehaviorIDTag>;

enum class ValueFlowNameKind : uint8_t {
    Property,
    Getter,
    Setter,
    Elements,
    UnknownKey,
    GetterUnderUnknownKey,
    SetterUnderUnknownKey,
    EveryProperty,
    EveryGetter,
    EverySetter,
    PrototypeLink,
    CopiedFrom,
    DeletedMember,
    DeletedMemberUnderUnknownKey,
    RedefinedMember,
    RedefinedMemberUnderUnknownKey,
    PrivateField,
    Slot,
    HiddenSlot,
    InternalFields,
    SteppedValues,
    ReturnedValue,
    SentValues,
    OpenedBy,
    ResolvedPromises,
    RejectedPromises,
    AllocatedAs,
    OpenedIterators,
    IteratedValues,
    IteratorResults,
    AwaitedValue,
    MemberWritten,
    EveryMemberWritten,
    EveryMemberRead,
    RestArgument,
    AddedNames,
    FoundThroughEscapedInstances,
    NamesOfMethods,
    NamespaceExports,
};

enum class ValueFlowNameSort : uint8_t { Value, Getter, Setter, Deletion, Redefinition, PrototypeLink, Copy, Other };
enum class ValueFlowNameReach : uint8_t { OneKey, Elements, UnknownKey, EveryKey, NoKey };

struct ValueFlowNameShape {
    ValueFlowNameSort sort;
    ValueFlowNameReach reach;

    constexpr bool isAccessor() const { return sort == ValueFlowNameSort::Getter || sort == ValueFlowNameSort::Setter; }
    constexpr bool isValueOrAccessor() const { return sort == ValueFlowNameSort::Value || isAccessor(); }
    constexpr bool mayBeAnyMember() const { return sort == ValueFlowNameSort::Copy || (reach == ValueFlowNameReach::UnknownKey && isValueOrAccessor()); }
};

constexpr ValueFlowNameShape shapeOf(ValueFlowNameKind kind)
{
    using Sort = ValueFlowNameSort;
    using Reach = ValueFlowNameReach;
    switch (kind) {
    case ValueFlowNameKind::Property:
        return { Sort::Value, Reach::OneKey };
    case ValueFlowNameKind::Getter:
        return { Sort::Getter, Reach::OneKey };
    case ValueFlowNameKind::Setter:
        return { Sort::Setter, Reach::OneKey };
    case ValueFlowNameKind::Elements:
        return { Sort::Value, Reach::Elements };
    case ValueFlowNameKind::UnknownKey:
        return { Sort::Value, Reach::UnknownKey };
    case ValueFlowNameKind::GetterUnderUnknownKey:
        return { Sort::Getter, Reach::UnknownKey };
    case ValueFlowNameKind::SetterUnderUnknownKey:
        return { Sort::Setter, Reach::UnknownKey };
    case ValueFlowNameKind::EveryProperty:
        return { Sort::Value, Reach::EveryKey };
    case ValueFlowNameKind::EveryGetter:
        return { Sort::Getter, Reach::EveryKey };
    case ValueFlowNameKind::EverySetter:
        return { Sort::Setter, Reach::EveryKey };
    case ValueFlowNameKind::PrototypeLink:
        return { Sort::PrototypeLink, Reach::NoKey };
    case ValueFlowNameKind::CopiedFrom:
        return { Sort::Copy, Reach::NoKey };
    case ValueFlowNameKind::DeletedMember:
        return { Sort::Deletion, Reach::OneKey };
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
        return { Sort::Deletion, Reach::UnknownKey };
    case ValueFlowNameKind::RedefinedMember:
        return { Sort::Redefinition, Reach::OneKey };
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
        return { Sort::Redefinition, Reach::UnknownKey };
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
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
    case ValueFlowNameKind::NamesOfMethods:
    case ValueFlowNameKind::NamespaceExports:
        return { Sort::Other, Reach::NoKey };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
    case ValueFlowNameKind::NamesOfMethods:
    case ValueFlowNameKind::NamespaceExports:
        return false;
    }
    return false;
}

struct ValueFlowName {
    ValueFlowNameKind kind { ValueFlowNameKind::Property };
    uint32_t payload { 0 };

    static constexpr ValueFlowName of(ValueFlowNameKind kind) { return { kind, 0 }; }
    static constexpr ValueFlowName property(PropertyKeyID key) { return { ValueFlowNameKind::Property, key.index() }; }
    static constexpr ValueFlowName getter(PropertyKeyID key) { return { ValueFlowNameKind::Getter, key.index() }; }
    static constexpr ValueFlowName setter(PropertyKeyID key) { return { ValueFlowNameKind::Setter, key.index() }; }
    static ValueFlowName property(const Program& program, PropertyKeyID key) { return key && program.isIndexLike(key) ? of(ValueFlowNameKind::Elements) : property(key); }
    static ValueFlowName getter(const Program& program, PropertyKeyID key) { return key && program.isIndexLike(key) ? of(ValueFlowNameKind::GetterUnderUnknownKey) : getter(key); }
    static ValueFlowName setter(const Program& program, PropertyKeyID key) { return key && program.isIndexLike(key) ? of(ValueFlowNameKind::SetterUnderUnknownKey) : setter(key); }

    friend constexpr bool operator==(const ValueFlowName&, const ValueFlowName&) = default;
    friend constexpr auto operator<=>(const ValueFlowName&, const ValueFlowName&) = default;
};

struct ValueFlowCellName : ValueFlowName {
    constexpr explicit ValueFlowCellName(ValueFlowNameKind kind, uint32_t payload = 0)
        : ValueFlowName { kind, payload }
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(shapeOf(kind).sort == ValueFlowNameSort::Other);
    }
};

inline bool namesElementByText(const Program& program, ValueFlowName name)
{
    ValueFlowNameShape shape = shapeOf(name.kind);
    PropertyKeyID key(name.payload);
    return shape.reach == ValueFlowNameReach::OneKey && shape.isValueOrAccessor() && key && program.isIndexLike(key);
}

inline constexpr ValueFlowCellName keyOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xffffffff };
inline constexpr ValueFlowCellName valueOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xfffffffe };
inline constexpr ValueFlowCellName sourcesOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xfffffffd };
inline constexpr ValueFlowCellName entriesPairsOfMap { ValueFlowNameKind::HiddenSlot, 0xfffffffc };
inline constexpr ValueFlowCellName describedObjectName { ValueFlowNameKind::HiddenSlot, 0xfffffffb };
inline constexpr ValueFlowCellName proxyItselfName { ValueFlowNameKind::HiddenSlot, 0xfffffffa };
inline constexpr ValueFlowCellName listedValuesOfProxyName { ValueFlowNameKind::HiddenSlot, 0xfffffff9 };
inline constexpr ValueFlowCellName descriptorsAnsweredByProxyName { ValueFlowNameKind::HiddenSlot, 0xfffffff8 };

inline constexpr ValueFlowName elementsName = ValueFlowName::of(ValueFlowNameKind::Elements);
inline constexpr ValueFlowName unknownKeyName = ValueFlowName::of(ValueFlowNameKind::UnknownKey);
inline constexpr ValueFlowName everyPropertyName = ValueFlowName::of(ValueFlowNameKind::EveryProperty);
inline constexpr ValueFlowName everyOwnPropertyName { ValueFlowNameKind::EveryProperty, 3 };
inline constexpr ValueFlowName everyGetterName = ValueFlowName::of(ValueFlowNameKind::EveryGetter);
inline constexpr ValueFlowName copiedFromName = ValueFlowName::of(ValueFlowNameKind::CopiedFrom);
inline constexpr ValueFlowName prototypeLinkName = ValueFlowName::of(ValueFlowNameKind::PrototypeLink);
inline constexpr ValueFlowName chainWithoutCopiesOntoRealmName { ValueFlowNameKind::PrototypeLink, 1 };
inline constexpr ValueFlowCellName allocatedAsName { ValueFlowNameKind::AllocatedAs };
inline constexpr ValueFlowCellName allocatedWithHolesName { ValueFlowNameKind::AllocatedAs, 1 };
inline constexpr ValueFlowCellName awaitedValueName { ValueFlowNameKind::AwaitedValue };

enum class ValueFlowMaskKind : uint8_t {
    Plain,
    Checked,
    Declared,
    Expected,
};

struct ValueFlowPlaceholders {
    ValueFlowObjectID untracked;
    ValueFlowObjectID unknown;
    ValueSetID objectsWithLayoutOfProgram;
    ValueSetID closures;
};

enum class ValueFlowUnknownSource : uint8_t {
    UndescribedNativeResult,
    UnseenCode,
    UncheckedConversionOfUntracked,
    DataFromOutside,
    ThrownByRunTime,
    ParameterOfEscapedFunction,
    ContentsOfEscapedObject,
    ResultOfUnknownCallee,
    ObjectWithoutLayout,
};

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
    ValueSetID familyObjects;
};

struct ValueFlowBodyRange {
    uint32_t statement { 0 };
    uint32_t statementLeavingBody { 0 };
    uint32_t edgeOnceReached { 0 };
    uint32_t seedOnceReached { 0 };
    uint32_t functionEntered { 0 };
};

enum class ValueFlowStatementKind : uint8_t {
    Operation,
    Lookup,
    CallGroup,
    Behavior,
    Escape,
    ModuleEvaluation,
    Coroutine,
    HostRoots,
    UsedBuiltins,
    UnseenCode,
    ModulesLoadedByName,
};

// What rules ask of the text of an operation. It is the same in every round, so it is read once, and no rule opens the body of the function for it.
enum class ValueFlowFactOfText : uint8_t {
    HasSecondOperand = 1 << 0,
    IsFreeName = 1 << 1,
    HasRareFacts = 1 << 2,
    HasCopyOfOperation = 1 << 3,
    // Of an ElemGet: `object[name]` in `for (name in object)`.
    ReadsUnderNameOfEnumerationOfSameObject = 1 << 4,
    // Of an ElemSet: objectsReadUnderKeyOfStore() finds one object, or two. They stand behind the operands of the statement.
    StoresWhatOneObjectHoldsUnderSameKey = 1 << 5,
    StoresWhatTwoObjectsHoldUnderSameKey = 1 << 6,
    // Of an IteratorResume: the step of `...rest` in a pattern, which runs once for each place from its own on.
    StepsRestElement = 1 << 7,
};

// Of the few operations of which one of these holds, in the order of functions and sites.
struct ValueFlowRareFactsOfText {
    FunctionID function;
    ValueID site;
    // entryNamedByOperations() of the callee of a call, or of a PropGet itself.
    BuiltinID entryNamed;
    // Of a call whose statement has no key: the member that a longer look at the text finds.
    PropertyKeyID keyOfMemberCalledOnItsHolder;
    // Of an ElemGet or an ElemSet: `object["$" + name]`: the text that is written in front of the rest of the key.
    StringID textThatKeyBeginsWith;
    std::optional<uint8_t> positionInPair { };
};

// An operation whose rule reads what the operation itself says: a layout and its fields, a module, the kind of an iterator. A Value holds all of that itself. In the order of functions and sites.
struct ValueFlowCopyOfOperation {
    FunctionID function;
    ValueID site;
    Value operation;
};

struct ValueFlowStatement {
    ValueFlowStatementKind kind { ValueFlowStatementKind::Operation };
    OptionSet<ValueFlowFactOfText> factsOfText;
    Opcode opcode { };
    FunctionID function;
    ValueID site;
    uint32_t firstOperand { 0 };
    uint32_t operandCount { 0 };
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;
    uint32_t firstSite { UINT32_MAX };
    // Of a PropGet: what it reads is tested, or is the callee of a call on the object it was read from, and goes nowhere else.
    bool isOnlyCalledOnItsHolder { false };
    // Of a ModuleNamespace: the object is tested, or is the receiver of a call, and goes nowhere else.
    bool isOnlyReceiverOfCalls { false };
    // Of a PropGet or an ElemGet: what it reads is tested, or a member is read off it of which the same holds, and nothing else is done with it.
    bool isOnlyTested { false };
    // What the tests in front of its block let through. Where one of these holds nothing the statement never runs.
    uint8_t guardCount { 0 };
    uint32_t firstGuard { 0 };
    // Four bits for each of the first eight operands: bit g says that the operand is the very value that guard g tested for the type of its cell, which never changes. The rules see it as the test lets it through.
    uint32_t guardsOfOperands { 0 };
    // Of an operation that names a key: that key. Of a call of a member read from its receiver: the key of that member.
    PropertyKeyID key;
};

enum class ValueFlowCallKind : uint8_t {
    Call,
    Construct,
    Get,
    Set,
    CallByNativeCode,
    ConstructByNativeCode,
    CallByHost,
};

constexpr bool isConstruction(ValueFlowCallKind kind)
{
    return kind == ValueFlowCallKind::Construct || kind == ValueFlowCallKind::ConstructByNativeCode;
}

inline constexpr unsigned numberOfGroupedArguments = 8;

struct ValueFlowCallGroup {
    ValueFlowNodeID thisValue;
    ValueFlowNodeID newTarget;
    ValueFlowNodeID firstArgument;
    ValueFlowNodeID otherArguments;
    ValueFlowNodeID laterArguments;
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;

    ValueFlowNodeID argument(unsigned index) const { return ValueFlowNodeID(firstArgument.index() + index); }
    ValueFlowNodeID argumentAt(unsigned position) const { return position < numberOfGroupedArguments ? argument(position) : laterArguments; }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
