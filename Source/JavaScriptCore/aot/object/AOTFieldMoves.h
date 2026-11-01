/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTObjectBuilder.h"
#include "AOTRuntimeType.h"
#include "AOTState.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "JSCJSValue.h"
#include "PropertyOffset.h"
#include "RememberedSlot.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC {

class JSFinalObject;
class JSGlobalObject;
class JSObject;
class Structure;
class VM;

namespace AOT {

class AcceptedValue;
class Object;
class RuntimeLayoutTable;
struct State;

enum class ValueKind : uint8_t {
    Int32 = 1,
    String = 2,
    True = 4,
    False = 8,
    Undefined = 16,
    Null = 32,
    Double = 64,
};

OptionSet<ValueKind> valueKindOf(JSValue);
OptionSet<ValueKind> kindsAlwaysAccepted(const RuntimeType&);
bool acceptsEveryValue(const RuntimeType&);
// Which of the values that the slot of one field holds the slot of another takes.
enum class SlotFit : uint8_t {
    Always,
    // Undefined and null lie in the slot of the source whatever the type of its field says.
    UnlessUndefinedOrNull,
    IsTested,
};
SlotFit fitOf(const RuntimeLayoutTable&, const FieldData& from, const FieldData& to);
inline bool alwaysFits(const RuntimeLayoutTable& layouts, const FieldData& from, const FieldData& to) { return fitOf(layouts, from, to) == SlotFit::Always; }
// Asks the representation too, so the store of what it returns cannot fail.
std::optional<AcceptedValue> acceptInSlot(Object& target, const FieldData&, JSValue);

struct FieldMovePlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FieldMovePlan);

    struct BoxedWordMove {
        uint16_t from { 0 };
        uint16_t to { 0 };
    };
    struct Move {
        // Null for a property of an ordinary object that lies `from` bytes below its butterfly.
        const FieldData* field { nullptr };
        uint32_t from { 0 };
        uint32_t to { 0 };
        bool isNarrowReferenceInCell { false };
    };

    bool isUsable() const { return !!structure; }
    bool movesNothing() const { return moves.isEmpty() && boxedWordMoves.isEmpty(); }

    void appendField(const FieldData& from, PropertyOffset to);
    void appendProperty(PropertyOffset from, PropertyOffset to);

    // An ordinary object with this Structure, all of whose properties lie in its cell.
    Structure* structure { nullptr };
    bool sourceHasDeferredClosures { false };
    Vector<Move, 8> moves;
    Vector<BoxedWordMove, 8> boxedWordMoves;
};

// Nothing else has seen the target, whose properties are still empty, so it may be left half filled where a move does not apply.
bool applyMovesToNewObject(VM&, const FieldMovePlan&, JSFinalObject* target, JSObject* source);

// The keys that a copy leaves out, as a run of identifier numbers in the image. The number of the run stands for the list.
struct ExcludedRun {
    uint32_t numberPlusOne { 0 };
    std::span<const uint32_t> identifiers;
};
JSFinalObject* tryCopyAsOrdinaryObject(VM&, JSGlobalObject*, JSObject* source, ExcludedRun);

struct FieldMovePlanKey {
    // The collector, which may run in a thread of its own, forgets an entry. The thread whose table of names holds the names of its plan releases it, when the place is taken again.
    bool isForgotten { false };
    uint64_t sourceWords { 0 };
    uint64_t targetWords { 0 };
    Structure* sourceStructure { nullptr };

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
    AOT_CLASS_THRESHOLD(unsigned, waysPerSet, 4);

    struct Set {
        std::array<std::unique_ptr<Entry>, waysPerSet> ways;
        uint8_t nextVictim { 0 };
    };
    using Sets = std::array<Set, numberOfSets>;

    Set& setFor(const FieldMovePlanKey&);

    MadeInArena<Sets> m_sets;
    const Entry* m_lastFound { nullptr };
    RememberedSlotSet m_writes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
