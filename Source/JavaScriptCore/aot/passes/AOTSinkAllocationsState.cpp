/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsState.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Entry, size_t inlineCapacity, typename Key>
static size_t positionOf(const Vector<Entry, inlineCapacity>& entries, Key Entry::* key, Key value)
{
    return std::ranges::lower_bound(entries, value, { }, key) - entries.begin();
}

template<typename Entry, size_t inlineCapacity, typename Key>
static const Entry* findEntry(const Vector<Entry, inlineCapacity>& entries, Key Entry::* key, Key value)
{
    size_t position = positionOf(entries, key, value);
    if (position == entries.size() || entries[position].*key != value)
        return nullptr;
    return &entries[position];
}

template<typename Entry, size_t inlineCapacity, typename Key>
static void setEntry(Vector<Entry, inlineCapacity>& entries, Key Entry::* key, const Entry& entry)
{
    size_t position = positionOf(entries, key, entry.*key);
    if (position < entries.size() && entries[position].*key == entry.*key) {
        entries[position] = entry;
        return;
    }
    entries.insert(position, entry);
}

template<typename Entry, size_t inlineCapacity, typename Key>
static void removeEntry(Vector<Entry, inlineCapacity>& entries, Key Entry::* key, Key value)
{
    size_t position = positionOf(entries, key, value);
    if (position < entries.size() && entries[position].*key == value)
        entries.removeAt(position);
}

bool ObjectState::hasCell() const
{
    return variants.containsIf([](const ObjectVariant& variant) {
        return variant.shape == cellShape;
    });
}

bool ObjectState::hasOneShape() const
{
    return std::ranges::all_of(variants, [&](const ObjectVariant& variant) {
        return variant.shape == variants[0].shape;
    });
}

bool ObjectState::hasConflictingTags() const
{
    for (size_t index = 1; index < variants.size(); ++index) {
        const ObjectVariant& variant = variants[index];
        if (variant.tagBit && variant.tagBit == variants[index - 1].tagBit)
            return true;
    }
    return false;
}

uint32_t ObjectState::tagBits() const
{
    uint32_t bits = 0;
    for (const ObjectVariant& variant : variants) {
        if (variant.tagBit)
            bits |= 1u << *variant.tagBit;
    }
    return bits;
}

VirtualObjectID ObjectState::objectHeldIn(uint32_t field) const
{
    const HeldObject* held = findEntry(heldObjects, &HeldObject::field, field);
    return held ? held->object : VirtualObjectID();
}

void ObjectState::hold(uint32_t field, VirtualObjectID object)
{
    setEntry(heldObjects, &HeldObject::field, HeldObject { field, object });
}

void ObjectState::release(uint32_t field)
{
    removeEntry(heldObjects, &HeldObject::field, field);
}

void ObjectState::assume(TypeID type)
{
    size_t position = std::ranges::lower_bound(assumedTypes, type) - assumedTypes.begin();
    if (position == assumedTypes.size() || assumedTypes[position] != type)
        assumedTypes.insert(position, type);
}

void ObjectState::keepAssumedTypesOf(const ObjectState& other)
{
    assumedTypes.removeAllMatching([&](TypeID type) {
        return !other.assumedTypes.contains(type);
    });
}

void ObjectState::addVariant(ObjectVariant variant)
{
    size_t position = std::ranges::lower_bound(variants, variant) - variants.begin();
    if (position < variants.size() && variants[position] == variant)
        return;
    variants.insert(position, variant);
}

void ObjectState::becomeCell()
{
    variants = { ObjectVariant { std::nullopt, cellShape } };
    heldObjects.clear();
    fieldsValidInCell.clearAll();
    cellJoinedAt = { };
    lacksFieldsOnSomeWay = false;
    isMissingOnSomeWay = false;
}

void ObjectState::normalize()
{
    if (!hasOneShape())
        return;

    VirtualShapeID shape = variants[0].shape;
    if (variants.size() > 1 || variants[0].tagBit)
        variants = { ObjectVariant { std::nullopt, shape } };
    if (shape == cellShape)
        cellJoinedAt = { };
}

SinkingState SinkingState::unreachableAt(BlockID block)
{
    SinkingState state;
    state.isReachable = false;
    state.block = block;
    return state;
}

SinkingState SinkingState::nothingAt(BlockID block)
{
    SinkingState state;
    state.block = block;
    return state;
}

const ObjectState* SinkingState::find(VirtualObjectID object) const
{
    const ObjectEntry* entry = findEntry(objects, &ObjectEntry::object, object);
    return entry ? &entry->state : nullptr;
}

std::optional<size_t> SinkingState::indexOf(VirtualObjectID object) const
{
    const ObjectEntry* entry = findEntry(objects, &ObjectEntry::object, object);
    if (!entry)
        return std::nullopt;
    return entry - objects.span().data();
}

ObjectState* SinkingState::find(VirtualObjectID object)
{
    return const_cast<ObjectState*>(std::as_const(*this).find(object));
}

ObjectState& SinkingState::add(VirtualObjectID object, ObjectState&& state)
{
    size_t position = positionOf(objects, &ObjectEntry::object, object);
    RELEASE_ASSERT(position == objects.size() || objects[position].object != object);
    objects.insert(position, ObjectEntry { object, WTF::move(state) });
    return objects[position].state;
}

void SinkingState::set(std::span<ObjectEntry> movedEntries)
{
    Vector<ObjectEntry*, 8> addedEntries;
    for (ObjectEntry& entry : movedEntries) {
        if (ObjectState* state = find(entry.object))
            *state = WTF::move(entry.state);
        else
            addedEntries.append(&entry);
    }
    if (addedEntries.isEmpty())
        return;

    AOT::sort(addedEntries, { }, &ObjectEntry::object);
    size_t source = objects.size();
    size_t destination = source + addedEntries.size();
    objects.grow(destination);
    for (size_t added = addedEntries.size(); added;) {
        if (source && objects[source - 1].object > addedEntries[added - 1]->object) {
            --source;
            --destination;
            objects[destination] = WTF::move(objects[source]);
            continue;
        }
        --added;
        --destination;
        objects[destination] = ObjectEntry { addedEntries[added]->object, WTF::move(addedEntries[added]->state) };
    }
}

size_t SinkingState::weight() const
{
    size_t result = aliases.size() + tests.size();
    for (const ObjectEntry& entry : objects)
        result += entry.state.variants.size() + entry.state.heldObjects.size() + entry.state.assumedTypes.size();
    return result;
}

void SinkingState::remove(VirtualObjectID object)
{
    removeEntry(objects, &ObjectEntry::object, object);
}

bool SinkingState::canReach(VirtualObjectID from, VirtualObjectID to) const
{
    BitVector visited;
    Vector<VirtualObjectID, 4> worklist { from };
    while (!worklist.isEmpty()) {
        VirtualObjectID object = worklist.takeLast();
        if (object == to)
            return true;
        if (!visited.add(object.index()))
            continue;
        const ObjectState* state = find(object);
        if (!state)
            continue;
        for (const HeldObject& held : state->heldObjects)
            worklist.append(held.object);
    }
    return false;
}

const AliasEntry* SinkingState::findAlias(ValueID value) const
{
    return findEntry(aliases, &AliasEntry::value, value);
}

void SinkingState::setAlias(ValueID value, VirtualObjectID object, AliasKind kind)
{
    setEntry(aliases, &AliasEntry::value, AliasEntry { value, object, kind });
}

bool SinkingState::isDestructured(VirtualObjectID object) const
{
    return aliases.containsIf([&](const AliasEntry& alias) {
        return alias.kind == AliasKind::Results && alias.object == object;
    });
}

const TestEntry* SinkingState::findTest(ValueID test) const
{
    return findEntry(tests, &TestEntry::test, test);
}

void SinkingState::setTest(const TestEntry& entry)
{
    setEntry(tests, &TestEntry::test, entry);
}

bool SinkingDecisions::isBuiltAtHead(BlockID block, VirtualObjectID object) const
{
    return m_objectsBuiltAtHeads.contains(pairOf(block.index(), object.index()));
}

bool SinkingDecisions::holdsCells(VirtualObjectID holder, uint32_t field) const
{
    return m_fieldsHoldingCells.contains(pairOf(holder.index(), field));
}

void SinkingDecisions::buildAtHead(BlockID block, VirtualObjectID object)
{
    if (m_objectsBuiltAtHeads.add(pairOf(block.index(), object.index())).isNewEntry) {
        m_blocksWithObjectsBuiltAtHeads.set(block.index());
        didDecide(object);
    }
}

void SinkingDecisions::holdCells(ValueID parameter, VirtualObjectID boundObject)
{
    if (m_parametersHoldingCells.add(parameter.index()))
        didDecide(boundObject);
}

void SinkingDecisions::holdCells(VirtualObjectID holder, uint32_t field)
{
    if (m_fieldsHoldingCells.add(pairOf(holder.index(), field)).isNewEntry)
        didDecide(holder);
}

void SinkingDecisions::didDecide(VirtualObjectID object)
{
    m_didChange = true;
    m_objectsOfNewDecisions.appendIfNotContains(object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
