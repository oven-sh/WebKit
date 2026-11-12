/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include <algorithm>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

void appendMembersAssignedByProgram(const Program& program, const LinkTables& tables, LayoutID layout, Vector<ClassMember>& members)
{
    for (const LinkedMember& above : std::ranges::equal_range(tables.membersAboveCreatedLayouts, layout, { }, &LinkedMember::classLayout)) {
        ClassMember member;
        member.key = above.key;
        member.side = above.side;
        member.kind = above.kind;
        member.function = above.function;
        member.isAssignedByProgram = true;
        members.append(member);
    }
    if (!program.at(layout).has(LayoutFlags::IsConstructorFunction))
        return;
    auto appendMethod = [&](ClassSide side, PropertyKeyID key, FunctionID function) {
        ClassMember member;
        member.key = key;
        member.side = side;
        member.kind = MethodKind::Method;
        member.function = function;
        member.isAssignedByProgram = true;
        members.append(member);
    };
    for (const LinkedNameOnPrototype& name : std::ranges::equal_range(tables.namesOnPrototypes, layout, { }, &LinkedNameOnPrototype::layout)) {
        if (!name.key)
            continue;
        for (auto [kind, function] : { std::pair { MethodKind::Getter, name.expectedGetter }, std::pair { MethodKind::Setter, name.expectedSetter } }) {
            if (!function)
                continue;
            ClassMember half;
            half.key = name.key;
            half.side = ClassSide::Prototype;
            half.kind = kind;
            half.function = function;
            half.isAssignedByProgram = true;
            members.append(half);
        }
        if (name.expected)
            appendMethod(ClassSide::Prototype, name.key, name.expected);
    }
    if (classOfConstructorFunction(program, program.at(layout).constructor) != layout)
        return;
    for (const LinkedNameOnFunction& name : std::ranges::equal_range(tables.namesOnFunctions, program.at(layout).constructor, { }, &LinkedNameOnFunction::holder)) {
        if (name.expected)
            appendMethod(ClassSide::Static, name.key, name.expected);
    }
}

static uint64_t wordOfNameInLinkPrototypes(LayoutID layout, PropertyKeyID key)
{
    return static_cast<uint64_t>(layout.index()) << 32 | key.bits();
}

void LinkFlow::indexConstructorFunctions()
{
    indexRootsOfClassRows();
    size_t numberOfLayouts = program.layouts.size();
    for (BitVector* byLayout : { &m_layoutsOfConstructorFunctions, &m_layoutsBelowConstructorFunctions, &m_layoutsWithFollowedPrototypes, &m_layoutsBelowFollowedPrototypes, &m_prototypesPassedByLookups, &m_prototypesWithDefinitions, &m_prototypesAboveLostLayouts, &m_prototypesPassedByNumbers })
        byLayout->ensureSize(numberOfLayouts);
    m_nearestConstructorFunctionsFrom.fill(LayoutID(), numberOfLayouts);
    auto noteConstructorFunctionAbove = [&](LayoutID below, LayoutID above) {
        LayoutID& nearest = m_nearestConstructorFunctionsFrom[below.index()];
        if (!nearest || program.layoutsExpectedBelow(above).size() < program.layoutsExpectedBelow(nearest).size())
            nearest = above;
    };

    for (size_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.has(LayoutFlags::IsConstructorFunction) || !layout.constructor)
            continue;
        const Function& constructor = program.at(layout.constructor);
        if (constructor.constructedClass != LayoutID(index) || !constructor.takesThisFromConstruction())
            continue;
        m_layoutsOfConstructorFunctions.quickSet(index);
        m_layoutsBelowConstructorFunctions.quickSet(index);
        noteConstructorFunctionAbove(LayoutID(index), LayoutID(index));
        for (LayoutID below : program.layoutsExpectedBelow(LayoutID(index))) {
            m_layoutsBelowConstructorFunctions.quickSet(below.index());
            noteConstructorFunctionAbove(below, LayoutID(index));
        }
    }

    m_prototypeHelperCandidates.ensureSize(program.functions.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const ThinSummary& summary = summaries.at(FunctionID(index));
        auto isOfParameter = [&](uint32_t node) {
            return node != noSummaryNode && summary.nodes[node].kind == SummaryNode::Kind::Parameter;
        };
        bool laysPrototype = std::ranges::any_of(summary.accesses, [&](const SummaryAccess& access) {
            return access.kind == SummaryAccess::Kind::Store && access.key == program.keyOf(BuiltinNames::prototype) && isOfParameter(access.subject);
        });
        if (laysPrototype || std::ranges::contains(summary.changes, SummaryChange::Kind::ChangesPrototype, &SummaryChange::kind))
            m_prototypeHelperCandidates.quickSet(index);
    }

    BitVector classesWithFollowedPrototypes = findClassesWithFollowedPrototypes();
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        if (!m_layoutsOfConstructorFunctions.quickGet(index) && !classesWithFollowedPrototypes.quickGet(index))
            continue;
        m_layoutsWithFollowedPrototypes.quickSet(index);
        m_layoutsBelowFollowedPrototypes.quickSet(index);
        for (LayoutID below : program.layoutsExpectedBelow(LayoutID(index)))
            m_layoutsBelowFollowedPrototypes.quickSet(below.index());
    }
}

static FunctionID functionNamedAtInLinkPrototypes(const Program& program, FunctionID asker, const SummaryNode& node)
{
    auto functionOf = [&](TypeID written) {
        if (!written || holdsOnlyPrimitives(written))
            return FunctionID();
        Type type = program.at(written);
        type.primitives &= ~Primitive::Absent;
        return !type.primitives && type.isJavaScriptValue() ? singleFunctionOf(TypeReader(program), type) : FunctionID();
    };
    switch (node.kind) {
    case SummaryNode::Kind::Closure:
        return FunctionID(node.first);
    case SummaryNode::Kind::Variable:
        return functionOf(program.provenTypeOf(program.variables[node.first].declared));
    case SummaryNode::Kind::Capture:
        return functionOf(program.provenTypeOf(program.at(node.second ? FunctionID(node.second - 1) : asker).captures[node.first]));
    default:
        return { };
    }
}

static LayoutID classNamedAtInLinkPrototypes(const Program& program, FunctionID asker, const SummaryNode& node)
{
    FunctionID constructor = functionNamedAtInLinkPrototypes(program, asker, node);
    LayoutID constructed = constructor && isClassConstructorOf(program.at(constructor).kind) ? program.at(constructor).constructedClass : LayoutID();
    bool isClass = constructed && program.at(constructed).kind == LayoutKind::Instance && program.at(constructed).constructor == constructor && !program.at(constructed).has(LayoutFlags::IsConstructorFunction);
    return isClass ? constructed : LayoutID();
}

static LayoutID classReadAtInLinkPrototypes(const Program& program, const ThinSummary& summary, FunctionID asker, uint32_t node)
{
    if (summary.nodes[node].kind != SummaryNode::Kind::Join)
        return classNamedAtInLinkPrototypes(program, asker, summary.nodes[node]);
    LayoutID read;
    for (const SummaryEdge& edge : summary.edges) {
        if (edge.to != node)
            continue;
        if (read)
            return { };
        read = classNamedAtInLinkPrototypes(program, asker, summary.nodes[edge.from]);
        if (!read)
            return { };
    }
    return read;
}

LayoutID LinkFlow::findClassNamedAt(FunctionID asker, const SummaryNode& node) const
{
    return classNamedAtInLinkPrototypes(program, asker, node);
}

bool LinkFlow::instancesMayHoldName(LayoutID layout, PropertyKeyID key) const
{
    return program.at(layout).findField(key) || std::ranges::any_of(program.layoutsExpectedBelow(layout), [&](LayoutID below) {
        return !!program.at(below).findField(key);
    });
}

BitVector LinkFlow::findClassesWithFollowedPrototypes()
{
    PropertyKeyID prototype = program.keyOf(BuiltinNames::prototype);
    Vector<std::pair<LayoutID, PropertyKeyID>, 8> namesOnPrototypes;
    HashMap<uint32_t, Vector<uint32_t, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> targetsOfMixins;
    Vector<FunctionID, 8> readersOfPrototypes;
    auto noteTargetOfMixin = [&](FunctionID function, const SummaryNode& target) {
        if (target.kind == SummaryNode::Kind::Parameter)
            targetsOfMixins.add(function.index(), Vector<uint32_t, 1>()).iterator->value.appendIfNotContains(target.first);
    };
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (isLibraryCode(program, program.functions[index]))
            continue;
        FunctionID function(index);
        const ThinSummary& summary = summaries.at(function);
        auto prototypeReadAt = [&](const SummaryNode& object) -> const SummaryAccess* {
            const SummaryAccess* read = object.kind == SummaryNode::Kind::ResultOfAccess ? &summary.accesses[object.first] : nullptr;
            return read && read->kind == SummaryAccess::Kind::Read && read->key == prototype && read->subject != noSummaryNode ? read : nullptr;
        };
        for (const SummaryAccess& access : summary.accesses) {
            bool isWrite = access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define;
            if (access.kind == SummaryAccess::Kind::Read && access.key == prototype && (readersOfPrototypes.isEmpty() || readersOfPrototypes.last() != function))
                readersOfPrototypes.append(function);
            if (!isWrite || access.subject == noSummaryNode)
                continue;
            const SummaryNode& object = summary.nodes[access.subject];
            if (!access.key && !access.isCopySpelledByLoop) {
                const SummaryAccess* read = access.kind == SummaryAccess::Kind::Store ? prototypeReadAt(object) : nullptr;
                if (LayoutID written = read ? classReadAtInLinkPrototypes(program, summary, function, read->subject) : LayoutID()) {
                    for (PropertyKeyID key : findComputedStoreNames(function, access.site).names)
                        namesOnPrototypes.append({ written, key });
                    if (storesUnderListedNames(function, access.site))
                        namesOnPrototypes.append({ written, PropertyKeyID() });
                }
                continue;
            }
            if (const SummaryAccess* read = prototypeReadAt(object)) {
                if (LayoutID written = classNamedAtInLinkPrototypes(program, function, summary.nodes[read->subject]))
                    namesOnPrototypes.append({ written, access.key });
                noteTargetOfMixin(function, summary.nodes[read->subject]);
            } else if (access.key && !classNamedAtInLinkPrototypes(program, function, object))
                m_namesStoredOnObjects.set(access.key.index());
        }
        for (const SummaryChange& change : summary.changes) {
            if (change.kind != SummaryChange::Kind::CopiesInto || change.definesWhatItCopies)
                continue;
            if (const SummaryAccess* read = prototypeReadAt(summary.nodes[change.subject])) {
                if (LayoutID written = classNamedAtInLinkPrototypes(program, function, summary.nodes[read->subject]))
                    namesOnPrototypes.append({ written, PropertyKeyID() });
                noteTargetOfMixin(function, summary.nodes[read->subject]);
            }
        }
    }
    for (uint32_t index = 0; !targetsOfMixins.isEmpty() && index < program.functions.size(); ++index) {
        FunctionID function(index);
        if (isLibraryCode(program, program.functions[index]))
            continue;
        const ThinSummary& summary = summaries.at(function);
        for (const SummaryCall& call : summary.calls) {
            FunctionID callee;
            if (call.callee == SummaryCall::Callee::Binding)
                callee = call.function;
            else if (call.callee == SummaryCall::Callee::Value && call.subject != noSummaryNode)
                callee = functionNamedAtInLinkPrototypes(program, function, summary.nodes[call.subject]);
            auto target = callee && !call.isConstruction && !call.passesList ? targetsOfMixins.find(callee.index()) : targetsOfMixins.end();
            if (target == targetsOfMixins.end())
                continue;
            uint32_t firstPassed = call.callee == SummaryCall::Callee::Binding ? 0 : program.at(callee).firstParameterIndex();
            for (uint32_t parameter : target->value) {
                if (parameter < firstPassed || parameter - firstPassed >= call.numberOfArguments)
                    continue;
                if (LayoutID written = classNamedAtInLinkPrototypes(program, function, summary.nodes[summary.arguments[call.firstArgument + parameter - firstPassed]]))
                    namesOnPrototypes.append({ written, PropertyKeyID() });
            }
        }
    }
    for (FunctionID function : readersOfPrototypes) {
        const ThinSummary& summary = summaries.at(function);
        for (const SummaryCall& call : summary.calls) {
            if (call.callee != SummaryCall::Callee::Value || call.subject == noSummaryNode || call.isConstruction || call.passesList)
                continue;
            for (uint32_t place = 0; place < call.numberOfArguments; ++place) {
                const SummaryNode& passed = summary.nodes[summary.arguments[call.firstArgument + place]];
                const SummaryAccess* read = passed.kind == SummaryNode::Kind::ResultOfAccess ? &summary.accesses[passed.first] : nullptr;
                if (!read || read->kind != SummaryAccess::Kind::Read || read->key != prototype || read->subject == noSummaryNode)
                    continue;
                LayoutID written = classReadAtInLinkPrototypes(program, summary, function, read->subject);
                FunctionID callee = written ? functionNamedAtInLinkPrototypes(program, function, summary.nodes[call.subject]) : FunctionID();
                if (!callee || isLibraryCode(program, program.at(callee)))
                    continue;
                uint32_t parameter = program.at(callee).firstParameterIndex() + place;
                const ThinSummary& called = summaries.at(callee);
                for (const SummaryAccess& access : called.accesses) {
                    if (access.kind != SummaryAccess::Kind::Store || access.subject == noSummaryNode || access.isCopySpelledByLoop)
                        continue;
                    const SummaryNode& target = called.nodes[access.subject];
                    if (target.kind != SummaryNode::Kind::Parameter || target.first != parameter)
                        continue;
                    if (access.key)
                        namesOnPrototypes.append({ written, access.key });
                    else {
                        for (PropertyKeyID key : findComputedStoreNames(callee, access.site).names)
                            namesOnPrototypes.append({ written, key });
                    }
                }
            }
        }
    }
    BitVector classes(program.layouts.size());
    for (auto [layout, key] : namesOnPrototypes) {
        if (!key || !instancesMayHoldName(layout, key))
            classes.quickSet(layout.index());
    }
    return classes;
}

LayoutID LinkFlow::layoutOfConstructorFunction(FunctionID function) const
{
    LayoutID constructed = program.at(function).constructedClass;
    return constructed && isOfConstructorFunction(constructed) && program.at(constructed).constructor == function ? constructed : LayoutID();
}

uint32_t LinkFlow::firstNameOnPrototypeOf(LayoutID layout) const
{
    auto found = m_namesOfPrototypes.find(layout.index());
    return found == m_namesOfPrototypes.end() ? UINT32_MAX : found->value.first;
}

const LinkFlow::NameOnPrototype* LinkFlow::findNameOnPrototype(LayoutID layout, PropertyKeyID key) const
{
    auto found = m_recordsOfNamesOnPrototypes.find(wordOfNameInLinkPrototypes(layout, key));
    return found == m_recordsOfNamesOnPrototypes.end() ? nullptr : &m_namesOnPrototypes[found->value];
}

bool LinkFlow::isDefinedByDescriptorSomewhere(PropertyKeyID key) const
{
    if (!key || liesInThread(key))
        return false;
    return m_keysDefinedByDescriptors.get(key.index()) || (m_someAccessorIsWrittenUnderComputedKey && program.at(key).kind != PropertyKeyKind::Text);
}

bool LinkFlow::mayBeDefinedUnderUnknownKeyOnPrototypeOf(LayoutID layout, PropertyKeyID key) const
{
    const NameOnPrototype* underSomeKey = findNameOnPrototype(layout, { });
    if (!underSomeKey)
        return false;
    return underSomeKey->isDefinedByDescriptor || (underSomeKey->isDefinedUnderSomeSymbol && (!key || program.at(key).kind != PropertyKeyKind::Text));
}

bool LinkFlow::hasOnlyPrimitives(const NameOnPrototype& name) const
{
    if (!name.key || !name.isStoredInto || name.mayHoldAccessor() || escapedLayouts.get(name.layout.index()))
        return false;
    const LinkType& stored = typeAt(name.stored);
    return stored.objects == LinkType::Objects::None && stored.functions == LinkType::Functions::None && stored.classes == LinkType::Classes::None
        && !stored.builtin && !stored.hasUnknownObjects && !stored.holdsLostBuiltins;
}

bool LinkFlow::hasNoFunctionsInSight(const NameOnPrototype& name) const
{
    if (!name.key || !name.isStoredInto || name.mayHoldAccessor())
        return false;
    const LinkType& stored = typeAt(name.storedInSight);
    return (stored.objects != LinkType::Objects::None || stored.classes != LinkType::Classes::None) && stored.functions == LinkType::Functions::None && !stored.builtin && !stored.hasDirectUnknownObjects() && !stored.holdsLostBuiltins && stored.objects != LinkType::Objects::Lost;
}

bool LinkFlow::mayLieOnPrototypeAbove(LayoutID layout, PropertyKeyID key, bool countsKeysThatNobodyKnows, PrimitivesOnPrototypes primitives) const
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!isBelowFollowedPrototype(layout))
        return false;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!followsNamesOnPrototypeOf(current))
            continue;
        const NameOnPrototype* underKey = findNameOnPrototype(current, key);
        bool isIgnored = underKey && primitives != PrimitivesOnPrototypes::Count && (hasOnlyPrimitives(*underKey) || (primitives == PrimitivesOnPrototypes::AreIgnoredWithObjects && hasNoFunctionsInSight(*underKey)));
        if (underKey && !isIgnored && (underKey->isStoredInto || underKey->mayHoldAccessor()))
            return true;
        if (!countsKeysThatNobodyKnows || !m_prototypesWithDefinitions.get(current.index()))
            continue;
        if (mayBeDefinedUnderUnknownKeyOnPrototypeOf(current, key))
            return true;
    }
    return false;
}

static void openHalvesOfAccessorInLinkPrototypes(LinkFlow& flow, LinkFlow::NameOnPrototype name)
{
    for (FlowNodeID half : { name.getter, name.setter }) {
        if (!half)
            continue;
        flow.arrive(half, LinkType::anything().asEscapeOnly());
        flow.addEdge(half, flow.unseen());
    }
}

bool LinkFlow::isPooledNameOnPrototype(uint32_t record) const
{
    const NameOnPrototype& name = m_namesOnPrototypes[record];
    return name.key && (m_prototypesAboveLostLayouts.get(name.layout.index()) || mayBeNamedByNumber(name.key));
}

void LinkFlow::poolNameOnPrototype(uint32_t record)
{
    NameOnPrototype name = m_namesOnPrototypes[record];
    addEdge(name.mayHoldAccessor() ? name.stored : name.storedInSight, foundBesideFieldsUnder(name.key));
}

uint32_t LinkFlow::recordOfNameOnPrototype(LayoutID layout, PropertyKeyID key)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    RELEASE_ASSERT(followsNamesOnPrototypeOf(layout));
    uint32_t record = static_cast<uint32_t>(m_namesOnPrototypes.size());
    auto added = m_recordsOfNamesOnPrototypes.add(wordOfNameInLinkPrototypes(layout, key), record);
    if (!added.isNewEntry)
        return added.iterator->value;

    FlowNodeID stored = makeNode();
    FlowNodeID storedInSight = makeNode();
    m_namesOnPrototypes.append(NameOnPrototype { layout, key, stored, storedInSight });
    if (isDefinedByDescriptorSomewhere(key)) {
        m_namesOnPrototypes[record].getter = makeNode();
        m_namesOnPrototypes[record].setter = makeNode();
    }
    auto namesOfLayout = m_namesOfPrototypes.add(layout.index(), NamesOfPrototype { record, record });
    if (!namesOfLayout.isNewEntry) {
        m_namesOnPrototypes[namesOfLayout.iterator->value.last].nextOfLayout = record;
        namesOfLayout.iterator->value.last = record;
    }
    addEdge(storedInSight, stored);
    if (key) {
        if (const NameOnPrototype* underAnyKey = findNameOnPrototype(layout, { }); underAnyKey && underAnyKey->isReadUnderAnyKey)
            addEdge(stored, underAnyKey->stored);
        if (isPooledNameOnPrototype(record))
            poolNameOnPrototype(record);
    }
    if (escapedLayouts.get(layout.index())) {
        arrive(stored, LinkType::anything().asEscapeOnly());
        addEdge(stored, unseen());
        openHalvesOfAccessorInLinkPrototypes(*this, m_namesOnPrototypes[record]);
    }
    return record;
}

void LinkFlow::noteInstancesLaidAsPrototype(const LinkType& laid)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    forEachLayoutOf(laid, [&](LayoutID layout) {
        layoutsWhoseInstancesAreLaidAsPrototypes.quickSet(layout.index());
    });
}

void LinkFlow::addConstructedPrototype(FunctionID function, LayoutID below, FlowNodeID instance)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    for (const ConstructedPrototype& listed : m_constructedPrototypes) {
        if (listed.function == function && listed.instance == instance)
            return;
    }
    m_constructedPrototypes.append({ function, below, instance, { }, false });
}

void LinkFlow::notePrototypeIsReplaced(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    m_layoutsWithReplacedPrototypes.quickSet(layout.index());
}

bool LinkFlow::prototypeKeepsConstructor(LayoutID made) const
{
    PropertyKeyID key = program.keyOf(BuiltinNames::constructor);
    if (!key || !isOfConstructorFunction(made) || escapedLayouts.get(made.index()) || lostLayouts.get(made.index()) || lostFunctions.get(program.at(made).constructor.index()))
        return false;
    if (m_layoutsWithReplacedPrototypes.get(made.index()) || namesMayBeIncomplete(made, ClassSide::Prototype))
        return false;
    if (findField(program, made, key) || findMembers(made, ClassSide::Prototype, key).mayAnswer() || mayHoldAccessorOnPrototype(made, key))
        return false;
    auto isStored = [&](PropertyKeyID stored) {
        const NameOnPrototype* name = findNameOnPrototype(made, stored);
        return name && (name->isStoredInto || name->isDefinedInSight);
    };
    return !isStored(key) && !isStored({ });
}

bool LinkFlow::isConstructedPrototype(LayoutID layout) const
{
    return std::ranges::contains(m_constructedPrototypes, layout, &ConstructedPrototype::parent);
}

LayoutID LinkFlow::findConstructedParent(const ConstructedPrototype& constructed) const
{
    const LinkType& type = typeAt(constructed.instance);
    LinkType instance;
    instance.objects = LinkType::Objects::Layout;
    instance.layout = type.layout;
    if (type.objects != LinkType::Objects::Layout || !(type == instance) || type.layout == constructed.below)
        return { };
    LayoutID parent = type.layout;
    if (!isOfConstructorFunction(parent) || escapedLayouts.get(parent.index()) || escapedLayouts.get(constructed.below.index()) || lostLayouts.get(parent.index()))
        return { };
    LinkType prototype;
    prototype.objects = LinkType::Objects::Prototype;
    prototype.layout = parent;
    return constructionKeepsThis(parent) && namesParentOfPrototype(prototype, constructed.below) ? parent : LayoutID();
}

bool LinkFlow::layConstructedPrototypes()
{
    bool hasLaid = false;
    for (size_t index = 0; index < m_constructedPrototypes.size(); ++index) {
        ConstructedPrototype constructed = m_constructedPrototypes[index];
        LinkType type = typeAt(constructed.instance);
        if (type.isNever())
            continue;
        if (constructed.isAbandoned) {
            noteInstancesLaidAsPrototype(type);
            continue;
        }
        LayoutID parent = findConstructedParent(constructed);
        if (parent && parent == constructed.parent)
            continue;
        hasLaid = true;
        noteInstancesLaidAsPrototype(type);
        if (!parent) {
            m_constructedPrototypes[index].isAbandoned = true;
            note(Note::ChangeOfLayout, constructed.below.index(), static_cast<uint32_t>(LayoutChange::ChangesPrototype));
            addEdge(constructed.instance, unseen());
            deferEscape(constructed.function);
            deferEscape(constructed.below);
            continue;
        }
        m_constructedPrototypes[index].parent = parent;
        const Layout& layout = program.at(parent);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            PropertyKeyID key = layout.fields[field].key;
            if (!key)
                continue;
            addEdge(this->field({ parent, field }), storedOnPrototypeOf(constructed.below, key, false, false));
            addEdge(findNameOnPrototype(constructed.below, key)->stored, this->field({ parent, field }));
        }
    }
    if (hasLaid)
        settle();
    return hasLaid;
}

FlowNodeID LinkFlow::storedOnPrototypeOf(LayoutID layout, PropertyKeyID key, bool isLaidByBodyOfFile, bool isDefinition)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    RELEASE_ASSERT(key);
    uint32_t record = recordOfNameOnPrototype(layout, key);
    if (!isLaidByBodyOfFile)
        m_namesOnPrototypes[record].isStoredIntoFromOutside = true;
    if (isDefinition)
        m_namesOnPrototypes[record].isDefinedInSight = true;
    if (!std::exchange(m_namesOnPrototypes[record].isStoredInto, true)) {
        auto noteFieldBelow = [&](LayoutID below) {
            if (std::optional<uint32_t> field = program.at(below).findField(key))
                noteFieldMayBeEmpty({ below, *field });
        };
        noteFieldBelow(layout);
        for (LayoutID below : program.layoutsExpectedBelow(layout))
            noteFieldBelow(below);
    }
    return m_namesOnPrototypes[record].storedInSight;
}

void LinkFlow::forEachPrototypePassedAbove(LayoutID layout, PropertyKeyID key, const Invocable<void(uint32_t record)> auto& functor)
{
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (followsNamesOnPrototypeOf(current)) {
            uint32_t record = recordOfNameOnPrototype(current, key);
            m_namesOnPrototypes[record].isPassedByLookups = true;
            m_prototypesPassedByLookups.set(current.index());
            functor(record);

            bool mayFindAccessor = m_prototypesWithDefinitions.get(current.index());
            if (mayFindAccessor && key)
                mayFindAccessor = m_namesOnPrototypes[record].isDefinedByDescriptor || mayBeDefinedUnderUnknownKeyOnPrototypeOf(current, key);
            if (mayFindAccessor)
                deferEscape(current);
            for (uint32_t other = mayFindAccessor && !key ? firstNameOnPrototypeOf(current) : UINT32_MAX; other != UINT32_MAX; other = m_namesOnPrototypes[other].nextOfLayout)
                letAccessorOnPrototypeEscape(other);
        }
        if (key && !std::ranges::equal_range(membersOf(current, ClassSide::Prototype), key, { }, &MemberOfClass::key).empty())
            break;
    }
    settle();
}

void LinkFlow::findKeysDefinedByDescriptors()
{
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        for (const SummaryChange& change : summaries.at(FunctionID(index)).changes) {
            if (change.laysAccessorAsWritten && change.key && !liesInThread(change.key))
                m_keysDefinedByDescriptors.set(change.key.index());
            m_someAccessorIsWrittenUnderComputedKey |= change.laysAccessorAsWritten && !change.key;
        }
    }
}

bool LinkFlow::letAccessorOnPrototypeEscape(uint32_t record)
{
    NameOnPrototype name = m_namesOnPrototypes[record];
    if (!name.isAccessorAsWritten)
        return false;
    addEdge(name.getter, unseen());
    addEdge(name.setter, unseen());
    return deferEscape(name.layout);
}

void LinkFlow::letAccessorsOnPrototypesAboveEscape(LayoutID layout, PropertyKeyID key)
{
    if (!isDefinedByDescriptorSomewhere(key) || !m_layoutsBelowFollowedPrototypes.get(layout.index()))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!followsNamesOnPrototypeOf(current))
            continue;
        uint32_t record = recordOfNameOnPrototype(current, key);
        m_namesOnPrototypes[record].isPassedByLookupsThatEnterNoAccessor = true;
        letAccessorOnPrototypeEscape(record);
    }
    settle();
}

Vector<FlowNodeID, 2> LinkFlow::halvesOfAccessorsOnPrototypesAbove(LayoutID layout, PropertyKeyID key, bool ofSetters)
{
    Vector<FlowNodeID, 2> halves;
    if (!isDefinedByDescriptorSomewhere(key) || !m_layoutsBelowFollowedPrototypes.get(layout.index()))
        return halves;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (followsNamesOnPrototypeOf(current)) {
            uint32_t record = recordOfNameOnPrototype(current, key);
            halves.append(ofSetters ? m_namesOnPrototypes[record].setter : m_namesOnPrototypes[record].getter);
        }
        if (!std::ranges::equal_range(membersOf(current, ClassSide::Prototype), key, { }, &MemberOfClass::key).empty())
            break;
    }
    return halves;
}

void LinkFlow::noteConstructionsMayLeaveFieldEmpty(FieldReference reference)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    noteFieldMayBeEmpty(reference);
    for (FieldReference listed : { reference, declarationOf(program, reference) })
        fieldsNotOfBirth.set(m_firstFieldOfLayout[listed.layout.index()] + listed.field);
}

void LinkFlow::noteFieldsThatAccessorAboveMayLeaveEmpty(LayoutID layout, PropertyKeyID key)
{
    auto noteFieldsOf = [&](LayoutID below) {
        const Vector<PropertyKeyID>& lookedUpByConstructors = m_listsOfKeysThatRowsOfConstructorsFill[m_keysThatRowsOfConstructorsFill[below.index()].lookedUpUntilFirstSight];
        if (!std::ranges::binary_search(lookedUpByConstructors, key)) {
            if (std::optional<FieldReference> field = findField(program, below, key))
                noteConstructionsMayLeaveFieldEmpty(*field);
            return;
        }
        if (m_layoutsWhoseConstructorsMeetAccessors.set(below.index()))
            return;
        size_t stepsLeft = program.layouts.size();
        for (LayoutID current = below; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
            const Layout& listing = program.at(current);
            for (uint32_t field = 0; field < listing.fields.size(); ++field)
                noteConstructionsMayLeaveFieldEmpty({ current, field });
            if (listing.inheritedFields == InheritedFields::AllOfRow)
                break;
        }
    };
    noteFieldsOf(layout);
    for (LayoutID below : program.layoutsExpectedBelow(layout))
        noteFieldsOf(below);
}

void LinkFlow::layAccessorOnPrototypeOf(LayoutID layout, PropertyKeyID key, FunctionID getter, FunctionID setter)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    uint32_t record = recordOfNameOnPrototype(layout, key);
    if (!std::exchange(m_namesOnPrototypes[record].isAccessorAsWritten, true)) {
        noteFieldsThatAccessorAboveMayLeaveEmpty(layout, key);
        if (isPooledNameOnPrototype(record))
            poolNameOnPrototype(record);
    }
    m_prototypesWithDefinitions.set(layout.index());
    NameOnPrototype name = m_namesOnPrototypes[record];
    auto lay = [&](FlowNodeID half, FunctionID function) {
        LinkType laid = fromTypeID(TypeTable::undefined);
        if (function) {
            laid = { };
            laid.functions = LinkType::Functions::Closure;
            laid.function = function;
        }
        arrive(half, laid);
    };
    lay(name.getter, getter);
    lay(name.setter, setter);

    const NameOnPrototype* underAnyKey = findNameOnPrototype(layout, { });
    if (name.isPassedByLookupsThatEnterNoAccessor || (underAnyKey && underAnyKey->isPassedByLookups) || (m_prototypesPassedByNumbers.get(layout.index()) && mayBeNamedByNumber(key)))
        letAccessorOnPrototypeEscape(record);
    settle();
}

FlowNodeID LinkFlow::foundOnPrototypesAbove(LayoutID layout, PropertyKeyID key, AccessorsAbove accessors)
{
    if (!m_layoutsBelowFollowedPrototypes.get(layout.index()))
        return { };
    if (accessors == AccessorsAbove::AreEnteredByNobody)
        letAccessorsOnPrototypesAboveEscape(layout, key);
    return nodeAskedOnceByLane(MadeOnDemand::FoundOnPrototypesAbove, static_cast<uint64_t>(layout.index()) << 32 | key.bits(), [&]() -> FlowNodeID {
        auto added = m_nodesFoundOnPrototypesAbove.add(wordOfNameInLinkPrototypes(layout, key), UINT32_MAX);
        if (!added.isNewEntry)
            return FlowNodeID(added.iterator->value);
        FlowNodeID found = makeNode();
        added.iterator->value = found.index();

        forEachPrototypePassedAbove(layout, key, [&](uint32_t record) {
            NameOnPrototype passed = m_namesOnPrototypes[record];
            if (!key && !std::exchange(m_namesOnPrototypes[record].isReadUnderAnyKey, true)) {
                for (uint32_t other = firstNameOnPrototypeOf(passed.layout); other != UINT32_MAX; other = m_namesOnPrototypes[other].nextOfLayout) {
                    NameOnPrototype name = m_namesOnPrototypes[other];
                    if (name.key)
                        addEdge(name.stored, passed.stored);
                }
            }
            addEdge(passed.stored, found);
        });
        return found;
    });
}

FlowNodeID LinkFlow::foundOnPrototypesAboveUnderNumbers(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!m_layoutsBelowFollowedPrototypes.get(layout.index()))
        return { };
    passPrototypesAboveUnderNumbers(layout);
    return foundBesideFieldsUnderAnyNumber();
}

void LinkFlow::passPrototypesAboveUnderNumbers(LayoutID layout)
{
    if (!m_layoutsBelowFollowedPrototypes.get(layout.index()))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!followsNamesOnPrototypeOf(current) || m_prototypesPassedByNumbers.set(current.index()) || !m_prototypesWithDefinitions.get(current.index()))
            continue;
        bool mayFindAccessor = false;
        for (uint32_t record = firstNameOnPrototypeOf(current); record != UINT32_MAX; record = m_namesOnPrototypes[record].nextOfLayout) {
            NameOnPrototype name = m_namesOnPrototypes[record];
            if (!name.mayHoldAccessor() || !mayBeNamedByNumber(name.key))
                continue;
            mayFindAccessor |= name.isDefinedByDescriptor;
            letAccessorOnPrototypeEscape(record);
        }
        if (mayFindAccessor)
            deferEscape(current);
    }
    settle();
}

void LinkFlow::lookForSetterOnPrototypesAbove(LayoutID layout, PropertyKeyID key, AccessorsAbove accessors)
{
    if (!m_layoutsBelowFollowedPrototypes.get(layout.index()))
        return;
    if (accessors == AccessorsAbove::AreEnteredByNobody)
        letAccessorsOnPrototypesAboveEscape(layout, key);
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    forEachPrototypePassedAbove(layout, key, [](uint32_t) { });
}

FunctionID parentConstructorOf(const Program& program, FunctionID function)
{
    const Function& record = program.at(function);
    auto isConstructorFunction = [&](FunctionID constructor, LayoutID classLayout) {
        const Function& constructing = program.at(constructor);
        return program.at(classLayout).kind == LayoutKind::Instance && program.at(classLayout).has(LayoutFlags::IsConstructorFunction) && program.at(classLayout).constructor == constructor && constructing.constructedClass == classLayout && constructing.takesThisFromConstruction();
    };
    LayoutID made = record.constructedClass;
    LayoutID above = made && isConstructorFunction(function, made) ? program.at(made).base : LayoutID();
    FunctionID parent = above ? program.at(above).constructor : FunctionID();
    return parent && parent != function && isConstructorFunction(parent, above) && program.at(parent).hasBody() && !program.at(parent).isCoroutine() ? parent : FunctionID();
}

bool LinkFlow::answersNothing(FunctionID function) const
{
    const ThinSummary& summary = summaries.at(function);
    return std::ranges::all_of(summary.edges, [&](const SummaryEdge& edge) {
        const SummaryNode& from = summary.nodes[edge.from];
        return summary.nodes[edge.to].kind != SummaryNode::Kind::Result || (from.kind == SummaryNode::Kind::Constant && TypeID::fromBits(from.first) == TypeID(TypeTable::undefined));
    });
}

bool LinkFlow::answersOwnThis(FunctionID function) const
{
    const Function& record = program.at(function);
    const ThinSummary& summary = summaries.at(function);
    std::optional<unsigned> thisParameter = record.thisParameterIndex();
    if (!record.hasBody() || record.isCoroutine() || summary.has(SummaryTrait::HasDirectEval))
        return false;
    auto answersThis = [&](uint32_t node, auto&& self, unsigned stepsLeft) -> bool {
        const SummaryNode& answered = summary.nodes[node];
        switch (answered.kind) {
        case SummaryNode::Kind::Parameter:
            return thisParameter && answered.first == *thisParameter;
        case SummaryNode::Kind::Constant:
            return TypeID::fromBits(answered.first) == TypeID(TypeTable::undefined);
        case SummaryNode::Kind::ResultOfCall:
            return summary.calls[answered.first].continuesConstruction && answersOwnThis(parentConstructorOf(program, function));
        case SummaryNode::Kind::Join:
            return stepsLeft && std::ranges::all_of(summary.edges, [&](const SummaryEdge& edge) {
                return edge.to != node || self(edge.from, self, stepsLeft - 1);
            });
        default:
            return false;
        }
    };
    static constexpr unsigned mostJoinsFollowed = 8;
    return std::ranges::all_of(summary.edges, [&](const SummaryEdge& edge) {
        return summary.nodes[edge.to].kind != SummaryNode::Kind::Result || answersThis(edge.from, answersThis, mostJoinsFollowed);
    });
}

void LinkFlow::noteConstructionLeavesParent(FunctionID function)
{
    LayoutID made = layoutOfConstructorFunction(function);
    if (!made)
        return;
    auto leaveFieldsEmpty = [&](LayoutID layout) {
        for (uint32_t field = 0; field < program.at(layout).fields.size(); ++field)
            noteFieldMayBeEmpty({ layout, field });
    };
    leaveFieldsEmpty(made);
    for (LayoutID below : program.layoutsExpectedBelow(made))
        leaveFieldsEmpty(below);
}

bool LinkFlow::mayNotBeLinkedYetBelow(LayoutID from, LayoutID classAbove) const
{
    if (!isBelowConstructorFunction(from))
        return false;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = from; current && current != classAbove && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (isOfConstructorFunction(current))
            return true;
    }
    return false;
}

FlowNodeID LinkFlow::objectsMadeThroughConstructorOf(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    auto added = m_nodesOfObjectsMadeThroughConstructors.add(layout.index(), UINT32_MAX);
    if (added.isNewEntry)
        added.iterator->value = makeNode().index();
    return FlowNodeID(added.iterator->value);
}

FlowNodeID LinkFlow::objectsMadeBySuperCallsOf(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    auto added = m_nodesOfObjectsMadeBySuperCalls.add(layout.index(), UINT32_MAX);
    if (added.isNewEntry)
        added.iterator->value = makeNode().index();
    return FlowNodeID(added.iterator->value);
}

FlowNodeID LinkFlow::objectsMadeForClassBelowConstructorFunction(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    auto added = m_nodesOfObjectsMadeForClassesBelow.add(layout.index(), UINT32_MAX);
    if (!added.isNewEntry)
        return FlowNodeID(added.iterator->value);
    FlowNodeID made = makeNode();
    added.iterator->value = made.index();
    noteAllocated(layout);
    LinkType objects;
    objects.objects = LinkType::Objects::Layout;
    objects.layout = layout;
    arriveFromSource(made, objects);
    return made;
}

void LinkFlow::defineOnPrototypeOf(LayoutID layout, PropertyKeyID key, UnknownKey unknownKey)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    uint32_t record = recordOfNameOnPrototype(layout, key);
    bool isUnderSomeSymbol = !key && unknownKey == UnknownKey::IsSomeSymbol;
    (isUnderSomeSymbol ? m_namesOnPrototypes[record].isDefinedUnderSomeSymbol : m_namesOnPrototypes[record].isDefinedByDescriptor) = true;
    m_prototypesWithDefinitions.set(layout.index());
    arrive(m_namesOnPrototypes[record].stored, LinkType::anything());
    if (isPooledNameOnPrototype(record))
        poolNameOnPrototype(record);

    bool isFoundByLookups = m_prototypesPassedByLookups.get(layout.index());
    if (isFoundByLookups && key) {
        const NameOnPrototype* underAnyKey = findNameOnPrototype(layout, { });
        isFoundByLookups = m_namesOnPrototypes[record].isPassedByLookups || (underAnyKey && underAnyKey->isPassedByLookups);
    }
    if (isFoundByLookups && isUnderSomeSymbol) {
        isFoundByLookups = false;
        for (uint32_t other = firstNameOnPrototypeOf(layout); other != UINT32_MAX; other = m_namesOnPrototypes[other].nextOfLayout) {
            const NameOnPrototype& name = m_namesOnPrototypes[other];
            isFoundByLookups |= name.isPassedByLookups && (!name.key || program.at(name.key).kind != PropertyKeyKind::Text);
        }
    }
    isFoundByLookups |= !isUnderSomeSymbol && m_prototypesPassedByNumbers.get(layout.index()) && mayBeNamedByNumber(key);
    if (isFoundByLookups)
        deferEscape(layout);
    settle();
}

void LinkFlow::tiePrototypesAboveLostLayout(LayoutID lost)
{
    if (!m_layoutsBelowFollowedPrototypes.get(lost.index()))
        return;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = lost; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!followsNamesOnPrototypeOf(current) || m_prototypesAboveLostLayouts.set(current.index()))
            continue;
        for (uint32_t record = firstNameOnPrototypeOf(current); record != UINT32_MAX; record = m_namesOnPrototypes[record].nextOfLayout) {
            if (m_namesOnPrototypes[record].key)
                poolNameOnPrototype(record);
        }
    }
}

bool LinkFlow::letPrototypesFoundThroughLostEscape()
{
    bool hasDeferred = false;
    for (size_t record = 0; record < m_namesOnPrototypes.size(); ++record) {
        NameOnPrototype name = m_namesOnPrototypes[record];
        if (!name.mayHoldAccessor() || !m_prototypesAboveLostLayouts.get(name.layout.index()) || escapedLayouts.get(name.layout.index()))
            continue;
        bool isLookedUp = m_anyFieldIsReadThroughLost || (name.key ? m_keysLookedUpThroughLost.contains(name.key.index()) : !m_keysLookedUpThroughLost.isEmpty());
        if (isLookedUp) {
            letAccessorOnPrototypeEscape(static_cast<uint32_t>(record));
            hasDeferred |= deferEscape(name.layout);
        }
    }
    settle();
    return hasDeferred;
}

void LinkFlow::openPrototypeOf(LayoutID layout)
{
    if (!followsNamesOnPrototypeOf(layout))
        return;
    for (uint32_t record = firstNameOnPrototypeOf(layout); record != UINT32_MAX; record = m_namesOnPrototypes[record].nextOfLayout) {
        FlowNodeID stored = m_namesOnPrototypes[record].stored;
        arrive(stored, LinkType::anything().asEscapeOnly());
        addEdge(stored, unseen());
        openHalvesOfAccessorInLinkPrototypes(*this, m_namesOnPrototypes[record]);
    }
}

class LinkFlow::CopiesOfMembers {
public:
    explicit CopiesOfMembers(LinkFlow& flow)
        : m_flow(flow)
        , m_program(flow.program)
    {
    }

    bool lay();

private:
    struct End {
        FunctionID function;
        uint32_t node { 0 };
        bool isHolderOfPrototypeObject { false };
    };
    struct Copy {
        End target;
        End source;
        bool copiesAccessorsAsTheyAre { false };
    };
    struct CallThatEnters {
        FunctionID callee;
        uint32_t call { 0 };

        friend auto operator<=>(const CallThatEnters&, const CallThatEnters&) = default;
    };
    enum class WayUp : uint8_t { None, ToCallers, ToMaker };

    const SummaryNode& nodeAt(const End& end) const LIFETIME_BOUND { return m_flow.summaries.at(end.function).nodes[end.node]; }
    LinkType typeSeenAt(const End&) const;
    bool holdsOneKindAloneAt(const End&) const;
    LayoutID findClassOfPrototypeObjectAt(const End&) const;
    bool namesLayoutsAt(const End&) const;
    bool listsMembersAt(const End& end) const { return findClassOfPrototypeObjectAt(end) || namesLayoutsAt(end); }
    bool mayHoldPrototypeObjectAt(const End&) const;
    bool mayHoldObjectAt(const End&) const;
    End originOf(End) const;
    WayUp wayUpFrom(const End&) const;

    void take(const Copy&);
    void lookAt(const Copy&);
    void takeUp(const Copy&, bool targetIsKnown, bool sourceIsKnown);
    void listCallsThatMayHandPrototypeObjectsOver();
    void listCallsOf(FunctionID caller, std::optional<uint32_t> parameterHandedOn);
    std::optional<End> findPassedArgument(uint32_t call, const End&) const;
    std::optional<End> findWhatMakerCaptures(FunctionID maker, const End&);

    void layMembers(const End& target, LayoutID classOfTarget, const End& source, bool copiesAccessorsAsTheyAre);
    void layMembersOfLayout(const End& target, LayoutID classOfTarget, LayoutID source, bool copiesAccessorsAsTheyAre);
    void layUnder(const End& target, LayoutID classOfTarget, PropertyKeyID, FlowNodeID held);
    bool holdsAccessor(LayoutID, FlowNodeID) const;

    LinkFlow& m_flow;
    const Program& m_program;
    bool m_hasLaidSome { false };
    bool m_somePrototypeObjectHasEscaped { false };
    Vector<Copy> m_copiesToLookAt;
    HashSet<std::pair<uint64_t, uint64_t>> m_copiesTaken;
    bool m_callsAreListed { false };
    Vector<CallThatEnters> m_callsThatEnter;
    Vector<std::pair<FunctionID, uint32_t>> m_parametersToFollow;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_parametersFollowed;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_makersWhoseCapturesAreListed;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_nodesOfCaptures;
};

LinkType LinkFlow::CopiesOfMembers::typeSeenAt(const End& end) const
{
    LinkType seen = m_flow.typeAt(m_flow.nodeOf(end.function, end.node)).directPart();
    seen.primitives = 0;
    return seen;
}

bool LinkFlow::CopiesOfMembers::holdsOneKindAloneAt(const End& end) const
{
    LinkType seen = typeSeenAt(end);
    LinkType alone;
    if (end.isHolderOfPrototypeObject) {
        alone.functions = LinkType::Functions::Closure;
        alone.function = seen.function;
    } else {
        alone.objects = LinkType::Objects::Prototype;
        alone.layout = seen.layout;
    }
    return seen == alone;
}

LayoutID LinkFlow::CopiesOfMembers::findClassOfPrototypeObjectAt(const End& end) const
{
    if (!holdsOneKindAloneAt(end))
        return { };
    LinkType seen = typeSeenAt(end);
    if (end.isHolderOfPrototypeObject)
        return m_flow.layoutOfConstructorFunction(seen.function);
    return m_flow.isOfConstructorFunction(seen.layout) ? seen.layout : LayoutID();
}

bool LinkFlow::CopiesOfMembers::namesLayoutsAt(const End& end) const
{
    if (end.isHolderOfPrototypeObject)
        return false;
    LinkType seen = typeSeenAt(end);
    bool namesLayouts = seen.objects == LinkType::Objects::Layout || seen.objects == LinkType::Objects::Set;
    return namesLayouts && seen.functions == LinkType::Functions::None && seen.classes == LinkType::Classes::None && !seen.builtin && !seen.hasUnknownObjects && !seen.holdsLostBuiltins;
}

bool LinkFlow::CopiesOfMembers::mayHoldPrototypeObjectAt(const End& end) const
{
    if (holdsOneKindAloneAt(end))
        return false;
    LinkType seen = typeSeenAt(end);
    if (end.isHolderOfPrototypeObject)
        return seen.hasUnknownObjects || seen.functions != LinkType::Functions::None;
    if (seen.objects == LinkType::Objects::Prototype || seen.objects == LinkType::Objects::Prototypes)
        return true;
    return m_somePrototypeObjectHasEscaped && (seen.hasUnknownObjects || seen.objects == LinkType::Objects::Lost);
}

bool LinkFlow::CopiesOfMembers::mayHoldObjectAt(const End& end) const
{
    if (end.isHolderOfPrototypeObject)
        return mayHoldPrototypeObjectAt(end);
    LinkType seen = typeSeenAt(end);
    return seen.hasUnknownObjects || seen.objects != LinkType::Objects::None;
}

auto LinkFlow::CopiesOfMembers::originOf(End end) const -> End
{
    const ThinSummary& summary = m_flow.summaries.at(end.function);
    for (size_t stepsLeft = summary.nodes.size(); stepsLeft--;) {
        const SummaryNode& node = summary.nodes[end.node];
        if (node.kind == SummaryNode::Kind::ResultOfAccess && !end.isHolderOfPrototypeObject) {
            const SummaryAccess& read = summary.accesses[node.first];
            if (read.kind != SummaryAccess::Kind::Read || read.isToContents() || read.key != m_program.keyOf(BuiltinNames::prototype))
                return end;
            end.node = read.subject;
            end.isHolderOfPrototypeObject = true;
            continue;
        }
        bool onlyHandsOn = node.kind == SummaryNode::Kind::Join || (node.kind == SummaryNode::Kind::Capture && node.second);
        if (!onlyHandsOn)
            return end;
        auto waysIn = std::ranges::equal_range(summary.edges.span(), end.node, { }, &SummaryEdge::to);
        if (waysIn.empty() || waysIn.front().from != waysIn.back().from)
            return end;
        end.node = waysIn.front().from;
    }
    return end;
}

auto LinkFlow::CopiesOfMembers::wayUpFrom(const End& end) const -> WayUp
{
    const SummaryNode& node = nodeAt(end);
    if (node.kind == SummaryNode::Kind::Parameter)
        return WayUp::ToCallers;
    if (node.kind == SummaryNode::Kind::Capture && !node.second)
        return WayUp::ToMaker;
    return WayUp::None;
}

void LinkFlow::CopiesOfMembers::take(const Copy& copy)
{
    if (!copy.target.node || !copy.source.node)
        return;
    auto wordOf = [](const End& end, bool more) {
        return static_cast<uint64_t>(end.function.index()) << 32 | static_cast<uint64_t>(end.node) << 2 | static_cast<uint64_t>(end.isHolderOfPrototypeObject) << 1 | static_cast<uint64_t>(more);
    };
    if (m_copiesTaken.add(std::pair { wordOf(copy.target, false), wordOf(copy.source, copy.copiesAccessorsAsTheyAre) }).isNewEntry)
        m_copiesToLookAt.append(copy);
}

void LinkFlow::CopiesOfMembers::lookAt(const Copy& copy)
{
    End target = findClassOfPrototypeObjectAt(copy.target) ? copy.target : originOf(copy.target);
    End source = listsMembersAt(copy.source) ? copy.source : originOf(copy.source);
    LayoutID classOfTarget = findClassOfPrototypeObjectAt(target);
    bool sourceIsKnown = listsMembersAt(source);
    if (classOfTarget && sourceIsKnown) {
        layMembers(target, classOfTarget, source, copy.copiesAccessorsAsTheyAre);
        return;
    }
    if ((!classOfTarget && !mayHoldPrototypeObjectAt(target)) || (!sourceIsKnown && !mayHoldObjectAt(source)))
        return;
    takeUp({ target, source, copy.copiesAccessorsAsTheyAre }, !!classOfTarget, sourceIsKnown);
}

void LinkFlow::CopiesOfMembers::takeUp(const Copy& copy, bool targetIsKnown, bool sourceIsKnown)
{
    End target = originOf(copy.target);
    End source = originOf(copy.source);
    const End& unknown = targetIsKnown ? source : target;
    WayUp way = wayUpFrom(unknown);
    auto goesUp = [&](const End& end) {
        return end.function == unknown.function && wayUpFrom(end) == way;
    };
    bool targetGoesUp = goesUp(target);
    bool sourceGoesUp = goesUp(source);
    if ((!targetIsKnown && !targetGoesUp) || (!sourceIsKnown && !sourceGoesUp))
        return;

    auto takeWhatIsFoundBy = [&](const auto& find) {
        std::optional<End> targetAbove = targetGoesUp ? find(target) : std::nullopt;
        std::optional<End> sourceAbove = sourceGoesUp ? find(source) : std::nullopt;
        if ((!targetAbove && !targetIsKnown) || (!sourceAbove && !sourceIsKnown))
            return;
        take({ targetAbove.value_or(copy.target), sourceAbove.value_or(copy.source), copy.copiesAccessorsAsTheyAre });
    };
    switch (way) {
    case WayUp::None:
        return;
    case WayUp::ToCallers:
        if (!m_callsAreListed)
            listCallsThatMayHandPrototypeObjectsOver();
        for (const CallThatEnters& entering : std::ranges::equal_range(m_callsThatEnter, unknown.function, { }, &CallThatEnters::callee)) {
            takeWhatIsFoundBy([&](const End& end) {
                return findPassedArgument(entering.call, end);
            });
        }
        return;
    case WayUp::ToMaker: {
        FunctionID maker = m_flow.summaries.findMakerAboveCopyOfMembers(unknown.function);
        if (!maker || !m_flow.reached.get(maker.index()))
            return;
        takeWhatIsFoundBy([&](const End& end) {
            return findWhatMakerCaptures(maker, end);
        });
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void LinkFlow::CopiesOfMembers::listCallsThatMayHandPrototypeObjectsOver()
{
    m_callsAreListed = true;
    for (size_t function : m_flow.functionsThatReadPrototypeObjects) {
        if (m_flow.reached.get(function))
            listCallsOf(FunctionID(static_cast<uint32_t>(function)), std::nullopt);
    }
    while (!m_parametersToFollow.isEmpty()) {
        auto [function, parameter] = m_parametersToFollow.takeLast();
        listCallsOf(function, parameter);
    }
    std::ranges::sort(m_callsThatEnter);
    removeRepeatedElements(m_callsThatEnter);
}

void LinkFlow::CopiesOfMembers::listCallsOf(FunctionID caller, std::optional<uint32_t> parameterHandedOn)
{
    const ThinSummary& summary = m_flow.summaries.at(caller);
    auto mayBePrototypeObjectOrItsHolder = [&](uint32_t node) {
        if (!node || node == noSummaryNode)
            return false;
        End argumentOrigin = originOf({ caller, node });
        if (parameterHandedOn)
            return nodeAt(argumentOrigin).kind == SummaryNode::Kind::Parameter && nodeAt(argumentOrigin).first == *parameterHandedOn;
        return argumentOrigin.isHolderOfPrototypeObject || findClassOfPrototypeObjectAt({ caller, node }) || findClassOfPrototypeObjectAt({ caller, node, true });
    };
    uint32_t firstCall = m_flow.firstCallOfFunction[caller.index()];
    for (uint32_t index = 0; index < summary.calls.size(); ++index) {
        const SummaryCall& record = summary.calls[index];
        if (record.callee == SummaryCall::Callee::Builtin || record.isConstruction || record.passesList || record.isMadeByProtocol)
            continue;
        auto list = [&](const auto& findParameterOf) {
            for (FunctionID callee : m_flow.calleesOfCalls[firstCall + index].listed()) {
                std::optional<uint32_t> parameter = findParameterOf(m_program.at(callee));
                if (!parameter)
                    continue;
                m_callsThatEnter.append({ callee, firstCall + index });
                if (m_parametersFollowed.add(static_cast<uint64_t>(callee.index()) << 32 | *parameter).isNewEntry)
                    m_parametersToFollow.append({ callee, *parameter });
            }
        };
        bool passesEntryArguments = record.callee == SummaryCall::Callee::Binding;
        if (!passesEntryArguments && mayBePrototypeObjectOrItsHolder(record.receiver)) {
            list([](const Function& callee) -> std::optional<uint32_t> {
                return callee.thisParameterIndex();
            });
        }
        for (uint32_t place = 0; place < record.numberOfArguments; ++place) {
            if (!mayBePrototypeObjectOrItsHolder(summary.arguments[record.firstArgument + place]))
                continue;
            list([&](const Function& callee) -> std::optional<uint32_t> {
                if (passesEntryArguments)
                    return place < callee.numberOfEntryParameters() ? std::optional(place) : std::nullopt;
                return place < callee.numberOfPositionalParameters() ? std::optional(callee.firstParameterIndex() + place) : std::nullopt;
            });
        }
    }
}

auto LinkFlow::CopiesOfMembers::findPassedArgument(uint32_t call, const End& end) const -> std::optional<End>
{
    FunctionID caller(static_cast<uint32_t>(std::ranges::upper_bound(m_flow.firstCallOfFunction, call) - m_flow.firstCallOfFunction.begin()) - 1);
    const ThinSummary& summary = m_flow.summaries.at(caller);
    const SummaryCall& record = summary.calls[call - m_flow.firstCallOfFunction[caller.index()]];
    const Function& callee = m_program.at(end.function);
    uint32_t parameter = nodeAt(end).first;
    auto argumentAt = [&](uint32_t place) {
        return place < record.numberOfArguments ? summary.arguments[record.firstArgument + place] : noSummaryNode;
    };
    uint32_t argumentNode = noSummaryNode;
    if (record.callee == SummaryCall::Callee::Binding)
        argumentNode = argumentAt(parameter);
    else {
        if (record.callee == SummaryCall::Callee::Name && record.receiver != noSummaryNode) {
            LinkType receiver = m_flow.typeAt(m_flow.nodeOf(caller, record.receiver));
            switch (receiver.functions) {
            case LinkType::Functions::None:
                break;
            case LinkType::Functions::Closure:
                if (receiver.function == end.function)
                    return std::nullopt;
                break;
            case LinkType::Functions::Set:
                if (std::ranges::contains(m_flow.functionsOfSet(receiver), end.function))
                    return std::nullopt;
                break;
            case LinkType::Functions::Constructors:
            case LinkType::Functions::Lost:
                return std::nullopt;
            }
        }
        switch (callee.entryParameterKind(parameter)) {
        case EntryParameter::This:
            argumentNode = record.receiver;
            break;
        case EntryParameter::Parameter:
            if (uint32_t position = parameter - callee.firstParameterIndex(); position < callee.numberOfPositionalParameters())
                argumentNode = argumentAt(position);
            break;
        case EntryParameter::Capture:
        case EntryParameter::Callee:
        case EntryParameter::NewTarget:
        case EntryParameter::ArgumentCount:
        case EntryParameter::HiddenRest:
            break;
        }
    }
    if (argumentNode == noSummaryNode)
        return std::nullopt;
    return End { caller, argumentNode, end.isHolderOfPrototypeObject };
}

auto LinkFlow::CopiesOfMembers::findWhatMakerCaptures(FunctionID maker, const End& end) -> std::optional<End>
{
    if (m_makersWhoseCapturesAreListed.add(maker.index()).isNewEntry) {
        const ThinSummary& summary = m_flow.summaries.at(maker);
        for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
            const SummaryNode& node = summary.nodes[index];
            if (node.kind == SummaryNode::Kind::Capture && node.second)
                m_nodesOfCaptures.add(static_cast<uint64_t>(node.second - 1) << 32 | node.first, index);
        }
    }
    auto found = m_nodesOfCaptures.find(static_cast<uint64_t>(end.function.index()) << 32 | nodeAt(end).first);
    if (found == m_nodesOfCaptures.end())
        return std::nullopt;
    return End { maker, found->value, end.isHolderOfPrototypeObject };
}

void LinkFlow::CopiesOfMembers::layMembers(const End& target, LayoutID classOfTarget, const End& source, bool copiesAccessorsAsTheyAre)
{
    if (LayoutID classOfSource = findClassOfPrototypeObjectAt(source)) {
        if (classOfSource == classOfTarget)
            return;
        for (uint32_t record = m_flow.firstNameOnPrototypeOf(classOfSource); record != UINT32_MAX; record = m_flow.m_namesOnPrototypes[record].nextOfLayout) {
            NameOnPrototype name = m_flow.m_namesOnPrototypes[record];
            if (name.key && name.isStoredInto)
                layUnder(target, classOfTarget, name.key, name.storedInSight);
        }
        return;
    }
    LinkType seen = typeSeenAt(source);
    if (seen.objects == LinkType::Objects::Layout) {
        layMembersOfLayout(target, classOfTarget, seen.layout, copiesAccessorsAsTheyAre);
        return;
    }
    for (LayoutID member : m_flow.layoutsOfSet(seen))
        layMembersOfLayout(target, classOfTarget, member, copiesAccessorsAsTheyAre);
}

void LinkFlow::CopiesOfMembers::layMembersOfLayout(const End& target, LayoutID classOfTarget, LayoutID source, bool copiesAccessorsAsTheyAre)
{
    for (const OwnValue& value : m_flow.ownValuesOf(source)) {
        if (value.key && !(copiesAccessorsAsTheyAre && holdsAccessor(source, value.node)))
            layUnder(target, classOfTarget, value.key, value.node);
    }
    for (uint32_t place = m_flow.m_newestAppendedFieldOfLayout[source.index()]; place != UINT32_MAX; place = m_flow.m_appendedFields[place].next) {
        AppendedField field = m_flow.m_appendedFields[place];
        layUnder(target, classOfTarget, field.key, field.node);
    }
}

void LinkFlow::CopiesOfMembers::layUnder(const End& target, LayoutID classOfTarget, PropertyKeyID key, FlowNodeID held)
{
    NameClass nameClass = m_flow.nameClassOf(key);
    if (nameClass == NameClass::Proto || mayBeElementKey(nameClass))
        return;
    const Function& whoCopies = m_flow.program.at(target.function);
    FlowNodeID laid = m_flow.storedOnPrototypeOf(classOfTarget, key, whoCopies.isModuleBody() && isLibraryCode(m_flow.program, whoCopies));
    if (!m_flow.m_edgesLaidByCopiesOfMembers.add(static_cast<uint64_t>(held.index()) << 32 | laid.index()).isNewEntry)
        return;
    m_flow.addEdge(held, laid);
    m_hasLaidSome = true;
}

bool LinkFlow::CopiesOfMembers::holdsAccessor(LayoutID layout, FlowNodeID node) const
{
    if (!m_flow.layoutsWithKnownAccessors.get(layout.index()))
        return false;
    return std::ranges::any_of(m_flow.accessorsLaidInFields, [&](const AccessorLaidInField& laid) {
        return (laid.appendedKey ? m_flow.findAppendedField(laid.field.layout, laid.appendedKey) : m_flow.field(laid.field)) == node;
    });
}

bool LinkFlow::CopiesOfMembers::lay()
{
    if (m_flow.m_layoutsOfConstructorFunctions.isEmpty())
        return false;
    for (size_t layout : m_flow.m_layoutsOfConstructorFunctions)
        m_somePrototypeObjectHasEscaped |= m_flow.escapedLayouts.get(layout);
    for (FunctionID function : m_flow.summaries.functionsThatCopyMembers()) {
        if (!m_flow.reached.get(function.index()))
            continue;
        for (const SummaryCopyOfMembers& copy : m_flow.summaries.at(function).copiesOfMembers)
            take({ { function, copy.target }, { function, copy.source }, copy.copiesAccessorsAsTheyAre });
    }
    while (!m_copiesToLookAt.isEmpty())
        lookAt(m_copiesToLookAt.takeLast());
    return m_hasLaidSome;
}

bool LinkFlow::layMembersThatCopiesBring()
{
    return CopiesOfMembers(*this).lay();
}

class LinkFlow::PrototypeHelperReader {
public:
    PrototypeHelperReader(const LinkFlow& flow, FunctionID id)
        : m_flow(flow)
        , m_program(flow.program)
        , m_id(id)
        , m_function(flow.program.at(id))
        , m_summary(flow.summaries.at(id))
        , m_operands(m_summary.nodes.size())
        , m_prototypeKey(flow.program.keyOf(BuiltinNames::prototype))
    {
    }

    PrototypeHelper read();

private:
    using Operand = PrototypeHelper::Operand;
    using Step = PrototypeHelper::Step;

    Operand operandAt(uint32_t node) const { return node == noSummaryNode ? Operand() : m_operands[node]; }
    bool isOther(uint32_t node) const { return operandAt(node).isOther(); }
    bool mayPass(const Operand&, TypeID passes) const;
    FunctionID functionKeptIn(VariableID);
    FunctionID findFunctionKeptIn(VariableID) const;
    FunctionID functionCapturedBy(FunctionID, uint32_t capture) const;
    FunctionID functionNamedBy(FunctionID user, uint32_t node);
    FunctionID functionNamedBy(uint32_t node) { return node == noSummaryNode ? FunctionID() : functionNamedBy(m_id, node); }
    bool isUsedElsewhere(FunctionID surrogate);
    bool nothingRunsBetween(ValueID first, ValueID last) const;
    bool isConstructionBySurrogate(const SummaryCall&);
    bool isCreation(const SummaryCall& call) const { return call.callee == SummaryCall::Callee::Builtin && m_program.builtins().canonicalID(call.builtin) == m_program.builtins().canonicalID(Builtins::ObjectCreate); }
    bool setsPrototype(const SummaryCall&) const;
    bool findOperands();
    bool hasNoOtherEffect() const;
    bool readAccess(const SummaryAccess&);
    bool readChange(const SummaryChange&);
    bool readDefinition(const SummaryChange&);
    bool readCall(uint32_t call);
    bool asksOwnName(const SummaryCall&);
    bool readsCopiedName(const SummaryAccess&) const;
    bool copiesName(const SummaryAccess&);
    bool readResult();
    bool isUsedOutsideResult(uint32_t node) const;
    bool isKnownNonFunction(const SummaryCall&, uint32_t argument) const;
    bool isOnlyLaid(uint32_t node) const;
    bool findObjectsLaid(uint32_t node, Vector<uint32_t, 2>& calls);
    bool takeObjectLaid(Operand child, uint32_t call);
    bool takeStoresOfSurrogate(Operand child, FunctionID surrogate);
    void add(Step step)
    {
        m_helper.steps.append(step);
    }

    const LinkFlow& m_flow;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    const ThinSummary& m_summary;
    Vector<Operand, 32> m_operands;
    PropertyKeyID m_prototypeKey;
    PrototypeHelper m_helper;
    struct ParentOfSurrogate {
        Operand parent;
        ValueID site;
    };
    HashMap<uint32_t, ParentOfSurrogate, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_parentsOfSurrogates;
    HashMap<uint32_t, FunctionID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsKeptInVariables;
    Vector<uint32_t, 2> m_constructionsLaid;
    HashMap<std::pair<uint32_t, uint32_t>, Operand, DefaultHash<std::pair<uint32_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_captures;
};

bool LinkFlow::PrototypeHelperReader::mayPass(const Operand& operand, TypeID passes) const
{
    if (!passes || operand.kind == Operand::Kind::Unknown)
        return true;
    LinkType held;
    if (operand.kind == Operand::Kind::Parameter) {
        held.functions = LinkType::Functions::Closure;
        held.function = m_id;
    } else
        held.hasUnknownObjects = true;
    return !m_flow.passedBy(held, passes).isNever();
}

FunctionID LinkFlow::PrototypeHelperReader::findFunctionKeptIn(VariableID id) const
{
    const Variable& variable = m_program.at(id);
    const Module& module = m_program.at(variable.module);
    if (variable.owner || !variable.flags.contains(VariableFlags::IsImmutable) || !module.body || module.kind == ModuleKind::RuntimeSource
        || variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun, VariableFlags::HoldsInstance }))
        return { };
    if (std::ranges::contains(module.exports, id, &ModuleExport::variable) || std::ranges::contains(module.syntheticExports, id, &SyntheticExport::variable))
        return { };
    for (const EvalScope& scope : m_program.evalScopes) {
        if (std::ranges::contains(scope.bindings, id, &EvalBinding::variable))
            return { };
    }
    if (variable.initialization == VariableInitialization::Function)
        return variable.function;
    FunctionID kept;
    const ThinSummary& body = m_flow.summaries.at(module.body);
    for (const SummaryEdge& edge : body.edges) {
        if (body.nodes[edge.to] != SummaryNode { SummaryNode::Kind::Variable, id.index(), 0 })
            continue;
        if (kept || body.nodes[edge.from].kind != SummaryNode::Kind::Closure)
            return { };
        kept = FunctionID(body.nodes[edge.from].first);
    }
    return kept;
}

FunctionID LinkFlow::PrototypeHelperReader::functionKeptIn(VariableID id)
{
    return m_functionsKeptInVariables.ensure(id.index(), [&] { return findFunctionKeptIn(id); }).iterator->value;
}

FunctionID LinkFlow::PrototypeHelperReader::functionCapturedBy(FunctionID closure, uint32_t capture) const
{
    const DeclaredCapture& declared = m_program.at(closure).captures[capture];
    return declared.isEstablished() && declared.written() ? singleFunctionOf(TypeReader(m_program), m_program.at(declared.written())) : FunctionID();
}

FunctionID LinkFlow::PrototypeHelperReader::functionNamedBy(FunctionID user, uint32_t index)
{
    const SummaryNode& node = m_flow.summaries.at(user).nodes[index];
    switch (node.kind) {
    case SummaryNode::Kind::Closure:
        return FunctionID(node.first);
    case SummaryNode::Kind::Variable:
        return functionKeptIn(VariableID(node.first));
    case SummaryNode::Kind::Capture:
        return functionCapturedBy(node.second ? FunctionID(node.second - 1) : user, node.first);
    default:
        return { };
    }
}

bool LinkFlow::PrototypeHelperReader::isUsedElsewhere(FunctionID surrogate)
{
    ModuleID module = m_program.at(surrogate).module;
    for (uint32_t other = 0; other < m_program.functions.size(); ++other) {
        FunctionID user(other);
        if (m_program.at(user).module != module || user == m_id || !m_program.at(user).hasBody())
            continue;
        const ThinSummary& summary = m_flow.summaries.at(user);
        for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
            if (functionNamedBy(user, index) != surrogate)
                continue;
            for (const SummaryEdge& edge : summary.edges) {
                if (edge.from != index)
                    continue;
                if (functionNamedBy(user, edge.to) != surrogate)
                    return true;
            }
            auto isUsed = [&](auto&&... operands) { return ((operands == index) || ...); };
            if (std::ranges::any_of(summary.accesses, [&](const SummaryAccess& access) { return isUsed(access.subject, access.receiver, access.value, access.keyNode, access.emptyHolderOrKeyPrefix); })
                || std::ranges::any_of(summary.calls, [&](const SummaryCall& call) { return isUsed(call.subject, call.receiver, call.newTarget, call.waitsFor); })
                || std::ranges::any_of(summary.changes, [&](const SummaryChange& change) { return isUsed(change.subject, change.other, change.keyNode, change.getter, change.getterInSight, change.setterInSight, change.valueInSight); })
                || std::ranges::any_of(summary.copiesOfMembers, [&](const SummaryCopyOfMembers& copy) { return isUsed(copy.target, copy.source); })
                || std::ranges::any_of(summary.operators, [&](const SummaryOperator& found) { return isUsed(found.operands[0], found.operands[1]); })
                || std::ranges::any_of(summary.inspections, [&](const ThinSummary::Inspection& inspection) { return isUsed(inspection.subject); })
                || std::ranges::any_of(summary.members, [&](const SummaryMember& member) { return isUsed(member.value); })
                || std::ranges::contains(summary.arguments, index))
                return true;
        }
    }
    return false;
}

bool LinkFlow::PrototypeHelperReader::nothingRunsBetween(ValueID first, ValueID last) const
{
    auto runsBetween = [&](const auto& found) { return found.site && found.site.index() > first.index() && found.site.index() < last.index(); };
    return std::ranges::none_of(m_summary.accesses, runsBetween) && std::ranges::none_of(m_summary.calls, runsBetween) && std::ranges::none_of(m_summary.changes, runsBetween) && std::ranges::none_of(m_summary.operators, runsBetween);
}

bool LinkFlow::PrototypeHelperReader::isConstructionBySurrogate(const SummaryCall& call)
{
    return call.callee == SummaryCall::Callee::Value && call.isConstruction && !call.passesList && !call.numberOfArguments && functionNamedBy(call.subject)
        && (call.newTarget == noSummaryNode || functionNamedBy(call.newTarget) == functionNamedBy(call.subject));
}

bool LinkFlow::PrototypeHelperReader::setsPrototype(const SummaryCall& call) const
{
    if (call.callee != SummaryCall::Callee::Builtin)
        return false;
    BuiltinID called = m_program.builtins().canonicalID(call.builtin);
    return called == m_program.builtins().canonicalID(Builtins::ObjectSetPrototypeOf) || called == m_program.builtins().canonicalID(Builtins::ReflectSetPrototypeOf);
}

bool LinkFlow::PrototypeHelperReader::findOperands()
{
    for (uint32_t index = 0; index < m_summary.nodes.size(); ++index) {
        const SummaryNode& node = m_summary.nodes[index];
        if (node.kind == SummaryNode::Kind::Parameter && m_function.entryParameterKind(node.first) == EntryParameter::Parameter && node.first - m_function.firstParameterIndex() < m_function.numberOfPositionalParameters())
            m_operands[index] = { Operand::Kind::Parameter, node.first };
    }
    auto meet = [&](uint32_t node, const Operand& operand) {
        Operand met = m_operands[node].isOther() || m_operands[node] == operand ? operand : Operand { Operand::Kind::Unknown, 0 };
        return std::exchange(m_operands[node], met) != met;
    };
    for (bool grows = true; std::exchange(grows, false);) {
        for (const SummaryEdge& edge : m_summary.edges) {
            const Operand from = m_operands[edge.from];
            if (from.isOther() || !mayPass(from, edge.passes))
                continue;
            const SummaryNode& to = m_summary.nodes[edge.to];
            if (to.kind == SummaryNode::Kind::Join || to.kind == SummaryNode::Kind::Result)
                grows |= meet(edge.to, from);
            else if (to.kind == SummaryNode::Kind::Capture && to.second) {
                auto added = m_captures.add({ to.second - 1, to.first }, from);
                if (!added.isNewEntry && added.iterator->value != from)
                    return false;
            } else
                return false;
        }
        for (uint32_t index = 0; index < m_summary.nodes.size(); ++index) {
            const SummaryNode& node = m_summary.nodes[index];
            if (node.kind != SummaryNode::Kind::ResultOfAccess)
                continue;
            const SummaryAccess& access = m_summary.accesses[node.first];
            if (access.kind == SummaryAccess::Kind::Read && !access.isToContents() && access.key == m_prototypeKey && operandAt(access.subject).kind == Operand::Kind::Parameter)
                grows |= meet(index, { Operand::Kind::PrototypeOfParameter, operandAt(access.subject).parameter });
        }
    }
    for (const SummaryEdge& edge : m_summary.edges) {
        if (m_operands[edge.to].isOther() || !m_operands[edge.from].isOther() || m_summary.nodes[edge.to].kind == SummaryNode::Kind::Result)
            continue;
        const SummaryNode& from = m_summary.nodes[edge.from];
        if (from.kind != SummaryNode::Kind::Constant || !holdsOnlyPrimitives(TypeID::fromBits(from.first)))
            m_operands[edge.to] = { Operand::Kind::Unknown, 0 };
    }
    return true;
}

bool LinkFlow::PrototypeHelperReader::hasNoOtherEffect() const
{
    bool copiesOnlyOperands = std::ranges::all_of(m_summary.copiesOfMembers, [&](const SummaryCopyOfMembers& copy) {
        return operandAt(copy.target).kind == Operand::Kind::Parameter && operandAt(copy.source).kind == Operand::Kind::Parameter;
    });
    if (!copiesOnlyOperands || !m_summary.members.isEmpty() || !m_summary.placesOfWrittenLists.isEmpty() || !m_summary.namesThatLoaderTakes.isEmpty() || !m_summary.namesThatHostLooksFor.isEmpty() || !m_summary.handlers.isEmpty())
        return false;
    for (const SummaryOperator& found : m_summary.operators) {
        for (uint32_t operand : found.operands) {
            if (!isOther(operand))
                return false;
        }
        for (uint32_t converted : found.converted) {
            if (!isOther(converted))
                return false;
        }
    }
    return std::ranges::all_of(m_summary.inspections, [&](const ThinSummary::Inspection& inspection) {
        return operandAt(inspection.subject).kind == Operand::Kind::Other || operandAt(inspection.subject).kind == Operand::Kind::Parameter;
    });
}

bool LinkFlow::PrototypeHelperReader::readsCopiedName(const SummaryAccess& read) const
{
    uint32_t place = static_cast<uint32_t>(&read - &m_summary.accesses[0]);
    uint32_t result = noSummaryNode;
    for (uint32_t index = 0; index < m_summary.nodes.size(); ++index) {
        if (m_summary.nodes[index].kind == SummaryNode::Kind::ResultOfAccess && m_summary.nodes[index].first == place)
            result = index;
    }
    if (read.key || operandAt(read.subject).kind != Operand::Kind::Parameter)
        return false;
    if (result == noSummaryNode)
        return true;
    auto isResult = [&](auto... operands) { return ((operands == result) || ...); };
    return std::ranges::none_of(m_summary.edges, [&](const SummaryEdge& edge) { return edge.from == result; }) && !std::ranges::contains(m_summary.arguments, result)
        && std::ranges::all_of(m_summary.accesses, [&](const SummaryAccess& access) {
            return !isResult(access.subject, access.receiver, access.keyNode, access.emptyHolderOrKeyPrefix) && (!isResult(access.value) || access.kind == SummaryAccess::Kind::Read || (access.kind == SummaryAccess::Kind::Store && !access.key && access.isCopySpelledByLoop));
        })
        && std::ranges::none_of(m_summary.calls, [&](const SummaryCall& call) { return isResult(call.subject, call.receiver, call.newTarget, call.waitsFor); })
        && std::ranges::none_of(m_summary.changes, [&](const SummaryChange& change) { return isResult(change.subject, change.other, change.keyNode, change.getter, change.getterInSight, change.setterInSight, change.valueInSight); })
        && std::ranges::none_of(m_summary.operators, [&](const SummaryOperator& found) { return isResult(found.operands[0], found.operands[1], found.converted[0], found.converted[1]); });
}

bool LinkFlow::PrototypeHelperReader::copiesName(const SummaryAccess& store)
{
    if (store.key || !store.isCopySpelledByLoop || operandAt(store.subject).kind != Operand::Kind::Parameter || store.value == noSummaryNode || m_summary.nodes[store.value].kind != SummaryNode::Kind::ResultOfAccess)
        return false;
    const SummaryAccess& read = m_summary.accesses[m_summary.nodes[store.value].first];
    Operand parent = operandAt(read.subject);
    bool isRead = read.kind == SummaryAccess::Kind::Read && !read.isToContents() && !read.key && read.keyNode == store.keyNode && parent.kind == Operand::Kind::Parameter;
    bool isSpelled = std::ranges::any_of(m_summary.changes, [&](const SummaryChange& change) {
        return change.site == store.site && change.kind == SummaryChange::Kind::CopiesInto && change.isSpelledByLoop && operandAt(change.subject) == operandAt(store.subject) && operandAt(change.other) == parent;
    });
    if (!isRead || !isSpelled)
        return false;
    Step step;
    step.kind = Step::Kind::CopiesNames;
    step.object = operandAt(store.subject);
    step.value = parent;
    add(step);
    return true;
}

bool LinkFlow::PrototypeHelperReader::asksOwnName(const SummaryCall& call)
{
    if (call.callee != SummaryCall::Callee::Name || call.isConstruction || call.passesList || call.isMadeByProtocol || call.passesOnGatheredArguments || call.subject != noSummaryNode || call.receiver == noSummaryNode || !isOther(call.newTarget) || !isOther(call.waitsFor))
        return false;
    bool isCallOfHeldBuiltin = call.key == m_program.keyOf(BuiltinNames::call) && isOther(call.receiver) && call.numberOfArguments;
    if (!isCallOfHeldBuiltin && (call.key != m_program.keyOf(BuiltinNames::hasOwnProperty) || m_flow.someBodyStoresHasOwnProperty()))
        return false;
    uint32_t asked = isCallOfHeldBuiltin ? m_summary.arguments[call.firstArgument] : call.receiver;
    if (operandAt(asked).kind != Operand::Kind::Parameter)
        return false;
    for (uint32_t argument = isCallOfHeldBuiltin ? 1 : 0; argument < call.numberOfArguments; ++argument) {
        if (!isOther(m_summary.arguments[call.firstArgument + argument]))
            return false;
    }
    Step step;
    step.kind = Step::Kind::AsksOwnName;
    step.object = operandAt(asked);
    if (isCallOfHeldBuiltin)
        step.stored = m_flow.nodeOf(m_id, call.receiver);
    add(step);
    return true;
}

bool LinkFlow::PrototypeHelperReader::isUsedOutsideResult(uint32_t node) const
{
    auto names = [&](uint32_t named) {
        return named == node;
    };
    auto isReturned = [&](const SummaryEdge& edge) {
        return m_summary.nodes[edge.to].kind == SummaryNode::Kind::Result || std::ranges::any_of(m_summary.returnStatements, [&](const SummaryReturn& returned) { return returned.node == edge.to; });
    };
    return std::ranges::any_of(m_summary.edges, [&](const SummaryEdge& edge) { return names(edge.from) && !isReturned(edge); }) || std::ranges::any_of(m_summary.arguments, names)
        || std::ranges::any_of(m_summary.accesses, [&](const SummaryAccess& access) { return names(access.subject) || names(access.receiver) || names(access.keyNode) || (names(access.value) && access.kind != SummaryAccess::Kind::Read); })
        || std::ranges::any_of(m_summary.calls, [&](const SummaryCall& call) { return names(call.subject) || names(call.receiver) || names(call.newTarget) || names(call.waitsFor); })
        || std::ranges::any_of(m_summary.changes, [&](const SummaryChange& change) { return names(change.subject) || names(change.other); })
        || std::ranges::any_of(m_summary.operators, [&](const SummaryOperator& found) { return std::ranges::any_of(found.operands, names); });
}

bool LinkFlow::PrototypeHelperReader::isKnownNonFunction(const SummaryCall& call, uint32_t argument) const
{
    const BodyIndex* index = call.site ? bodiesLookedAt(m_flow.looks).find(m_id) : nullptr;
    if (!index || index->isParameter(call.site) || index->opcodeOf(call.site) != Opcode::Call)
        return false;
    auto arguments = index->value(call.site).as<Opcode::Call>().argumentsOperands();
    if (argument >= arguments.size())
        return false;
    ValueID value = arguments[argument];
    while (value) {
        if (!index->isParameter(value)) {
            Opcode opcode = index->opcodeOf(value);
            if (opcode != Opcode::Convert && opcode != Opcode::NarrowByTest)
                return false;
            value = index->value(value).operand(opcode == Opcode::Convert ? 0 : OperandIndex::NarrowByTest::left);
            continue;
        }
        ValueID subject = index->valueNarrowedInto(value);
        std::optional<BodyIncomingEdge> edge = subject ? index->onlyIncomingEdge(index->blockOf(value)) : std::nullopt;
        if (!edge)
            return false;
        BodyTerminator end = index->terminator(edge->from);
        if (end.kind() == TerminatorKind::Branch && edge->targetIndex == 1) {
            ValueID condition = end.as<TerminatorKind::Branch>().conditionOperand();
            if (condition && !index->isParameter(condition) && index->opcodeOf(condition) == Opcode::TypeTest && index->value(condition).as<Opcode::TypeTest>().testedType() == TypeID(TypeTable::function))
                return true;
        }
        value = subject;
    }
    return false;
}

bool LinkFlow::PrototypeHelperReader::isOnlyLaid(uint32_t node) const
{
    Vector<uint32_t, 4> names { node };
    for (size_t place = 0; place < names.size(); ++place) {
        for (const SummaryEdge& edge : m_summary.edges) {
            if (edge.from != names[place])
                continue;
            if (m_summary.nodes[edge.to].kind != SummaryNode::Kind::Join)
                return false;
            if (!names.contains(edge.to))
                names.append(edge.to);
        }
    }
    auto isNamed = [&](auto... operands) { return (names.contains(operands) || ...); };
    return std::ranges::none_of(m_summary.arguments, isNamed)
        && std::ranges::all_of(m_summary.accesses, [&](const SummaryAccess& access) {
            bool isLaid = access.kind == SummaryAccess::Kind::Store && !access.isToContents() && access.key == m_prototypeKey && operandAt(access.subject).kind == Operand::Kind::Parameter;
            return !isNamed(access.subject, access.receiver, access.keyNode, access.emptyHolderOrKeyPrefix) && (isLaid || !isNamed(access.value));
        })
        && std::ranges::none_of(m_summary.calls, [&](const SummaryCall& call) { return isNamed(call.subject, call.receiver, call.newTarget, call.waitsFor); })
        && std::ranges::none_of(m_summary.changes, [&](const SummaryChange& change) { return isNamed(change.subject, change.keyNode, change.getter, change.getterInSight, change.setterInSight, change.valueInSight) || (!change.heirIsMadeByCall && isNamed(change.other)); })
        && std::ranges::none_of(m_summary.operators, [&](const SummaryOperator& found) { return isNamed(found.operands[0], found.operands[1], found.converted[0], found.converted[1]); });
}

bool LinkFlow::PrototypeHelperReader::findObjectsLaid(uint32_t node, Vector<uint32_t, 2>& calls)
{
    if (!isOnlyLaid(node))
        return false;
    const SummaryNode& laid = m_summary.nodes[node];
    if (laid.kind == SummaryNode::Kind::ResultOfCall && !laid.second) {
        const SummaryCall& call = m_summary.calls[laid.first];
        if (!isConstructionBySurrogate(call) && !(isCreation(call) && call.numberOfArguments == 1))
            return false;
        calls.append(laid.first);
        return true;
    }
    if (laid.kind != SummaryNode::Kind::Join)
        return false;
    bool isLaidHere = false;
    for (const SummaryEdge& edge : m_summary.edges) {
        if (edge.to != node)
            continue;
        if (m_summary.nodes[edge.from].kind == SummaryNode::Kind::Join || !findObjectsLaid(edge.from, calls))
            return false;
        isLaidHere = true;
    }
    return isLaidHere;
}

bool LinkFlow::PrototypeHelperReader::takeStoresOfSurrogate(Operand child, FunctionID surrogate)
{
    const Function& function = m_program.at(surrogate);
    const ThinSummary& summary = m_flow.summaries.at(surrogate);
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    if (!function.hasBody() || function.isCoroutine() || function.constructorForm() == ConstructorForm::NotConstructor || isClassConstructorOf(function.kind) || !summary.calls.isEmpty() || !summary.changes.isEmpty() || summary.has(SummaryTrait::HasDirectEval) || summary.has(SummaryTrait::UsesArguments))
        return false;
    for (const SummaryOperator& found : summary.operators) {
        if (found.opcode != Opcode::ToThis)
            return false;
    }
    auto isThis = [&](uint32_t node) {
        auto isParameter = [&](uint32_t from) {
            return thisParameter && summary.nodes[from] == SummaryNode { SummaryNode::Kind::Parameter, *thisParameter, 0 };
        };
        if (isParameter(node))
            return true;
        bool comesFromThis = false;
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to != node)
                continue;
            if (!isParameter(edge.from))
                return false;
            comesFromThis = true;
        }
        return comesFromThis && summary.nodes[node].kind == SummaryNode::Kind::Join;
    };
    for (const SummaryEdge& edge : summary.edges) {
        const SummaryNode& from = summary.nodes[edge.from];
        const SummaryNode& to = summary.nodes[edge.to];
        bool givesUndefined = to.kind == SummaryNode::Kind::Result && from.kind == SummaryNode::Kind::Constant && TypeID::fromBits(from.first) == TypeID(TypeTable::undefined);
        bool isThrown = to.kind == SummaryNode::Kind::Thrown && from.kind == SummaryNode::Kind::Unknown;
        if (!givesUndefined && !isThrown && !isThis(edge.to))
            return false;
    }
    for (const SummaryAccess& access : summary.accesses) {
        if (access.kind != SummaryAccess::Kind::Store || access.isToContents() || !access.key || !isThis(access.subject) || access.value == noSummaryNode)
            return false;
        NameClass nameClass = m_flow.nameClassOf(access.key);
        if (nameClass == NameClass::Proto || mayBeElementKey(nameClass) || isPrivateNameInLink(m_program, access.key))
            return false;
        Step step;
        step.kind = Step::Kind::StoresName;
        step.object = child;
        step.key = access.key;
        const SummaryNode& stored = summary.nodes[access.value];
        if (stored.kind == SummaryNode::Kind::Capture && !stored.second) {
            auto captured = m_captures.find({ surrogate.index(), stored.first });
            if (captured != m_captures.end())
                step.value = captured->value;
        }
        if (step.value.kind == Operand::Kind::Unknown)
            return false;
        if (step.value.isOther())
            step.stored = m_flow.nodeOf(surrogate, access.value);
        add(step);
    }
    return true;
}

bool LinkFlow::PrototypeHelperReader::takeObjectLaid(Operand child, uint32_t place)
{
    const SummaryCall& call = m_summary.calls[place];
    Operand prototypeOfChild { Operand::Kind::PrototypeOfParameter, child.parameter };
    Operand parent;
    if (isCreation(call)) {
        parent = operandAt(m_summary.arguments[call.firstArgument]);
        if (parent.isOther()) {
            uint32_t node = m_summary.arguments[call.firstArgument];
            bool holdsNullAlone = m_summary.nodes[node].kind == SummaryNode::Kind::Join && std::ranges::all_of(m_summary.edges, [&](const SummaryEdge& edge) {
                return edge.to != node || (!m_operands[edge.from].isOther() && !mayPass(m_operands[edge.from], edge.passes));
            });
            return holdsNullAlone;
        }
    } else {
        FunctionID surrogate = functionNamedBy(call.subject);
        auto found = m_parentsOfSurrogates.find(surrogate.index());
        const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_id);
        bool isLinkedFirst = found != m_parentsOfSurrogates.end() && index && found->value.site.index() < call.site.index() && index->blockOf(found->value.site) == index->blockOf(call.site);
        if (!isLinkedFirst || !takeStoresOfSurrogate(prototypeOfChild, surrogate))
            return false;
        bool isMadeHere = m_summary.nodes[call.subject].kind == SummaryNode::Kind::Closure;
        if (!isMadeHere && (!nothingRunsBetween(found->value.site, call.site) || isUsedElsewhere(surrogate)))
            return false;
        parent = found->value.parent;
        m_constructionsLaid.append(place);
    }
    if (parent.kind != Operand::Kind::PrototypeOfParameter)
        return false;
    Step link;
    link.kind = Step::Kind::LinksParent;
    link.object = prototypeOfChild;
    link.value = parent;
    add(link);
    return true;
}

bool LinkFlow::PrototypeHelperReader::readAccess(const SummaryAccess& access)
{
    Operand subject = operandAt(access.subject);
    if (!isOther(access.receiver) || !isOther(access.keyNode))
        return false;
    if (access.isToContents())
        return subject.isOther() && isOther(access.value) && isOther(access.emptyHolderOrKeyPrefix);
    bool isPrototypeProperty = access.key == m_prototypeKey;
    switch (access.kind) {
    case SummaryAccess::Kind::Read:
        return subject.isOther() || (subject.kind == Operand::Kind::Parameter && isPrototypeProperty) || readsCopiedName(access);
    case SummaryAccess::Kind::Has:
        return subject.isOther();
    case SummaryAccess::Kind::Define:
        return isOther(access.value) && (subject.isOther() || access.isByDescriptor);
    case SummaryAccess::Kind::Store:
        break;
    }
    if (copiesName(access))
        return true;
    Operand value = operandAt(access.value);
    if (subject.isOther()) {
        if (FunctionID surrogate = isPrototypeProperty && value.kind == Operand::Kind::PrototypeOfParameter ? functionNamedBy(access.subject) : FunctionID())
            return m_parentsOfSurrogates.add(surrogate.index(), ParentOfSurrogate { value, access.site }).isNewEntry;
        return value.isOther();
    }
    if (subject.kind == Operand::Kind::Unknown || value.kind == Operand::Kind::Unknown || !access.key)
        return false;
    if (subject.kind == Operand::Kind::Parameter && isPrototypeProperty)
        return true;
    NameClass nameClass = m_flow.nameClassOf(access.key);
    Step step;
    step.object = subject;
    step.value = value;
    step.key = access.key;
    if (nameClass == NameClass::Proto) {
        step.kind = Step::Kind::LinksParent;
        add(step);
        return !value.isOther();
    }
    if (mayBeElementKey(nameClass) || isPrototypeProperty || isPrivateNameInLink(m_program, access.key))
        return false;
    step.kind = Step::Kind::StoresName;
    if (value.isOther())
        step.stored = m_flow.nodeOf(m_id, access.value);
    add(step);
    return true;
}

bool LinkFlow::PrototypeHelperReader::readDefinition(const SummaryChange& change)
{
    Operand subject = operandAt(change.subject);
    Operand value = operandAt(change.valueInSight);
    if (subject.kind == Operand::Kind::Unknown || value.kind == Operand::Kind::Unknown || !isOther(change.other) || !change.key || change.keyNode != noSummaryNode || !change.descriptorIsInSight || !change.descriptorLaysNoAccessor || change.laysAccessorAsWritten)
        return false;
    NameClass nameClass = m_flow.nameClassOf(change.key);
    bool isPrototypeProperty = change.key == m_prototypeKey;
    if (nameClass == NameClass::Proto || mayBeElementKey(nameClass) || isPrivateNameInLink(m_program, change.key))
        return false;
    if (change.valueInSight == noSummaryNode)
        return !change.laysValueInSight;
    if (isPrototypeProperty)
        return false;
    Step step;
    step.kind = Step::Kind::StoresName;
    step.object = subject;
    step.value = value;
    step.key = change.key;
    if (value.isOther())
        step.stored = m_flow.nodeOf(m_id, change.valueInSight);
    add(step);
    return true;
}

bool LinkFlow::PrototypeHelperReader::readChange(const SummaryChange& change)
{
    Operand subject = operandAt(change.subject);
    Operand other = operandAt(change.other);
    for (uint32_t node : { change.keyNode, change.getter, change.getterInSight, change.setterInSight }) {
        if (!isOther(node))
            return false;
    }
    if (change.kind == SummaryChange::Kind::BecomesPrototype) {
        bool isCreated = change.heirIsMadeByCall && change.other != noSummaryNode && m_summary.nodes[change.other].kind == SummaryNode::Kind::ResultOfCall;
        return other.isOther() && (subject.isOther() || isCreated);
    }
    if (change.kind == SummaryChange::Kind::DefinesByDescriptor && !subject.isOther())
        return readDefinition(change);
    if (change.kind == SummaryChange::Kind::CopiesInto && change.isSpelledByLoop && subject.kind == Operand::Kind::Parameter && other.kind == Operand::Kind::Parameter)
        return true;
    if (!isOther(change.valueInSight))
        return false;
    if (subject.isOther() && other.isOther())
        return true;
    if (change.kind != SummaryChange::Kind::ChangesPrototype || subject.isOther() || other.isOther() || subject.kind == Operand::Kind::Unknown || other.kind == Operand::Kind::Unknown)
        return false;
    Step step;
    step.kind = Step::Kind::LinksParent;
    step.object = subject;
    step.value = other;
    add(step);
    return true;
}

bool LinkFlow::PrototypeHelperReader::readCall(uint32_t place)
{
    const SummaryCall& call = m_summary.calls[place];
    if (asksOwnName(call))
        return true;
    for (uint32_t node : { call.subject, call.receiver, call.newTarget, call.waitsFor }) {
        if (!isOther(node))
            return false;
    }
    bool passesOperand = false;
    for (uint32_t argument = 0; argument < call.numberOfArguments; ++argument)
        passesOperand |= !isOther(m_summary.arguments[call.firstArgument + argument]) && !isKnownNonFunction(call, argument);
    if (!passesOperand)
        return true;
    if (call.passesList || call.isConstruction || call.isMadeByProtocol || call.passesOnGatheredArguments)
        return false;
    uint32_t result = noSummaryNode;
    for (uint32_t index = 0; index < m_summary.nodes.size(); ++index) {
        if (m_summary.nodes[index].kind == SummaryNode::Kind::ResultOfCall && m_summary.nodes[index].first == place)
            result = index;
    }
    bool resultIsUsed = result != noSummaryNode && isUsedOutsideResult(result);
    if (call.callee == SummaryCall::Callee::Builtin) {
        if (setsPrototype(call))
            return !resultIsUsed;
        if (!isCreation(call))
            return false;
        return result == noSummaryNode || isOnlyLaid(result);
    }
    if (call.callee != SummaryCall::Callee::Value || call.subject == noSummaryNode)
        return false;
    Step step;
    step.kind = Step::Kind::CallsFunction;
    step.call = place;
    step.resultIsUsed = resultIsUsed;
    add(step);
    return true;
}

bool LinkFlow::PrototypeHelperReader::readResult()
{
    for (uint32_t index = 0; index < m_summary.nodes.size(); ++index) {
        if (m_summary.nodes[index].kind != SummaryNode::Kind::Result)
            continue;
        for (const SummaryEdge& edge : m_summary.edges) {
            if (edge.to != index)
                continue;
            const SummaryNode& from = m_summary.nodes[edge.from];
            Operand operand = m_operands[edge.from];
            bool isPrimitive = from.kind == SummaryNode::Kind::Constant && holdsOnlyPrimitives(TypeID::fromBits(from.first));
            if (operand.isOther() && isPrimitive)
                continue;
            m_helper.result = PrototypeHelper::Result::IsUnknown;
        }
        uint32_t onlyFrom = noSummaryNode;
        bool hasOne = true;
        for (const SummaryEdge& edge : m_summary.edges) {
            if (edge.to != index)
                continue;
            hasOne &= onlyFrom == noSummaryNode && !edge.passes;
            onlyFrom = edge.from;
        }
        if (hasOne && onlyFrom != noSummaryNode && m_operands[onlyFrom].kind == Operand::Kind::Parameter && m_summary.nodes[onlyFrom].kind == SummaryNode::Kind::Parameter) {
            m_helper.result = PrototypeHelper::Result::IsParameter;
            m_helper.returnedParameter = m_operands[onlyFrom].parameter;
        }
    }
    return true;
}

LinkFlow::PrototypeHelper LinkFlow::PrototypeHelperReader::read()
{
    PrototypeHelper none;
    if (!m_function.hasBody() || m_function.isCoroutine() || isClassConstructorOf(m_function.kind) || m_function.numberOfPositionalParameters() < 2 || m_function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest })
        || m_summary.has(SummaryTrait::HasDirectEval) || m_summary.has(SummaryTrait::UsesArguments) || m_summary.has(SummaryTrait::Suspends) || m_summary.has(SummaryTrait::OnlyPassesOnGatheredArguments))
        return none;
    if (!findOperands() || !hasNoOtherEffect())
        return none;
    for (const SummaryAccess& access : m_summary.accesses) {
        if (!readAccess(access))
            return none;
    }
    for (const SummaryChange& change : m_summary.changes) {
        if (!readChange(change))
            return none;
    }
    for (uint32_t call = 0; call < m_summary.calls.size(); ++call) {
        if (!readCall(call))
            return none;
    }
    for (const SummaryAccess& access : m_summary.accesses) {
        Operand subject = operandAt(access.subject);
        if (access.kind != SummaryAccess::Kind::Store || access.isToContents() || access.key != m_prototypeKey || subject.kind != Operand::Kind::Parameter)
            continue;
        Step step;
        step.kind = Step::Kind::LaysPrototype;
        step.object = subject;
        add(step);
        if (access.storesObjectMadeForIt)
            continue;
        Vector<uint32_t, 2> laid;
        if (access.value == noSummaryNode || !findObjectsLaid(access.value, laid))
            return none;
        for (uint32_t call : laid) {
            if (!takeObjectLaid(subject, call))
                return none;
        }
    }
    for (uint32_t index = 0; index < m_summary.nodes.size(); ++index) {
        FunctionID named = functionNamedBy(index);
        if (!named)
            continue;
        bool isSurrogate = m_parentsOfSurrogates.contains(named.index());
        bool capturesOperand = m_summary.nodes[index].kind == SummaryNode::Kind::Closure && std::ranges::any_of(m_captures, [&](const auto& captured) { return captured.key.first == named.index(); });
        if (!isSurrogate && !capturesOperand)
            continue;
        if (!isSurrogate || std::ranges::any_of(m_summary.edges, [&](const SummaryEdge& edge) { return edge.from == index; }) || std::ranges::contains(m_summary.arguments, index))
            return none;
        for (const SummaryAccess& access : m_summary.accesses) {
            bool storesPrototype = access.subject == index && access.kind == SummaryAccess::Kind::Store && access.key == m_prototypeKey;
            if ((access.subject == index && !storesPrototype) || access.receiver == index || access.value == index || access.keyNode == index)
                return none;
        }
        for (uint32_t place = 0; place < m_summary.calls.size(); ++place) {
            const SummaryCall& call = m_summary.calls[place];
            if ((call.subject == index && !m_constructionsLaid.contains(place)) || call.receiver == index || (call.newTarget == index && call.subject != index))
                return none;
        }
        for (const SummaryChange& change : m_summary.changes) {
            if (change.subject == index || change.other == index)
                return none;
        }
    }
    for (const auto& captured : m_captures) {
        bool isOfClosureMadeHere = std::ranges::any_of(m_summary.nodes, [&](const SummaryNode& node) { return node.kind == SummaryNode::Kind::Closure && node.first == captured.key.first; });
        if (!isOfClosureMadeHere)
            return none;
    }
    for (uint32_t place : m_constructionsLaid)
        m_helper.surrogates.append(functionNamedBy(m_summary.calls[place].subject));
    readResult();
    auto take = [&](const Operand& operand) {
        if (!operand.isOther() && !m_helper.parameters.contains(operand.parameter))
            m_helper.parameters.append(operand.parameter);
    };
    for (const Step& step : m_helper.steps) {
        take(step.object);
        take(step.value);
        m_helper.linksOperands |= step.kind == Step::Kind::LaysPrototype || step.kind == Step::Kind::LinksParent;
        if (step.kind != Step::Kind::CallsFunction)
            continue;
        const SummaryCall& call = m_summary.calls[step.call];
        for (uint32_t argument = 0; argument < call.numberOfArguments; ++argument)
            take(operandAt(m_summary.arguments[call.firstArgument + argument]));
    }
    m_helper.isOne = !m_helper.steps.isEmpty();
    if (m_helper.result == PrototypeHelper::Result::IsParameter && !m_helper.parameters.contains(m_helper.returnedParameter))
        m_helper.result = PrototypeHelper::Result::IsOther;
    return WTF::move(m_helper);
}

const LinkFlow::PrototypeHelper* LinkFlow::findPrototypeHelper(FunctionID function)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    auto& helper = m_prototypeHelpers.ensure(function.index(), [&] {
        return makeUniqueWithoutFastMallocCheck<PrototypeHelper>(PrototypeHelperReader(*this, function).read());
    }).iterator->value;
    return helper->isOne ? helper.get() : nullptr;
}

LayoutID LinkFlow::classOfHolder(FunctionID function) const
{
    if (LayoutID layout = layoutOfConstructorFunction(function))
        return layout;
    const Function& record = program.at(function);
    return isClassConstructorOf(record.kind) ? record.constructedClass : LayoutID();
}

LinkFlow::HelperProgress LinkFlow::visitPrototypeHelper(const HelperEntered& helperEntered, Vector<HelperEntered, 4>& entered, const Invocable<bool(const PrototypeHelperStep&)> auto& visit, const Invocable<void(FlowNodeID)> auto& watch) const
{
    using Operand = PrototypeHelper::Operand;
    if (!helperEntered.helper)
        return HelperProgress::GivesUp;
    const PrototypeHelper& helper = *helperEntered.helper;
    const ThinSummary& summary = summaries.at(helperEntered.function);
    auto functionAt = [&](FlowNodeID node) {
        const LinkType& held = typeAt(node);
        LinkType alone;
        alone.primitives = held.primitives;
        alone.escapeOnlyPrimitives = held.escapeOnlyPrimitives;
        alone.functions = LinkType::Functions::Closure;
        alone.function = held.function;
        return held.functions == LinkType::Functions::Closure && held == alone && !(held.primitives & ~Primitive::Undefined) ? held.function : FunctionID();
    };
    for (uint32_t parameter : helper.parameters) {
        FlowNodeID node = helperEntered.arguments[parameter];
        if (node == unseen())
            return HelperProgress::GivesUp;
        if (typeAt(node).isNever())
            return HelperProgress::Waits;
        if (!functionAt(node))
            return HelperProgress::GivesUp;
    }
    auto functionOf = [&](const Operand& operand) {
        return operand.isOther() ? FunctionID() : functionAt(helperEntered.arguments[operand.parameter]);
    };
    for (const PrototypeHelper::Step& step : helper.steps) {
        if (step.kind != PrototypeHelper::Step::Kind::CallsFunction) {
            PrototypeHelperStep taken;
            taken.kind = step.kind;
            taken.writer = helperEntered.function;
            taken.object = functionOf(step.object);
            taken.objectIsPrototype = step.object.kind == Operand::Kind::PrototypeOfParameter;
            taken.value = functionOf(step.value);
            taken.valueIsPrototype = step.value.kind == Operand::Kind::PrototypeOfParameter;
            taken.key = step.key;
            taken.stored = step.value.kind == Operand::Kind::Parameter ? helperEntered.arguments[step.value.parameter] : step.stored;
            if (step.kind == PrototypeHelper::Step::Kind::AsksOwnName && step.stored)
                watch(step.stored);
            if (!layoutOfConstructorFunction(taken.object) || !visit(taken))
                return HelperProgress::GivesUp;
            continue;
        }

        const SummaryCall& call = summary.calls[step.call];
        auto operandOfArgument = [&](uint32_t place) -> std::optional<uint32_t> {
            if (place >= call.numberOfArguments)
                return std::nullopt;
            const SummaryNode& node = summary.nodes[summary.arguments[call.firstArgument + place]];
            return node.kind == SummaryNode::Kind::Parameter ? std::optional(node.first) : std::nullopt;
        };
        FlowNodeID calleeNode = nodeOf(helperEntered.function, call.subject);
        watch(calleeNode);
        LinkType callee = typeAt(calleeNode);
        callee.primitives = 0;
        bool setsPrototype = false;
        if (callee.builtin) {
            BuiltinID called = program.builtins().canonicalID(callee.builtin);
            if (called != program.builtins().canonicalID(Builtins::ObjectSetPrototypeOf) && called != program.builtins().canonicalID(Builtins::ReflectSetPrototypeOf))
                return HelperProgress::GivesUp;
            setsPrototype = true;
        }
        if (callee.classes != LinkType::Classes::None) {
            if (callee.classes != LinkType::Classes::Site || callee.builtinClass != BuiltinClasses::BoundFunction)
                return HelperProgress::GivesUp;
            FlowNodeID target = findContentsOf(callee, slotOf(program.builtins().findSlot(BuiltinClasses::BoundFunction, ContentsSlot::Value)));
            FlowNodeID boundArguments = findContentsOf(callee, slotOf(program.builtins().findSlot(BuiltinClasses::BoundFunction, ContentsSlot::Element)));
            if (boundArguments) {
                watch(boundArguments);
                if (!typeAt(boundArguments).isNever())
                    return HelperProgress::GivesUp;
            }
            if (!target)
                return HelperProgress::GivesUp;
            watch(target);
            const LinkType& bound = typeAt(target);
            LinkType builtinAlone;
            builtinAlone.builtin = bound.builtin;
            if (!bound.isNever()) {
                if (!bound.builtin || !(bound == builtinAlone) || program.builtins().canonicalID(bound.builtin) != program.builtins().canonicalID(Builtins::ObjectSetPrototypeOf))
                    return HelperProgress::GivesUp;
                setsPrototype = true;
            }
        }
        if (callee.objects != LinkType::Objects::None || callee.hasUnknownObjects || callee.holdsLostBuiltins)
            return HelperProgress::GivesUp;
        if (setsPrototype) {
            std::optional<uint32_t> child = operandOfArgument(0);
            std::optional<uint32_t> parent = operandOfArgument(1);
            if (!child || !parent || step.resultIsUsed)
                return HelperProgress::GivesUp;
            PrototypeHelperStep taken;
            taken.kind = PrototypeHelper::Step::Kind::LinksParent;
            taken.writer = helperEntered.function;
            taken.object = functionAt(helperEntered.arguments[*child]);
            taken.value = functionAt(helperEntered.arguments[*parent]);
            if (!taken.object || !taken.value || !layoutOfConstructorFunction(taken.object) || !visit(taken))
                return HelperProgress::GivesUp;
        }
        Vector<FunctionID, 4> functions;
        switch (callee.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            functions.append(callee.function);
            break;
        case LinkType::Functions::Set:
            functions.append(functionsOfSet(callee));
            break;
        case LinkType::Functions::Constructors:
        case LinkType::Functions::Lost:
            return HelperProgress::GivesUp;
        }
        for (FunctionID function : functions) {
            const Function& record = program.at(function);
            const PrototypeHelper* called = knownPrototypeHelper(function);
            if (!called || (step.resultIsUsed && called->result != PrototypeHelper::Result::IsOther))
                return HelperProgress::GivesUp;
            HelperEntered inner;
            inner.helper = called;
            inner.function = function;
            inner.arguments.fill(unseen(), record.numberOfEntryParameters());
            for (uint32_t position = 0; position < record.numberOfPositionalParameters(); ++position) {
                if (std::optional<uint32_t> operand = operandOfArgument(position); operand && helper.parameters.contains(*operand))
                    inner.arguments[record.firstParameterIndex() + position] = helperEntered.arguments[*operand];
            }
            bool isEntered = false;
            for (const HelperEntered& outer : entered) {
                if (outer.helper != called)
                    continue;
                if (outer.arguments != inner.arguments)
                    return HelperProgress::GivesUp;
                isEntered = true;
            }
            if (isEntered)
                continue;
            entered.append(inner);
            HelperProgress progress = visitPrototypeHelper(inner, entered, visit, watch);
            entered.removeLast();
            if (progress != HelperProgress::IsDone)
                return progress;
        }
    }
    return HelperProgress::IsDone;
}

bool LinkFlow::someBodyStoresHasOwnProperty() const
{
    if (!m_someBodyStoresHasOwnProperty) {
        PropertyKeyID key = program.keyOf(BuiltinNames::hasOwnProperty);
        bool isStored = false;
        for (uint32_t index = 0; index < program.functions.size() && !isStored; ++index) {
            if (!program.at(FunctionID(index)).hasBody())
                continue;
            const ThinSummary& summary = summaries.at(FunctionID(index));
            isStored = std::ranges::any_of(summary.accesses, [&](const SummaryAccess& access) { return access.key == key && (access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define); })
                || std::ranges::contains(summary.changes, key, &SummaryChange::key);
        }
        m_someBodyStoresHasOwnProperty = isStored;
    }
    return *m_someBodyStoresHasOwnProperty;
}

bool LinkFlow::findsOwnNameBuiltin(const PrototypeHelperStep& step) const
{
    for (BuiltinName name : { BuiltinNames::hasOwnProperty, BuiltinNames::call }) {
        if (StringID text = program.stringOf(name); text && program.isOpenedSomewhere(text))
            return false;
    }
    if (!step.stored)
        return everyFunctionAboveIsConstructorFunction(step.object);
    const LinkType& held = typeAt(step.stored);
    LinkType builtinAlone;
    builtinAlone.builtin = held.builtin;
    builtinAlone.primitives = held.primitives;
    bool isBuiltinAlone = held == builtinAlone && !(held.primitives & ~Primitive::UndefinedOrNull);
    return held.isNever() || (isBuiltinAlone && (!held.builtin || program.builtins().canonicalID(held.builtin) == program.builtins().canonicalID(Builtins::ObjectPrototypeHasOwnProperty)));
}

bool LinkFlow::takePrototypeHelperStep(const PrototypeHelperStep& step)
{
    auto prototypeOf = [&](LayoutID layout) {
        LinkType type;
        type.objects = LinkType::Objects::Prototype;
        type.layout = layout;
        return type;
    };
    auto closuresOf = [&](FunctionID function) {
        LinkType type;
        type.functions = LinkType::Functions::Closure;
        type.function = function;
        return type;
    };
    LayoutID child = layoutOfConstructorFunction(step.object);
    LayoutID classOfValue = step.value ? classOfHolder(step.value) : LayoutID();
    switch (step.kind) {
    case PrototypeHelper::Step::Kind::LaysPrototype:
        if (child)
            notePrototypeIsReplaced(child);
        return true;
    case PrototypeHelper::Step::Kind::LinksParent:
        if (step.objectIsPrototype != step.valueIsPrototype || !classOfValue)
            return false;
        if (step.objectIsPrototype) {
            if (!namesParentOfPrototype(prototypeOf(classOfValue), child))
                return false;
            noteWhatMayMakeThenableOf(prototypeOf(child), step.writer, std::nullopt);
            return true;
        }
        if (!namesParentOfFunction(closuresOf(step.value), step.object))
            return false;
        noteWhatMayMakeThenableOf(closuresOf(step.object), step.writer, std::nullopt);
        linkFunctionBelow(step.object, step.value);
        return true;
    case PrototypeHelper::Step::Kind::StoresName: {
        if (step.valueIsPrototype && !classOfValue)
            return false;
        uint64_t place = static_cast<uint64_t>(child.index()) << 32 | step.key.bits();
        uint64_t stored = static_cast<uint64_t>(step.valueIsPrototype) << 33 | static_cast<uint64_t>(step.objectIsPrototype) << 32 | (step.valueIsPrototype ? classOfValue.index() : step.stored.index());
        if (!m_namesStoredByHelpers.add({ place, stored }).isNewEntry)
            return true;
        FlowNodeID value = step.stored;
        if (step.valueIsPrototype) {
            value = makeSharedNode();
            arrive(value, prototypeOf(classOfValue));
        }
        storeOnClassHolder(step.writer, child, step.objectIsPrototype ? ClassSide::Prototype : ClassSide::Static, step.key, value);
        return true;
    }
    case PrototypeHelper::Step::Kind::CopiesNames:
        if (!everyFunctionAboveIsConstructorFunction(step.value))
            return false;
        noteWhatMayMakeThenableOf(closuresOf(step.object), step.writer, std::nullopt);
        linkFunctionBelow(step.object, step.value);
        return true;
    case PrototypeHelper::Step::Kind::AsksOwnName:
        return findsOwnNameBuiltin(step);
    case PrototypeHelper::Step::Kind::CallsFunction:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const LinkFlow::PrototypeHelper* LinkFlow::knownPrototypeHelper(FunctionID function) const
{
    auto found = m_prototypeHelpers.find(function.index());
    return found != m_prototypeHelpers.end() && found->value->isOne ? found->value.get() : nullptr;
}

LinkFlow::HelperEntered LinkFlow::enteredBy(const PrototypeHelperCall& call) const
{
    HelperEntered entered;
    entered.helper = knownPrototypeHelper(call.function);
    entered.function = call.function;
    entered.arguments = call.arguments;
    return entered;
}

void LinkFlow::followPrototypeHelperCall(uint32_t call, FunctionID helper, Vector<FlowNodeID, 4>&& arguments)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!m_prototypeHelperCallsListed.add({ call + 1, helper.index() + 1 }).isNewEntry)
        return;
    uint32_t record = static_cast<uint32_t>(m_prototypeHelperCalls.size());
    auto made = makeUniqueWithoutFastMallocCheck<PrototypeHelperCall>();
    made->function = helper;
    made->arguments = WTF::move(arguments);
    m_prototypeHelperCalls.append(WTF::move(made));
    for (FlowNodeID argument : m_prototypeHelperCalls[record]->arguments) {
        if (argument != unseen())
            observe(argument, FlowObserver { FlowObserver::Kind::PassedToBeLinked, helper, record });
    }
}

void LinkFlow::abandonHelperCall(PrototypeHelperCall& call)
{
    call.state = PrototypeHelperCall::State::IsGivenUp;
    for (uint32_t parameter = 0; parameter < call.arguments.size(); ++parameter) {
        if (call.arguments[parameter] != unseen())
            addEdge(call.arguments[parameter], this->parameter(call.function, parameter));
    }
}

void LinkFlow::prototypeHelperCallObserved(const FlowObserver& observer)
{
    PrototypeHelperCall& call = *m_prototypeHelperCalls[observer.record];
    if (call.state == PrototypeHelperCall::State::IsGivenUp)
        return;
    auto watch = [&](FlowNodeID node) {
        FlowObserver again { FlowObserver::Kind::PassedToBeLinked, call.function, observer.record };
        if (isWatchedForFirstTime(node, again))
            observe(node, again);
    };
    readCalledHelpers(call.function);
    HelperEntered root = enteredBy(call);
    Vector<HelperEntered, 4> entered { root };
    switch (visitPrototypeHelper(root, entered, [&](const PrototypeHelperStep& step) { return takePrototypeHelperStep(step); }, watch)) {
    case HelperProgress::Waits:
        return;
    case HelperProgress::IsDone:
        call.state = PrototypeHelperCall::State::IsLinked;
        return;
    case HelperProgress::GivesUp:
        abandonHelperCall(call);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void LinkFlow::readCalledHelpers(FunctionID function)
{
    Vector<FunctionID, 4> pending { function };
    Vector<FunctionID, 8> listed;
    while (!pending.isEmpty()) {
        FunctionID current = pending.takeLast();
        if (listed.contains(current))
            continue;
        listed.append(current);
        const PrototypeHelper* helper = findPrototypeHelper(current);
        if (!helper)
            continue;
        for (const PrototypeHelper::Step& step : helper->steps) {
            if (step.kind != PrototypeHelper::Step::Kind::CallsFunction)
                continue;
            const LinkType& callee = typeAt(nodeOf(current, summaries.at(current).calls[step.call].subject));
            if (callee.functions == LinkType::Functions::Closure)
                pending.append(callee.function);
            else if (callee.functions == LinkType::Functions::Set)
                pending.append(functionsOfSet(callee));
        }
    }
}

Vector<LinkFlow::PrototypeHelperStep> LinkFlow::prototypeHelperSteps() const
{
    Vector<PrototypeHelperStep> steps;
    for (const auto& call : m_prototypeHelperCalls) {
        if (call->state != PrototypeHelperCall::State::IsLinked)
            continue;
        HelperEntered root = enteredBy(*call);
        Vector<HelperEntered, 4> entered { root };
        visitPrototypeHelper(root, entered, [&](const PrototypeHelperStep& step) {
            steps.append(step);
            return true;
        }, [](FlowNodeID) { });
    }
    return steps;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
