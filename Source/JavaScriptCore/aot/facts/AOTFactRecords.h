/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTDiagnostics.h"

#include "AOTID.h"
#include "AOTLayout.h"
#include "AOTObjects.h"
#include "AOTOpcode.h"
#include "AOTSchemaEnums.h"
#include "AOTSchemaRecords.h"
#include "AOTTypeArena.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr Witness noWitness { Wire::none, Wire::none, noSourceLocation };

constexpr bool hasFunction(const Witness& witness) { return witness.function != Wire::none; }

constexpr bool isLowerWitness(const Witness& witness, const Witness& other)
{
    if (!hasFunction(witness))
        return false;
    return !hasFunction(other) || std::tie(witness.function, witness.value) < std::tie(other.function, other.value);
}

template<typename Row>
struct FactRows {
    Vector<uint32_t> firstRowOfOwner;
    Vector<Row> rows;

    uint32_t numberOfOwners() const { return firstRowOfOwner.isEmpty() ? 0 : static_cast<uint32_t>(firstRowOfOwner.size()) - 1; }

    std::span<const Row> of(uint32_t owner) const LIFETIME_BOUND
    {
        if (owner >= numberOfOwners())
            return { };
        return rows.span().subspan(firstRowOfOwner[owner], firstRowOfOwner[owner + 1] - firstRowOfOwner[owner]);
    }

    template<typename Tag>
    std::span<const Row> of(ID<Tag> owner) const LIFETIME_BOUND { return owner ? of(owner.index()) : std::span<const Row> { }; }
};

template<typename Row, typename Key, typename Member>
const Row* findFactRow(std::span<const Row> ascendingRows, Key key, Member Row::* member)
{
    auto found = std::ranges::lower_bound(ascendingRows, key, { }, member);
    return found != ascendingRows.end() && (*found).*member == key ? &*found : nullptr;
}

template<typename Record>
Record factRecordAt(const Vector<Record>& records, uint32_t index)
{
    return index < records.size() ? records[index] : Record { };
}

struct Summary {
    uint32_t sizeOfBody { 0 };
    bool hasNoLoops { false };
};

struct PositionFacts {
    TypeID proven;
};

struct LocationFacts {
    friend bool operator==(const LocationFacts&, const LocationFacts&) = default;

    InitializationState initialization { InitializationState::Unknown };
};

struct VariableFacts {
    LocationFacts location;
};

enum class FieldFact : uint8_t {
    IsAccessor = 1 << 0,
    IsEnforced = 1 << 1,
    HoldsRawValueFromBirth = 1 << 2,
    SlotHoldsUndefinedAndNull = 1 << 3,
};

struct FieldFacts {
    friend bool operator==(const FieldFacts&, const FieldFacts&) = default;

    LocationFacts location;
    OptionSet<FieldStability> stability;
    OptionSet<FieldForm> formOfValue;
    OptionSet<FieldFact> facts;
    TypeID slotType;
};

enum class LayoutFact : uint8_t {
    ObjectsAreDictionaries = 1 << 0,
};

struct LayoutFacts {
    OptionSet<LayoutFact> facts;
};

struct ClassFacts {
    ExecutionCount evaluations { ExecutionCount::Unknown };
    BaseState baseState { BaseState::Unknown };
    bool isEvaluatedOncePerInstance { false };
    LayoutID base;
    LayoutID prototypeBase;
};

struct LocaleUse {
    LocaleUseKind kind { LocaleUseKind::Unknown };
    StringID tag;
    BuiltinID builtin;
    Witness witness { noWitness };
};

struct RegExpTextInSight {
    StringID pattern;
    uint16_t flags { 0 };
    bool isOneOfAllAtSomeCall { false };
};

struct RegExpTextWorkedOut {
    String pattern;
    uint16_t flags { 0 };
    bool isOneOfAllAtSomeCall { false };
};

struct RegExpCallWithoutText {
    FunctionID function;
    ValueID site;
};

struct WorkerEntry {
    StringID path;
    bool hasOptions { false };
    Witness witness { noWitness };
};

bool raisesEventWhenCalled(const BuiltinTable&, BuiltinID);

struct RefusedCode {
    bool evaluatedText { false };
    bool hostEvaluatedText { false };
    bool codeOutsideImage { false };
};
RefusedCode refusedCodeOf(const Program&);
bool refusesTextAsUnseenCode(const BuiltinTable&, BuiltinID canonical, RefusedCode);
std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable&, BuiltinID, RefusedCode);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
