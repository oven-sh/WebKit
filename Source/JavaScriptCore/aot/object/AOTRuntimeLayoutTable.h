/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTProgramTables.h"
#include "AOTRuntimeType.h"
#include "JSCJSValue.h"
#include <wtf/Noncopyable.h>

namespace JSC {

class VM;

namespace AOT {

class Object;
struct State;

class RuntimeLayoutTable {
    WTF_MAKE_NONCOPYABLE(RuntimeLayoutTable);
public:
    explicit RuntimeLayoutTable(State&);
    ~RuntimeLayoutTable();

    const LayoutData& layoutForShape(uint32_t shape) const { return m_tables.layoutForShape(shape); }
    bool hasLayoutForShape(uint32_t shape) const { return m_tables.hasLayoutForShape(shape); }
    const AllocationData& allocation(uint32_t number) const { return m_tables.allocations()[number]; }
    const RuntimeType& typeAt(uint32_t byteOffset) const { return m_tables.typeAt(byteOffset); }
    const RuntimeType& typeOf(const FieldData& field) const { return typeAt(field.type); }
    const RuntimeType* typeWithIndex(uint32_t typeIndex) const;

    const FieldData* field(VM&, const LayoutData&, UniquedStringImpl*);
    UniquedStringImpl* nameOf(VM&, const FieldData&);

    bool valueHasType(const RuntimeType&, JSValue) const;
    bool cellPassesTest(const RuntimeCellTest&, JSCell*) const;
    bool shapeHasType(const RuntimeType&, uint32_t shape) const;
    // Only the engine stores a brand. What it stores is a class token, which is no JavaScript value, so no type describes it.
    bool fieldAccepts(const FieldData& field, JSValue value) const { return field.isBrand() || valueHasType(typeOf(field), value); }
    bool passes(const ShapeTestData& test, uint32_t shape) const { return m_tables.passes(test, shape); }
    Object* compiledObject(JSValue) const;

    bool hasSymbolKey(const FieldData&) const;
    bool hasStringKey(const FieldData&) const;
    bool hasPositionAmongKeys(const LayoutData&, UniquedStringImpl*) const;
    bool mayBeAbsent(const FieldData&) const;
    bool everyObjectHadRemovedField(const LayoutData&) const;
    void didReadAbsentField(VM& vm, const LayoutData& layout, UniquedStringImpl* name) const
    {
        if (!m_tables.removedFields().empty()) [[unlikely]]
            failIfFieldWasRemoved(vm, layout, name);
    }
    void willAddProperty(VM& vm, const LayoutData& layout, UniquedStringImpl* name) const
    {
        if (!m_tables.addedNames().empty()) [[unlikely]]
            failIfNameIsNotListed(vm, layout, name);
    }

private:
    struct CachedField {
        uint32_t shape;
        UniquedStringImpl* name;
        const FieldData* field;
    };
    static constexpr unsigned numberOfCachedFields = 4096;

    const FieldData* findField(VM&, const LayoutData&, UniquedStringImpl*);
    void failIfFieldWasRemoved(VM&, const LayoutData&, UniquedStringImpl*) const;
    void failIfNameIsNotListed(VM&, const LayoutData&, UniquedStringImpl*) const;

    State& m_state;
    const ProgramTables& m_tables;
    CachedField* m_cachedFields { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
