/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTDiagnostics.h"
#include "AOTEffects.h"
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

// What the answers of Facts are made of. Every record claims nothing as it is default-constructed.

inline constexpr Witness noWitness { Wire::none, Wire::none, noSourceLocation };

constexpr bool hasFunction(const Witness& witness) { return witness.function != Wire::none; }

constexpr bool isLowerWitness(const Witness& witness, const Witness& other)
{
    if (!hasFunction(witness))
        return false;
    return !hasFunction(other) || std::tie(witness.function, witness.value) < std::tie(other.function, other.value);
}

// Rows grouped by a dense owner number. An owner past the end has no rows, so an empty table knows nothing.
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

// ---- What the scan leaves of a function. Nothing about locals.

struct Summary {
    // In operations of the body as written.
    uint32_t sizeOfBody { 0 };
    bool hasNoLoops { false };
};

// ---- Types at positions

// No type: nothing is said.
struct PositionFacts {
    TypeID proven;
};

struct FunctionEffects {
    Effects onReturn { Effects::unknownCode() };
    Effects onThrow { Effects::unknownCode() };
};

// ---- Variables and fields

struct LocationFacts {
    StoreCount stores { StoreCount::Unknown };
    InitializationState initialization { InitializationState::Unknown };
    ReadState reads { ReadState::Unknown };
    ClosureDeferral deferral { ClosureDeferral::Unknown };
};

struct VariableFacts {
    LocationFacts location;
};

enum class FieldFact : uint8_t {
    IsAccessor = 1 << 0,
    // The run time turns away what does not fit, so the slot's type holds whoever writes.
    IsEnforced = 1 << 1,
    HoldsRawValueFromBirth = 1 << 2,
    SlotHoldsUndefinedAndNull = 1 << 3,
};

struct FieldFacts {
    LocationFacts location;
    Occupancy occupancy { Occupancy::Unknown };
    OptionSet<FieldStability> stability;
    // Beside Field::form of a declared field, which every reader adds for itself.
    OptionSet<FieldForm> formOfValue;
    OptionSet<FieldFact> facts;
    // None: Plan goes by what is written.
    TypeID slotType;
};

// ---- Objects, layouts, classes

enum class FrameVerdict : uint8_t { Heap, MayLiveInFrame };

enum class ObjectFact : uint8_t {
    // Of an instance of a built-in class: it has the members that its class gave it, and so has everything above it.
    IsPristine = 1 << 0,
    // The kind of storage holds for the whole program and is read without a test.
    HasDecidedStorage = 1 << 1,
    ContentsAreProven = 1 << 2,
    HasNoHoles = 1 << 3,
    // A RegExp: pattern and flags are those of its creation, and lastIndex holds a number.
    IsAsCreated = 1 << 4,
};

struct ObjectFacts {
    AllocationClassID allocationClass;
    OptionSet<ObjectFact> facts;
    FrameVerdict frame { FrameVerdict::Heap };
    AllocationHeapCause heapCause { AllocationHeapCause::NotTracked };
    ArrayStorage arrayStorage { ArrayStorage::Other };
};

enum class LayoutFact : uint8_t {
    ObjectsAreDictionaries = 1 << 0,
    MayLiveInFrame = 1 << 1,
};

struct LayoutFacts {
    OptionSet<LayoutFact> facts;
};

struct ClassFacts {
    ExecutionCount evaluations { ExecutionCount::Unknown };
    BaseState baseState { BaseState::Unknown };
    // At BaseState::Layout.
    LayoutID base;
    // At BaseState::BuiltinClass.
    BuiltinClassID baseClass;
    // At BaseState::None, and never more than an expectation.
    LayoutID prototypeBase;
};

// ---- The program

struct LocaleUse {
    LocaleUseKind kind { LocaleUseKind::Unknown };
    StringID tag;
    BuiltinID builtin;
    Witness witness { noWitness };
};

struct RegExpTextInSight {
    StringID pattern;
    // OptionSet<Yarr::Flags>::toRaw() of what ImageRegExp::significantFlags() keeps.
    uint16_t flags { 0 };
    // At some call where the pair arrives every text and all flags are listed.
    bool isOneOfAllAtSomeCall { false };
};

// The same for a text that the program spells nowhere. Image interns it.
struct RegExpTextWorkedOut {
    String pattern;
    uint16_t flags { 0 };
    bool isOneOfAllAtSomeCall { false };
};

// A call of RegExp in the program's own code at which no text has been worked out: whatever arrives there is compiled when the program runs.
struct RegExpCallWithoutText {
    FunctionID function;
    ValueID site;
};

struct WorkerEntry {
    StringID path;
    bool isGuess { false };
    bool hasOptions { false };
    Witness witness { noWitness };
};

// ---- --explain

struct MutationWitness {
    ObjectID object;
    ObjectMutation mutation { ObjectMutation::ReachNativeCode };
    Witness witness { noWitness };
};

// ---- The table of built-ins

// A call of the built-in evaluates a text, loads a module by a name, runs code that nobody has read or loads machine code, in a build that refuses none of them. By the table alone.
// Yes claims nothing. callOrReadOfBuiltinIsEvent() of ir/AOTObjects.h asks, and the scan goes by that for unseenCode().
bool raisesEventWhenCalled(const BuiltinTable&, BuiltinID);

// What the run time of this build turns away. Nothing refused claims nothing.
struct RefusedCode {
    bool evaluatedText { false };
    bool hostEvaluatedText { false };
    bool codeOutsideImage { false };
};
RefusedCode refusedCodeOf(const Program&);
// The call throws before it looks at the text.
bool refusesTextAsUnseenCode(const BuiltinTable&, BuiltinID canonical, RefusedCode);
// What a record of Facts::unseenCode() says of a call of the built-in. Nothing: the call lets nothing in, or it throws in such a build.
std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable&, BuiltinID, RefusedCode);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
