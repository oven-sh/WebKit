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

    friend constexpr bool operator==(const ValueFlowName&, const ValueFlowName&) = default;
    friend constexpr auto operator<=>(const ValueFlowName&, const ValueFlowName&) = default;
};

inline constexpr ValueFlowName keyOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xffffffff };
inline constexpr ValueFlowName valueOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xfffffffe };
inline constexpr ValueFlowName sourcesOfEntriesPair { ValueFlowNameKind::HiddenSlot, 0xfffffffd };
inline constexpr ValueFlowName entriesPairsOfMap { ValueFlowNameKind::HiddenSlot, 0xfffffffc };
inline constexpr ValueFlowName describedObjectName { ValueFlowNameKind::HiddenSlot, 0xfffffffb };

inline constexpr ValueFlowName elementsName = ValueFlowName::of(ValueFlowNameKind::Elements);
inline constexpr ValueFlowName unknownKeyName = ValueFlowName::of(ValueFlowNameKind::UnknownKey);
inline constexpr ValueFlowName everyPropertyName = ValueFlowName::of(ValueFlowNameKind::EveryProperty);
inline constexpr ValueFlowName everyGetterName = ValueFlowName::of(ValueFlowNameKind::EveryGetter);
inline constexpr ValueFlowName copiedFromName = ValueFlowName::of(ValueFlowNameKind::CopiedFrom);
inline constexpr ValueFlowName prototypeLinkName = ValueFlowName::of(ValueFlowNameKind::PrototypeLink);
inline constexpr ValueFlowName chainWithoutCopiesOntoRealmName { ValueFlowNameKind::PrototypeLink, 1 };
inline constexpr ValueFlowName allocatedAsName = ValueFlowName::of(ValueFlowNameKind::AllocatedAs);
inline constexpr ValueFlowName awaitedValueName = ValueFlowName::of(ValueFlowNameKind::AwaitedValue);

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

struct ValueFlowStatement {
    ValueFlowStatementKind kind { ValueFlowStatementKind::Operation };
    Opcode opcode { };
    FunctionID function;
    ValueID site;
    uint32_t firstOperand { 0 };
    uint32_t operandCount { 0 };
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;
    uint32_t firstSite { UINT32_MAX };
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
