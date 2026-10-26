/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTRefusedContents.h"
#include "AOTSharedMemo.h"
#include "AOTThreshold.h"
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class PropertyAccessCache {
    WTF_MAKE_NONCOPYABLE(PropertyAccessCache);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PropertyAccessCache);
public:
    using ListID = SharedLayoutLists::ListID;

    AOT_CLASS_THRESHOLD(size_t, fewestLayouts, 8);
    static constexpr ListID noList = ~0ull;

    template<typename Key, typename Value>
    struct Answer {
        Key key;
        Value value;
    };

    struct PartitionKey {
        ListID candidates { noList };
        PropertyKeyID key;
        Access access { Access::Read };
        bool isOwnLookup { false };
        const FlowResults* flow { nullptr };

        friend bool operator==(const PartitionKey&, const PartitionKey&) = default;
    };

    using Partition = Answer<PartitionKey, MemberPartition>;

    struct FieldArmKey {
        ListID layouts { noList };
        PropertyKeyID key;
        bool isPrivate { false };

        friend bool operator==(const FieldArmKey&, const FieldArmKey&) = default;
    };

    using FieldArm = Answer<FieldArmKey, FieldArmState>;

    struct CreationKey {
        ListID layouts { noList };
        PropertyKeyID key;
        Access access { Access::Store };
        const FlowResults* flow { nullptr };

        friend bool operator==(const CreationKey&, const CreationKey&) = default;
    };

    using Creation = Answer<CreationKey, std::optional<TestsOfEmptySlot>>;

    struct DeclaredTypesKey {
        ListID layouts { noList };
        PropertyKeyID key;

        friend bool operator==(const DeclaredTypesKey&, const DeclaredTypesKey&) = default;
    };

    struct DeclaredTypes {
        DeclaredTypesKey key;
        bool areTheSame { false };
    };

    using NarrowSlots = Answer<DeclaredTypesKey, LayoutList>;

    struct Optionality {
        DeclaredTypesKey key;
        bool isOptionalInSomeLayout { false };
        bool isOptionOfRecordInEveryLayout { false };
    };

    struct ProvenAccessKey {
        Opcode opcode { Opcode::Undefined };
        bool isListedByAllocation { false };
        bool isLeftOutByUnfinishedAllocation { false };
        bool definesPropertyOfObjectMadeHere { false };
        PropertyKeyID key;
        LayoutID declaringLayout;
        TypeID receiver;
        TypeID stored;

        friend bool operator==(const ProvenAccessKey&, const ProvenAccessKey&) = default;
    };

    using ProvenNamedAccess = Answer<ProvenAccessKey, ProvenAccess>;

    struct InheritedMemberKey {
        LayoutID declaredClass;
        BuiltinClassID builtinClass;
        bool isOfInstances { false };
        PropertyKeyID key;
        Access access { Access::Read };
        const FlowResults* flow { nullptr };

        friend bool operator==(const InheritedMemberKey&, const InheritedMemberKey&) = default;
    };

    using InheritedMember = Answer<InheritedMemberKey, MemberAnswer>;

    struct CodeOfOperand {
        TypeID operand;
        OperandBehavior behavior;
        bool calledUnknownCode { false };
        EffectDescription added;
    };

    struct MethodsOfName {
        BuiltinName name;
        Vector<MemberOfClasses, 4> methods;
    };

    PropertyAccessCache() = default;

    SharedLayoutLists lists;
    SharedMemo<Partition> partitions;
    SharedMemo<FieldArm> fieldArms;
    SharedMemo<Creation> creations;
    SharedMemo<DeclaredTypes> declaredTypes;
    SharedMemo<NarrowSlots> narrowSlots;
    SharedMemo<Optionality> optionalities;
    SharedMemo<ProvenNamedAccess> provenAccesses;
    SharedMemo<MethodsOfName> methodsByName;
    SharedMemo<CodeOfOperand> codeOfOperands;
    SharedMemo<InheritedMember> inheritedMembers;
    RefusedContents refusedContents;
    bool checksAnswers { false };
};

template<typename Entry, typename Key>
const Entry& findOrCompute(SharedMemo<Entry>& memo LIFETIME_BOUND, const Key& key, uint32_t hash, NOESCAPE const Invocable<Entry()> auto& compute)
{
    auto isWanted = [&](const Entry& entry) {
        return entry.key == key;
    };
    if (const Entry* known = memo.find(hash, isWanted))
        return *known;
    return memo.add(hash, compute(), isWanted);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
