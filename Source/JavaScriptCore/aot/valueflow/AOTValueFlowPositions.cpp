/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowPositions.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTLayout.h"
#include "AOTNameClass.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

ValueFlowLayoutsOfSites findLayoutsOfSites(const ValueFlowResultInputs& inputs)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowBodies& bodies = inputs.bodies();
    const ValueFlowSolver& solver = inputs.solver();
    const ValueSets& sets = inputs.sets();
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    ValueFlowLayoutsOfSites found;
    auto& bySite = found.bySite;

    for (uint32_t index : findIndicesInParallel(inputs.loops(), universe.numberOfObjects(), [&](unsigned index) {
        ValueFlowObjectID object(index);
        const ValueFlowSite* site = universe.siteOf(object);
        if (!site || !site->layout || site->origin != ValueFlowSiteOrigin::Operation || !program.prototypeMaySelectOtherClass(site->layout))
            return false;
        return mayAllocateOtherLayout(program, program.at(universe.functionOf(object)).at(site->value));
    }))
        bySite.add(index, ValueFlowLayoutsOfSite { { universe.siteOf(ValueFlowObjectID(index))->layout }, true });
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Instance && program.layouts[index].constructor)
            bySite.add(universe.instanceOf(LayoutID(index)).index(), ValueFlowLayoutsOfSite { { LayoutID(index) }, false });
    }
    if (bySite.isEmpty())
        return found;

    Vector<const ValueFlowStatement*> statements = collectInParallel<const ValueFlowStatement*>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<const ValueFlowStatement*>& list) {
        if (!inputs.isReached(FunctionID(index)))
            return;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            ValueFlowObjectID object = statement.opcode == Opcode::NewObject ? universe.siteObject(statement) : ValueFlowObjectID();
            if (statement.opcode == Opcode::NewClass ? !!program.functions[index].at(statement.site).heritageOperand() : object && bySite.contains(object.index()))
                list.append(&statement);
        }
    });
    auto layoutAt = [&](const ValueFlowStatement& statement) {
        return program.at(statement.function).at(statement.site).layout();
    };

    Vector<ValueSetID> heritageOfClass(FillWith { }, program.layouts.size(), ValueSets::nothing);
    ValueSetID heritagesOfClasses = maker.unite(universe.objects(), maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Null)));
    for (const ValueFlowStatement* statement : statements) {
        if (statement->opcode != Opcode::NewClass)
            continue;
        ValueSetID& heritage = heritageOfClass[layoutAt(*statement).index()];
        heritage = maker.unite(heritage, maker.intersect(inputs.possibleValuesOf(maker, bodies.operandsOf(*statement)[OperandIndex::NewClass::heritage]), heritagesOfClasses));
    }
    auto layoutAbove = [&](LayoutID layout) {
        ValueFlowObjectID heritage = sets.onlyMember(heritageOfClass[layout.index()]);
        FunctionID constructor = heritage ? universe.closureFunction(heritage) : FunctionID();
        return constructor ? program.at(constructor).constructedClass : LayoutID();
    };
    auto isBelow = [&](LayoutID layout, LayoutID base) {
        for (size_t steps = 0; layout && steps <= program.layouts.size(); ++steps) {
            if (layout == base)
                return true;
            if (!program.at(layout).has(LayoutFlags::HasHeritage))
                return false;
            layout = layoutAbove(layout);
            if (layout && !isClassLayout(program.at(layout)))
                return false;
        }
        return false;
    };

    for (const ValueFlowStatement* statement : statements) {
        if (statement->opcode != Opcode::NewObject)
            continue;
        ValueFlowObjectID allocation = universe.siteObject(*statement);
        ValueSetID prototypes = inputs.possibleValuesOf(maker, bodies.operandsOf(*statement)[OperandIndex::NewObject::prototype]);
        ValueFlowLayoutsOfSite picked { { layoutAt(*statement) }, ValueSets::isComplement(prototypes) || sets.meet(prototypes, universe.placeholdersAlone()) };
        LayoutID ownClass = declaredLayoutOf(program, layoutAt(*statement));
        sets.forEachMember(ValueSets::isComplement(prototypes) ? ValueSets::nothing : prototypes, [&](ValueFlowObjectID prototype) {
            LayoutID requested = universe.classLayoutOf(prototype);
            if (!requested || requested == ownClass || !isClassLayout(program.at(requested)))
                return;
            ValueFlowObjectID made = universe.objectAllocatedUnder(program, allocation, prototype);
            ValueFlowLayoutsOfSite& picksOfMade = made == allocation ? picked : bySite.find(made.index())->value;
            if (!isBelow(requested, ownClass))
                picksOfMade.mayBeOtherObject = true;
            else if (made == allocation)
                picked.layouts.append(requested);
        });
        bySite.set(allocation.index(), WTF::move(picked));
    }

    auto closuresIn = [&](ValueSetID set) {
        if (ValueSets::isComplement(set) || sets.contains(set, ValueFlowUniverse::untrackedObject()))
            set = maker.unite(set, solver.possibleValuesOf(ValueFlowProgramNodes::lost()));
        return maker.intersect(set, universe.closures());
    };
    ValueSetID heritagesOutsideProgram = maker.unite(universe.builtinObjects(), maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Null)));
    auto getsOwnLayoutFromEach = [&](LayoutID newTarget, std::span<const LayoutID> sortedConstructorFunctions) {
        size_t numberOnChain = 0;
        for (size_t steps = 0; newTarget && steps <= program.layouts.size(); ++steps, newTarget = layoutAbove(newTarget)) {
            numberOnChain += std::ranges::binary_search(sortedConstructorFunctions, newTarget);
            if (!program.at(newTarget).has(LayoutFlags::HasHeritage) || sets.isSubset(heritageOfClass[newTarget.index()], heritagesOutsideProgram))
                return numberOnChain == sortedConstructorFunctions.size();
        }
        return true;
    };

    ValueSetID newTargetsFromOperands = closuresIn(solver.possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::NewTargetsFromOperands)));
    if (sets.isEmpty(newTargetsFromOperands))
        return found;
    uint32_t firstMadeNode = solver.input().numberOfNodes;
    for (uint32_t position : findIndicesInParallel(inputs.loops(), solver.graph().numberOfNodes() - firstMadeNode, [&](unsigned position) {
        ValueFlowNodeID node(firstMadeNode + position);
        const ValueFlowNodeKey& key = *solver.keyOfMadeNode(node);
        return key.kind == ValueFlowMadeNodeKind::CallGroup && isConstruction(key.callKind()) && solver.firstOfMadeNodesAt(solver.indexOfMadeNodes(key)) == node;
    })) {
        ValueFlowNodeID node(firstMadeNode + position);
        Vector<LayoutID, 8> constructorFunctions;
        bool constructsFunctionWithoutLayout = false;
        sets.forEachMember(closuresIn(solver.keyOfMadeNode(node)->set), [&](ValueFlowObjectID callee) {
            const Function& function = program.at(universe.closureFunction(callee));
            if (function.kind != FunctionKind::Plain)
                return;
            if (function.constructedClass)
                constructorFunctions.append(function.constructedClass);
            else
                constructsFunctionWithoutLayout = true;
        });
        if (constructorFunctions.isEmpty() && !constructsFunctionWithoutLayout)
            continue;
        AOT::sort(constructorFunctions);
        constructorFunctions.shrink(constructorFunctions.size() - std::ranges::unique(constructorFunctions).size());

        sets.forEachMember(maker.intersect(closuresIn(solver.possibleValuesOf(callGroupAt(node).newTarget)), newTargetsFromOperands), [&](ValueFlowObjectID newTarget) {
            LayoutID layout = program.at(universe.closureFunction(newTarget)).constructedClass;
            if (!layout || (!constructsFunctionWithoutLayout && getsOwnLayoutFromEach(layout, constructorFunctions.span())))
                return;
            if (auto instance = bySite.find(universe.instanceOf(layout).index()); instance != bySite.end())
                instance->value.mayBeOtherObject = true;
        });
    }
    return found;
}

ValueFlowPositions::ValueFlowPositions(const ValueFlowResultInputs& inputs, TypeArena& types, ValueFlowPositionCounts& counts, uint32_t switchSubject, ValueSetMaker* makerOfThread)
    : m_inputs(inputs)
    , m_types(types)
    , m_counts(counts)
    , m_switchSubject(switchSubject)
    , m_maker(makerOfThread)
{
}

ValueFlowPositions::~ValueFlowPositions()
{
    if (m_hasBorrowedMaker)
        m_inputs.makerPool().giveBack(*m_maker);
}

ValueSetMaker& ValueFlowPositions::maker()
{
    if (!m_maker) {
        m_maker = &m_inputs.makerPool().take();
        m_hasBorrowedMaker = true;
    }
    return *m_maker;
}

const ValueFlowLayoutsOfSite* ValueFlowPositions::findLayoutsPickedBy(ValueFlowObjectID object)
{
    if (!m_inputs.layoutsOfSites) {
        std::call_once(m_counts.hasFoundLayoutsOfSites, [&] {
            m_counts.layoutsOfSites = findLayoutsOfSites(m_inputs);
        });
    }
    const auto& bySite = (m_inputs.layoutsOfSites ? *m_inputs.layoutsOfSites : m_counts.layoutsOfSites).bySite;
    auto found = bySite.find(object.index());
    return found == bySite.end() ? nullptr : &found->value;
}

static bool picksOnlyAmong(const ValueFlowLayoutsOfSite& picked, std::span<const LayoutID> members)
{
    return !picked.mayBeOtherObject && std::ranges::all_of(picked.layouts, [&](LayoutID layout) {
        return std::ranges::binary_search(members, layout);
    });
}

bool ValueFlowPositions::isSurelyInside(ValueSetID set, const Type& type, std::optional<TypeID> number)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID picking = universe.objectsThatPickTheirLayout();
    if (!number)
        number = program.types.find(type);
    ValueSetID surelyInside = number ? universe.membersSurelyInside(maker(), program, *number) : universe.membersSurelyInside(maker(), program, type);
    if (!m_inputs.sets().isSubset(maker().intersect(set, ValueSets::complement(picking)), surelyInside))
        return false;
    if (type.anyObject)
        return true;
    std::span<const LayoutID> members = membersOf(program, type);
    return !m_inputs.sets().anyMember(maker().intersect(set, picking), [&](ValueFlowObjectID object) {
        const ValueFlowLayoutsOfSite* picked = findLayoutsPickedBy(object);
        return !picked || !picksOnlyAmong(*picked, members);
    });
}

std::optional<Type> ValueFlowPositions::typeWhereNothingPasses(ValueSetID set, const Type& type)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    if (ValueSets::isComplement(set) || holds(set, ValueFlowUniverse::unknownObject()) || holds(set, ValueFlowUniverse::untrackedObject()) || !type.isJavaScriptValue())
        return std::nullopt;
    if (!m_inputs.sets().meet(set, universe.objects()))
        return std::nullopt;
    bool mayPass = m_inputs.sets().anyMember(set, [&](ValueFlowObjectID member) {
        if (member == ValueFlowUniverse::primitive(PrimitiveSet::Undefined) || member == ValueFlowUniverse::primitive(PrimitiveSet::Null) || member == ValueFlowUniverse::primitive(PrimitiveSet::Absent))
            return false;
        Type alone = typeOfObject(member);
        return alone.anyObject || alone.isSubsetOf(type, program) || ((!universe.isObject(member) || findLayoutsPickedBy(member)) && alone.overlaps(type, program));
    });
    if (mayPass)
        return std::nullopt;
    return withinLimits(typeOf(set));
}

bool ValueFlowPositions::holdsObjectThatMayLieOutside(ValueSetID set, const Type& type)
{
    if (type.anyObject)
        return false;
    if (holds(set, ValueFlowUniverse::untrackedObject()))
        return true;
    std::span<const LayoutID> members = membersOf(m_inputs.program, type);
    return m_inputs.sets().anyMember(maker().intersect(set, m_inputs.universe().objectsThatPickTheirLayout()), [&](ValueFlowObjectID object) {
        const ValueFlowLayoutsOfSite* picked = findLayoutsPickedBy(object);
        return picked && !picksOnlyAmong(*picked, members);
    });
}

bool ValueFlowPositions::turnsAwayConstructedObject(ValueSetID set, const Type& type)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    if (ValueSets::isComplement(set) || !type.isJavaScriptValue() || !m_inputs.sets().meet(set, universe.constructedObjects()))
        return false;
    return !m_inputs.sets().isSubset(maker().intersect(set, universe.constructedObjects()), universe.maskOf(maker(), m_inputs.program, type));
}

bool ValueFlowPositions::addInstanceOfClass(Type& type, ValueFlowObjectID member, BuiltinClassID builtinClass)
{
    Type instance = Type::builtinClass(builtinClass);
    ClassPart& part = instance.classes[0];
    const ValueFlowSite* site = m_inputs.universe().siteOf(member);
    if (const ValueFlowContainerFacts* facts = m_inputs.containers ? m_inputs.containers->find(member) : nullptr) {
        if (isEnabled(FlowResultKind::ProvenContents) && !(site && site->origin == ValueFlowSiteOrigin::TypedResult)) {
            part.writtenContents = facts->contents;
            part.contentsAreProven = facts->contentsAreProven;
        }
        if (isEnabled(FlowResultKind::ArrayStorageKinds) && part.contentsAreProven)
            part.arrayStorage = facts->arrayStorage;
        if (isEnabled(FlowResultKind::PristineInstances))
            part.provenPristine = facts->pristine;
    }
    bool isRegExp = builtinClass == BuiltinClasses::RegExp;
    if (isRegExp && site && site->origin == ValueFlowSiteOrigin::Operation) {
        const Value& creation = m_inputs.program.at(m_inputs.universe().functionOf(member)).at(site->value);
        if (creation.opcode == Opcode::NewRegExp)
            instance.regExpLiteralSet.add({ creation.pattern(), creation.regExpFlags().toRaw() });
    }
    const ClassPart* listed = type.findClassPart(builtinClass);
    if (listed && *listed == part && (!isRegExp || type.regExpLiteralSet == instance.regExpLiteralSet))
        return false;
    Type united = type.unionWith(instance, m_types);
    bool saysSomethingElse = united.classes != type.classes || united.regExpLiteralSet != type.regExpLiteralSet;
    type = WTF::move(united);
    return saysSomethingElse;
}

BuiltinClassID ValueFlowPositions::addUnlessInstanceOfClass(Type& type, Vector<LayoutID, 8>& layouts, ValueFlowObjectID member)
{
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = m_inputs.universe();

    switch (universe.kindOf(member)) {
    case ValueFlowObjectKind::Primitive: {
        uint16_t kind = static_cast<uint16_t>(1u << member.index());
        if (kind == Primitive::LiteralString)
            kind = Primitive::String;
        else if (kind == Primitive::LiteralSymbol)
            kind = Primitive::Symbol;
        type.primitives |= kind & Primitive::Defined;
        if (kind == Primitive::Integer)
            type.integers = Range::full();
        return { };
    }
    case ValueFlowObjectKind::Key: {
        PropertyKeyID id = universe.keyOf(member);
        const PropertyKey& key = program.at(id);
        if (key.kind == PropertyKeyKind::Text)
            type.stringLiterals.add(key.text);
        else if (key.kind == PropertyKeyKind::RegisteredSymbol || key.kind == PropertyKeyKind::WellKnownSymbol
            || (key.kind == PropertyKeyKind::CreatedSymbol && m_inputs.symbolsCreatedOnce && m_inputs.symbolsCreatedOnce->get(id.index()))) {
            type.symbolLiterals.add(id);
            type.primitives |= Primitive::LiteralSymbol;
        } else
            type.primitives |= isSymbolKind(key.kind) ? Primitive::Symbol : Primitive::Symbol | Primitive::String;
        return { };
    }
    case ValueFlowObjectKind::Closure: {
        FunctionID function = universe.closureFunction(member);
        const RuntimeFunctionEntry* entry = findRuntimeFunction(program, program.at(function).enclosingRuntimeFunction);
        if (entry && entry->function == function && !m_inputs.isReached(function))
            type.anyFunction = true;
        else
            type.functions.add(function);
        return { };
    }
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin: {
        BuiltinID builtin = builtins.canonicalID(universe.builtinOf(member));
        if (!isObjectKind(builtins.at(builtin).kind) || builtin == Builtins::UndeclaredNativeCode)
            break;
        type.builtins.add(builtin);
        return { };
    }
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::Site: {
        if (LayoutID layout = universe.layoutOf(member); layout && !m_inputs.isLayoutOfDictionaries(layout)) {
            const ValueFlowSite* site = universe.siteOf(member);
            if (site && site->origin == ValueFlowSiteOrigin::BuiltinResult)
                break;
            const ValueFlowLayoutsOfSite* picked = findLayoutsPickedBy(member);
            if (picked && picked->mayBeOtherObject)
                break;
            if (picked)
                layouts.appendVector(picked->layouts);
            else
                layouts.append(layout);
            return { };
        }
        BuiltinClassID builtinClass = m_inputs.classOf(member);
        if (!builtinClass)
            break;
        if (!builtins.at(builtinClass).hasClosureInstances)
            return builtinClass;
        type.anyFunction = true;
        return { };
    }
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
        type.anyObject = true;
        return { };
    case ValueFlowObjectKind::Namespace:
        if (LayoutID layout = program.layoutOfWholeNamespace(universe.moduleOf(member))) {
            layouts.append(layout);
            return { };
        }
        break;
    case ValueFlowObjectKind::ResultOfBuiltin:
        if (BuiltinClassID builtinClass = m_inputs.classOf(member); builtinClass && !builtins.at(builtinClass).hasClosureInstances)
            return builtinClass;
        break;
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Prototype:
        break;
    }
    type.anyObject = type.anyFunction = true;
    return { };
}

Type ValueFlowPositions::finish(Type&& type, Type&& instances, std::span<const LayoutID> layouts)
{
    type.classes = WTF::move(instances.classes);
    type.regExpLiteralSet = WTF::move(instances.regExpLiteralSet);
    type.canonicalize();
    if (!layouts.empty() && !type.anyObject)
        type.layouts = m_types.completeLayoutSet(layouts);
    return WTF::move(type);
}

Type ValueFlowPositions::typeOfObject(ValueFlowObjectID member)
{
    Type type;
    Type instance;
    Vector<LayoutID, 8> layouts;
    if (BuiltinClassID builtinClass = addUnlessInstanceOfClass(type, layouts, member))
        addInstanceOfClass(instance, member, builtinClass);
    return finish(WTF::move(type), WTF::move(instance), layouts.span());
}

Type ValueFlowPositions::typeOf(ValueSetID set)
{
    if (auto found = m_indexOfSet.find(set.index()); found != m_indexOfSet.end())
        return m_typesOfSets[found->value];
    Type type = typeOfMembers(set);
    m_indexOfSet.add(set.index(), m_typesOfSets.size());
    m_typesOfSets.append(type);
    return type;
}

Type ValueFlowPositions::typeOfMembers(ValueSetID set)
{
    AOT_THRESHOLD(uint32_t, fewestMembersOfSharedSet, 32);
    AOT_THRESHOLD(size_t, mostBytesOfTypesOfSets, 256 * MB);
    const ValueFlowUniverse& universe = m_inputs.universe();
    bool isShared = m_counts.sharesTypesOfSets && m_inputs.sets().size(set) >= fewestMembersOfSharedSet
        && isEnabled(FlowResultKind::ProvenContents) && isEnabled(FlowResultKind::ArrayStorageKinds) && isEnabled(FlowResultKind::PristineInstances);
    uint32_t hash = IntHash<uint32_t>::hash(set.index());
    auto isWanted = [&](const ValueFlowSharedTypeOfSet& known) {
        return known.set == set;
    };
    Type instances;
    if (const ValueFlowSharedTypeOfSet* known = isShared ? m_counts.typesOfSets.find(hash, isWanted) : nullptr) {
        if (known->firstInstanceMeetsOtherMembers)
            instances.primitives = Primitive::Undefined;
        for (uint32_t index = 0; index < known->instances.size(); ++index) {
            if (index == known->numberOfInstancesBeforeAnyObject)
                instances.anyObject = true;
            addInstanceOfClass(instances, known->instances[index].object, known->instances[index].builtinClass);
        }
        return finish(Type(known->withoutLayoutsAndInstances), WTF::move(instances), known->layouts.span());
    }

    ValueFlowSharedTypeOfSet shared { .set = set };
    Type& type = shared.withoutLayoutsAndInstances;
    Vector<LayoutID, 8> layouts;
    Vector<StringID, 16> texts;
    bool hasMetInstance = false;
    m_inputs.sets().forEachMember(set, [&](ValueFlowObjectID member) {
        if (member == ValueFlowUniverse::untrackedObject())
            return;
        bool isKey = universe.kindOf(member) == ValueFlowObjectKind::Key;
        if (const PropertyKey* key = isKey ? &m_inputs.program.at(universe.keyOf(member)) : nullptr; key && key->kind == PropertyKeyKind::Text) {
            if (!(type.primitives & Primitive::String))
                texts.append(key->text);
            return;
        }
        if (!isKey && type.anyObject && instances.classes.isEmpty())
            return;
        BuiltinClassID builtinClass = addUnlessInstanceOfClass(type, layouts, member);
        if (!builtinClass)
            return;
        if (!std::exchange(hasMetInstance, true) && !type.isNever()) {
            shared.firstInstanceMeetsOtherMembers = true;
            instances.primitives = Primitive::Undefined;
        }
        if (type.anyObject && !instances.anyObject) {
            shared.numberOfInstancesBeforeAnyObject = shared.instances.size();
            instances.anyObject = true;
        }
        if (addInstanceOfClass(instances, member, builtinClass))
            shared.instances.append({ member, builtinClass });
    });
    if (texts.size() > m_inputs.build.limit(LimitID::MaxLiteralsInType))
        type.primitives |= Primitive::String;
    else if (!texts.isEmpty())
        type.stringLiterals = SortedSet<StringID>(texts.span());
    type.canonicalize();
    if (!type.anyObject) {
        AOT::sort(layouts);
        shared.layouts.append(layouts.span().first(layouts.size() - std::ranges::unique(layouts).size()));
    }

    if (!isShared)
        return finish(WTF::move(type), WTF::move(instances), shared.layouts.span());
    Type result = finish(Type(type), WTF::move(instances), shared.layouts.span());
    size_t bytes = sizeof(ValueFlowSharedTypeOfSet) + type.stringLiterals.span().size_bytes() + type.symbolLiterals.span().size_bytes() + type.functions.span().size_bytes() + type.builtins.span().size_bytes()
        + shared.layouts.sizeInBytes() + shared.instances.sizeInBytes();
    if (m_counts.bytesOfTypesOfSets.fetch_add(bytes, std::memory_order_relaxed) + bytes <= mostBytesOfTypesOfSets)
        m_counts.typesOfSets.add(hash, WTF::move(shared), isWanted);
    return result;
}

Type ValueFlowPositions::withinLimits(Type&& type)
{
    BuildContext& build = m_inputs.build;
    bool namesLayouts = isEnabled(FlowResultKind::ProvenLayoutSets) && membersOf(m_inputs.program, type).size() <= build.limit(LimitID::MaxLayoutsInType);
    if (type.layouts && !namesLayouts)
        type.anyObject = type.anyFunction = true;

    bool listsLiterals = isEnabled(FlowResultKind::LiteralValues);
    if (!type.stringLiterals.isEmpty() && (!listsLiterals || type.stringLiterals.size() > build.limit(LimitID::MaxLiteralsInType)))
        type.primitives |= Primitive::String;
    if (!type.symbolLiterals.isEmpty() && !listsLiterals)
        type.primitives |= Primitive::Symbol;
    if (!listsLiterals || type.regExpLiteralSet.size() > build.limit(LimitID::MaxRegExpLiteralsInType))
        type.regExpLiteralSet.clear();
    if (!type.functions.isEmpty() && (!isEnabled(FlowResultKind::ExactFunctions) || type.functions.size() > build.limit(LimitID::MaxFunctionsInType)))
        type.anyFunction = true;
    if (type.builtins.size() > build.limit(LimitID::MaxBuiltinsInType))
        type.anyObject = type.anyFunction = true;
    type.canonicalize();
    return WTF::move(type);
}

ValueFlowObjectID ValueFlowPositions::findStranger(ValueSetID set, const Type& family)
{
    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueFlowObjectID outside;
    ValueFlowObjectID undecided;
    BitVector layoutsInside;
    auto sureLayoutOf = [&](ValueFlowObjectID member, const ValueFlowLayoutsOfSite* picked) {
        if (picked || (universe.kindOf(member) != ValueFlowObjectKind::Site && universe.kindOf(member) != ValueFlowObjectKind::Instance))
            return LayoutID();
        LayoutID layout = universe.layoutOf(member);
        if (!layout || m_inputs.isLayoutOfDictionaries(layout))
            return LayoutID();
        const ValueFlowSite* site = universe.siteOf(member);
        return site && site->origin == ValueFlowSiteOrigin::BuiltinResult ? LayoutID() : layout;
    };
    bool hasMetObjectThatPicksAnyLayout = false;
    m_inputs.sets().anyMember(set, [&](ValueFlowObjectID member) {
        bool mayPickLayout = universe.kindOf(member) == ValueFlowObjectKind::Site || universe.kindOf(member) == ValueFlowObjectKind::Instance;
        const ValueFlowLayoutsOfSite* picked = mayPickLayout ? findLayoutsPickedBy(member) : nullptr;
        LayoutID sureLayout = sureLayoutOf(member, picked);
        if (member == ValueFlowUniverse::untrackedObject() || (sureLayout && layoutsInside.get(sureLayout.index())))
            return false;
        // All of these have one type, and the first of them has been looked at.
        if (picked && picked->mayBeOtherObject && !m_inputs.isLayoutOfDictionaries(universe.layoutOf(member)) && std::exchange(hasMetObjectThatPicksAnyLayout, true))
            return false;
        Type type = typeOfObject(member);
        if (type.isSubsetOf(family, program)) {
            if (sureLayout)
                layoutsInside.set(sureLayout.index());
        } else if (!picked || !type.overlaps(family, program))
            outside = member;
        else if (!undecided)
            undecided = member;
        return !!outside;
    });
    return outside ? outside : undecided;
}

static bool holdsOnlyObjectsOf(Type& held, const Type& family, const Program& program)
{
    uint16_t primitives = std::exchange(held.primitives, 0);
    Range integers = std::exchange(held.integers, Range::empty());
    SortedSet<StringID> stringLiterals = std::exchange(held.stringLiterals, { });
    SortedSet<PropertyKeyID> symbolLiterals = std::exchange(held.symbolLiterals, { });
    bool isSubset = held.isSubsetOf(family, program);
    held.primitives = primitives;
    held.integers = integers;
    held.stringLiterals = WTF::move(stringLiterals);
    held.symbolLiterals = WTF::move(symbolLiterals);
    return isSubset;
}

std::optional<Type> ValueFlowPositions::familyBehindDictionaries(const Type& family)
{
    if (m_inputs.typesBehindDictionaries.empty())
        return std::nullopt;
    auto namesDictionaries = [&](LayoutSetID set) {
        if (!TypeTable::isProvisional(set) && set.index() < m_inputs.layoutSetsNamingDictionaries->size())
            return m_inputs.layoutSetsNamingDictionaries->quickGet(set.index());
        return std::ranges::any_of(m_inputs.program.at(set).written(), [&](LayoutID member) { return m_inputs.isLayoutOfDictionaries(member); });
    };
    bool isChanged = family.layouts && namesDictionaries(family.layouts);
    for (const ClassPart& part : family.classes) {
        for (TypeID contents : part.writtenContents)
            isChanged |= typeBehindDictionaries(m_inputs.typesBehindDictionaries, contents) != contents;
    }
    if (!isChanged)
        return std::nullopt;
    return typeBehindDictionaries(m_inputs.program, *m_inputs.layoutsOfDictionaries, m_inputs.typesBehindDictionaries, family, m_types);
}

template<typename KindOfType>
ValueFlowPositionResult ValueFlowPositions::decide(KindOfType kindOfFamily, ValueSetID set, DeadZone deadZone, const ValueFlowNumbers* numbers, ValueFlowNodeID node)
{
    constexpr bool isProven = std::is_same_v<KindOfType, ProvenType>;
    static_assert(isProven || std::is_same_v<KindOfType, ExpectedType>);
    // A layout that is picked later is picked inside the type of a position, which every store is held to.
    bool holdsInEveryVersion = false;
    if constexpr (isProven)
        holdsInEveryVersion = kindOfFamily.scope() == ProofScope::EveryVersion;
    const Program& program = m_inputs.program;
    const Type& familyAsRead = program.at(kindOfFamily.payload());
    std::optional<Type> familyBehind = familyBehindDictionaries(familyAsRead);
    const Type& family = familyBehind ? *familyBehind : familyAsRead;
    if (!family.isJavaScriptValue() || m_inputs.sets().isEmpty(set))
        return { };
    if (!isEnabled(FlowResultKind::NarrowedTypes))
        return { .proof = ValueFlowProof::Disabled };
    bool holdsUnknownValue = ValueSets::isComplement(set) || holds(set, ValueFlowUniverse::unknownObject());
    bool holdsUntrackedObject = holds(set, ValueFlowUniverse::untrackedObject());
    bool hasDeadZoneToRemove = family.primitives & Primitive::Absent && deadZone != DeadZone::MayBeObserved && isEnabled(FlowResultKind::Initialization);
    if ((holdsUnknownValue || holdsUntrackedObject) && family.isAnything() && hasDeadZoneToRemove)
        return { .proof = ValueFlowProof::Proven, .narrowed = family.withoutPrimitives(Primitive::Absent) };
    if (holdsUnknownValue)
        return { .proof = ValueFlowProof::IllTyped };
    if (holdsUntrackedObject && (!isProven || !m_inputs.universe().namesProgramObjectsAlone(program, family)))
        return { .proof = ValueFlowProof::Untracked };

    Type held = typeOf(set);
    if (numbers)
        numbers->narrow(held);
    if (deadZone == DeadZone::UndefinedAtStartIsNeverObserved)
        held.primitives &= ~Primitive::Undefined;
    if (deadZone == DeadZone::UndefinedAtStartMayBeObserved)
        held.primitives |= Primitive::Undefined;
    if (!isEnabled(FlowResultKind::RemovedNullish))
        held.primitives |= family.primitives & Primitive::UndefinedOrNull;
    if (deadZone != DeadZone::MayBeObserved)
        held.primitives &= ~Primitive::Absent;
    if (!isEnabled(FlowResultKind::Initialization))
        held.primitives |= family.primitives & Primitive::Absent;

    bool hasPassedTest = holdsInEveryVersion && node && m_inputs.solver().graph().maskKindOf(node) == ValueFlowMaskKind::Checked;
    bool isInside = hasPassedTest || (isProven ? holdsOnlyObjectsOf(held, family, program) : held.isSubsetOf(family, program));
    auto liesOutsideGuessedRangeAlone = [&] {
        if (held.isInteger() && family.isNumber())
            return true;
        if (!(held.primitives & Primitive::Integer) || !(family.primitives & Primitive::Integer))
            return false;
        Type withinGuessedRange = held;
        withinGuessedRange.integers = family.integers;
        return withinGuessedRange.isSubsetOf(family, program);
    };
    if (!isInside && !isProven && liesOutsideGuessedRangeAlone())
        return { .proof = ValueFlowProof::Proven, .narrowed = WTF::move(held) };
    if (!isInside) {
        ValueFlowObjectID stranger = findStranger(isProven ? maker().intersect(set, m_inputs.universe().objects()) : set, family);
        bool isUndecided = stranger && findLayoutsPickedBy(stranger) && typeOfObject(stranger).overlaps(family, program);
        if (!isUndecided) {
            m_counts.setsOutsideFamilies.fetch_add(1, std::memory_order_relaxed);
            if (m_counts.listener) {
                Locker locker { m_counts.lockOfListener };
                (*m_counts.listener)(placeOf(node), held, family);
            }
            return { .proof = ValueFlowProof::OutsideFamily, .stranger = stranger };
        }
        if (!holdsInEveryVersion)
            return { .proof = ValueFlowProof::UndecidedLayout, .stranger = stranger };
    }

    if (holdsUntrackedObject)
        held = held.unionWith(family.withOnlyObjects(), m_types);
    held = withinLimits(WTF::move(held));
    Type narrowed = held.intersectWith(family, m_types);
    if (narrowed.isNever() || (narrowed == family && !familyBehind))
        return { .proof = ValueFlowProof::Proven };
    bool isWhatFlowHolds = !holdsUntrackedObject && narrowed == held;
    return { .proof = ValueFlowProof::Proven, .narrowed = WTF::move(narrowed), .isWhatFlowHolds = isWhatFlowHolds };
}

ValueFlowPositionResult ValueFlowPositions::resultAt(ProvenType family, ValueSetID set, DeadZone deadZone, const ValueFlowNumbers* numbers)
{
    return decide(family, set, deadZone, numbers);
}

ValueFlowPositionResult ValueFlowPositions::resultAt(ExpectedType family, ValueSetID set, DeadZone deadZone)
{
    return decide(family, set, deadZone, nullptr);
}

ValueFlowPositionResult ValueFlowPositions::resultAt(const DeclaredType& declared, ValueSetID set, DeadZone deadZone)
{
    if (std::optional<ProvenInEveryVersion> established = declared.established())
        return resultAt(*established, set, deadZone);
    return resultAt(declared.expected(), set, deadZone);
}

FlowPositionUpdate ValueFlowPositions::updateFrom(const ValueFlowPositionResult& result, const Type& family)
{
    FlowPositionUpdate update;
    update.isProven = result.proof == ValueFlowProof::Proven;
    if (family.hasObjects() && result.proof != ValueFlowProof::NothingArrives)
        (update.isProven ? m_counts.provenPositions : m_counts.unprovenPositions).fetch_add(1, std::memory_order_relaxed);
    if (update.isProven && result.narrowed) {
        m_counts.narrowedPositions.fetch_add(1, std::memory_order_relaxed);
        update.type = m_types.intern(*result.narrowed);
    }
    return update;
}

FlowPositionUpdate ValueFlowPositions::updateFor(const DeclaredType& declared, ValueFlowNodeID node, DeadZone deadZone, const ValueFlowNumbers* numbers)
{
    const ValueFlowSolver& solver = m_inputs.solver();
    const Type& family = m_inputs.program.at(declared.written());
    std::optional<ProvenInEveryVersion> established = declared.established();
    ValueSetID possible = solver.possibleValuesOf(node);
    ValueFlowPositionResult result = established ? decide<ProvenType>(*established, possible, deadZone, numbers, node) : decide(declared.expected(), possible, deadZone, numbers, node);
    if (result.proof == ValueFlowProof::Proven && solver.holdsObjectOutsideFamily(node))
        result = { .proof = ValueFlowProof::OutsideFamily };

    FlowPositionUpdate update = updateFrom(result, family);
    if (update.isProven)
        return update;
    if (result.proof != ValueFlowProof::NothingArrives && result.proof != ValueFlowProof::Disabled && numbers && established) {
        Type narrowed = family;
        numbers->narrow(narrowed);
        if (narrowed != family) {
            m_counts.narrowedPositions.fetch_add(1, std::memory_order_relaxed);
            update.isProven = true;
            update.type = m_types.intern(narrowed);
            return update;
        }
    }
    FlowSiteLoss loss = lossOf(result, node);
    update.cause = loss.cause;
    update.witness = loss.witness;
    if (family.isJavaScriptValue() && family.primitives & Primitive::Absent && deadZone != DeadZone::MayBeObserved && isEnabled(FlowResultKind::Initialization))
        update.type = m_types.intern(family.withoutPrimitives(Primitive::Absent));
    return update;
}

void ValueFlowPositions::takeWhatArrives(const DeclaredType& declared, ValueFlowNodeID node, FlowPositionUpdate& update)
{
    const Type& expected = m_inputs.program.at(declared.expected().predicted());
    if (update.isProven || declared.site || !declared.isExpected() || !expected.layouts)
        return;
    ValueFlowPositionResult arriving = resultAt(ExpectedType(TypeTable::anything), m_inputs.solver().possibleValuesOf(node));
    if (arriving.proof != ValueFlowProof::Proven || !arriving.narrowed || arriving.narrowed->anyObject || arriving.narrowed->primitives & ~expected.primitives)
        return;
    update.type = m_types.intern(*arriving.narrowed);
    update.replacesExpectation = true;
}

std::optional<LayoutSet> ValueFlowPositions::receiversAt(ValueSetID set)
{
    const Program& program = m_inputs.program;
    if (ValueSets::isComplement(set) || m_inputs.sets().meet(set, m_inputs.universe().placeholdersAlone()))
        return std::nullopt;

    Type held = typeOf(set);
    if (held.anyObject || held.anyFunction || !held.classes.isEmpty() || !held.functions.isEmpty() || !held.builtins.isEmpty())
        return std::nullopt;
    LayoutSet receivers = LayoutSet::complete(Vector<LayoutID>(membersOf(program, held)));
    canonicalize(receivers);
    return receivers;
}

ValueSetID ValueFlowPositions::withoutWhatPlaceholdersCover(ValueSetID set)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    auto leaveOut = [&](ValueFlowObjectID placeholder, ValueSetID placeholderAlone, ValueFlowNodeID covered) {
        if (holds(set, placeholder))
            set = maker().unite(maker().intersect(set, ValueSets::complement(m_inputs.solver().possibleValuesOf(covered))), placeholderAlone);
    };
    leaveOut(ValueFlowUniverse::unknownObject(), universe.unknownObjectAlone(), ValueFlowProgramNodes::escaped());
    leaveOut(ValueFlowUniverse::untrackedObject(), universe.untrackedObjectAlone(), ValueFlowProgramNodes::lost());
    return set;
}

std::optional<CalleeSet> ValueFlowPositions::calleesAt(ValueSetID possible, ValueSetID seen, const ValueFlowOperand* calleeOfConstruction)
{
    if (ValueSets::isComplement(possible) || ValueSets::isComplement(seen))
        return std::nullopt;

    const Program& program = m_inputs.program;
    const ValueFlowUniverse& universe = m_inputs.universe();
    bool mayBeAnyCallee = m_inputs.sets().meet(possible, universe.placeholdersAlone());
    ValueSetID arriving = mayBeAnyCallee ? seen : possible;
    ValueSetID listed = withoutWhatPlaceholdersCover(arriving);
    bool isConstruction = !!calleeOfConstruction;
    if (isConstruction && listed != arriving) {
        ValueSetID followed = calleeOfConstruction->constant ? calleeOfConstruction->constant : ValueSets::nothing;
        if (calleeOfConstruction->node)
            followed = maker().unite(followed, m_inputs.solver().closuresFollowedToConstruction(calleeOfConstruction->node));
        if (m_inputs.sets().size(followed) <= m_inputs.build.limit(LimitID::MaxValueFlowMembersToldApart))
            listed = maker().unite(listed, maker().intersect(arriving, followed));
    }

    Vector<FunctionID> functions;
    Vector<BuiltinID> builtins;
    bool isComplete = !mayBeAnyCallee;
    m_inputs.sets().forEachMember(listed, [&](ValueFlowObjectID member) {
        if (!universe.isObject(member))
            return;
        if (FunctionID function = universe.closureFunction(member)) {
            const Function& callee = program.at(function);
            bool throwsInFrontOfBody = isConstruction ? callee.constructorForm() == ConstructorForm::NotConstructor : callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor;
            if (!throwsInFrontOfBody)
                functions.append(function);
        } else if (holds(universe.builtinFunctions(), member))
            builtins.append(program.builtins().canonicalID(universe.builtinOf(member)));
        else
            isComplete = false;
    });
    CalleeSet callees = isComplete ? CalleeSet::complete(WTF::move(functions), WTF::move(builtins)) : CalleeSet::open(WTF::move(functions), WTF::move(builtins));
    canonicalize(callees);
    return callees;
}

bool isDataFromOutside(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const BuiltinTable& builtins = inputs.program.builtins();
    if (object == universe.resultOf(Builtins::JSONParse, 0) || object == universe.resultOf(Builtins::JSONParse, 1))
        return true;
    const ValueFlowSite* site = universe.isObject(object) ? universe.siteOf(object) : nullptr;
    FunctionID function = site ? universe.functionOf(object) : FunctionID();
    if (!function || site->origin != ValueFlowSiteOrigin::BuiltinResult || !site->value)
        return false;
    bool answersPlainData = false;
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(function.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.site != site->value)
            continue;
        std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
        ValueSetID callees = !operands.empty() && operands[0].node ? inputs.solver().possibleValuesOf(operands[0].node) : ValueSets::nothing;
        inputs.sets().forEachMember(ValueSets::isComplement(callees) ? ValueSets::nothing : callees, [&](ValueFlowObjectID callee) {
            BuiltinID builtin = universe.isObject(callee) ? universe.builtinOf(callee) : BuiltinID();
            if (!builtin)
                return;
            for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(builtin)))
                answersPlainData |= overload.result.type && builtins.at(overload.result.type).flags.contains(BuiltinTypeFlag::HasPlainData);
        });
    }
    return answersPlainData;
}

FlowSiteLoss ValueFlowPositions::lossOf(const ValueFlowPositionResult& result, ValueFlowNodeID node)
{
    Witness witnessOfStranger = result.stranger && m_inputs.universe().isObject(result.stranger) ? witnessOfAllocation(m_inputs, result.stranger) : noWitness;
    switch (result.proof) {
    case ValueFlowProof::Proven:
    case ValueFlowProof::NothingArrives:
        return { };
    case ValueFlowProof::Disabled:
        return { .cause = UnprovenCause::ResultDisabled };
    case ValueFlowProof::IllTyped: {
        FlowSiteLoss loss { .cause = UnprovenCause::UnknownObject };
        if (!node)
            return loss;
        if (std::optional<ValueFlowUnknownSource> source = m_inputs.solver().unknownSourceOf(node))
            loss.cause = unprovenCauseOf(*source);
        if (m_inputs.solver().input().remembersWhoLoggedEachEdge) {
            std::call_once(m_counts.hasFoundBirths, [&] {
                m_counts.births.emplace(m_inputs);
            });
            loss.witness = m_counts.births->witnessOf(node);
            loss.hasOtherRoots = m_counts.births->hasSeveralRoots(node);
            if (std::optional<ValueFlowUnknownSource> source = m_counts.births->sourceOf(node))
                loss.cause = unprovenCauseOf(*source);
        }
        return loss;
    }
    case ValueFlowProof::Untracked:
        return { .cause = UnprovenCause::Untracked };
    case ValueFlowProof::OutsideFamily:
        return { .cause = result.stranger && isDataFromOutside(m_inputs, result.stranger) ? UnprovenCause::DataFromOutside : UnprovenCause::LayoutOutsideFamily, .witness = witnessOfStranger };
    case ValueFlowProof::UndecidedLayout:
        return { .cause = UnprovenCause::OtherNewTarget, .witness = witnessOfStranger };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String ValueFlowPositions::placeOf(ValueFlowNodeID node) const
{
    const Program& program = m_inputs.program;
    const ValueFlowProgramNodes& nodes = m_inputs.bodies().nodes();
    if (!node)
        return makeString("subject "_s, m_switchSubject);
    if (FunctionID function = nodes.functionOf(node); function && function.index() < program.functions.size()) {
        uint32_t firstResult = nodes.result(function, 0).index();
        if (node.index() < firstResult)
            return makeString('f', function.index(), " entry parameter "_s, node.index() - nodes.firstNodeOf(function));
        return makeString('f', function.index(), " result "_s, node.index() - firstResult);
    }
    uint32_t firstVariable = nodes.variable(VariableID(0u)).index();
    if (node.index() >= firstVariable && node.index() - firstVariable < program.variables.size())
        return makeString('g', node.index() - firstVariable);
    for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
        uint32_t firstField = nodes.field(LayoutID(layout), 0).index();
        if (node.index() >= firstField && node.index() - firstField < program.layouts[layout].fields.size())
            return makeString('L', layout, '.', node.index() - firstField);
    }
    return makeString("node "_s, node.index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
