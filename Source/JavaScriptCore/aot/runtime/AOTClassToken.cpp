/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassToken.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "Symbol.h"
#include <algorithm>
#include <wtf/text/SymbolImpl.h>

namespace JSC { namespace AOT {

static constexpr int32_t noBrandField = -1;

static ALWAYS_INLINE unsigned nameSlotOf(const JSCellButterfly* butterfly, uint32_t field)
{
    RELEASE_ASSERT(static_cast<uint64_t>(ClassToken::firstNameSlot) + field < butterfly->length());
    return ClassToken::firstNameSlot + field;
}

static bool hasFieldNamed(const LayoutData& layout, uint32_t name)
{
    std::span<const FieldData> fields = layout.fields();
    return std::ranges::any_of(fields, [&](const FieldData& field) {
        return field.name == name;
    });
}

static const FieldData* findBrand(const ClassTable& classes, const LayoutData& declaring)
{
    const LayoutData* base = classes.baseOf(declaring);
    for (const FieldData& field : declaring.fields()) {
        if (field.isBrand() && !(base && hasFieldNamed(*base, field.name)))
            return &field;
    }
    return nullptr;
}

JSCellButterfly* ClassToken::create(VM& vm, const LayoutData& classLayout)
{
    JSCellButterfly* butterfly = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, firstNameSlot + classLayout.numberOfFields);
    butterfly->setIndex(vm, classIndexSlot, jsNumber(classLayout.index));
    return butterfly;
}

static int32_t locationOf(PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return static_cast<int32_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue));
    return static_cast<int32_t>(offsetInButterfly(offset) * static_cast<intptr_t>(sizeof(EncodedJSValue)));
}

static PropertyOffset offsetAt(int32_t location)
{
    if (location > 0)
        return static_cast<PropertyOffset>((location - JSObject::offsetOfInlineStorage()) / sizeof(EncodedJSValue));
    return static_cast<PropertyOffset>(firstOutOfLineOffset + offsetInButterfly(firstOutOfLineOffset) - location / static_cast<int32_t>(sizeof(EncodedJSValue)));
}

std::optional<PropertyOffset> ClassToken::findInKnownStructure(JSCell* token, uint32_t field, JSObject* object)
{
    JSValue records = uncheckedDowncast<JSCellButterfly>(token)->get(knownStructuresSlot);
    if (!records) [[unlikely]]
        return std::nullopt;
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(records.asCell());
    JSValue number = jsNumber(static_cast<int32_t>(object->structureID().bits()));
    for (unsigned record = 0; record < recordsPerField; ++record) {
        unsigned first = slotOfRecord(field, record);
        if (butterfly->get(first + StructureNumber) != number)
            continue;
        int32_t brand = butterfly->get(first + LocationOfBrand).asInt32();
        if (brand && object->getDirect(offsetAt(brand)) != JSValue(token))
            return std::nullopt;
        return offsetAt(butterfly->get(first + LocationOfValue).asInt32());
    }
    return std::nullopt;
}

void ClassToken::makeRoomForKnownStructures(VM& vm, State& state, JSCell* token)
{
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(token);
    if (butterfly->get(knownStructuresSlot)) [[likely]]
        return;
    butterfly->setIndex(vm, knownStructuresSlot, JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, slotOfRecord(classLayoutOf(state, token).numberOfFields, 0)));
}

void ClassToken::addKnownStructure(VM& vm, JSCell* token, uint32_t field, Structure* structure, const KnownLocation& location)
{
    JSValue records = uncheckedDowncast<JSCellButterfly>(token)->get(knownStructuresSlot);
    if (!records)
        return;
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(records.asCell());
    for (unsigned record = 0; record < recordsPerField; ++record) {
        unsigned first = slotOfRecord(field, record);
        if (butterfly->get(first + StructureNumber))
            continue;
        butterfly->setIndex(vm, first + KeptStructure, structure);
        butterfly->setIndex(vm, first + LocationOfValue, jsNumber(locationOf(location.value)));
        butterfly->setIndex(vm, first + LocationOfBrand, jsNumber(location.brand ? locationOf(*location.brand) : 0));
        butterfly->setIndex(vm, first + StructureNumber, jsNumber(static_cast<int32_t>(structure->id().bits())));
        return;
    }
}

uint32_t ClassToken::classIndexOf(JSCell* token)
{
    return uncheckedDowncast<JSCellButterfly>(token)->get(classIndexSlot).asUInt32();
}

const LayoutData& ClassToken::classLayoutOf(State& state, JSCell* token)
{
    const LayoutData* classLayout = state.objects->classes.classWithIndex(classIndexOf(token));
    RELEASE_ASSERT(classLayout);
    return *classLayout;
}

const FieldData* ClassToken::brandOf(State& state, const LayoutData& classLayout, JSCell* token)
{
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(token);
    JSValue brandField = butterfly->get(brandFieldSlot);
    if (!brandField) [[unlikely]] {
        const FieldData* brand = findBrand(state.objects->classes, classLayout);
        brandField = jsNumber(brand ? static_cast<int32_t>(classLayout.indexOf(*brand)) : noBrandField);
        butterfly->setIndex(*state.instance.vm, brandFieldSlot, brandField);
    }
    int32_t index = brandField.asInt32();
    return index == noBrandField ? nullptr : &classLayout.fields()[index];
}

Symbol* ClassToken::privateNameIfCreated(JSCell* token, uint32_t field)
{
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(token);
    JSValue name = butterfly->get(nameSlotOf(butterfly, field));
    return name ? asSymbol(name) : nullptr;
}

Symbol* ClassToken::privateNameOf(VM& vm, State& state, JSCell* token, uint32_t field)
{
    if (Symbol* name = privateNameIfCreated(token, field))
        return name;
    const ImageText& text = Image::current()->text();
    String description = text.createString(text.identifier(classLayoutOf(state, token).fields()[field].name).text());
    Symbol* name = Symbol::create(vm, PrivateSymbolImpl::create(*description.impl()).get());
    auto* butterfly = uncheckedDowncast<JSCellButterfly>(token);
    butterfly->setIndex(vm, nameSlotOf(butterfly, field), name);
    return name;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
