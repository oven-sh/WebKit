/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassHierarchy.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT {

ClassHierarchy::ClassHierarchy(State&)
{
}

void ClassHierarchy::didDefineClass(const LayoutData& classLayout)
{
    if (classLayout.assumedBase() == LayoutData::noBase)
        return;
    Vector<uint32_t, 2>& subclasses = m_definedSubclassesByClass.add(classLayout.assumedBase(), Vector<uint32_t, 2>()).iterator->value;
    if (!subclasses.contains(classLayout.index))
        subclasses.append(classLayout.index);
}

std::span<const uint32_t> ClassHierarchy::definedSubclassesOf(const LayoutData& classLayout) const
{
    auto found = m_definedSubclassesByClass.find(classLayout.index);
    if (found == m_definedSubclassesByClass.end())
        return { };
    return found->value.span();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
