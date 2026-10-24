/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "AOTVirtualObjectShapes.h"
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

struct ObjectVariant {
    std::optional<uint8_t> tagBit;
    VirtualShapeID shape;

    friend bool operator==(const ObjectVariant&, const ObjectVariant&) = default;
    friend auto operator<=>(const ObjectVariant&, const ObjectVariant&) = default;
};

struct HeldObject {
    uint32_t field { 0 };
    VirtualObjectID object;

    friend bool operator==(const HeldObject&, const HeldObject&) = default;
};

struct ObjectState {
    Vector<ObjectVariant, 1> variants;
    Vector<HeldObject, 1> heldObjects;
    BitVector fieldsValidInCell;
    BlockID cellJoinedAt;
    Vector<TypeID, 1> assumedTypes;
    // On some way here a test has left only variants that store nothing, and that way carries no fields.
    bool lacksFieldsOnSomeWay { false };
    // Some way into a block in front of here does not have the object, and hands on unspecified values for its fields. No name reaches it behind that block: it is there where the tag of its holder says so.
    bool isMissingOnSomeWay { false };

    static ObjectState withShape(VirtualShapeID shape) { return { { ObjectVariant { std::nullopt, shape } }, { }, { }, { }, { }, false, false }; }

    bool hasCell() const;
    bool isCell() const { return variants.size() == 1 && variants[0].shape == cellShape; }
    bool isVirtual() const { return !hasCell(); }
    bool hasOneShape() const;
    bool needsTag() const { return !hasOneShape(); }
    bool hasConflictingTags() const;
    uint32_t tagBits() const;

    VirtualObjectID objectHeldIn(uint32_t field) const;
    void hold(uint32_t field, VirtualObjectID);
    void release(uint32_t field);

    void assume(TypeID);
    void keepAssumedTypesOf(const ObjectState&);

    void addVariant(ObjectVariant);
    void becomeCell();
    void normalize();

    friend bool operator==(const ObjectState&, const ObjectState&) = default;
};

struct ObjectEntry {
    VirtualObjectID object;
    ObjectState state;

    friend bool operator==(const ObjectEntry&, const ObjectEntry&) = default;
};

enum class AliasKind : uint8_t { Object, Guard, Results };

struct AliasEntry {
    ValueID value;
    VirtualObjectID object;
    AliasKind kind { AliasKind::Object };

    friend bool operator==(const AliasEntry&, const AliasEntry&) = default;
};

struct TestEntry {
    ValueID test;
    VirtualObjectID object;
    std::optional<bool> result;
    uint32_t tagBitsWhenTrue { 0 };

    friend bool operator==(const TestEntry&, const TestEntry&) = default;
};

struct SinkingState {
    bool isReachable { true };
    bool isAtHead { true };
    BlockID block;
    Vector<ObjectEntry, 2> objects;
    Vector<AliasEntry, 4> aliases;
    Vector<TestEntry, 1> tests;

    static SinkingState unreachableAt(BlockID);
    static SinkingState nothingAt(BlockID);

    ObjectState* find(VirtualObjectID);
    const ObjectState* find(VirtualObjectID) const;
    std::optional<size_t> indexOf(VirtualObjectID) const;
    ObjectState& add(VirtualObjectID, ObjectState&&);
    void set(std::span<ObjectEntry> movedEntries);
    size_t weight() const;
    void remove(VirtualObjectID);
    bool canReach(VirtualObjectID from, VirtualObjectID to) const;

    const AliasEntry* findAlias(ValueID) const;
    void setAlias(ValueID, VirtualObjectID, AliasKind);
    bool isDestructured(VirtualObjectID) const;

    const TestEntry* findTest(ValueID) const;
    void setTest(const TestEntry&);

    void dump(PrintStream&) const;

    friend bool operator==(const SinkingState&, const SinkingState&) = default;
};

class SinkingDecisions {
public:
    bool isBuiltAtHead(BlockID, VirtualObjectID) const;
    bool buildsAtHeadOf(BlockID block) const { return m_blocksWithObjectsBuiltAtHeads.get(block.index()); }
    // A parameter that takes cells, or a cast that stays a value of the program: the name is no alias of an object.
    bool holdsCells(ValueID parameterOrCast) const { return m_parametersHoldingCells.get(parameterOrCast.index()); }
    bool holdsCells(VirtualObjectID holder, uint32_t field) const;

    void buildAtHead(BlockID, VirtualObjectID);
    void holdCells(ValueID parameterOrCast, VirtualObjectID boundObject);
    void holdCellsAsBefore(ValueID parameter) { m_parametersHoldingCells.set(parameter.index()); }
    void holdObjectsAgain(ValueID parameter) { m_parametersHoldingCells.clear(parameter.index()); }
    void holdCells(VirtualObjectID holder, uint32_t field);

    bool takeDidChange() { return std::exchange(m_didChange, false); }
    std::span<const VirtualObjectID> objectsOfNewDecisions() const LIFETIME_BOUND { return m_objectsOfNewDecisions.span(); }
    void forgetNewDecisions() { m_objectsOfNewDecisions.clear(); }

private:
    using PairSet = UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    static uint64_t pairOf(uint32_t first, uint32_t second) { return static_cast<uint64_t>(first) << 32 | second; }
    void didDecide(VirtualObjectID);

    PairSet m_objectsBuiltAtHeads;
    BitVector m_blocksWithObjectsBuiltAtHeads;
    PairSet m_fieldsHoldingCells;
    BitVector m_parametersHoldingCells;
    Vector<VirtualObjectID, 4> m_objectsOfNewDecisions;
    bool m_didChange { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
