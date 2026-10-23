/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFieldTable.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class Heat;
struct PlacedSlot;
struct PlannedLayout;

// The form under which the table of fields lists a field with such a slot, by the cases of FieldTableEntry::forField(). None for Form::Field, whose record says how it is read.
JS_EXPORT_PRIVATE std::optional<FieldTableEntry::Form> formInFieldTable(const PlacedSlot&);

// A read by name finds its field in the table of fields and then asks which form the entry has, one form after the other.
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
