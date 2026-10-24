/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTRefusedContents.h"
#include "AOTSharedMemo.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// What the plan of an access takes from a loop over the layouts of its receiver, once per list of layouts and key.
class PropertyAccessCache {
    WTF_MAKE_NONCOPYABLE(PropertyAccessCache);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PropertyAccessCache);
public:
    using ListID = SharedLayoutLists::ListID;

    // A shorter list is looked through faster than it is looked up.
    AOT_CLASS_THRESHOLD(size_t, fewestLayouts, 8);
    static constexpr ListID noList = ~0ull;

    struct PartitionKey {
        ListID candidates { noList };
        PropertyKeyID key;
        Access access { Access::Read };
        bool isOwnLookup { false };
        const FlowResults* flow { nullptr };

        friend bool operator==(const PartitionKey&, const PartitionKey&) = default;
    };

    struct Partition {
        PartitionKey key;
        MemberPartition partition;
    };

    struct FieldArmKey {
        ListID layouts { noList };
        PropertyKeyID key;
        bool isPrivate { false };

        friend bool operator==(const FieldArmKey&, const FieldArmKey&) = default;
    };

    struct FieldArm {
        FieldArmKey key;
        FieldArmState state;
    };

    struct CreationKey {
        ListID layouts { noList };
        PropertyKeyID key;
        Access access { Access::Store };
        const FlowResults* flow { nullptr };

        friend bool operator==(const CreationKey&, const CreationKey&) = default;
    };

    struct Creation {
        CreationKey key;
        std::optional<CreationTests> tests;
    };

    struct DeclaredTypesKey {
        ListID layouts { noList };
        PropertyKeyID key;

        friend bool operator==(const DeclaredTypesKey&, const DeclaredTypesKey&) = default;
    };

    struct DeclaredTypes {
        DeclaredTypesKey key;
        bool areTheSame { false };
    };

    struct NarrowSlots {
        DeclaredTypesKey key;
        LayoutList layouts;
    };

    struct Optionality {
        DeclaredTypesKey key;
        bool isOptionalInSomeLayout { false };
        bool isOptionOfRecordInEveryLayout { false };
    };

    // All that provenAccessOf() takes from a named access and its function.
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

    struct ProvenNamedAccess {
        ProvenAccessKey key;
        ProvenAccess access;
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
    RefusedContents refusedContents;
    // Every answer that is given again is computed once more and compared.
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
