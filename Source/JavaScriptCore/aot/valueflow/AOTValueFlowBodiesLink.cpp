/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowBodies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include "AOTValueFlowBodyCopies.h"
#include "AOTValueSets.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/MonotonicTime.h>

namespace JSC { namespace AOT { namespace Typed {

ValueFlowObjectID ValueFlowUniverse::objectOf(BuiltinID builtin) const
{
    return ValueFlowObjectID(m_firstBuiltin + m_builtins->canonicalID(builtin).index());
}

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

ValueFlowObjectID ValueFlowUniverse::resultOf(BuiltinID builtin, uint32_t ordinal) const
{
    return ValueFlowObjectID(m_firstResultOfBuiltin + 2 * m_builtins->canonicalID(builtin).index() + ordinal);
}

// The sites of the body itself, which stand in front of those of the bodies copied into it.
std::span<const ValueFlowSite> ValueFlowUniverse::sitesOf(FunctionID function) const
{
    uint32_t first = m_firstSiteOfFunction[function.index()];
    std::span<const ValueFlowSite> all = m_sites.subspan(first, m_firstSiteOfFunction[function.index() + 1] - first);
    auto firstCopied = std::ranges::partition_point(all, [](const ValueFlowSite& site) {
        return !site.function;
    });
    return all.first(firstCopied - all.begin());
}

ValueFlowObjectID ValueFlowUniverse::siteObject(const ValueFlowStatement& statement, uint32_t ordinal) const
{
    if (statement.firstSite == UINT32_MAX)
        return { };
    // The ordinals of one operation rise. Where they do not, the sites of the next copy of the same body begin.
    FunctionID copiedFunction = m_sites[statement.firstSite].function;
    uint32_t end = copiedFunction ? m_sites.size() : m_firstSiteOfFunction[statement.function.index() + 1];
    for (uint32_t site = statement.firstSite; site < end && m_sites[site].value == statement.site && m_sites[site].function == copiedFunction; ++site) {
        if (site != statement.firstSite && m_sites[site].ordinal <= m_sites[site - 1].ordinal)
            break;
        if (m_sites[site].ordinal == ordinal)
            return m_objectOfSite[site];
    }
    return { };
}

ValueFlowObjectID ValueFlowUniverse::originalOf(ValueFlowObjectID object) const
{
    const ValueFlowSite* site = siteOf(object);
    if (!site || !site->function)
        return object;
    return siteObject(site->function, site->value, site->ordinal);
}

ValueFlowObjectID ValueFlowUniverse::siteObject(FunctionID function, ValueID value, uint32_t ordinal) const
{
    std::span<const ValueFlowSite> sites = sitesOf(function);
    auto keyOf = [](const ValueFlowSite& site) {
        return std::pair { site.value, site.ordinal };
    };
    auto found = std::ranges::lower_bound(sites, std::pair { value, ordinal }, { }, keyOf);
    if (found == sites.end() || keyOf(*found) != std::pair { value, ordinal })
        return { };
    return m_objectOfSite[m_firstSiteOfFunction[function.index()] + static_cast<uint32_t>(found - sites.begin())];
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
    if (index < m_firstSite)
        return ValueFlowObjectKind::RestArray;
    if (index < m_firstKey)
        return ValueFlowObjectKind::Site;
    return ValueFlowObjectKind::Key;
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
    case ValueFlowObjectKind::Site: {
        if (FunctionID function = m_sites[m_siteOfObject[index - m_firstSite]].function)
            return function;
        auto next = std::ranges::upper_bound(m_firstSiteOfFunction, m_siteOfObject[index - m_firstSite]);
        return FunctionID(static_cast<uint32_t>(next - m_firstSiteOfFunction.begin()) - 1);
    }
    default:
        return { };
    }
}

FunctionID ValueFlowUniverse::closureFunction(ValueFlowObjectID object) const
{
    return object.index() >= m_firstClosure && object.index() < m_firstCoroutineResult ? FunctionID(object.index() - m_firstClosure) : FunctionID();
}

BuiltinID ValueFlowUniverse::builtinOf(ValueFlowObjectID object) const
{
    switch (kindOf(object)) {
    case ValueFlowObjectKind::Builtin:
        return BuiltinID(object.index() - m_firstBuiltin);
    case ValueFlowObjectKind::ModuleBuiltin:
        return m_moduleBuiltins[object.index() - m_firstModuleBuiltin].builtin;
    default:
        return { };
    }
}

ModuleID ValueFlowUniverse::moduleOf(ValueFlowObjectID object) const
{
    switch (kindOf(object)) {
    case ValueFlowObjectKind::ModuleBuiltin:
        return m_moduleBuiltins[object.index() - m_firstModuleBuiltin].module;
    case ValueFlowObjectKind::Namespace:
        return ModuleID(object.index() - m_firstNamespace);
    default:
        return { };
    }
}

PropertyKeyID ValueFlowUniverse::keyOf(ValueFlowObjectID object) const
{
    return object.index() >= m_firstKey ? PropertyKeyID(object.index() - m_firstKey) : PropertyKeyID();
}

const ValueFlowSite* ValueFlowUniverse::siteOf(ValueFlowObjectID object) const
{
    if (object.index() < m_firstSite || object.index() >= m_firstKey)
        return nullptr;
    return &m_sites[m_siteOfObject[object.index() - m_firstSite]];
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
    default:
        return { };
    }
}

LayoutID ValueFlowUniverse::classLayoutOf(ValueFlowObjectID object) const
{
    return kindOf(object) == ValueFlowObjectKind::Prototype ? LayoutID((object.index() - m_firstLayoutObject) / 2) : LayoutID();
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
    case ValueFlowObjectKind::CoroutineResult:
        return m_classOfCoroutineResult[functionOf(object).index()];
    default:
        return { };
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
    // A value of the kind may be one of the literals.
    if (type.primitives & Primitive::AnyString)
        members.append(primitive(PrimitiveSet::String));
    if (type.primitives & Primitive::AnySymbol)
        members.append(primitive(PrimitiveSet::Symbol));
    if (type.primitives & Primitive::String)
        parts.append(m_textKeys);
    else {
        for (StringID literal : type.stringLiterals) {
            if (std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = literal }))
                members.append(keyConstant(*key));
        }
    }
    if (type.primitives & Primitive::Symbol)
        parts.append(m_symbolKeys);
    else {
        for (PropertyKeyID literal : type.symbolLiterals)
            members.append(keyConstant(literal));
    }

    if (type.anyObject)
        parts.append(m_objects);
    else if (type.hasObjects()) {
        parts.append(m_objectsWithAnyLayout);
        // A test for a function cannot tell a built-in object from one.
        if (type.anyFunction) {
            parts.append(m_callables);
            parts.append(m_builtinObjects);
        }
        for (LayoutID layout : membersOf(program, type))
            parts.append(m_objectsOfLayout[layout.index()]);
        for (const ClassPart& part : type.classes)
            parts.append(m_objectsOfClass[part.builtinClass.index()]);
        for (FunctionID function : type.functions) {
            members.append(closureOf(function));
            // The function object of a built-in may be a closure of a function of the library.
            if (!isLibraryCode(program, program.at(function)))
                continue;
            for (uint32_t index = 0; index < m_builtins->builtinCount(); ++index) {
                if (mayBeClosureOf(program.types, BuiltinID(index), function))
                    members.append(objectOf(BuiltinID(index)));
            }
        }
        for (BuiltinID builtin : type.builtins)
            appendObjectsOf(builtin, members);
    }

    parts.append(maker.of(members.span()));
    ValueSetID mask = maker.uniteAll(parts.span());
    return listsEveryObjectInside(program, type) ? maker.intersect(mask, ValueSets::complement(m_unknownObjectAlone)) : mask;
}

// Only the program allocates an object with one of its layouts, at a site that names the layout. So an object that nobody follows and that is inside a type of such layouts alone is one of the objects listed under them.
// Native code makes objects with the table's layouts. Where nothing allocates any of the layouts the type is taken to say nothing.
bool ValueFlowUniverse::listsEveryObjectInside(const Program& program, const Type& type) const
{
    if (!type.layouts || type.anyObject || type.anyFunction || !type.classes.isEmpty() || !type.functions.isEmpty() || !type.builtins.isEmpty())
        return false;
    bool isAllocated = false;
    for (LayoutID layout : membersOf(program, type)) {
        if (m_isLayoutOfTable.quickGet(layout.index()))
            return false;
        isAllocated |= m_isAllocatedByProgram.quickGet(layout.index());
    }
    return isAllocated;
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
    if (type.primitives & Primitive::String)
        parts.append(m_textKeys);
    else {
        for (StringID literal : type.stringLiterals) {
            if (std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = literal }))
                members.append(keyConstant(*key));
        }
    }
    if (type.primitives & Primitive::Symbol)
        parts.append(m_symbolKeys);
    else {
        for (PropertyKeyID literal : type.symbolLiterals)
            members.append(keyConstant(literal));
    }
    if (type.anyObject && type.anyFunction)
        parts.append(m_objects);
    else if (type.anyFunction)
        parts.append(m_closures);
    else {
        for (FunctionID function : type.functions)
            members.append(closureOf(function));
    }
    // A built-in object of the realm is the one object of its type. One that each module has of its own is many.
    for (BuiltinID builtin : type.builtins) {
        if (m_builtins->at(builtin).scope == BuiltinScope::Realm)
            members.append(objectOf(builtin));
    }

    parts.append(maker.of(members.span()));
    return maker.uniteAll(parts.span());
}

FunctionID ValueFlowProgramNodes::functionOf(ValueFlowNodeID node) const
{
    if (node.index() < m_firstNodeOfFunction[0])
        return { };
    auto next = std::ranges::upper_bound(m_firstNodeOfFunction, node.index());
    return FunctionID(static_cast<uint32_t>(next - m_firstNodeOfFunction.begin()) - 1);
}

class ValueFlowLinker {
    WTF_MAKE_NONCOPYABLE(ValueFlowLinker);
public:
    ValueFlowLinker(const Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
        , m_loops(build.parallelLoops())
        , m_numberOfFunctions(program.functions.size())
    {
    }

    ValueFlowBodies link()
    {
        MonotonicTime start = MonotonicTime::now();
        auto takeTime = [&](Seconds& time) {
            MonotonicTime now = MonotonicTime::now();
            time = now - start;
            start = now;
        };

        m_result.m_universe.m_builtinTables = makeUnique<ValueFlowBuiltinTables>(m_program.builtins());
        const ValueFlowBuiltinTables& tables = *m_result.m_universe.m_builtinTables;
        m_bodies = ParallelArray<ValueFlowBody>(m_loops, m_numberOfFunctions + 1, [&](unsigned index) {
            if (index == m_numberOfFunctions)
                return makeHostBody(m_program, tables);
            return reduceBody(m_program, tables, FunctionID(index));
        });
        copyBodiesIntoCallSites(m_program, m_build, m_bodies);
        takeTime(m_result.m_times.reduce);
        numberObjects();
        numberNodes();
        takeTime(m_result.m_times.number);
        m_result.m_sets = makeUnique<ValueSets>(m_result.m_universe.m_numberOfObjects);
        makeMasksOfUniverse();
        takeTime(m_result.m_times.makeMasks);
        layOutLists();
        forEachIndexInParallel(m_loops, m_bodies.size(), [&](unsigned index, unsigned worker) {
            linkBody(index, makerOf(worker), m_maskOfType[worker]);
        });
        takeTime(m_result.m_times.link);
        return WTF::move(m_result);
    }

private:
    using Body = ValueFlowBody;

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
        for (unsigned index = 0; index < m_bodies.size(); ++index) {
            for (const Body::Constant& constant : m_bodies[index].constants) {
                for (const ValueFlowObjectName& object : constant.objects) {
                    if (object.kind == ValueFlowObjectKind::ModuleBuiltin)
                        universe.m_moduleBuiltins.append({ BuiltinID(object.index), object.module });
                }
            }
        }
        std::ranges::sort(universe.m_moduleBuiltins);
        universe.m_moduleBuiltins.shrink(std::ranges::unique(universe.m_moduleBuiltins).begin() - universe.m_moduleBuiltins.begin());

        universe.m_firstBuiltin = ValueFlowUniverse::numberOfPrimitiveKinds + 1;
        universe.m_firstModuleBuiltin = universe.m_firstBuiltin + m_program.builtins().builtinCount();
        universe.m_firstNamespace = universe.m_firstModuleBuiltin + universe.m_moduleBuiltins.size();
        universe.m_firstLayoutObject = universe.m_firstNamespace + m_program.modules.size();

        universe.m_firstResultOfBuiltin = universe.m_firstLayoutObject + 2 * m_program.layouts.size();
        universe.m_firstClosure = universe.m_firstResultOfBuiltin + 2 * m_program.builtins().builtinCount();
        universe.m_firstCoroutineResult = universe.m_firstClosure + m_numberOfFunctions;
        universe.m_firstRestArray = universe.m_firstCoroutineResult + m_numberOfFunctions;
        universe.m_firstSite = universe.m_firstRestArray + m_numberOfFunctions;

        uint32_t numberOfSites = 0;
        universe.m_firstSiteOfFunction.reserveInitialCapacity(m_numberOfFunctions + 1);
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            universe.m_firstSiteOfFunction.append(numberOfSites);
            numberOfSites += m_bodies[index].sites.size();
        }
        universe.m_firstSiteOfFunction.append(numberOfSites);
        RELEASE_ASSERT(m_bodies[m_numberOfFunctions].sites.isEmpty());
        universe.m_firstKey = universe.m_firstSite + numberOfSites;
        universe.m_numberOfObjects = universe.m_firstKey + m_program.types.numberOfPropertyKeys();

        universe.m_sites.reserveInitialCapacity(numberOfSites);
        for (unsigned index = 0; index < m_numberOfFunctions; ++index)
            universe.m_sites.appendVector(m_bodies[index].sites);

        auto isBuiltinResult = [](const ValueFlowSite& site) {
            return site.origin == ValueFlowSiteOrigin::BuiltinResult;
        };
        universe.m_firstBuiltinResultSite = universe.m_firstKey - static_cast<uint32_t>(std::ranges::count_if(universe.m_sites, isBuiltinResult));
        universe.m_objectOfSite.grow(numberOfSites);
        universe.m_siteOfObject.grow(numberOfSites);
        uint32_t nextSite = universe.m_firstSite;
        uint32_t nextBuiltinResultSite = universe.m_firstBuiltinResultSite;
        for (uint32_t site = 0; site < numberOfSites; ++site) {
            uint32_t object = isBuiltinResult(universe.m_sites[site]) ? nextBuiltinResultSite++ : nextSite++;
            universe.m_objectOfSite[site] = ValueFlowObjectID(object);
            universe.m_siteOfObject[object - universe.m_firstSite] = site;
        }

        universe.m_layoutOfNamespace.grow(m_program.modules.size());
        for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
            if (ModuleID module = m_program.layouts[index].namespaceOf)
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
        for (unsigned index = 0; index < m_bodies.size(); ++index) {
            const Body& body = m_bodies[index];
            nodes.m_firstNodeOfFunction.append(next);
            nodes.m_firstResultOfFunction.append(next + body.result(0));
            nodes.m_thrownOfFunction.append(next + body.thrown());
            next += body.nodes.size();
        }
        nodes.m_firstNodeOfFunction.append(next);
        nodes.m_numberOfNodes = next;
    }

    void makeMasksOfUniverse()
    {
        ValueFlowUniverse& universe = m_result.m_universe;
        const BuiltinTable& builtins = m_program.builtins();
        m_makers.grow(std::max(1u, m_loops.width()) + 1);
        m_maskOfType.grow(m_makers.size());
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

        // A literal is a key or a string of which only the kind is known, so the two bits for literals stand for nothing.
        for (uint16_t rest = Primitive::All & ~(Primitive::LiteralString | Primitive::LiteralSymbol); rest; rest &= rest - 1)
            list.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
        universe.m_anyPrimitive = take();
        universe.m_keys = rangeOf(universe.m_firstKey, universe.m_numberOfObjects);
        universe.m_primitives = maker.unite(rangeOf(0, ValueFlowUniverse::numberOfPrimitiveKinds), universe.m_keys);
        universe.m_undefinedValue = maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Undefined));
        universe.m_unknownObjectAlone = maker.of(ValueFlowUniverse::unknownObject());
        universe.m_objects = ValueSets::complement(universe.m_primitives);
        universe.m_knownObjects = maker.intersect(universe.m_objects, ValueSets::complement(universe.m_unknownObjectAlone));
        universe.m_builtinObjects = rangeOf(universe.m_firstBuiltin, universe.m_firstNamespace);
        universe.m_closures = rangeOf(universe.m_firstClosure, universe.m_firstCoroutineResult);

        // Nearly every key is a text.
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

        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (isCallableBuiltin(builtins, BuiltinID(index)))
                list.append(ValueFlowObjectID(universe.m_firstBuiltin + index));
        }
        for (uint32_t index = 0; index < universe.m_moduleBuiltins.size(); ++index) {
            if (isCallableBuiltin(builtins, universe.m_moduleBuiltins[index].builtin))
                list.append(ValueFlowObjectID(universe.m_firstModuleBuiltin + index));
        }
        universe.m_builtinFunctions = take();

        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::IsLockedObject))
                list.append(ValueFlowObjectID(universe.m_firstBuiltin + index));
        }
        universe.m_lockedObjects = take();
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.hasLockedPrototypeLink(BuiltinID(index)))
                list.append(ValueFlowObjectID(universe.m_firstBuiltin + index));
        }
        universe.m_objectsWithLockedPrototypeLink = take();

        list.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
        universe.m_strings = maker.unite(take(), universe.m_textKeys);
        list.append(ValueFlowUniverse::primitive(PrimitiveSet::Symbol));
        universe.m_symbols = maker.unite(take(), universe.m_symbolKeys);

        Vector<Vector<ValueFlowObjectID>> ofLayout(m_program.layouts.size());
        Vector<Vector<ValueFlowObjectID>> ofClass(builtins.classCount());
        for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
            ofLayout[index].append(universe.instanceOf(LayoutID(index)));
            if (ModuleID module = m_program.layouts[index].namespaceOf)
                ofLayout[index].append(universe.namespaceOf(module));
        }
        for (unsigned index = 0; index < m_numberOfFunctions; ++index) {
            ofClass[BuiltinClasses::Array.index()].append(universe.restArrayOf(FunctionID(index)));
            if (BuiltinClassID builtinClass = universe.m_classOfCoroutineResult[index])
                ofClass[builtinClass.index()].append(universe.coroutineResultOf(FunctionID(index)));
        }
        // Nothing says what an object is that comes from an operation without a layout and a class, or that a built-in has allocated.
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
        // Native code allocates no object with a layout of the program: what a built-in makes has one of the table's layouts or none.
        universe.m_objectsWithAnyLayout = maker.unite(take(), universe.m_unknownObjectAlone);
        ValueSetID madeByBuiltins = maker.unite(rangeOf(universe.m_firstResultOfBuiltin, universe.m_firstClosure), rangeOf(universe.m_firstBuiltinResultSite, universe.m_firstKey));
        universe.m_objectsThatMayBeAnything = maker.unite(universe.m_objectsWithAnyLayout, madeByBuiltins);
        BitVector isLayoutOfTable(m_program.layouts.size());
        for (uint32_t index = 0; index < builtins.layoutCount(); ++index) {
            if (LayoutID layout = m_program.layoutOf(BuiltinLayoutID(index)))
                isLayoutOfTable.quickSet(layout.index());
        }

        // An object with a layout whose class extends something may be an instance of any built-in class.
        Vector<ValueSetID> inheritingLayouts;
        universe.m_objectsOfLayout = Vector<ValueSetID>(ofLayout.size(), [&](size_t index) {
            ValueSetID objects = maker.of(ofLayout[index].span());
            if (m_program.layouts[index].has(LayoutFlags::HasHeritage))
                inheritingLayouts.append(objects);
            return isLayoutOfTable.quickGet(index) ? maker.unite(objects, madeByBuiltins) : objects;
        });
        Vector<ValueSetID> ofLayoutsOfProgram;
        universe.m_isAllocatedByProgram.ensureSize(ofLayout.size());
        for (unsigned index = 0; index < ofLayout.size(); ++index) {
            if (isLayoutOfTable.quickGet(index))
                continue;
            ofLayoutsOfProgram.append(universe.m_objectsOfLayout[index]);
            // Every layout has the object that stands for what its constructor makes.
            if (ofLayout[index].size() > 1 || m_program.layouts[index].constructor)
                universe.m_isAllocatedByProgram.quickSet(index);
        }
        universe.m_objectsWithLayoutOfProgram = maker.uniteAll(ofLayoutsOfProgram.span());
        universe.m_isLayoutOfTable = WTF::move(isLayoutOfTable);
        // A test for a class cannot tell a built-in object from an instance.
        ValueSetID inEveryClass = maker.unite(maker.unite(maker.uniteAll(inheritingLayouts.span()), universe.m_builtinObjects), universe.m_objectsThatMayBeAnything);

        auto hasSameCellTypeTest = [&](BuiltinClassID first, BuiltinClassID second) {
            const BuiltinClass& a = builtins.at(first);
            const BuiltinClass& b = builtins.at(second);
            return a.instanceTest == BuiltinInstanceTest::JSTypeEquals && b.instanceTest == BuiltinInstanceTest::JSTypeEquals && a.type == b.type;
        };
        // An object that is allocated as an instance of a class may be one of a class below it, and it passes the tests of the classes above it.
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
        Vector<ValueSetID> parts;
        Vector<ValueSetID> callableInstances;
        universe.m_objectsOfClass = Vector<ValueSetID>(builtins.classCount(), [&](size_t index) {
            BuiltinClassID tested(static_cast<uint32_t>(index));
            bool hasCallableInstances = Type::builtinClass(tested).overlaps(Type::function(), m_program);
            if (hasCallableInstances)
                callableInstances.append(allocatedAs[index]);
            // A class without a test may hold any object.
            if (builtins.at(tested).instanceTest == BuiltinInstanceTest::None)
                return universe.m_objects;

            parts.shrink(0);
            parts.append(inEveryClass);
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
        universe.m_callables = maker.uniteAll({ { universe.m_builtinFunctions, universe.m_closures, universe.m_otherCallables, universe.m_unknownObjectAlone } });

        universe.m_builtinTables->makeSets(m_program, universe, maker);
    }

    // Each body writes its own stretch of every list, so the lists are the same on any number of threads.
    void layOutLists()
    {
        ValueFlowBodyRange next;
        uint32_t nextOperand = 0;
        uint32_t nextEdge = 0;
        uint32_t nextMask = 0;
        uint32_t nextObservedValue = 0;
        for (unsigned index = 0; index < m_bodies.size(); ++index) {
            const Body& body = m_bodies[index];
            ValueFlowBodyRange range = next;
            range.statementLeavingBody = next.statement + body.firstStatementLeavingBody;
            range.copiedStatement = next.statement + body.statements.size() - body.numberOfCopiedStatements;
            m_result.m_ranges.append(range);
            m_firstOperand.append(nextOperand);
            m_firstEdge.append(nextEdge);
            m_firstMask.append(nextMask);
            m_firstObservedValue.append(nextObservedValue);

            next.statement += body.statements.size();
            next.edgeOnceReached += body.edgesOnceReached.size();
            next.seedOnceReached += body.seedsOnceReached.size();
            next.functionEntered += body.functionsEnteredOnceReached.size();
            nextOperand += body.operands.size();
            nextEdge += body.edges.size();
            nextObservedValue += body.observedValues.size();
            nextMask += std::ranges::count_if(body.nodes, [](const Body::Node& node) {
                return !!node.mask;
            });
        }
        next.statementLeavingBody = next.statement;
        next.copiedStatement = next.statement;
        for (unsigned index = 0; index < m_bodies.size(); ++index) {
            for (const Body::Copy& copy : m_bodies[index].copies)
                m_result.m_copies.append({ copy.function, nodeNamed(index, ValueFlowNodeName::ofBody(copy.firstNode)), copy.numberOfNodes });
        }
        m_result.m_ranges.append(next);

        m_result.m_statements.grow(next.statement);
        m_result.m_operands.grow(nextOperand);
        m_result.m_edges.grow(nextEdge);
        m_result.m_masks.grow(nextMask);
        m_result.m_edgesOnceReached.grow(next.edgeOnceReached);
        m_result.m_seedsOnceReached.grow(next.seedOnceReached);
        m_result.m_functionsEnteredOnceReached.grow(next.functionEntered);
        m_result.m_observedValues.grow(nextObservedValue);
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
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void linkBody(unsigned index, ValueSetMaker& maker, HashMap<TypeID, ValueSetID>& maskOfType)
    {
        const Body& body = m_bodies[index];
        const ValueFlowUniverse& universe = m_result.m_universe;
        const ValueFlowBodyRange& range = m_result.m_ranges[index];
        FunctionID function(index);

        Vector<ValueSetID, 32> constants = body.constants.map<Vector<ValueSetID, 32>>([&](const Body::Constant& constant) {
            Vector<ValueFlowObjectID, 8> members;
            for (uint16_t rest = constant.primitives; rest; rest &= rest - 1)
                members.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
            for (const ValueFlowObjectName& object : constant.objects)
                members.append(universe.objectNamed(function, object));
            return maker.of(members.span());
        });
        Vector<ValueSetID, 32> masks = body.masks.map<Vector<ValueSetID, 32>>([&](const Body::Mask& mask) {
            ValueSetID set = ValueSets::anything;
            for (TypeID type : mask.inside) {
                ValueSetID inside = maskOfType.ensure(type, [&] {
                    return universe.maskOf(maker, m_program, m_program.at(type));
                }).iterator->value;
                set = maker.intersect(set, inside);
            }
            for (TypeID type : mask.outside)
                set = maker.intersect(set, ValueSets::complement(universe.membersSurelyInside(maker, m_program, m_program.at(type))));
            return set;
        });
        auto link = [&](const Body::Operand& operand) {
            return ValueFlowOperand { nodeNamed(index, operand.node), constants[operand.constant], masks[operand.mask] };
        };
        auto linkEdge = [&](const Body::Edge& edge) {
            return ValueFlowEdge { nodeNamed(index, edge.from), nodeNamed(index, edge.to) };
        };

        uint32_t nextMask = m_firstMask[index];
        for (uint32_t node = 0; node < body.nodes.size(); ++node) {
            if (uint32_t mask = body.nodes[node].mask)
                m_result.m_masks[nextMask++] = { nodeNamed(index, ValueFlowNodeName::ofBody(node)), masks[mask] };
        }
        std::ranges::transform(body.edges, m_result.m_edges.mutableSpan().subspan(m_firstEdge[index]).begin(), linkEdge);
        std::ranges::transform(body.edgesOnceReached, m_result.m_edgesOnceReached.mutableSpan().subspan(range.edgeOnceReached).begin(), linkEdge);
        std::ranges::transform(body.seedsOnceReached, m_result.m_seedsOnceReached.mutableSpan().subspan(range.seedOnceReached).begin(), [&](const Body::Seed& seed) {
            return ValueFlowNodeSet { nodeNamed(index, seed.node), maker.pass(constants[seed.constant], masks[seed.mask], universe.standIn()) };
        });
        std::ranges::copy(body.functionsEnteredOnceReached, m_result.m_functionsEnteredOnceReached.mutableSpan().subspan(range.functionEntered).begin());
        std::ranges::transform(body.operands, m_result.m_operands.mutableSpan().subspan(m_firstOperand[index]).begin(), link);
        std::ranges::transform(body.statements, m_result.m_statements.mutableSpan().subspan(range.statement).begin(), [&](const Body::Statement& statement) {
            return ValueFlowStatement {
                statement.kind,
                statement.opcode,
                statement.function ? statement.function : body.function,
                statement.site,
                m_firstOperand[index] + statement.firstOperand,
                statement.operandCount,
                nodeNamed(index, statement.result),
                nodeNamed(index, statement.thrown),
                statement.copiedCallee,
                statement.firstSite == Body::none ? UINT32_MAX : universe.m_firstSiteOfFunction[index] + statement.firstSite,
            };
        });
        std::ranges::transform(body.observedValues, m_result.m_observedValues.mutableSpan().subspan(m_firstObservedValue[index]).begin(), [&](const Body::ObservedValue& observed) {
            return ValueFlowObservedValue { body.function, observed.test, link(observed.operand) };
        });
    }

    const Program& m_program;
    BuildContext& m_build;
    const ParallelLoops& m_loops;
    unsigned m_numberOfFunctions;
    ParallelArray<ValueFlowBody> m_bodies;
    Vector<std::unique_ptr<ValueSetMaker>> m_makers;
    // One for each thread.
    Vector<HashMap<TypeID, ValueSetID>> m_maskOfType;
    Vector<uint32_t> m_firstOperand;
    Vector<uint32_t> m_firstEdge;
    Vector<uint32_t> m_firstMask;
    Vector<uint32_t> m_firstObservedValue;
    ValueFlowBodies m_result;
};

ValueFlowBodies::ValueFlowBodies() = default;
ValueFlowBodies::ValueFlowBodies(ValueFlowBodies&&) = default;
ValueFlowBodies& ValueFlowBodies::operator=(ValueFlowBodies&&) = default;
ValueFlowBodies::~ValueFlowBodies() = default;

ValueFlowBodies reduceBodies(const Program& program, BuildContext& build)
{
    return ValueFlowLinker(program, build).link();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
