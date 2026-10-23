/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFormsExpectedOfNames.h"

#if ENABLE(AOT)

#include "AOTHeat.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

using ExpectedForm = FieldTableEntry::Form;

static constexpr unsigned numberOfExpectedForms = static_cast<unsigned>(ExpectedForm::Field);

// A layout that is estimated never to be allocated still says what a field of the name holds.
static constexpr double weightOfLayoutItself = 1;

std::optional<ExpectedForm> formInFieldTable(const PlacedSlot& slot)
{
    auto formOfWords = [&](ExpectedForm form, uint32_t width) -> std::optional<ExpectedForm> {
        if (slot.offset % width || slot.offset / width > FieldTableEntry::maxOperand)
            return std::nullopt;
        return form;
    };
    if (!slot.hasStorage())
        return std::nullopt;
    switch (slot.representation) {
    case SlotRepresentation::JSValue:
    case SlotRepresentation::Cell:
        return formOfWords(slot.isOutOfLine() ? ExpectedForm::OutOfLineValue : ExpectedForm::InlineValue, sizeof(EncodedJSValue));
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::DeferredClosure:
        return slot.isOutOfLine() ? std::nullopt : formOfWords(ExpectedForm::InlineNarrowCell, sizeof(uint32_t));
    case SlotRepresentation::Int32:
    case SlotRepresentation::OptionalInt32:
        return slot.isOutOfLine() ? std::nullopt : formOfWords(ExpectedForm::InlineInt32, sizeof(uint32_t));
    default:
        return std::nullopt;
    }
}

static bool slotHoldsValueOfProperty(const Field& declaration)
{
    return !declaration.form.contains(FieldForm::IsAccessor) && !declaration.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden });
}

FormsExpectedOfNames::FormsExpectedOfNames(const Program& program, std::span<const PlannedLayout> layouts, const Heat& heat)
{
    Vector<std::array<double, numberOfExpectedForms>> weights;
    for (unsigned index = 0; index < layouts.size(); ++index) {
        const PlannedLayout& planned = layouts[index];
        if (planned.storage != LayoutStorage::CompiledObject)
            continue;
        LayoutID id(index);
        const Layout& layout = program.at(id);
        double weight = weightOfLayoutItself + heat.allocationsOf(id);
        for (FieldIndex field = 0; field < layout.fields.size() && field < planned.slots.size(); ++field) {
            PropertyKeyID key = layout.fields[field].key;
            std::optional<ExpectedForm> form = formInFieldTable(planned.slots[field]);
            if (!key || !form || !slotHoldsValueOfProperty(fieldAt(program, declarationOf(program, { id, field }))))
                continue;
            if (key.index() >= weights.size())
                weights.grow(key.index() + 1);
            weights[key.index()][static_cast<unsigned>(*form)] += weight;
        }
    }

    m_formsPlusOne.fill(0, weights.size());
    for (unsigned key = 0; key < weights.size(); ++key) {
        const auto& ofForms = weights[key];
        unsigned best = std::ranges::max_element(ofForms) - ofForms.begin();
        if (ofForms[best] > 0)
            m_formsPlusOne[key] = best + 1;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
