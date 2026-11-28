/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTFieldTable.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTState.h"
#include "JSArray.h"
#include "JSCJSValueInlines.h"
#include "JSCellInlines.h"

namespace JSC { namespace AOT {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

static constexpr ptrdiff_t bytesPerCacheLine = 64;

static ALWAYS_INLINE void prefetchLines(const void* start, unsigned lines)
{
    const uint8_t* line = static_cast<const uint8_t*>(start);
    __builtin_prefetch(line);
    while (--lines) [[unlikely]] {
        line += bytesPerCacheLine;
        __builtin_prefetch(line);
    }
}

alignas(16) static constexpr uint64_t wordsOfNoCell[2] { };

static ALWAYS_INLINE const JSCell* noCell() { return reinterpret_cast<const JSCell*>(wordsOfNoCell); }

static ALWAYS_INLINE const JSCell* elementAt(const JSCell* cell, uint64_t position)
{
    if (cell->type() != ArrayType || (cell->indexingType() & IndexingShapeMask) != ContiguousShape)
        return noCell();
    Butterfly* butterfly = const_cast<JSObject*>(uncheckedDowncast<JSObject>(cell))->butterfly();
    if (position >= butterfly->publicLength())
        return noCell();
    JSValue value = butterfly->contiguous().atUnsafe(position).get();
    return value && value.isCell() ? value.asCell() : noCell();
}

static_assert(!static_cast<unsigned>(FieldTableEntry::Form::InlineValue));

static ALWAYS_INLINE void prefetchFields(const JSCell* cell, std::span<const PrefetchRequest::Step> steps, const JSCell** row, const uint32_t* entries, uintptr_t cageBase)
{
    if (!isCompiledObject(cell)) {
        for (const PrefetchRequest::Step& field : steps) {
            if (field.step.slotOrCount != PrefetchStep::noSlot)
                row[field.step.slotOrCount] = noCell();
        }
        return;
    }
    const uint8_t* words = reinterpret_cast<const uint8_t*>(cell);
    const uint32_t* entriesOfShape = entries + entries[static_cast<int32_t>(uncheckedDowncast<Object>(cell)->shapeWord())];
    for (const PrefetchRequest::Step& field : steps) {
        uint32_t entry = entriesOfShape[field.name];
        uint32_t place = entry ^ (field.name << FieldTableEntry::nameShift | static_cast<uint32_t>(FieldTableEntry::Form::InlineNarrowCell) << FieldTableEntry::formShift);
        const JSCell* found;
        if (place <= FieldTableEntry::maxOperand) [[likely]] {
            uint32_t word = *reinterpret_cast<const uint32_t*>(words + place * sizeof(uint32_t));
            found = word > undefinedCell32 ? MarkedBlockCage::decode(cageBase, word) : noCell();
        } else if (uint32_t widePlace = entry ^ field.name << FieldTableEntry::nameShift; widePlace <= FieldTableEntry::maxOperand && field.name) {
            JSValue value = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(words + widePlace * sizeof(EncodedJSValue)));
            found = value && value.isCell() ? value.asCell() : noCell();
        } else
            found = noCell();
        if (field.step.slotOrCount != PrefetchStep::noSlot)
            row[field.step.slotOrCount] = found;
        prefetchLines(found, field.step.linesOrDistance);
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPrefetchElements, uint64_t, (JSCell* array, uint64_t position, uint32_t request, void* pointer))
{
    using Rows = State::PrefetchedRows;
    Instance* instance = static_cast<Instance*>(pointer);
    State& state = *instance->state;
    const auto& requested = requestAt<PrefetchRequest>(instance, request);
    unsigned loop = request / sizeof(uint32_t) % State::maxPrefetchingLoops;
    State::PrefetchingLoop& tag = state.prefetchingLoops[loop];
    if (!state.prefetchedRows) [[unlikely]]
        state.prefetchedRows = makeUniqueWithoutFastMallocCheck<std::array<Rows, State::maxPrefetchingLoops>>();
    Rows& rows = (*state.prefetchedRows)[loop];
    if (tag.array != array || tag.request != request || tag.nextPosition != position) [[unlikely]] {
        tag.array = array;
        tag.request = request;
        for (auto& row : rows)
            row.fill(noCell());
    }
    tag.nextPosition = position + 1;
    const uint32_t* entries = instance->image.fieldTableEntries;
    const uintptr_t cageBase = instance->cellCageBase;
    const JSCell** row = nullptr;
    const PrefetchRequest::Step* end = requested.steps + requested.count;
    for (const PrefetchRequest::Step* current = requested.steps; current != end; ++current) {
        PrefetchStep step = current->step;
        switch (step.kind) {
        case PrefetchStep::Kind::Fields:
            prefetchFields(row[step.parent], unsafeMakeSpan(current + 1, step.slotOrCount), row, entries, cageBase);
            current += step.slotOrCount;
            break;
        case PrefetchStep::Kind::Field:
            RELEASE_ASSERT_NOT_REACHED();
        case PrefetchStep::Kind::Row:
            row = rows[(position + step.linesOrDistance) % PrefetchStep::maxRows].data();
            break;
        case PrefetchStep::Kind::Element: {
            row = rows[(position + step.slotOrCount) % PrefetchStep::maxRows].data();
            const JSCell* element = elementAt(array, position + step.slotOrCount);
            row[0] = element;
            prefetchLines(element, step.linesOrDistance);
            break;
        }
        case PrefetchStep::Kind::FirstElement:
        case PrefetchStep::Kind::SecondElement: {
            const JSCell* element = elementAt(row[step.parent], step.kind == PrefetchStep::Kind::SecondElement);
            row[step.slotOrCount] = element;
            prefetchLines(element, step.linesOrDistance);
            break;
        }
        case PrefetchStep::Kind::Storage:
            if (const JSCell* parent = row[step.parent]; parent->type() == ArrayType)
                prefetchLines(reinterpret_cast<const uint8_t*>(const_cast<JSObject*>(uncheckedDowncast<JSObject>(parent))->butterfly()) - sizeof(IndexingHeader), step.linesOrDistance);
            break;
        }
    }
    return 0;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
