/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSCell;
class JSObject;
class Structure;
class VM;

namespace AOT {

struct State;

struct ClassStructures {
    explicit operator bool() const { return !!classObjectStructure; }

    Structure* classObjectStructure { nullptr };
    Structure* prototypeStructure { nullptr };
};

class ClassStructureCache {
    WTF_MAKE_NONCOPYABLE(ClassStructureCache);
public:
    explicit ClassStructureCache(State&);

    ClassStructures find(uint32_t classIndex, JSObject* classObjectParentOrNull, JSCell* prototypeParentOrNull) const;
    void add(uint32_t classIndex, JSObject* classObjectParentOrNull, JSCell* prototypeParentOrNull, ClassStructures);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using Key = std::tuple<uint32_t, JSObject*, JSCell*>;

    static Key keyFor(uint32_t classIndex, JSObject* classObjectParentOrNull, JSCell* prototypeParentOrNull) { return { classIndex + 1, classObjectParentOrNull, prototypeParentOrNull }; }

    UncheckedKeyHashMap<Key, ClassStructures> m_structures;
    RememberedSlotSet m_writes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
