/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTVector.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct State;

class ClassHierarchy {
    WTF_MAKE_NONCOPYABLE(ClassHierarchy);
public:
    explicit ClassHierarchy(State&);

    void didDefineClass(const LayoutData& classLayout);
    std::span<const uint32_t> definedSubclassesOf(const LayoutData& classLayout) const;

private:
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_definedSubclassesByClass;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
