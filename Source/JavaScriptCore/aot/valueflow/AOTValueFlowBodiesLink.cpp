/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltins.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperands.h"
#include "AOTPhaseTimes.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include "AOTValueSets.h"
#include <algorithm>
#include <wtf/MonotonicTime.h>

namespace JSC { namespace AOT { namespace Typed {

ValueFlowObjectID ValueFlowUniverse::objectOf(BuiltinID builtin, ModuleID module) const
{
    ModuleBuiltin wanted { m_builtins->canonicalID(builtin), module };
    auto found = std::ranges::lower_bound(m_moduleBuiltins, wanted);
    if (found == m_moduleBuiltins.end() || *found != wanted)
        return { };
    return ValueFlowObjectID(m_firstModuleBuiltin + static_cast<uint32_t>(found - m_moduleBuiltins.begin()));
}

void ValueFlowUniverse::appendObjectsOf(BuiltinID builtin, Vector<ValueFlowObjectID, 32>& objects) const
{
    BuiltinID canonical = m_builtins->canonicalID(builtin);
    objects.append(objectOf(canonical));
    auto first = std::ranges::lower_bound(m_moduleBuiltins, canonical, { }, &ModuleBuiltin::builtin);
    for (; first != m_moduleBuiltins.end() && first->builtin == canonical; ++first)
        objects.append(ValueFlowObjectID(m_firstModuleBuiltin + static_cast<uint32_t>(first - m_moduleBuiltins.begin())));
}

ValueFlowObjectID ValueFlowUniverse::siteObject(const ValueFlowStatement& statement, uint32_t ordinal) const
{
    if (statement.firstSite == UINT32_MAX)
        return { };
    if (m_sites[statement.firstSite].ordinal == ordinal)
        return m_objectOfSite[statement.firstSite];
    auto keyOf = [](const ValueFlowSite& site) {
        return std::pair { site.value, site.ordinal };
    };
    std::span<const ValueFlowSite> sites = m_sites.subspan(statement.firstSite, m_firstSiteOfFunction[statement.function.index() + 1] - statement.firstSite);
    auto found = std::ranges::lower_bound(sites, std::pair { statement.site, ordinal }, { }, keyOf);
    if (found == sites.end() || keyOf(*found) != std::pair { statement.site, ordinal })
        return { };
    return m_objectOfSite[statement.firstSite + static_cast<uint32_t>(found - sites.begin())];
}

bool ValueFlowUniverse::isSiteObjectOf(const ValueFlowStatement& statement, ValueFlowObjectID object) const
{
    if (statement.firstSite == UINT32_MAX)
        return false;
    uint32_t end = m_firstSiteOfFunction[statement.function.index() + 1];
    for (uint32_t site = statement.firstSite; site < end && m_sites[site].value == statement.site; ++site) {
        if (m_objectOfSite[site] == object)
            return true;
    }
    return false;
}

ValueFlowObjectID ValueFlowUniverse::siteObject(FunctionID function, ValueID value, uint32_t ordinal) const
{
    uint32_t first = m_firstSiteOfFunction[function.index()];
    std::span<const ValueFlowSite> sites = m_sites.subspan(first, m_firstSiteOfFunction[function.index() + 1] - first);
    auto keyOf = [](const ValueFlowSite& site) {
        return std::pair { site.value, site.ordinal };
    };
    auto found = std::ranges::lower_bound(sites, std::pair { value, ordinal }, { }, keyOf);
    if (found == sites.end() || keyOf(*found) != std::pair { value, ordinal })
        return { };
    return m_objectOfSite[first + static_cast<uint32_t>(found - sites.begin())];
}

ValueFlowObjectID ValueFlowUniverse::objectNamed(FunctionID body, const ValueFlowObjectName& name) const
{
    switch (name.kind) {
    case ValueFlowObjectKind::Primitive:
        return ValueFlowObjectID(name.index);
    case ValueFlowObjectKind::UnknownObject:
        return unknownObject();
    case ValueFlowObjectKind::Builtin:
        return objectOf(BuiltinID(name.index));
    case ValueFlowObjectKind::ModuleBuiltin:
        return objectOf(BuiltinID(name.index), name.module);
    case ValueFlowObjectKind::Namespace:
        return namespaceOf(ModuleID(name.index));
    case ValueFlowObjectKind::Closure:
        return closureOf(FunctionID(name.index));
    case ValueFlowObjectKind::CoroutineResult:
        return coroutineResultOf(FunctionID(name.index));
    case ValueFlowObjectKind::RestArray:
        return restArrayOf(FunctionID(name.index));
    case ValueFlowObjectKind::FunctionPrototype:
        return prototypeObjectOf(FunctionID(name.index));
    case ValueFlowObjectKind::ConstructedObject:
        return constructedObjectOf(FunctionID(name.index));
    case ValueFlowObjectKind::Site:
        return m_objectOfSite[m_firstSiteOfFunction[body.index()] + name.index];
    case ValueFlowObjectKind::Instance:
        return instanceOf(LayoutID(name.index));
    case ValueFlowObjectKind::Prototype:
        return prototypeOf(LayoutID(name.index));
    case ValueFlowObjectKind::ResultOfBuiltin:
        return ValueFlowObjectID(m_firstResultOfBuiltin + name.index);
    case ValueFlowObjectKind::Key:
        return keyConstant(PropertyKeyID(name.index));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueFlowObjectKind ValueFlowUniverse::kindOf(ValueFlowObjectID object) const
{
    uint32_t index = object.index();
    RELEASE_ASSERT(index < m_numberOfObjects);
    if (index >= m_firstSite)
        return index < m_firstKey ? ValueFlowObjectKind::Site : ValueFlowObjectKind::Key;
    if (index < numberOfPrimitiveKinds)
        return ValueFlowObjectKind::Primitive;
    if (index < m_firstBuiltin)
        return ValueFlowObjectKind::UnknownObject;
    if (index < m_firstModuleBuiltin)
        return ValueFlowObjectKind::Builtin;
    if (index < m_firstNamespace)
        return ValueFlowObjectKind::ModuleBuiltin;
    if (index < m_firstLayoutObject)
        return ValueFlowObjectKind::Namespace;
    if (index < m_firstResultOfBuiltin)
        return (index - m_firstLayoutObject) % 2 ? ValueFlowObjectKind::Prototype : ValueFlowObjectKind::Instance;
    if (index < m_firstClosure)
        return ValueFlowObjectKind::ResultOfBuiltin;
    if (index < m_firstCoroutineResult)
        return ValueFlowObjectKind::Closure;
    if (index < m_firstRestArray)
        return ValueFlowObjectKind::CoroutineResult;
    if (index < m_firstFunctionPrototype)
        return ValueFlowObjectKind::RestArray;
    if (index < m_firstConstructedObject)
        return ValueFlowObjectKind::FunctionPrototype;
    return ValueFlowObjectKind::ConstructedObject;
}

FunctionID ValueFlowUniverse::functionOf(ValueFlowObjectID object) const
{
    uint32_t index = object.index();
    switch (kindOf(object)) {
    case ValueFlowObjectKind::Closure:
        return FunctionID(index - m_firstClosure);
    case ValueFlowObjectKind::CoroutineResult:
        return FunctionID(index - m_firstCoroutineResult);
    case ValueFlowObjectKind::RestArray:
        return FunctionID(index - m_firstRestArray);
    case ValueFlowObjectKind::FunctionPrototype:
        return FunctionID(index - m_firstFunctionPrototype);
    case ValueFlowObjectKind::ConstructedObject:
        return FunctionID(index - m_firstConstructedObject);
    case ValueFlowObjectKind::Site:
        return m_functionOfSiteObject[index - m_firstSite];
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinID ValueFlowUniverse::builtinOf(ValueFlowObjectID object) const
{
    switch (kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
        return BuiltinID(object.index() - m_firstBuiltin);
    case ValueFlowObjectKind::ModuleBuiltin:
        return m_moduleBuiltins[object.index() - m_firstModuleBuiltin].builtin;
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::Closure:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
    case ValueFlowObjectKind::Site:
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ModuleID ValueFlowUniverse::moduleWithBody(FunctionID function) const
{
    auto found = std::ranges::lower_bound(m_modulesWithBodies, function, { }, &std::pair<FunctionID, ModuleID>::first);
    return found != m_modulesWithBodies.end() && found->first == function ? found->second : ModuleID();
}

ModuleID ValueFlowUniverse::moduleOf(ValueFlowObjectID object) const
{
    switch (kindOf(object)) {
    case ValueFlowObjectKind::ModuleBuiltin:
        return m_moduleBuiltins[object.index() - m_firstModuleBuiltin].module;
    case ValueFlowObjectKind::Namespace:
        return ModuleID(object.index() - m_firstNamespace);
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::Closure:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
    case ValueFlowObjectKind::Site:
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool layoutIsDefinedByClass(const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
}

LayoutID ValueFlowUniverse::layoutOf(ValueFlowObjectID object) const
{
    switch (kindOf(object)) {
    case ValueFlowObjectKind::Site:
        return siteOf(object)->layout;
    case ValueFlowObjectKind::Instance:
        return LayoutID((object.index() - m_firstLayoutObject) / 2);
    case ValueFlowObjectKind::Namespace:
        return m_layoutOfNamespace[object.index() - m_firstNamespace];
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
    case ValueFlowObjectKind::Closure:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueFlowObjectID ValueFlowUniverse::objectAllocatedUnder(const Program& program, ValueFlowObjectID site, ValueFlowObjectID prototype) const
{
    LayoutID requested = classLayoutOf(prototype);
    if (!requested || !layoutIsDefinedByClass(program.at(requested)))
        return site;
    LayoutID allocated = siteOf(site)->layout;
    LayoutID ownClass = declaredLayoutOf(program, allocated);
    if (requested == ownClass)
        return allocated == ownClass ? instanceOf(requested) : site;
    LayoutID current = requested;
    for (size_t steps = 0; steps < program.layouts.size() && current && program.at(current).has(LayoutFlags::HasHeritage); ++steps) {
        LayoutID above = predictedHeritageOf(program.at(current));
        if (above == ownClass || (!above && !m_classesNamedAboveSeveralClasses.get(current.index())))
            return instanceOf(requested);
        current = above;
    }
    return site;
}

BuiltinClassID ValueFlowUniverse::classOf(ValueFlowObjectID object) const
{
    switch (kindOf(object)) {
    case ValueFlowObjectKind::Site:
        return siteOf(object)->builtinClass;
    case ValueFlowObjectKind::Closure:
        return BuiltinClasses::Function;
    case ValueFlowObjectKind::RestArray:
        return BuiltinClasses::Array;
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
        return BuiltinClasses::Object;
    case ValueFlowObjectKind::CoroutineResult:
        return m_classOfCoroutineResult[functionOf(object).index()];
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ValueFlowUniverse::appendFunctionObjectsOf(const Program& program, const Type& type, Vector<ValueFlowObjectID, 32>& members, Vector<ValueSetID, 8>& parts) const
{
    bool listsFunctionOfLibrary = false;
    for (FunctionID function : type.functions) {
        members.append(closureOf(function));
        if (!isLibraryCode(program, program.at(function)))
            continue;
        listsFunctionOfLibrary = true;
        if (const RuntimeFunctionData* record = findTableRecordOf(program, function)) {
            for (BuiltinID implemented : record->implements) {
                if (m_isCanonicalEntryOfCallableBuiltin.get(implemented.index()))
                    members.append(objectOf(implemented));
            }
        }
        auto found = m_builtinsThatMayBeClosureOf.find(function.index());
        if (found != m_builtinsThatMayBeClosureOf.end())
            members.append(found->value.span());
    }
    if (listsFunctionOfLibrary)
        parts.append(m_builtinsThatMayBeAnyLibraryClosure);
    for (BuiltinID builtin : type.builtins)
        appendObjectsOf(builtin, members);
}

static ValueSetID rememberedSetOr(std::atomic<uint32_t>* remembered, NOESCAPE const Invocable<ValueSetID()> auto& make)
{
    if (remembered) {
        if (uint32_t setPlusOne = remembered->load(std::memory_order_acquire))
            return ValueSetID(setPlusOne - 1);
    }
    ValueSetID set = make();
    if (remembered)
        remembered->store(set.index() + 1, std::memory_order_release);
    return set;
}

ValueSetID ValueFlowUniverse::objectsWithLayoutIn(ValueSetMaker& maker, const Program& program, LayoutSetID layouts) const
{
    if (!layouts)
        return ValueSets::nothing;
    return rememberedSetOr(layouts.index() < m_numberOfRememberedLayoutSets ? &m_objectsWithLayoutIn[layouts.index()] : nullptr, [&] {
        Vector<ValueSetID, 8> objectsOfLayouts;
        for (LayoutID layout : program.at(layouts).completeMembers())
            objectsOfLayouts.append(m_objectsOfLayout[layout.index()]);
        return maker.uniteAll(objectsOfLayouts.span());
    });
}

static void appendKeysInside(const ValueFlowUniverse& universe, const Program& program, const Type& type, ValueSetID textKeys, ValueSetID symbolKeys, Vector<ValueFlowObjectID, 32>& members, auto& parts)
{
    if (type.primitives & Primitive::String)
        parts.append(textKeys);
    else {
        for (StringID literal : type.stringLiterals) {
            if (std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = literal }))
                members.append(universe.keyConstant(*key));
        }
    }
    if (type.primitives & Primitive::Symbol)
        parts.append(symbolKeys);
    else {
        for (PropertyKeyID literal : type.symbolLiterals)
            members.append(universe.keyConstant(literal));
    }
}

ValueSetID ValueFlowUniverse::maskOf(ValueSetMaker& maker, const Program& program, const Type& type) const
{
    if (!type.isJavaScriptValue() || type.isAnything())
        return ValueSets::anything;

    Vector<ValueFlowObjectID, 32> members;
    Vector<ValueSetID, 8> parts;
    for (uint16_t rest = type.primitives & ~(Primitive::LiteralString | Primitive::LiteralSymbol); rest; rest &= rest - 1)
        members.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
    if (type.primitives & Primitive::AnyString)
        members.append(primitive(PrimitiveSet::String));
    if (type.primitives & Primitive::AnySymbol)
        members.append(primitive(PrimitiveSet::Symbol));
    appendKeysInside(*this, program, type, m_textKeys, m_symbolKeys, members, parts);

    if (type.anyObject)
        parts.append(m_objects);
    else if (type.hasObjects()) {
        parts.append(m_objectsWithAnyLayout);
        if (type.anyFunction) {
            parts.append(m_callables);
            parts.append(m_builtinObjects);
        }
        parts.append(objectsWithLayoutIn(maker, program, type.layouts));
        for (const ClassPart& part : type.classes)
            parts.append(m_objectsOfClass[part.builtinClass.index()]);
        appendFunctionObjectsOf(program, type, members, parts);
    }

    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

ValueSetID ValueFlowUniverse::familyObjectsOf(ValueSetMaker& maker, const Program& program, const Type& type) const
{
    Vector<ValueFlowObjectID, 32> members;
    Vector<ValueSetID, 8> parts;
    parts.append(objectsWithLayoutIn(maker, program, type.layouts));
    appendFunctionObjectsOf(program, type, members, parts);
    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

bool ValueFlowUniverse::namesProgramObjectsAlone(const Program& program, const Type& type) const
{
    if (!type.isJavaScriptValue() || type.anyObject || type.anyFunction || !type.classes.isEmpty())
        return false;
    if (!type.layouts)
        return !type.functions.isEmpty() || !type.builtins.isEmpty();
    bool isAllocated = !type.functions.isEmpty() || !type.builtins.isEmpty();
    for (LayoutID layout : membersOf(program, type)) {
        if (m_isLayoutOfTable.quickGet(layout.index()))
            return false;
        isAllocated |= m_isAllocatedByProgram.quickGet(layout.index());
    }
    return isAllocated;
}

ValueSetID ValueFlowUniverse::objectsSurelyWithLayoutIn(ValueSetMaker& maker, const Program& program, LayoutSetID layouts) const
{
    if (!layouts)
        return ValueSets::nothing;
    return rememberedSetOr(layouts.index() < m_numberOfRememberedLayoutSets ? &m_objectsSurelyWithLayoutIn[layouts.index()] : nullptr, [&] {
        Vector<ValueSetID, 4> objectsOfLayouts;
        for (LayoutID layout : program.at(layouts).completeMembers()) {
            if (!m_isLayoutOfTable.quickGet(layout.index()))
                objectsOfLayouts.append(m_objectsOfLayout[layout.index()]);
        }
        ValueSetID withoutSureLayout = maker.unite(m_objectsThatPickTheirLayout, m_objectsThatMayBeAnything);
        return maker.intersect(maker.uniteAll(objectsOfLayouts.span()), ValueSets::complement(withoutSureLayout));
    });
}

ValueSetID ValueFlowUniverse::membersSurelyInside(ValueSetMaker& maker, const Program& program, const Type& type) const
{
    if (!type.isJavaScriptValue())
        return ValueSets::nothing;

    Vector<ValueFlowObjectID, 32> members;
    Vector<ValueSetID, 4> parts;
    uint16_t wholeKinds = type.primitives & ~(Primitive::LiteralString | Primitive::LiteralSymbol);
    if (type.integers != Range::full())
        wholeKinds &= ~Primitive::Integer;
    for (uint16_t rest = wholeKinds; rest; rest &= rest - 1)
        members.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
    appendKeysInside(*this, program, type, m_textKeys, m_symbolKeys, members, parts);
    if (type.anyObject)
        parts.append(m_objects);
    else {
        if (type.anyFunction)
            parts.append(m_closures);
        for (FunctionID function : type.functions)
            members.append(closureOf(function));
        parts.append(objectsSurelyWithLayoutIn(maker, program, type.layouts));
        for (const ClassPart& part : type.classes)
            parts.append(m_sureInstancesOfClass[part.builtinClass.index()]);
    }
    for (BuiltinID builtin : type.builtins) {
        if (m_builtins->at(builtin).scope == BuiltinScope::Realm)
            members.append(objectOf(builtin));
    }

    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

ValueSetID ValueFlowUniverse::membersSurelyInside(ValueSetMaker& maker, const Program& program, TypeID type) const
{
    return rememberedSetOr(type.index() < m_numberOfRememberedTypes ? &m_membersSurelyInsideType[type.index()] : nullptr, [&] {
        return membersSurelyInside(maker, program, program.at(type));
    });
}

FunctionID ValueFlowProgramNodes::functionOf(ValueFlowNodeID node) const
{
    if (node.index() < m_firstNodeOfFunction[0])
        return { };
    auto next = std::ranges::upper_bound(m_firstNodeOfFunction, node.index());
    return FunctionID(static_cast<uint32_t>(next - m_firstNodeOfFunction.begin()) - 1);
}

ValueFlowNodeID ValueFlowProgramNodes::valueOf(const UnobservedFieldDefinition& definition) const
{
    for (const UnobservedDefinitions& entry : std::ranges::equal_range(m_unobservedDefinitionsByInitializer, definition.initializer, { }, &UnobservedDefinitions::initializer)) {
        if (entry.areObservedWhereObjectRefusesDefinition == definition.isObservedWhereObjectRefusesDefinition)
            return entry.value;
    }
    return { };
}

bool ValueFlowProgramNodes::isValueOfUnobservedDefinitions(FunctionID initializer, ValueFlowNodeID node) const
{
    return std::ranges::any_of(std::ranges::equal_range(m_unobservedDefinitionsByInitializer, initializer, { }, &UnobservedDefinitions::initializer), [&](const UnobservedDefinitions& entry) {
        return entry.value == node;
    });
}

std::span<const ValueFlowProgramNodes::UnobservedDefinitions> ValueFlowProgramNodes::unobservedDefinitionsBehindSuperOf(LayoutID classCallingSuper) const
{
    auto found = std::ranges::equal_range(m_unobservedDefinitionsByClass, classCallingSuper, { }, &UnobservedDefinitions::classCallingSuper);
    return std::span(found.begin(), found.end());
}

class ValueFlowLinker {
    WTF_MAKE_NONCOPYABLE(ValueFlowLinker);
public:
    Vector<LayoutID> findRecordsThatTypedCodeTakes() const
    {
        BitVector isSeenLayout(m_program.layouts.size());
        BitVector isSeenType(m_program.types.size());
        Vector<LayoutID> layouts;
        Vector<TypeID, 16> types;
        auto take = [&](TypeID type) {
            if (type && !isSeenType.quickSet(type.index()))
                types.append(type);
        };
        for (const Function& function : m_program.functions) {
            if (!function.flags.contains(FunctionFlags::IsChecked) || function.role != FunctionRole::Source || !function.module)
                continue;
            const Module& module = m_program.at(function.module);
            if (module.kind == ModuleKind::RuntimeSource || module.flags.contains(ModuleFlags::IsLibrary))
                continue;
            bool takesRecord = false;
            for (const DeclaredType& parameter : function.parameters) {
                take(parameter.written());
                takesRecord |= parameter.written() && !membersOf(m_program, m_program.at(parameter.written())).empty();
            }
            if (!takesRecord)
                continue;
            for (const Value& value : function.values) {
                if (value.opcode == Opcode::Check)
                    take(value.expectedType().predicted());
            }
        }
        while (!types.isEmpty()) {
            const Type& type = m_program.at(types.takeLast());
            for (const ClassPart& part : type.classes) {
                for (TypeID contents : part.writtenContents)
                    take(contents);
            }
            for (LayoutID member : membersOf(m_program, type)) {
                if (isSeenLayout.quickSet(member.index()))
                    continue;
                layouts.append(member);
                for (const Field& field : m_program.at(member).fields)
                    take(field.declared.written());
            }
        }

        constexpr OptionSet<LayoutFlags> flagsOfOtherLayouts { LayoutFlags::HasHeritage, LayoutFlags::IsConstructorFunction, LayoutFlags::HasMembersWithoutLayout, LayoutFlags::IsTypeParameter, LayoutFlags::IsNeverAllocated, LayoutFlags::IsDerived };
        layouts.removeAllMatching([&](LayoutID id) {
            const Layout& layout = m_program.at(id);
            if (layout.kind != LayoutKind::Record || layout.constructor || layout.base || layout.flags.containsAny(flagsOfOtherLayouts))
                return true;
            bool requiresField = false;
            for (const Field& field : layout.fields) {
                if (!field.flags.contains(FieldFlags::IsRequired))
                    continue;
                requiresField = true;
                const Type& type = m_program.at(field.declared.expected().predicted());
                bool admitsData = type.primitives & (Primitive::Defined & ~Primitive::Undefined) || type.anyObject || type.layouts || !type.classes.isEmpty() || !type.stringLiterals.isEmpty();
                if (field.flags.contains(FieldFlags::IsMethod) || !admitsData)
                    return true;
            }
            return !requiresField;
        });
        if (layouts.size() > m_build.limit(LimitID::MaxLayoutsInType))
            return { };
        AOT::sort(layouts);
        return layouts;
    }

    ValueFlowLinker(const Program& program, BuildContext& build, Vector<ValueFlowNotedOperations>&& notedOperations)
        : m_program(program)
        , m_build(build)
        , m_loops(build.parallelLoops())
        , m_numberOfFunctions(program.functions.size())
    {
        m_result.m_notedOperations = WTF::move(notedOperations);
    }

    ValueFlowBodies link()
    {
        MonotonicTime start = MonotonicTime::now();
        PhaseTimes times(m_build, PassID::AnalyzeFlow);
        auto takeTime = [&](Seconds& time, ASCIILiteral phase) {
            MonotonicTime now = MonotonicTime::now();
            time = now - start;
            start = now;
            times.didFinish(phase);
        };

        m_result.m_universe.m_builtinTables = makeUnique<ValueFlowBuiltinTables>(m_program.builtins(), m_build.limit(LimitID::MaxObjectsOfTypedResult));
        const ValueFlowBuiltinTables& tables = *m_result.m_universe.m_builtinTables;
        if (m_result.m_notedOperations.isEmpty()) {
            m_result.m_notedOperations.fill(0, m_numberOfFunctions);
            forEachIndexInParallel(m_loops, m_numberOfFunctions, [&](unsigned index) {
                m_result.m_notedOperations[index] = notedOperationsOf(m_program.functions[index]);
            });
        }
        RELEASE_ASSERT(m_result.m_notedOperations.size() == m_numberOfFunctions);
        m_result.m_unobservedDefinitions = UnobservedFieldDefinitions(m_program, m_build, [&](FunctionID function) {
            return m_result.holdsAnyOf(function, notedOperations({ ValueFlowNotedOperation::CallDirect }));
        });
        Vector<uint32_t> largestFirst = largeFunctions(m_program, m_loops);
        largestFirst.insert(0, m_numberOfFunctions);
        runTasksInParallel(m_loops, {
            [&] {
                m_result.m_universe.m_recordsThatTypedCodeTakes = findRecordsThatTypedCodeTakes();
            },
            [&] {
                m_bodies = ParallelArray<ValueFlowBody>(m_loops, m_numberOfFunctions + 1, largestFirst.span(), [&](unsigned index) {
                    if (index == m_numberOfFunctions)
                        return makeHostBody(m_program, tables, m_result.m_notedOperations.span());
                    return reduceBody(m_program, tables, FunctionID(index), &m_result.m_unobservedDefinitions);
                });
            },
        });
        for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
            FunctionID body = m_program.modules[index].body;
            if (!body || !m_program.at(body).hasBody() || m_program.at(body).module == ModuleID(index))
                continue;
            ValueFlowBody& reduced = m_bodies[body.index()];
            reduced.statements.append({ { }, { }, static_cast<uint32_t>(reduced.operands.size()), 0, { }, { }, ValueFlowStatementKind::ModuleEvaluation });
        }
        m_result.m_valuesBehindChecks.grow(m_numberOfFunctions);
        m_result.m_knowsValuesBehindChecks.fill(0, m_numberOfFunctions);
        forEachIndexInParallel(m_loops, (m_numberOfFunctions + bodiesInChunk - 1) / bodiesInChunk, [&](unsigned chunk) {
            unsigned end = std::min(m_numberOfFunctions, (chunk + 1) * bodiesInChunk);
            for (unsigned index = chunk * bodiesInChunk; index < end; ++index) {
                m_result.m_knowsValuesBehindChecks[index] = m_bodies[index].knowsValuesBehindChecks;
                m_result.m_valuesBehindChecks[index] = WTF::move(m_bodies[index].valuesBehindChecks);
            }
        });
        takeTime(m_result.m_times.reduce, "solve: bodies: reduced"_s);
        runTasksInParallel(m_loops, {
            [&] {
                numberObjects();
                takeTime(m_result.m_times.number, "solve: bodies: objects numbered"_s);
                m_result.m_sets = makeUnique<ValueSets>(m_result.m_universe.m_numberOfObjects);
                makeMasksOfUniverse();
                takeTime(m_result.m_times.makeMasks, "solve: bodies: masks of the universe"_s);
            },
            [&] {
                PhaseTimes timesBeside(m_build, PassID::AnalyzeFlow);
                numberNodes();
                layOutLists();
                timesBeside.didFinish("solve: bodies: nodes numbered and lists laid out, beside the masks"_s);
            },
        });
        m_result.m_universe.m_builtinTables->makeSets(m_program, m_result.m_universe, *m_result.m_sets, m_loops, [&] {
            forEachIndexInParallel(m_loops, m_bodies.size(), [&](unsigned index, unsigned worker) {
                linkBody(index, makerOf(worker), m_setsOfType[worker]);
            });
        });
        takeTime(m_result.m_times.link, "solve: bodies: linked"_s);
        m_build.destroyLater(WTF::move(m_bodies));
        m_build.destroyLater(WTF::move(m_makers));
        m_build.destroyLater(WTF::move(m_setsOfType));
        return WTF::move(m_result);
    }

private:
    using Body = ValueFlowBody;

    struct SetsOfType {
        ValueSetID mask;
        ValueSetID familyObjects;
    };

    struct LinkedMask {
        ValueSetID set { ValueSets::anything };
        ValueFlowMaskKind kind { ValueFlowMaskKind::Plain };
        ValueSetID familyObjects;
    };

    struct FirstInLists {
        uint32_t operand;
        uint32_t edge;
        uint32_t mask;
        uint32_t observedValue;
        uint32_t comparedValues;
        uint32_t valueAliveForComparisonsOnly;
    };

    static constexpr unsigned bodiesInChunk = 1024;

    template<typename Sums>
    Sums sumOverBodies(NOESCAPE const Invocable<void(unsigned, Sums&)> auto& add, NOESCAPE const Invocable<void(unsigned, const Sums&)> auto& place) const
    {
        unsigned count = m_bodies.size();
        Vector<Sums> sumsOfChunks((count + bodiesInChunk - 1) / bodiesInChunk);
        forEachIndexInParallel(m_loops, sumsOfChunks.size(), [&](unsigned chunk) {
            Sums sums { };
            unsigned end = std::min(count, (chunk + 1) * bodiesInChunk);
            for (unsigned index = chunk * bodiesInChunk; index < end; ++index)
                add(index, sums);
            sumsOfChunks[chunk] = sums;
        });
        Sums total { };
        for (Sums& sums : sumsOfChunks) {
            Sums ofChunk = std::exchange(sums, total);
            total += ofChunk;
        }
        forEachIndexInParallel(m_loops, sumsOfChunks.size(), [&](unsigned chunk) {
            Sums sums = sumsOfChunks[chunk];
            unsigned end = std::min(count, (chunk + 1) * bodiesInChunk);
            for (unsigned index = chunk * bodiesInChunk; index < end; ++index) {
                place(index, sums);
                add(index, sums);
            }
        });
        return total;
    }

    ValueSetMaker& makerOf(unsigned worker)
    {
        std::unique_ptr<ValueSetMaker>& maker = m_makers[worker];
        if (!maker)
            maker = makeUnique<ValueSetMaker>(*m_result.m_sets);
        return *maker;
    }

    void numberObjects()
    {
        ValueFlowUniverse& universe = m_result.m_universe;
        universe.m_builtins = &m_program.builtins();
        universe.m_moduleBuiltins = collectInParallel<ValueFlowUniverse::ModuleBuiltin>(m_loops, m_bodies.size(), [&](unsigned index, Vector<ValueFlowUniverse::ModuleBuiltin>& found) {
            for (const Body::Constant& constant : m_bodies[index].constants) {
                for (const ValueFlowObjectName& object : constant.objects) {
                    if (object.kind == ValueFlowObjectKind::ModuleBuiltin)
                        found.append({ BuiltinID(object.index), object.module });
                }
            }
        });
        AOT::sort(universe.m_moduleBuiltins);
        universe.m_moduleBuiltins.shrink(std::ranges::unique(universe.m_moduleBuiltins).begin() - universe.m_moduleBuiltins.begin());
        for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
            if (FunctionID body = m_program.modules[index].body)
                universe.m_modulesWithBodies.append({ body, ModuleID(index) });
        }
        AOT::sort(universe.m_modulesWithBodies);

        universe.m_firstBuiltin = ValueFlowUniverse::markOfLoadedBun().index() + 1;
        universe.m_firstModuleBuiltin = universe.m_firstBuiltin + m_program.builtins().builtinCount();
        universe.m_firstNamespace = universe.m_firstModuleBuiltin + universe.m_moduleBuiltins.size();
        universe.m_firstLayoutObject = universe.m_firstNamespace + m_program.modules.size();

        universe.m_firstResultOfBuiltin = universe.m_firstLayoutObject + 2 * m_program.layouts.size();
        universe.m_firstClosure = universe.m_firstResultOfBuiltin + 2 * m_program.builtins().builtinCount() + m_program.builtins().hostObjects().size();
        universe.m_firstCoroutineResult = universe.m_firstClosure + m_numberOfFunctions;
        universe.m_firstRestArray = universe.m_firstCoroutineResult + m_numberOfFunctions;
        universe.m_firstFunctionPrototype = universe.m_firstRestArray + m_numberOfFunctions;
        universe.m_firstConstructedObject = universe.m_firstFunctionPrototype + m_numberOfFunctions;
        universe.m_firstSite = universe.m_firstConstructedObject + m_numberOfFunctions;

        RELEASE_ASSERT(m_bodies.size() == m_numberOfFunctions + 1 && m_bodies[m_numberOfFunctions].sites.isEmpty());
        universe.m_firstSiteOfFunction.grow(m_numberOfFunctions + 1);
        uint32_t numberOfSites = sumOverBodies<uint32_t>([&](unsigned index, uint32_t& sites) {
            sites += m_bodies[index].sites.size();
        }, [&](unsigned index, const uint32_t& sites) {
            universe.m_firstSiteOfFunction[index] = sites;
        });
        universe.m_sites.grow(numberOfSites);
        forEachIndexInParallel(m_loops, (m_numberOfFunctions + bodiesInChunk - 1) / bodiesInChunk, [&](unsigned chunk) {
            unsigned end = std::min(m_numberOfFunctions, (chunk + 1) * bodiesInChunk);
            for (unsigned index = chunk * bodiesInChunk; index < end; ++index)
                std::ranges::copy(m_bodies[index].sites, universe.m_sites.mutableSpan().subspan(universe.m_firstSiteOfFunction[index]).begin());
        });

        Vector<uint32_t> firstTypedResultOfLayout(m_program.layouts.size(), [](size_t) {
            return UINT32_MAX;
        });
        Vector<uint32_t> siteWithObjectOf(numberOfSites, [](size_t site) {
            return static_cast<uint32_t>(site);
        });
        uint32_t numberOfSiteObjects = numberOfSites;
        for (uint32_t site = 0; site < numberOfSites; ++site) {
            const ValueFlowSite& record = universe.m_sites[site];
            if (record.origin != ValueFlowSiteOrigin::TypedResult || !record.layout)
                continue;
            uint32_t& first = firstTypedResultOfLayout[record.layout.index()];
            if (first == UINT32_MAX)
                first = site;
            else {
                siteWithObjectOf[site] = first;
                --numberOfSiteObjects;
            }
        }
        universe.m_firstKey = universe.m_firstSite + numberOfSiteObjects;
        universe.m_numberOfObjects = universe.m_firstKey + m_program.types.numberOfPropertyKeys();

        auto isBuiltinResult = [](const ValueFlowSite& site) {
            return site.origin == ValueFlowSiteOrigin::BuiltinResult;
        };
        universe.m_firstBuiltinResultSite = universe.m_firstKey - static_cast<uint32_t>(std::ranges::count_if(universe.m_sites, isBuiltinResult));
        universe.m_objectOfSite.grow(numberOfSites);
        universe.m_siteOfObject.grow(numberOfSiteObjects);
        uint32_t nextSite = universe.m_firstSite;
        uint32_t nextBuiltinResultSite = universe.m_firstBuiltinResultSite;
        for (uint32_t site = 0; site < numberOfSites; ++site) {
            if (siteWithObjectOf[site] != site) {
                universe.m_objectOfSite[site] = universe.m_objectOfSite[siteWithObjectOf[site]];
                continue;
            }
            uint32_t object = isBuiltinResult(universe.m_sites[site]) ? nextBuiltinResultSite++ : nextSite++;
            universe.m_objectOfSite[site] = ValueFlowObjectID(object);
            universe.m_siteOfObject[object - universe.m_firstSite] = site;
        }
        universe.m_functionOfSiteObject.grow(numberOfSiteObjects);
        forEachIndexInParallel(m_loops, (m_numberOfFunctions + bodiesInChunk - 1) / bodiesInChunk, [&](unsigned chunk) {
            unsigned end = std::min(m_numberOfFunctions, (chunk + 1) * bodiesInChunk);
            for (unsigned index = chunk * bodiesInChunk; index < end; ++index) {
                for (uint32_t site = universe.m_firstSiteOfFunction[index]; site < universe.m_firstSiteOfFunction[index + 1]; ++site) {
                    if (siteWithObjectOf[site] == site)
                        universe.m_functionOfSiteObject[universe.m_objectOfSite[site].index() - universe.m_firstSite] = FunctionID(index);
                }
            }
        });

        universe.m_layoutOfNamespace.grow(m_program.modules.size());
        for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
            if (ModuleID module = m_program.layouts[index].namespaceOf; module && m_program.layoutOfWholeNamespace(module))
                universe.m_layoutOfNamespace[module.index()] = LayoutID(index);
        }
        universe.m_classOfCoroutineResult.grow(m_numberOfFunctions);
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            const Function& function = m_program.functions[index];
            bool isAsync = function.has(FunctionFlags::IsAsync);
            if (function.has(FunctionFlags::IsGenerator))
                universe.m_classOfCoroutineResult[index] = isAsync ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
            else if (isAsync)
                universe.m_classOfCoroutineResult[index] = BuiltinClasses::Promise;
        }
    }

    void numberNodes()
    {
        ValueFlowProgramNodes& nodes = m_result.m_nodes;
        uint32_t next = nodes.m_firstVariable + m_program.variables.size();
        nodes.m_firstFieldOfLayout.reserveInitialCapacity(m_program.layouts.size());
        for (const Layout& layout : m_program.layouts) {
            nodes.m_firstFieldOfLayout.append(next);
            next += layout.fields.size();
        }
        nodes.m_firstNodeOfFunction.grow(m_bodies.size() + 1);
        nodes.m_firstResultOfFunction.grow(m_bodies.size());
        nodes.m_thrownOfFunction.grow(m_bodies.size());
        next += sumOverBodies<uint32_t>([&](unsigned index, uint32_t& numberOfNodes) {
            numberOfNodes += m_bodies[index].nodes.size();
        }, [&](unsigned index, const uint32_t& numberOfNodes) {
            const Body& body = m_bodies[index];
            nodes.m_firstNodeOfFunction[index] = next + numberOfNodes;
            nodes.m_firstResultOfFunction[index] = next + numberOfNodes + body.result(0);
            nodes.m_thrownOfFunction[index] = next + numberOfNodes + body.thrown();
        });
        nodes.m_firstNodeOfFunction[m_bodies.size()] = next;
        nodes.m_numberOfNodes = next;
        nodes.m_resultSummaries.grow(m_bodies.size());

        for (const UnobservedFieldDefinition& definition : m_result.m_unobservedDefinitions.all()) {
            uint32_t value = m_bodies[definition.initializer.index()].valueOfUnobservedDefinitions[definition.isObservedWhereObjectRefusesDefinition];
            if (value == Body::none || nodes.valueOf(definition))
                continue;
            ValueFlowNodeID node(nodes.m_firstNodeOfFunction[definition.initializer.index()] + value);
            nodes.m_unobservedDefinitionsByInitializer.append({ definition.classLayout, definition.initializer, node, definition.isObservedWhereObjectRefusesDefinition });
            LayoutID current = definition.classLayout;
            for (size_t steps = 0; current && m_program.at(current).has(LayoutFlags::HasHeritage) && steps < m_program.layouts.size(); ++steps) {
                nodes.m_unobservedDefinitionsByClass.append({ current, definition.initializer, node, definition.isObservedWhereObjectRefusesDefinition && current == definition.classLayout });
                current = predictedHeritageOf(m_program.at(current));
            }
        }
        AOT::stableSort(nodes.m_unobservedDefinitionsByClass, { }, &ValueFlowProgramNodes::UnobservedDefinitions::classCallingSuper);
    }

    void fileAllocationsOfBaseClassesUnderClassesBelow(ValueSetMaker& maker, Vector<Vector<ValueFlowObjectID>>& ofLayout)
    {
        const ValueFlowUniverse& universe = m_result.m_universe;
        Vector<Vector<ValueFlowObjectID>> allocatedByClass(m_program.layouts.size());
        Vector<ValueFlowObjectID> allocatedByAnyClass;
        Vector<uint32_t> sitesOfClasses = collectInParallel<uint32_t>(m_loops, m_numberOfFunctions, [&](unsigned index, Vector<uint32_t>& found) {
            const Function& function = m_program.functions[index];
            for (uint32_t site = universe.m_firstSiteOfFunction[index]; site < universe.m_firstSiteOfFunction[index + 1]; ++site) {
                const ValueFlowSite& record = universe.m_sites[site];
                if (record.layout && record.origin == ValueFlowSiteOrigin::Operation && mayAllocateOtherLayout(m_program, function.at(record.value)))
                    found.append(site);
            }
        });
        for (uint32_t site : sitesOfClasses) {
            allocatedByClass[declaredLayoutOf(m_program, universe.m_sites[site].layout).index()].append(universe.m_objectOfSite[site]);
            allocatedByAnyClass.append(universe.m_objectOfSite[site]);
        }
        Vector<ValueFlowObjectID> objectsThatPickTheirLayout = allocatedByAnyClass;
        Vector<ValueFlowObjectID> instancesOfClasses;
        Vector<ValueFlowObjectID> constructorsWithLayouts;
        BitVector classesNamedAboveOneClass(m_program.layouts.size());
        for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
            const Layout& layout = m_program.layouts[index];
            if (!layoutIsDefinedByClass(layout) || declaredLayoutOf(m_program, LayoutID(index)) != LayoutID(index))
                continue;
            if (LayoutID above = predictedHeritageOf(layout); above && classesNamedAboveOneClass.set(above.index()))
                m_result.m_universe.m_classesNamedAboveSeveralClasses.set(above.index());
        }
        for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
            const Layout& layout = m_program.layouts[index];
            if (layout.kind == LayoutKind::Instance && layout.constructor) {
                objectsThatPickTheirLayout.append(universe.instanceOf(LayoutID(index)));
                if (m_program.at(layout.constructor).constructedClass == LayoutID(index))
                    constructorsWithLayouts.append(universe.closureOf(layout.constructor));
            }
            if (layoutIsDefinedByClass(layout))
                instancesOfClasses.append(universe.instanceOf(LayoutID(index)));
        }
        m_result.m_universe.m_objectsThatPickTheirLayout = maker.of(objectsThatPickTheirLayout.span());
        m_result.m_universe.m_instancesOfClasses = maker.of(instancesOfClasses.span());
        m_result.m_universe.m_constructorsWithLayouts = maker.of(constructorsWithLayouts.span());
        if (allocatedByAnyClass.isEmpty())
            return;

        for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
            if (!layoutIsDefinedByClass(m_program.layouts[index]))
                continue;
            LayoutID current(index);
            for (size_t steps = 0; steps < m_program.layouts.size() && m_program.at(current).has(LayoutFlags::HasHeritage); ++steps) {
                current = predictedHeritageOf(m_program.at(current));
                if (!current) {
                    ofLayout[index].appendVector(allocatedByAnyClass);
                    break;
                }
                ofLayout[index].appendVector(allocatedByClass[current.index()]);
            }
        }
    }

    void appendHoldersOfLibraryExports(Vector<ValueFlowObjectID>& holders)
    {
        const ValueFlowUniverse& universe = m_result.m_universe;
        for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
            const Module& module = m_program.modules[index];
            if (module.kind != ModuleKind::RuntimeSource)
                continue;
            holders.append(universe.namespaceOf(ModuleID(index)));
            if (!module.body)
                continue;

            const Function& body = m_program.at(module.body);
            BitVector isVisited(body.values.size());
            Vector<ValueID, 8> worklist;
            auto visit = [&](ValueID value) {
                if (value && !isVisited.set(value.index()))
                    worklist.append(value);
            };
            for (const Block& block : body.blocks) {
                if (block.terminator.kind == TerminatorKind::Return)
                    visit(block.terminator.valueOperand());
            }
            while (!worklist.isEmpty()) {
                ValueID id = worklist.takeLast();
                const Value& value = body.at(id);
                if (ValueFlowObjectID object = universe.siteObject(module.body, id))
                    holders.append(object);
                if (value.opcode == Opcode::GetModuleVariable) {
                    for (const Value& store : body.values) {
                        if (store.opcode == Opcode::PutModuleVariable && store.variable() == value.variable())
                            visit(store.valueOperand());
                    }
                    continue;
                }
                for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
                    if (operandUsesOf(value.opcode, operandIndex).contains(OperandUse::Returns))
                        visit(value.operand(operandIndex));
                }
            }
        }
    }

    void makeMasksOfUniverse()
    {
        ValueFlowUniverse& universe = m_result.m_universe;
        const BuiltinTable& builtins = m_program.builtins();
        m_makers.grow(std::max(1u, m_loops.width()) + 1);
        m_setsOfType.grow(m_makers.size());
        ValueSetMaker& maker = makerOf(0);

        Vector<ValueFlowObjectID> list;
        auto take = [&] {
            ValueSetID set = maker.of(list.span());
            list.shrink(0);
            return set;
        };
        auto rangeOf = [&](uint32_t first, uint32_t end) {
            return maker.ofRange(ValueFlowObjectID(first), ValueFlowObjectID(end));
        };
        enum class OfModules : bool { No, Yes };
        auto builtinObjectsWhere = [&](OfModules ofModules, NOESCAPE const Invocable<bool(BuiltinID)> auto& holds) {
            for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
                if (holds(BuiltinID(index)))
                    list.append(ValueFlowObjectID(universe.m_firstBuiltin + index));
            }
            for (uint32_t index = 0; ofModules == OfModules::Yes && index < universe.m_moduleBuiltins.size(); ++index) {
                if (holds(universe.m_moduleBuiltins[index].builtin))
                    list.append(ValueFlowObjectID(universe.m_firstModuleBuiltin + index));
            }
            return take();
        };

        for (uint16_t rest = Primitive::All & ~(Primitive::LiteralString | Primitive::LiteralSymbol); rest; rest &= rest - 1)
            list.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
        universe.m_anyPrimitive = take();
        universe.m_keys = rangeOf(universe.m_firstKey, universe.m_numberOfObjects);
        universe.m_primitives = maker.unite(rangeOf(0, ValueFlowUniverse::numberOfPrimitiveKinds), universe.m_keys);
        universe.m_undefinedValue = maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Undefined));
        universe.m_unknownObjectAlone = maker.of(ValueFlowUniverse::unknownObject());
        universe.m_untrackedObjectAlone = maker.of(ValueFlowUniverse::untrackedObject());
        universe.m_placeholdersAlone = maker.unite(universe.m_unknownObjectAlone, universe.m_untrackedObjectAlone);
        universe.m_objects = ValueSets::complement(universe.m_primitives);
        universe.m_knownObjects = maker.intersect(universe.m_objects, ValueSets::complement(maker.unite(universe.m_placeholdersAlone, maker.unite(maker.of(ValueFlowUniverse::markOfNamedBun()), maker.unite(maker.of(ValueFlowUniverse::markOfMemberLeftOut()), maker.of(ValueFlowUniverse::markOfLoadedBun()))))));
        universe.m_constructedObjects = rangeOf(universe.m_firstConstructedObject, universe.m_firstSite);
        universe.m_builtinObjects = rangeOf(universe.m_firstBuiltin, universe.m_firstNamespace);
        universe.m_namespaces = rangeOf(universe.m_firstNamespace, universe.m_firstLayoutObject);
        universe.m_closures = rangeOf(universe.m_firstClosure, universe.m_firstCoroutineResult);

        Vector<ValueFlowObjectID> symbolKeys;
        for (uint32_t index = 0; index < m_program.types.numberOfPropertyKeys(); ++index) {
            PropertyKeyKind kind = m_program.at(PropertyKeyID(index)).kind;
            if (kind == PropertyKeyKind::Text)
                continue;
            list.append(universe.keyConstant(PropertyKeyID(index)));
            if (isSymbolKind(kind))
                symbolKeys.append(universe.keyConstant(PropertyKeyID(index)));
        }
        universe.m_textKeys = maker.intersect(universe.m_keys, ValueSets::complement(take()));
        universe.m_symbolKeys = maker.of(symbolKeys.span());
        forEachEmbeddedFilePath(m_program, [&](ModuleID, StringID path) {
            if (std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = path }))
                list.append(universe.keyConstant(*key));
        });
        universe.m_embeddedFilePaths = take();

        universe.m_builtinFunctions = builtinObjectsWhere(OfModules::Yes, [&](BuiltinID builtin) {
            return isCallableBuiltin(builtins, builtin);
        });

        BitVector isFunctionOfProtocol(builtins.builtinCount());
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinName name = builtins.at(BuiltinID(index)).name;
            bool isNameOfProtocol = name == BuiltinNames::iteratorSymbol || name == BuiltinNames::asyncIteratorSymbol || name == BuiltinNames::next || name == BuiltinNames::returnKeyword
                || name == BuiltinNames::throwKeyword || name == BuiltinNames::then;
            if (isNameOfProtocol && isCallableBuiltin(builtins, BuiltinID(index)))
                isFunctionOfProtocol.quickSet(builtins.canonicalID(BuiltinID(index)).index());
        }
        universe.m_builtinFunctionsOfProtocols = builtinObjectsWhere(OfModules::Yes, [&](BuiltinID builtin) {
            return isFunctionOfProtocol.quickGet(builtins.canonicalID(builtin).index());
        });
        auto readsSpecies = [](const BuiltinParameter& parameter) {
            return parameter.uses != OperandUses::all() && parameter.protocols.contains(Protocol::Species);
        };
        universe.m_builtinFunctionsThatReadSpecies = builtinObjectsWhere(OfModules::Yes, [&](BuiltinID builtin) {
            return isCallableBuiltin(builtins, builtin) && std::ranges::any_of(builtins.overloadsOf(builtins.canonicalID(builtin)), [&](const BuiltinOverload& overload) {
                if (overload.code.model || !doRecordsDescribe(builtins, overload))
                    return false;
                return (overload.receiver && readsSpecies(*overload.receiver)) || std::ranges::any_of(overload.parameters, readsSpecies);
            });
        });
        constexpr OperandUses usesChangingObject = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype };
        universe.m_builtinFunctionsThatActOnReceiver = builtinObjectsWhere(OfModules::Yes, [&](BuiltinID builtin) {
            return isCallableBuiltin(builtins, builtin) && std::ranges::any_of(builtins.overloadsOf(builtins.canonicalID(builtin)), [&](const BuiltinOverload& overload) {
                if (overload.mode != BuiltinCallMode::Call || !overload.receiver || overload.code.model || !doRecordsDescribe(builtins, overload))
                    return false;
                return readsSpecies(*overload.receiver) || overload.receiver->uses.containsAny(usesChangingObject);
            });
        });

        universe.m_isCanonicalEntryOfCallableBuiltin = BitVector(builtins.builtinCount());
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            BuiltinID builtin(index);
            if (!isCallableBuiltin(builtins, builtin))
                continue;
            universe.m_isCanonicalEntryOfCallableBuiltin.quickSet(builtins.canonicalID(builtin).index());
            LibraryClosuresOfBuiltin closures = libraryClosuresOf(m_program.types, builtin);
            if (closures.mayBeAnyFunctionOfLibrary) {
                list.append(universe.objectOf(builtin));
                continue;
            }
            for (FunctionID function : closures.functions)
                universe.m_builtinsThatMayBeClosureOf.add(function.index(), Vector<ValueFlowObjectID, 2>()).iterator->value.append(universe.objectOf(builtin));
        }
        universe.m_builtinsThatMayBeAnyLibraryClosure = take();

        universe.m_objectsOfRealm = builtinObjectsWhere(OfModules::No, [&](BuiltinID builtin) {
            return builtins.at(builtin).scope == BuiltinScope::Realm;
        });
        universe.m_lockedObjects = builtinObjectsWhere(OfModules::No, [&](BuiltinID builtin) {
            return builtins.at(builtin).flags.contains(BuiltinFlag::IsLockedObject);
        });
        universe.m_objectsWithLockedPrototypeLink = builtinObjectsWhere(OfModules::No, [&](BuiltinID builtin) {
            return builtins.hasLockedPrototypeLink(builtin);
        });
        for (uint32_t index = 0; index < m_program.modules.size(); ++index)
            list.append(universe.namespaceOf(ModuleID(index)));
        universe.m_objectsBornWithoutPrototype = builtinObjectsWhere(OfModules::Yes, [&](BuiltinID builtin) {
            return builtins.at(builtin).prototypeLink != BuiltinPrototypeLink::Builtin && builtins.canonicalID(builtin) != Builtins::ObjectPrototype;
        });

        for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
            const Layout& layout = m_program.layouts[index];
            if (layoutIsDefinedByClass(layout) && layout.module && m_program.at(layout.module).kind == ModuleKind::RuntimeSource)
                list.append(universe.prototypeOf(LayoutID(index)));
        }
        universe.m_prototypeObjectsOfLibraryClasses = take();
        appendHoldersOfLibraryExports(list);
        universe.m_holdersOfLibraryExports = take();
        universe.m_objectsOfLibraryFoundByNameAlone = maker.unite(universe.m_holdersOfLibraryExports, universe.m_prototypeObjectsOfLibraryClasses);

        list.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
        universe.m_strings = maker.unite(take(), universe.m_textKeys);
        list.append(ValueFlowUniverse::primitive(PrimitiveSet::Symbol));
        universe.m_symbols = maker.unite(take(), universe.m_symbolKeys);

        Vector<Vector<ValueFlowObjectID>> ofLayout(m_program.layouts.size());
        Vector<Vector<ValueFlowObjectID>> ofClass(builtins.classCount());
        for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
            ofLayout[index].append(universe.instanceOf(LayoutID(index)));
            if (ModuleID module = m_program.layouts[index].namespaceOf; module && m_program.layoutOfWholeNamespace(module))
                ofLayout[index].append(universe.namespaceOf(module));
        }
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            if (BuiltinClassID builtinClass = universe.m_classOfCoroutineResult[index])
                ofClass[builtinClass.index()].append(universe.coroutineResultOf(FunctionID(index)));
        }
        for (uint32_t site = 0; site < universe.m_sites.size(); ++site) {
            ValueFlowObjectID object = universe.m_objectOfSite[site];
            if (LayoutID layout = universe.m_sites[site].layout)
                ofLayout[layout.index()].append(object);
            if (universe.m_sites[site].origin == ValueFlowSiteOrigin::BuiltinResult)
                continue;
            if (BuiltinClassID builtinClass = universe.m_sites[site].builtinClass)
                ofClass[builtinClass.index()].append(object);
            else if (!universe.m_sites[site].layout)
                list.append(object);
        }
        fileAllocationsOfBaseClassesUnderClassesBelow(maker, ofLayout);
        universe.m_objectsWithAnyLayout = maker.unite(take(), universe.m_placeholdersAlone);
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            if (m_bodies[index].answersOnlyWhatItConstructs)
                list.append(universe.closureOf(FunctionID(index)));
        }
        universe.m_constructorsAnsweringOnlyWhatTheyConstruct = take();
        ValueSetID madeByBuiltins = maker.unite(rangeOf(universe.m_firstResultOfBuiltin, universe.m_firstClosure), rangeOf(universe.m_firstBuiltinResultSite, universe.m_firstKey));
        universe.m_objectsThatMayBeAnything = maker.unite(universe.m_objectsWithAnyLayout, madeByBuiltins);
        BitVector isLayoutOfTable(m_program.layouts.size());
        for (uint32_t index = 0; index < builtins.layoutCount(); ++index) {
            if (LayoutID layout = m_program.layoutOf(BuiltinLayoutID(index)))
                isLayoutOfTable.quickSet(layout.index());
        }

        auto mayBeAllocatedByBuiltin = [&](LayoutID layout) {
            for (size_t steps = 0; steps < m_program.layouts.size() && layout; ++steps) {
                if (!m_program.at(layout).has(LayoutFlags::HasHeritage))
                    return false;
                layout = predictedHeritageOf(m_program.at(layout));
            }
            return true;
        };
        Vector<ValueSetID> allocatedWithLayout(ofLayout.size());
        forEachIndexInParallel(m_loops, ofLayout.size(), [&](unsigned index, unsigned worker) {
            allocatedWithLayout[index] = makerOf(worker).of(ofLayout[index].span());
        });
        Vector<ValueSetID> inheritingLayouts;
        universe.m_objectsOfLayout = Vector<ValueSetID>(ofLayout.size(), [&](size_t index) {
            ValueSetID objects = allocatedWithLayout[index];
            if (mayBeAllocatedByBuiltin(LayoutID(static_cast<uint32_t>(index))))
                inheritingLayouts.append(objects);
            return isLayoutOfTable.quickGet(index) ? maker.unite(objects, madeByBuiltins) : objects;
        });
        Vector<ValueSetID> ofLayoutsOfProgram;
        universe.m_isAllocatedByProgram.ensureSize(ofLayout.size());
        for (unsigned index = 0; index < ofLayout.size(); ++index) {
            if (isLayoutOfTable.quickGet(index))
                continue;
            ofLayoutsOfProgram.append(universe.m_objectsOfLayout[index]);
            if (ofLayout[index].size() > 1 || m_program.layouts[index].constructor)
                universe.m_isAllocatedByProgram.quickSet(index);
        }
        universe.m_objectsWithLayoutOfProgram = maker.uniteAll(ofLayoutsOfProgram.span());
        universe.m_isLayoutOfTable = WTF::move(isLayoutOfTable);
        ValueSetID inEveryClassButNoBuiltinObject = maker.unite(maker.uniteAll(inheritingLayouts.span()), universe.m_objectsThatMayBeAnything);
        ValueSetID inEveryClass = maker.unite(inEveryClassButNoBuiltinObject, universe.m_builtinObjects);

        auto hasSameCellTypeTest = [&](BuiltinClassID first, BuiltinClassID second) {
            const BuiltinClass& a = builtins.at(first);
            const BuiltinClass& b = builtins.at(second);
            return a.instanceTest == BuiltinInstanceTest::JSTypeEquals && b.instanceTest == BuiltinInstanceTest::JSTypeEquals && a.type == b.type;
        };
        auto mayBeInstanceOf = [&](BuiltinClassID allocatedAs, BuiltinClassID tested) {
            if (builtins.isSameOrSubclassOf(tested, allocatedAs))
                return true;
            for (BuiltinClassID current = allocatedAs; current; current = builtins.at(current).parent) {
                if (current == tested || hasSameCellTypeTest(current, tested))
                    return true;
            }
            return false;
        };
        Vector<ValueSetID> allocatedAs = ofClass.map([&](const Vector<ValueFlowObjectID>& members) {
            return maker.of(members.span());
        });
        ValueSetID& allocatedAsArray = allocatedAs[BuiltinClasses::Array.index()];
        allocatedAsArray = maker.unite(allocatedAsArray, rangeOf(universe.m_firstRestArray, universe.m_firstFunctionPrototype));
        ValueSetID& allocatedAsObject = allocatedAs[BuiltinClasses::Object.index()];
        allocatedAsObject = maker.unite(allocatedAsObject, rangeOf(universe.m_firstFunctionPrototype, universe.m_firstSite));
        Vector<ValueSetID> parts;
        Vector<ValueSetID> callableInstances;
        universe.m_objectsOfClass = Vector<ValueSetID>(builtins.classCount(), [&](size_t index) {
            BuiltinClassID tested(static_cast<uint32_t>(index));
            bool hasCallableInstances = Type::builtinClass(tested).overlaps(Type::function(), m_program);
            if (hasCallableInstances)
                callableInstances.append(allocatedAs[index]);
            if (builtins.at(tested).instanceTest == BuiltinInstanceTest::None)
                return universe.m_objects;

            parts.shrink(0);
            parts.append(tested == BuiltinClasses::Promise ? inEveryClassButNoBuiltinObject : inEveryClass);
            if (hasCallableInstances)
                parts.append(universe.m_closures);
            for (uint32_t other = 0; other < builtins.classCount(); ++other) {
                if (allocatedAs[other] != ValueSets::nothing && mayBeInstanceOf(BuiltinClassID(other), tested))
                    parts.append(allocatedAs[other]);
            }
            return maker.uniteAll(parts.span());
        });
        universe.m_sureInstancesOfClass = Vector<ValueSetID>(builtins.classCount(), [&](size_t index) {
            BuiltinClassID tested(static_cast<uint32_t>(index));
            if (builtins.at(tested).instanceTest == BuiltinInstanceTest::None)
                return ValueSets::nothing;
            parts.shrink(0);
            for (uint32_t other = 0; other < builtins.classCount(); ++other) {
                if (allocatedAs[other] != ValueSets::nothing && builtins.isSameOrSubclassOf(BuiltinClassID(other), tested))
                    parts.append(allocatedAs[other]);
            }
            return maker.uniteAll(parts.span());
        });
        universe.m_arrays = universe.m_objectsOfClass[BuiltinClasses::Array.index()];

        ValueSetID knownObjectsThatMayBeAnything = maker.intersect(universe.m_objectsThatMayBeAnything, universe.m_knownObjects);
        universe.m_otherCallables = maker.unite(maker.uniteAll(callableInstances.span()), knownObjectsThatMayBeAnything);
        universe.m_callables = maker.uniteAll({ { universe.m_builtinFunctions, universe.m_closures, universe.m_otherCallables, universe.m_placeholdersAlone } });

        universe.m_numberOfRememberedLayoutSets = m_program.types.numberOfLayoutSets();
        universe.m_objectsSurelyWithLayoutIn = std::make_unique<std::atomic<uint32_t>[]>(universe.m_numberOfRememberedLayoutSets);
        universe.m_objectsWithLayoutIn = std::make_unique<std::atomic<uint32_t>[]>(universe.m_numberOfRememberedLayoutSets);
        universe.m_closuresCalledDirectlyBy = std::make_unique<std::atomic<uint32_t>[]>(m_program.functions.size());
        for (uint32_t index = m_program.modules.size(); index--;) {
            RuntimeModuleID runtimeModule = m_program.modules[index].runtimeModule;
            if (!runtimeModule)
                continue;
            if (universe.m_moduleOfRuntimeModule.size() <= runtimeModule.index())
                universe.m_moduleOfRuntimeModule.grow(runtimeModule.index() + 1);
            universe.m_moduleOfRuntimeModule[runtimeModule.index()] = ModuleID(index);
        }
        static_assert(!ValueSets::nothing.index());
        universe.m_boundFunctionsBindingNoArgument = std::make_unique<std::atomic<uint64_t>[]>(1);
        universe.m_numberOfRememberedTypes = m_program.types.size();
        universe.m_membersSurelyInsideType = std::make_unique<std::atomic<uint32_t>[]>(universe.m_numberOfRememberedTypes);
    }

    void layOutLists()
    {
        struct Places {
            Places& operator+=(const Places& other)
            {
                range.statement += other.range.statement;
                range.edgeOnceReached += other.range.edgeOnceReached;
                range.seedOnceReached += other.range.seedOnceReached;
                range.functionEntered += other.range.functionEntered;
                first.operand += other.first.operand;
                first.edge += other.first.edge;
                first.mask += other.first.mask;
                first.observedValue += other.first.observedValue;
                first.comparedValues += other.first.comparedValues;
                first.valueAliveForComparisonsOnly += other.first.valueAliveForComparisonsOnly;
                return *this;
            }

            ValueFlowBodyRange range;
            FirstInLists first { };
        };
        Vector<uint32_t> numberOfNodesWithMask(m_bodies.size());
        forEachIndexInParallel(m_loops, m_bodies.size(), [&](unsigned index) {
            numberOfNodesWithMask[index] = static_cast<uint32_t>(std::ranges::count_if(m_bodies[index].nodes, [](const Body::Node& node) {
                return !!node.mask;
            }));
        });
        m_result.m_ranges.grow(m_bodies.size() + 1);
        m_firstInLists.grow(m_bodies.size());
        Places end = sumOverBodies<Places>([&](unsigned index, Places& places) {
            const Body& body = m_bodies[index];
            places.range.statement += body.statements.size();
            places.range.edgeOnceReached += body.edgesOnceReached.size();
            places.range.seedOnceReached += body.seedsOnceReached.size();
            places.range.functionEntered += body.functionsEnteredOnceReached.size();
            places.first.operand += body.operands.size();
            places.first.edge += body.edges.size();
            places.first.observedValue += body.observedValues.size();
            places.first.comparedValues += body.comparedValues.size();
            places.first.valueAliveForComparisonsOnly += body.valuesAliveForComparisonsOnly.size();
            places.first.mask += numberOfNodesWithMask[index];
        }, [&](unsigned index, const Places& places) {
            ValueFlowBodyRange range = places.range;
            range.statementLeavingBody = places.range.statement + m_bodies[index].firstStatementLeavingBody;
            m_result.m_ranges[index] = range;
            m_firstInLists[index] = places.first;
        });
        ValueFlowBodyRange next = end.range;
        next.statementLeavingBody = next.statement;
        m_result.m_ranges[m_bodies.size()] = next;

        static_assert(std::is_trivially_copyable_v<ValueFlowStatement> && std::is_trivially_copyable_v<ValueFlowOperand>);
        m_result.m_statements = MallocSpan<ValueFlowStatement>::malloc(next.statement * sizeof(ValueFlowStatement));
        m_result.m_operands = MallocSpan<ValueFlowOperand>::malloc(end.first.operand * sizeof(ValueFlowOperand));
        runTasksInParallel(m_loops, {
            [&] {
                m_result.m_edges.grow(end.first.edge);
            },
            [&] {
                m_result.m_observedValues.grow(end.first.observedValue);
            },
            [&] {
                m_result.m_masks.grow(end.first.mask);
                m_result.m_edgesOnceReached.grow(next.edgeOnceReached);
                m_result.m_seedsOnceReached.grow(next.seedOnceReached);
                m_result.m_functionsEnteredOnceReached.grow(next.functionEntered);
                m_result.m_comparedValues.grow(end.first.comparedValues);
                m_result.m_valuesAliveForComparisonsOnly.grow(end.first.valueAliveForComparisonsOnly);
            },
        });
    }

    ValueFlowNodeID nodeNamed(unsigned body, const ValueFlowNodeName& name) const
    {
        using Kind = ValueFlowNodeName::Kind;
        const ValueFlowProgramNodes& nodes = m_result.m_nodes;
        switch (name.kind) {
        case Kind::None:
            return { };
        case Kind::OfBody:
            return ValueFlowNodeID(nodes.m_firstNodeOfFunction[body] + name.owner);
        case Kind::Escaped:
            return ValueFlowProgramNodes::escaped();
        case Kind::Variable:
            return nodes.variable(VariableID(name.owner));
        case Kind::Field:
            return nodes.field(LayoutID(name.owner), name.index);
        case Kind::EntryParameter:
            return nodes.entryParameter(FunctionID(name.owner), name.index);
        case Kind::Result:
            return nodes.result(FunctionID(name.owner), name.index);
        case Kind::Thrown:
            return nodes.thrown(FunctionID(name.owner));
        case Kind::OfHost:
            return nodes.ofHost(static_cast<ValueFlowHostNode>(name.owner));
        case Kind::Hub:
            return nodes.hub(ValueFlowBehaviorID(name.owner));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    LinkedMask linkMask(const Body::Mask& mask, ValueSetMaker& maker, HashMap<TypeID, SetsOfType>& setsOfType) const
    {
        if (mask.admitsEverything())
            return { };
        const ValueFlowUniverse& universe = m_result.m_universe;
        LinkedMask linked { ValueSets::anything, mask.kind, { } };
        for (TypeID type : mask.inside) {
            const SetsOfType& sets = setsOfType.ensure(type, [&] {
                const Type& record = m_program.at(type);
                return SetsOfType { universe.maskOf(maker, m_program, record), universe.namesProgramObjectsAlone(m_program, record) ? universe.familyObjectsOf(maker, m_program, record) : ValueSetID() };
            }).iterator->value;
            linked.set = maker.intersect(linked.set, sets.mask);
            if (sets.familyObjects)
                linked.familyObjects = linked.familyObjects ? maker.intersect(linked.familyObjects, sets.familyObjects) : sets.familyObjects;
        }
        for (TypeID type : mask.outside)
            linked.set = maker.intersect(linked.set, ValueSets::complement(universe.membersSurelyInside(maker, m_program, type)));
        for (auto [builtin, module] : mask.outsideBuiltinsOfModules) {
            if (ValueFlowObjectID object = universe.objectOf(builtin, module))
                linked.set = maker.intersect(linked.set, ValueSets::complement(maker.of(object)));
        }
        if (linked.familyObjects)
            linked.familyObjects = maker.intersect(linked.familyObjects, linked.set);
        return linked;
    }

    void linkBody(unsigned index, ValueSetMaker& maker, HashMap<TypeID, SetsOfType>& setsOfType)
    {
        const Body& body = m_bodies[index];
        const ValueFlowUniverse& universe = m_result.m_universe;
        const ValueFlowBodyRange& range = m_result.m_ranges[index];
        const FirstInLists& first = m_firstInLists[index];
        FunctionID function(index);

        Vector<ValueSetID, 32> constants = body.constants.map<Vector<ValueSetID, 32>>([&](const Body::Constant& constant) {
            Vector<ValueFlowObjectID, 8> members;
            for (uint16_t rest = constant.primitives; rest; rest &= rest - 1)
                members.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
            for (const ValueFlowObjectName& object : constant.objects)
                members.append(universe.objectNamed(function, object));
            return maker.of(members.span());
        });
        Vector<LinkedMask, 32> masks = body.masks.map<Vector<LinkedMask, 32>>([&](const Body::Mask& mask) {
            return linkMask(mask, maker, setsOfType);
        });
        ValueFlowPlaceholders placeholders = universe.placeholders();
        auto link = [&](const Body::Operand& operand) {
            const LinkedMask& mask = masks[operand.mask];
            return ValueFlowOperand { nodeNamed(index, operand.node), constants[operand.constant], mask.set, mask.kind };
        };
        auto linkEdge = [&](const Body::Edge& edge) {
            return ValueFlowEdge { nodeNamed(index, edge.from), nodeNamed(index, edge.to) };
        };

        if (body.ownResult != Body::none) {
            ValueFlowProgramNodes::ResultSummary& summary = m_result.m_nodes.m_resultSummaries[index];
            summary.ownResult = nodeNamed(index, ValueFlowNodeName::ofBody(body.ownResult));
            for (const Body::PassedParameter& passed : body.passedParameters)
                summary.passedParameters.append({ passed.entryParameter, masks[passed.mask].set, masks[passed.mask].kind });
        }

        uint32_t nextMask = first.mask;
        for (uint32_t node = 0; node < body.nodes.size(); ++node) {
            if (uint32_t mask = body.nodes[node].mask)
                m_result.m_masks[nextMask++] = { nodeNamed(index, ValueFlowNodeName::ofBody(node)), masks[mask].set, masks[mask].kind, masks[mask].familyObjects };
        }
        std::ranges::transform(body.edges, m_result.m_edges.mutableSpan().subspan(first.edge).begin(), linkEdge);
        std::ranges::transform(body.edgesOnceReached, m_result.m_edgesOnceReached.mutableSpan().subspan(range.edgeOnceReached).begin(), linkEdge);
        std::ranges::transform(body.seedsOnceReached, m_result.m_seedsOnceReached.mutableSpan().subspan(range.seedOnceReached).begin(), [&](const Body::Seed& seed) {
            const LinkedMask& mask = masks[seed.mask];
            ValueSetID constant = constants[seed.constant];
            if (mask.kind == ValueFlowMaskKind::Plain)
                return ValueFlowNodeSet { nodeNamed(index, seed.node), maker.intersect(constant, mask.set) };
            return ValueFlowNodeSet { nodeNamed(index, seed.node), maker.pass(constant, mask.set, mask.familyObjects, mask.kind, placeholders) };
        });
        std::ranges::copy(body.functionsEnteredOnceReached, m_result.m_functionsEnteredOnceReached.mutableSpan().subspan(range.functionEntered).begin());
        std::ranges::transform(body.operands, m_result.m_operands.mutableSpan().subspan(first.operand).begin(), link);
        std::ranges::transform(body.statements, m_result.m_statements.mutableSpan().subspan(range.statement).begin(), [&](const Body::Statement& statement) {
            return ValueFlowStatement {
                statement.kind,
                statement.opcode,
                body.function,
                statement.site,
                first.operand + statement.firstOperand,
                statement.operandCount,
                nodeNamed(index, statement.result),
                nodeNamed(index, statement.thrown),
                statement.firstSite == Body::none ? UINT32_MAX : universe.m_firstSiteOfFunction[index] + statement.firstSite,
                statement.isOnlyCalledOnItsHolder,
            };
        });
        std::ranges::transform(body.observedValues, m_result.m_observedValues.mutableSpan().subspan(first.observedValue).begin(), [&](const Body::ObservedValue& observed) {
            return ValueFlowObservedValue { body.function, observed.test, link(observed.operand) };
        });
        std::ranges::transform(body.valuesAliveForComparisonsOnly, m_result.m_valuesAliveForComparisonsOnly.mutableSpan().subspan(first.valueAliveForComparisonsOnly).begin(), [&](ValueID value) {
            return ValueFlowValueOfFunction { body.function, value };
        });
        std::ranges::transform(body.comparedValues, m_result.m_comparedValues.mutableSpan().subspan(first.comparedValues).begin(), [&](const Body::ComparedValues& compared) {
            return ValueFlowComparedValues { body.function, link(compared.left), link(compared.right) };
        });
    }

    const Program& m_program;
    BuildContext& m_build;
    const ParallelLoops& m_loops;
    unsigned m_numberOfFunctions;
    ParallelArray<ValueFlowBody> m_bodies;
    Vector<std::unique_ptr<ValueSetMaker>> m_makers;
    Vector<HashMap<TypeID, SetsOfType>> m_setsOfType;
    Vector<FirstInLists> m_firstInLists;
    ValueFlowBodies m_result;
};

ValueFlowBodies::ValueFlowBodies() = default;
ValueFlowBodies::ValueFlowBodies(ValueFlowBodies&&) = default;
ValueFlowBodies& ValueFlowBodies::operator=(ValueFlowBodies&&) = default;
ValueFlowBodies::~ValueFlowBodies() = default;

void ValueFlowBodies::addBytesTo(ValueFlowBytesByTable& bytes) const
{
    bytes.add("bodies: statements"_s, m_statements.sizeInBytes());
    bytes.add("bodies: operands"_s, m_operands.sizeInBytes());
    bytes.addList("bodies: edges"_s, m_edges);
    bytes.addList("bodies: edges once reached"_s, m_edgesOnceReached);
    bytes.addList("bodies: masks"_s, m_masks);
    auto addRest = [&](const auto&... lists) {
        (bytes.addList("bodies: the rest"_s, lists), ...);
    };
    addRest(m_seedsOnceReached, m_functionsEnteredOnceReached, m_observedValues, m_comparedValues, m_valuesAliveForComparisonsOnly, m_notedOperations, m_knowsValuesBehindChecks, m_ranges);
    addRest(m_nodes.m_firstFieldOfLayout, m_nodes.m_firstNodeOfFunction, m_nodes.m_firstResultOfFunction, m_nodes.m_thrownOfFunction, m_nodes.m_resultSummaries);
    bytes.addList("bodies: values behind checks"_s, m_valuesBehindChecks);
    for (const BitVector& values : m_valuesBehindChecks)
        bytes.add("bodies: values behind checks"_s, values.size() > 63 ? values.size() / 8 : 0);
    bytes.addList("universe: sites"_s, m_universe.m_sites);
    auto addObjects = [&](const auto&... lists) {
        (bytes.addList("universe: objects of sites, functions, layouts"_s, lists), ...);
    };
    addObjects(m_universe.m_firstSiteOfFunction, m_universe.m_objectOfSite, m_universe.m_siteOfObject, m_universe.m_functionOfSiteObject, m_universe.m_classOfCoroutineResult, m_universe.m_objectsOfLayout);
}

ValueFlowNotedOperations notedOperationsOf(const Function& function)
{
    static constexpr auto notedOperationsOfOpcode = [] {
        std::array<ValueFlowNotedOperations, numberOfEnumerators<Opcode>> table { };
        for (size_t index = 0; index < table.size(); ++index)
            table[index] = notedOperationsOf(static_cast<Opcode>(index));
        return table;
    }();
    ValueFlowNotedOperations operations = 0;
    for (const Value& value : function.values)
        operations |= notedOperationsOfOpcode[static_cast<size_t>(value.opcode)];
    return operations;
}

ValueFlowBodies reduceBodies(const Program& program, BuildContext& build, Vector<ValueFlowNotedOperations>&& notedOperations)
{
    return ValueFlowLinker(program, build, WTF::move(notedOperations)).link();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
