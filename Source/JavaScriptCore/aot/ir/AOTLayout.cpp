/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLayout.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTThreads.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned minFieldsToIndex = 8;

static auto orderByKey(const Vector<Field>& fields)
{
    return [&fields](uint32_t a, uint32_t b) {
        return std::pair(fields[a].key.index(), a) < std::pair(fields[b].key.index(), b);
    };
}

void Layout::indexFields()
{
    m_indexedFieldCount = static_cast<uint32_t>(fields.size());
    m_fieldsByKey.clear();
    if (fields.size() < minFieldsToIndex)
        return;
    m_fieldsByKey.reserveInitialCapacity(fields.size());
    for (uint32_t i = 0; i < fields.size(); ++i) {
        if (fields[i].key)
            m_fieldsByKey.append(i);
    }
    std::ranges::sort(m_fieldsByKey, orderByKey(fields));
}

std::optional<uint32_t> Layout::findField(PropertyKeyID key) const
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size());
    if (!key)
        return std::nullopt;
    if (fields.size() < minFieldsToIndex) {
        for (uint32_t i = 0; i < fields.size(); ++i) {
            if (fields[i].key == key)
                return i;
        }
        return std::nullopt;
    }
    auto found = std::ranges::lower_bound(m_fieldsByKey, key.index(), { }, [&](uint32_t i) {
        return fields[i].key.index();
    });
    if (found != m_fieldsByKey.end() && fields[*found].key == key)
        return *found;
    return std::nullopt;
}

uint32_t Layout::appendField(Field&& field)
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size());
    uint32_t index = static_cast<uint32_t>(fields.size());
    fields.append(WTF::move(field));
    if (fields.size() == minFieldsToIndex) {
        indexFields();
        return index;
    }
    ++m_indexedFieldCount;
    if (fields.size() < minFieldsToIndex || !fields[index].key)
        return index;
    auto position = std::ranges::lower_bound(m_fieldsByKey, index, orderByKey(fields));
    m_fieldsByKey.insert(position - m_fieldsByKey.begin(), index);
    return index;
}

const Field& fieldAt(const Program& program, FieldReference reference)
{
    return program.at(reference.layout).fields[reference.field];
}

FieldReference declarationOf(const Program& program, FieldReference reference)
{
    for (size_t steps = 0; fieldAt(program, reference).flags.contains(FieldFlags::IsInherited); ++steps) {
        RELEASE_ASSERT(steps < program.layouts.size());
        const Field& field = fieldAt(program, reference);
        reference = { field.originLayout, *field.originField };
    }
    return reference;
}

const FieldFlow& flowOf(const Program& program, FieldReference reference)
{
    return fieldAt(program, declarationOf(program, reference)).flow;
}

bool isNeverStored(const Program& program, FieldReference reference)
{
    const Field& declaration = fieldAt(program, declarationOf(program, reference));
    const LocationFlow& location = declaration.flow.location;
    return location.stores == StoreCount::Never && (location.writers == WriterState::AllKnown || declaration.flags.contains(FieldFlags::IsMethod));
}

static LayoutID baseOf(const Layout& layout, BaseChain chain)
{
    switch (layout.flow.baseState) {
    case BaseState::Layout:
        return layout.flow.base;
    case BaseState::Unknown:
        return chain == BaseChain::Predicted ? predictedHeritageOf(layout) : LayoutID();
    case BaseState::None:
    case BaseState::BuiltinClass:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<FieldReference> findField(const Program& program, LayoutID id, PropertyKeyID key, BaseChain chain)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (auto field = layout.findField(key))
            return FieldReference { current, *field };
        current = baseOf(layout, chain);
    }
    return std::nullopt;
}

LayoutID expectedBaseOf(const Layout& layout)
{
    switch (layout.flow.baseState) {
    case BaseState::Layout:
        return layout.flow.base;
    case BaseState::Unknown:
        return layout.predictedBase;
    case BaseState::None:
        return layout.classFlow.prototypeBase;
    case BaseState::BuiltinClass:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LayoutID predictedHeritageOf(const Layout& layout)
{
    return layout.has(LayoutFlags::HasHeritage) ? layout.predictedBase : LayoutID();
}

BuiltinClassID builtinBaseClassOf(const Program& program, LayoutID id)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const LayoutFlow& flow = program.at(current).flow;
        if (flow.baseState == BaseState::BuiltinClass)
            return flow.baseClass;
        current = baseOf(program.at(current), BaseChain::Proven);
    }
    return { };
}

bool isEvaluatedOnce(const Program& program, const Layout& layout)
{
    if (layout.classFlow.evaluations != ExecutionCount::AtMostOnce)
        return false;
    if (!layout.has(LayoutFlags::IsConstructorFunction))
        return true;
    return layout.constructor && program.at(layout.constructor).captures.isEmpty();
}

bool hasSingletonClosure(const Program& program, FunctionID id)
{
    LayoutID constructed = program.at(id).constructedClass;
    if (!constructed)
        return false;
    const Layout& layout = program.at(constructed);
    return layout.has(LayoutFlags::IsConstructorFunction) && layout.constructor == id && isEvaluatedOnce(program, layout);
}

Layout createDerivedLayout(const Program& program, LayoutID base)
{
    const Layout& declared = program.at(base);
    Layout layout;
    layout.kind = declared.kind;
    layout.name = declared.name;
    layout.module = declared.module;
    layout.position = declared.position;
    layout.flags = { LayoutFlags::IsDerived };
    layout.flow.baseState = BaseState::Layout;
    layout.flow.base = base;
    return layout;
}

LayoutID declaredLayoutOf(const Program& program, LayoutID id)
{
    LayoutID current = id;
    while (current && program.at(current).isDerived())
        current = program.at(current).flow.base;
    return current;
}

Vector<LayoutSetID, 2> declaredFamiliesOf(const Program& program, LayoutID id)
{
    Vector<LayoutSetID, 2> families;
    for (LayoutID supertype : program.at(declaredLayoutOf(program, id)).supertypes) {
        if (program.at(supertype).kind == LayoutKind::Record)
            families.append(program.declaredMembersOf(supertype));
    }
    return families;
}

bool isDeclaredIn(const Program& program, std::span<const LayoutSetID> families, PropertyKeyID key)
{
    return std::ranges::any_of(families, [&](LayoutSetID family) {
        return std::ranges::any_of(program.at(family).members, [&](LayoutID member) {
            return program.at(member).findField(key).has_value();
        });
    });
}

void initializeLayouts(Program& program, BuildContext& build)
{
    forEachIndexInParallel(build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        Layout& layout = program.layouts[index];
        if (!layout.ownFieldCount)
            layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
        layout.indexFields();
    });
    program.indexClasses();
    program.indexNames();
}

void initializeAddedLayout(Program& program, LayoutID id)
{
    Layout& layout = program.at(id);
    layout.ownFieldCount = static_cast<uint32_t>(layout.fields.size());
    layout.indexFields();
    program.setDeclaredMembers(id, program.types.intern(LayoutSet { { id }, true }));
}

static bool shouldAppendInheritedFields(const Layout& layout)
{
    if (layout.flow.baseState != BaseState::Layout)
        return false;
    return layout.isDerived() || std::ranges::none_of(layout.fields, [](const Field& field) {
        return field.flags.contains(FieldFlags::IsInherited);
    });
}

static void appendBaseFields(Program& program, LayoutID id)
{
    Layout& layout = program.at(id);
    LayoutID base = layout.flow.base;
    const Layout& baseLayout = program.at(base);
    for (uint32_t i = 0; i < baseLayout.fields.size(); ++i) {
        const Field& baseField = baseLayout.fields[i];
        if (std::optional<uint32_t> present = layout.findField(baseField.key)) {
            Field& field = layout.fields[*present];
            if (field.flags.contains(FieldFlags::IsInherited))
                field.declared = baseField.declared;
            continue;
        }
        FieldReference declaration = declarationOf(program, { base, i });
        Field field = baseField;
        field.flags.add(FieldFlags::IsInherited);
        field.originLayout = declaration.layout;
        field.originField = declaration.field;
        field.flow = { };
        layout.appendField(WTF::move(field));
    }
}

void appendInheritedFields(Program& program, LayoutID id)
{
    Vector<LayoutID, 8> chain;
    for (LayoutID current = id; shouldAppendInheritedFields(program.at(current)); current = program.at(current).flow.base) {
        RELEASE_ASSERT(chain.size() < program.layouts.size());
        chain.append(current);
    }
    while (!chain.isEmpty())
        appendBaseFields(program, chain.takeLast());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
