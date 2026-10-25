/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class UseLists;

BuiltinID builtinParentNamedByClassRecord(const Program&, const Layout&);

struct ClassDefinitionSite {
    LayoutID layout;
    ValueID classObject;
    Vector<ValueID, 8> describedByRecord;
    Vector<ValueID, 2> copies;
    Vector<ValueID, 2> stores;
};

std::optional<ClassDefinitionSite> findClassDefinitionSite(const Program&, const Function&, const UseLists&, ValueID newClass);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
