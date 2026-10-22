/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFieldTable.h"
#include "AOTIR.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Heat;
struct PlacedSlot;
struct PlannedLayout;

// The form under which the table of fields lists a field with such a slot, by the cases of FieldTableEntry::forField(). None for Form::Field, whose record says how it is read.
JS_EXPORT_PRIVATE std::optional<FieldTableEntry::Form> formInFieldTable(const PlacedSlot&);

// A read by name finds its field in the table of fields and then asks which form the entry has, one form after the other. The form that stands first costs the least.
// Fields of one name mostly hold one kind of value, whichever layout has them. So for each name: the form of most objects that have a field of the name, each layout counted as often as the program is
// estimated to allocate it. A prediction by the name alone: it holds for a receiver of which nothing is known, two equal reads have equal code, and nothing may rely on it.
class FormsExpectedOfNames {
public:
    FormsExpectedOfNames() = default;
    JS_EXPORT_PRIVATE FormsExpectedOfNames(const Program&, std::span<const PlannedLayout>, const Heat&);

    std::optional<FieldTableEntry::Form> of(PropertyKeyID key) const
    {
        if (!key || key.index() >= m_formsPlusOne.size() || !m_formsPlusOne[key.index()])
            return std::nullopt;
        return static_cast<FieldTableEntry::Form>(m_formsPlusOne[key.index()] - 1);
    }

    unsigned endOfNames() const { return m_formsPlusOne.size(); }

private:
    Vector<uint8_t> m_formsPlusOne;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
