/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVirtualObjectShapes.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

KeyOrderTable::KeyOrderTable()
{
    m_nodes.append(Node { });
}

KeyOrderID KeyOrderTable::append(KeyOrderID order, uint32_t field)
{
    uint64_t key = static_cast<uint64_t>(order.index()) << 32 | field;
    return m_children.ensure(key, [&] {
        KeyOrderID child(m_nodes.size());
        m_nodes.append(Node { order, field, lengthOf(order) + 1 });
        return child;
    }).iterator->value;
}

Vector<uint32_t, 8> KeyOrderTable::fieldsOf(KeyOrderID order) const
{
    Vector<uint32_t, 8> result(lengthOf(order));
    size_t position = result.size();
    while (order != emptyKeyOrder) {
        const Node& node = m_nodes[order.index()];
        result[--position] = node.field;
        order = node.parent;
    }
    return result;
}

VirtualFamily::VirtualFamily()
{
    m_shapes.append(VirtualShape { });
}

uint32_t VirtualFamily::addDescription(const VirtualObjectDescription& description)
{
    auto index = static_cast<uint32_t>(m_descriptions.size());
    m_descriptions.append(description);
    return index;
}

uint32_t VirtualFamily::ensureField(const VirtualField& field)
{
    if (std::optional<uint32_t> existing = findField(field))
        return *existing;
    auto number = static_cast<uint32_t>(m_fields.size());
    m_fields.append(field);
    m_hasUnprovenFields |= field.kind == VirtualFieldKind::UnprovenNamed;
    return number;
}

std::optional<uint32_t> VirtualFamily::findField(const VirtualField& field) const
{
    size_t number = m_fields.find(field);
    if (number == notFound)
        return std::nullopt;
    return static_cast<uint32_t>(number);
}

std::optional<uint32_t> VirtualFamily::findCounterpart(uint32_t field) const
{
    if (!m_hasUnprovenFields)
        return std::nullopt;
    const VirtualField& known = m_fields[field];
    switch (known.kind) {
    case VirtualFieldKind::Named:
        return findField(VirtualField::unprovenNamed(known.key));
    case VirtualFieldKind::UnprovenNamed:
        return findField(VirtualField::named(known.key));
    case VirtualFieldKind::LayoutField:
    case VirtualFieldKind::Element:
    case VirtualFieldKind::Capture:
    case VirtualFieldKind::Length:
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
    case VirtualFieldKind::Operand:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

VirtualShapeID VirtualFamily::intern(VirtualShape&& shape)
{
    size_t existing = m_shapes.find(shape);
    if (existing != notFound)
        return VirtualShapeID(existing);
    VirtualShapeID id(m_shapes.size());
    m_shapes.append(WTF::move(shape));
    return id;
}

static uint64_t tagBitKey(BlockID block, VirtualShapeID shape)
{
    return static_cast<uint64_t>(block.index()) << 32 | shape.index();
}

std::optional<uint8_t> VirtualFamily::tagBitFor(BlockID block, VirtualShapeID shape, uint64_t maxTagBits)
{
    if (std::optional<uint8_t> existing = findTagBit(block, shape))
        return existing;
    if (m_numberOfTagBits >= std::min<uint64_t>(maxTagBits, maxTagBitsInInt32))
        return std::nullopt;
    auto tagBit = static_cast<uint8_t>(m_numberOfTagBits++);
    m_tagBits.add(tagBitKey(block, shape), tagBit);
    return tagBit;
}

std::optional<uint8_t> VirtualFamily::findTagBit(BlockID block, VirtualShapeID shape) const
{
    return m_tagBits.getOptional(tagBitKey(block, shape));
}

void VirtualFamily::shareTagBit(BlockID block, VirtualShapeID shape, uint8_t existingTagBit)
{
    RELEASE_ASSERT(existingTagBit < m_numberOfTagBits);
    bool isNewEntry = m_tagBits.add(tagBitKey(block, shape), existingTagBit).isNewEntry;
    RELEASE_ASSERT(isNewEntry);
}

std::optional<uint8_t> VirtualFamily::reserveConsecutiveTagBits(unsigned count, uint64_t maxTagBits)
{
    RELEASE_ASSERT(m_numberOfTagBits == m_numberOfReservedTagBits);
    if (static_cast<uint64_t>(m_numberOfTagBits) + count > std::min<uint64_t>(maxTagBits, maxTagBitsInInt32))
        return std::nullopt;
    auto first = static_cast<uint8_t>(m_numberOfTagBits);
    m_numberOfTagBits += count;
    m_numberOfReservedTagBits += count;
    return first;
}

void VirtualFamily::forgetTagBits()
{
    m_tagBits.clear();
    m_numberOfTagBits = m_numberOfReservedTagBits;
}

TypeID typeOfShape(const ShapeContext& context, const VirtualShape& shape)
{
    switch (shape.kind) {
    case VirtualShapeKind::Cell:
        return { };
    case VirtualShapeKind::Constant:
        return typeOf(shape.constant);
    case VirtualShapeKind::Fields:
        return context.family.descriptionOf(shape).type;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasStored(const VirtualShape& shape, uint32_t field)
{
    return shape.kind == VirtualShapeKind::Fields && shape.stored.get(field);
}

bool hasStoredCounterpart(const VirtualFamily& family, const VirtualShape& shape, uint32_t field)
{
    std::optional<uint32_t> counterpart = family.findCounterpart(field);
    return counterpart && hasStored(shape, *counterpart);
}

bool hasUnprovenField(const VirtualFamily& family, const VirtualShape& shape)
{
    if (shape.kind != VirtualShapeKind::Fields || !family.hasUnprovenFields())
        return false;
    for (size_t field : shape.stored) {
        if (family.fieldAt(static_cast<uint32_t>(field)).kind == VirtualFieldKind::UnprovenNamed)
            return true;
    }
    return false;
}

// The order of the keys lists the Named field of a key, whichever of the two is stored.
static void addStoredField(const VirtualFamily& family, KeyOrderTable& orders, VirtualShape& shape, uint32_t field)
{
    bool wasStored = shape.stored.set(field);
    if (wasStored)
        return;
    std::optional<uint32_t> counterpart = family.findCounterpart(field);
    if (counterpart && shape.stored.get(*counterpart)) {
        shape.stored.clear(*counterpart);
        return;
    }
    if (!family.descriptionOf(shape).hasKeyOrder || !family.fieldAt(field).isProperty())
        return;
    bool isUnproven = family.fieldAt(field).kind == VirtualFieldKind::UnprovenNamed;
    RELEASE_ASSERT(!isUnproven || counterpart);
    shape.order = orders.append(shape.order, isUnproven ? *counterpart : field);
}

VirtualShape shapeAtAllocation(const VirtualObjectContext& context, VirtualFamily& family, KeyOrderTable& orders, uint32_t descriptionIndex)
{
    const VirtualObjectDescription& description = family.descriptionAt(descriptionIndex);
    VirtualShape shape;
    shape.kind = VirtualShapeKind::Fields;
    shape.descriptionIndex = descriptionIndex;
    shape.length = initialLengthOf(context.function, description);
    forEachInitialField(context, description, [&](const VirtualField& initialField, ValueID) {
        addStoredField(family, orders, shape, family.ensureField(initialField));
    });
    if (description.kind == VirtualObjectKind::Array)
        addStoredField(family, orders, shape, family.ensureField(VirtualField::part(VirtualFieldKind::Length)));
    return shape;
}

VirtualShape shapeAfterStore(const VirtualFamily& family, KeyOrderTable& orders, const VirtualShape& shape, uint32_t field)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields);
    ASSERT(family.descriptionOf(shape).kind != VirtualObjectKind::Array || family.fieldAt(field).kind != VirtualFieldKind::Element || family.fieldAt(field).index < shape.length);
    VirtualShape result = shape;
    addStoredField(family, orders, result, field);
    return result;
}

VirtualShape shapeAfterDelete(const VirtualFamily& family, KeyOrderTable& orders, const VirtualShape& shape, uint32_t namedField)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields && family.fieldAt(namedField).kind == VirtualFieldKind::Named);
    VirtualShape result = shape;
    bool wasStored = result.stored.clear(namedField);
    if (std::optional<uint32_t> counterpart = family.findCounterpart(namedField))
        wasStored |= result.stored.clear(*counterpart);
    if (!wasStored || !family.descriptionOf(shape).hasKeyOrder)
        return result;
    result.order = emptyKeyOrder;
    for (uint32_t field : orders.fieldsOf(shape.order)) {
        if (field != namedField)
            result.order = orders.append(result.order, field);
    }
    return result;
}

VirtualShape shapeAfterAppend(VirtualFamily& family, const VirtualShape& shape, bool isHole, uint32_t& appendedField)
{
    RELEASE_ASSERT(shape.kind == VirtualShapeKind::Fields);
    RELEASE_ASSERT(family.descriptionOf(shape).kind == VirtualObjectKind::Array);
    appendedField = family.ensureField(VirtualField::element(shape.length));
    VirtualShape result = shape;
    ++result.length;
    if (!isHole)
        result.stored.set(appendedField);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
