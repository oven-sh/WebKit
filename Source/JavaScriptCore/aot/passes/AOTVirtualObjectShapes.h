/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTKnownValue.h"
#include "AOTVirtualObject.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using KeyOrderID = ID<struct KeyOrderIDTag>;
using VirtualShapeID = ID<struct VirtualShapeIDTag>;
using VirtualFamilyID = ID<struct VirtualFamilyIDTag>;

inline constexpr KeyOrderID emptyKeyOrder { 0u };
inline constexpr VirtualShapeID cellShape { 0u };

class KeyOrderTable {
    WTF_MAKE_NONCOPYABLE(KeyOrderTable);
public:
    KeyOrderTable();

    KeyOrderID append(KeyOrderID, uint32_t field);
    uint32_t lengthOf(KeyOrderID order) const { return m_nodes[order.index()].length; }
    Vector<uint32_t, 8> fieldsOf(KeyOrderID) const;

private:
    struct Node {
        KeyOrderID parent;
        uint32_t field { 0 };
        uint32_t length { 0 };
    };

    Vector<Node, 8> m_nodes;
    UncheckedKeyHashMap<uint64_t, KeyOrderID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_children;
};

enum class VirtualShapeKind : uint8_t { Cell, Constant, Fields };

struct VirtualShape {
    VirtualShapeKind kind { VirtualShapeKind::Cell };
    VirtualConstant constant { VirtualConstant::Undefined };
    uint32_t descriptionIndex { 0 };
    uint32_t length { 0 };
    KeyOrderID order { emptyKeyOrder };
    BitVector stored;

    static VirtualShape ofConstant(VirtualConstant constant) { return { VirtualShapeKind::Constant, constant, 0, 0, emptyKeyOrder, { } }; }

    friend bool operator==(const VirtualShape&, const VirtualShape&) = default;
};

class VirtualFamily {
    WTF_MAKE_NONCOPYABLE(VirtualFamily);
public:
    VirtualFamily();
    VirtualFamily(VirtualFamily&&) = default;
    VirtualFamily& operator=(VirtualFamily&&) = default;

    uint32_t addDescription(const VirtualObjectDescription&);
    const VirtualObjectDescription& descriptionAt(uint32_t index) const LIFETIME_BOUND { return m_descriptions[index]; }
    const VirtualObjectDescription& descriptionOf(const VirtualShape& shape) const LIFETIME_BOUND { return m_descriptions[shape.descriptionIndex]; }
    std::span<const VirtualObjectDescription> descriptions() const LIFETIME_BOUND { return m_descriptions.span(); }

    uint32_t ensureField(const VirtualField&);
    std::optional<uint32_t> findField(const VirtualField&) const;
    const VirtualField& fieldAt(uint32_t field) const LIFETIME_BOUND { return m_fields[field]; }
    uint32_t numberOfFields() const { return m_fields.size(); }

    VirtualShapeID intern(VirtualShape&&);
    const VirtualShape& at(VirtualShapeID shape) const LIFETIME_BOUND { return m_shapes[shape.index()]; }
    uint32_t numberOfShapes() const { return m_shapes.size(); }

    std::optional<uint8_t> tagBitFor(BlockID, VirtualShapeID, uint64_t maxTagBits);
    std::optional<uint8_t> findTagBit(BlockID, VirtualShapeID) const;
    void shareTagBit(BlockID, VirtualShapeID, uint8_t existingTagBit);
    std::optional<uint8_t> reserveConsecutiveTagBits(unsigned count, uint64_t maxTagBits);
    unsigned numberOfTagBits() const { return m_numberOfTagBits; }
    void forgetTagBits();

    void dump(PrintStream&) const;

private:
    Vector<VirtualObjectDescription, 1> m_descriptions;
    Vector<VirtualField, 8> m_fields;
    Vector<VirtualShape, 4> m_shapes;
    UncheckedKeyHashMap<uint64_t, uint8_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_tagBits;
    unsigned m_numberOfTagBits { 0 };
    unsigned m_numberOfReservedTagBits { 0 };
};

inline constexpr unsigned maxTagBitsInInt32 = 31;

struct ShapeContext {
    const VirtualObjectContext& objects;
    const VirtualFamily& family;
    const KeyOrderTable& orders;
};

TypeID typeOfShape(const ShapeContext&, const VirtualShape&);
bool hasStored(const VirtualShape&, uint32_t field);

VirtualShape shapeAtAllocation(const VirtualObjectContext&, VirtualFamily&, KeyOrderTable&, uint32_t descriptionIndex);
VirtualShape shapeAfterStore(const VirtualFamily&, KeyOrderTable&, const VirtualShape&, uint32_t field);
VirtualShape shapeAfterAppend(VirtualFamily&, const VirtualShape&, bool isHole, uint32_t& appendedField);

enum class CastOutcome : uint8_t { Unknown, Passes, Throws };

KnownBoolean evaluateTest(const ShapeContext&, const VirtualShape&, const Value& test, uint32_t operandIndex);
KnownBoolean evaluateTestOfTwoShapes(const ShapeContext&, const VirtualShape& left, const VirtualShape& right, bool areSameObject, const Value& test);
CastOutcome evaluateCast(const ShapeContext&, const VirtualShape&, const Value& cast);
std::optional<uint32_t> findShapeCase(const ShapeContext&, const VirtualShape&, const Terminator& switchOnShape);

inline constexpr OpcodeSet evaluatedTests {
    Opcode::PropHas, Opcode::ElemHas, Opcode::StrictEq, Opcode::ToBoolean, Opcode::TypeTest, Opcode::PrivateHas, Opcode::ShapeTest,
    Opcode::IsSlotFilled, Opcode::HasNoExtraProperties, Opcode::IsNotFrozen, Opcode::HasLayoutPrototype, Opcode::BelongsToInstance,
    Opcode::IsPristine, Opcode::HasKeyOrder, Opcode::IsShapeEpochCurrent, Opcode::IsClosureOf, Opcode::PropHasOwn, Opcode::HasArrayElement,
    Opcode::HasArrayStorage, Opcode::IsInBounds,
};

inline constexpr OpcodeSet testsLeftToRun {
    Opcode::IsNotThenable, Opcode::IsMemberUnchanged, Opcode::IsPrototypeLinkUnchanged, Opcode::IsClassEvaluatedOnce,
    Opcode::IsRequireEdgeReady, Opcode::IsEnumeratorCurrent,
};

static_assert((evaluatedTests | testsLeftToRun | opcodesInterpretedAs(Interpretation::Test)) == (evaluatedTests | testsLeftToRun));
static_assert(!evaluatedTests.intersects(testsLeftToRun));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
