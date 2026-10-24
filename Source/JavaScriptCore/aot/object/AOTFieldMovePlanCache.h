/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFieldMoves.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <array>
#include <memory>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct State;

enum class FieldMovePlanPurpose : uint8_t { CloneAs, CopyFields, CopyAsOrdinaryObject };
static constexpr unsigned numberOfFieldMovePlanPurposes = 3;
static_assert(static_cast<unsigned>(FieldMovePlanPurpose::CopyAsOrdinaryObject) + 1 == numberOfFieldMovePlanPurposes);

struct FieldMovePlanKey {
    FieldMovePlanPurpose purpose { FieldMovePlanPurpose::CloneAs };
    uint64_t sourceWords { 0 };
    uint64_t targetWords { 0 };
    Structure* sourceStructure { nullptr };
    BitVector replacedFields;

    friend bool operator==(const FieldMovePlanKey&, const FieldMovePlanKey&) = default;
};

class FieldMovePlanCache {
    WTF_MAKE_NONCOPYABLE(FieldMovePlanCache);
public:
    explicit FieldMovePlanCache(State&);
    ~FieldMovePlanCache();

    const FieldMovePlan* find(const FieldMovePlanKey&);
    const FieldMovePlan& add(VM&, FieldMovePlanKey&&, std::unique_ptr<FieldMovePlan>&&);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    struct Entry {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Entry);

        FieldMovePlanKey key;
        std::unique_ptr<FieldMovePlan> plan;
    };

    static constexpr unsigned numberOfSets = 256;
    static constexpr unsigned waysPerSet = 4;

    struct Set {
        std::array<std::unique_ptr<Entry>, waysPerSet> ways;
        uint8_t nextVictim { 0 };
    };
    using Sets = std::array<Set, numberOfSets>;

    static unsigned hashOf(const FieldMovePlanKey&);
    Set& setFor(const FieldMovePlanKey&);

    std::unique_ptr<Sets> m_sets;
    std::array<const Entry*, numberOfFieldMovePlanPurposes> m_lastFound { };
    RememberedSlotSet m_writes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
