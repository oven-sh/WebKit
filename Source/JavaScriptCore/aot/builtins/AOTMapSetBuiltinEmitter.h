/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHashTableBuiltinEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class MapSetBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit MapSetBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_MAP_SET_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

private:
    enum class WeakLookup : uint8_t { MapGet, MapHas, SetHas };

    void createFromIterable(const InlineCall&, HashTableOwner);
    void clear(const InlineCall&, HashTableOwner);
    void createIterator(const InlineCall&, HashTableOwner);
    void lookUpWeakTable(const InlineCall&, WeakLookup);
    void probeWeakTable(const InlineCall&, WeakLookup, CellPointer table, Boxed key);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
