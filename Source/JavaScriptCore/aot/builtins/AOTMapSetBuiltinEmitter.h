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

    void generateSetFromIterable(const InlineCall&);
    void generateMapFromEntries(const InlineCall&);
    void generateWeakMapGet(const InlineCall&);
    void generateWeakMapHas(const InlineCall&);
    void generateWeakSetHas(const InlineCall&);
    void generateMapDelete(const InlineCall&);
    void generateSetDelete(const InlineCall&);
    void generateMapClear(const InlineCall&);
    void generateSetClear(const InlineCall&);
    void generateMapIterator(const InlineCall&);
    void generateSetIterator(const InlineCall&);

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
