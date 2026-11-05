/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void appendMembersAssignedToPrototype(const Program& program, const LinkTables& tables, LayoutID layout, Vector<ClassMember>& members)
{
    if (!program.at(layout).has(LayoutFlags::IsConstructorFunction))
        return;
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
        if (!name.expected)
            continue;
        ClassMember member;
        member.key = name.key;
        member.side = ClassSide::Prototype;
        member.kind = MethodKind::Method;
        member.function = name.expected;
        member.isWritableData = true;
        member.isAssignedByProgram = true;
        members.append(member);
    }
}

static uint64_t wordOfNameInLinkPrototypes(LayoutID layout, PropertyKeyID key)
{
    return static_cast<uint64_t>(layout.index()) << 32 | key.bits();
}

void LinkFlow::indexConstructorFunctions()
{
    size_t numberOfLayouts = program.layouts.size();
    for (BitVector* byLayout : { &m_layoutsOfConstructorFunctions, &m_layoutsBelowConstructorFunctions, &m_prototypesPassedByLookups, &m_prototypesWithDefinitions, &m_prototypesAboveLostLayouts, &m_prototypesPassedByNumbers })
        byLayout->ensureSize(numberOfLayouts);

    for (size_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.has(LayoutFlags::IsConstructorFunction) || !layout.constructor)
            continue;
        const Function& constructor = program.at(layout.constructor);
        if (constructor.constructedClass != LayoutID(index) || !constructor.takesThisFromConstruction())
            continue;
        m_layoutsOfConstructorFunctions.quickSet(index);
        m_layoutsBelowConstructorFunctions.quickSet(index);
        for (LayoutID below : program.layoutsExpectedBelow(LayoutID(index)))
            m_layoutsBelowConstructorFunctions.quickSet(below.index());
    }
}

LayoutID LinkFlow::layoutOfConstructorFunction(FunctionID function) const
{
    LayoutID constructed = program.at(function).constructedClass;
    return constructed && isOfConstructorFunction(constructed) && program.at(constructed).constructor == function ? constructed : LayoutID();
}

const LinkFlow::NameOnPrototype* LinkFlow::findNameOnPrototype(LayoutID layout, PropertyKeyID key) const
{
    auto found = m_recordsOfNamesOnPrototypes.find(wordOfNameInLinkPrototypes(layout, key));
    return found == m_recordsOfNamesOnPrototypes.end() ? nullptr : &m_namesOnPrototypes[found->value];
}

bool LinkFlow::mayLieOnPrototypeAbove(LayoutID layout, PropertyKeyID key, bool countsKeysThatNobodyKnows) const
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!isBelowConstructorFunction(layout))
        return false;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!isOfConstructorFunction(current))
            continue;
        const NameOnPrototype* underKey = findNameOnPrototype(current, key);
        const NameOnPrototype* underSomeKey = findNameOnPrototype(current, { });
        if ((underKey && (underKey->isStoredInto || underKey->isDefinedByDescriptor)) || (countsKeysThatNobodyKnows && underSomeKey && underSomeKey->isDefinedByDescriptor))
            return true;
    }
    return false;
}

uint32_t LinkFlow::recordOfNameOnPrototype(LayoutID layout, PropertyKeyID key)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    RELEASE_ASSERT(isOfConstructorFunction(layout));
    uint32_t record = static_cast<uint32_t>(m_namesOnPrototypes.size());
    auto added = m_recordsOfNamesOnPrototypes.add(wordOfNameInLinkPrototypes(layout, key), record);
    if (!added.isNewEntry)
        return added.iterator->value;

    FlowNodeID stored = makeNode();
    FlowNodeID storedInSight = makeNode();
    m_namesOnPrototypes.append(NameOnPrototype { layout, key, stored, storedInSight });
    addEdge(storedInSight, stored);
    if (key) {
        if (const NameOnPrototype* underAnyKey = findNameOnPrototype(layout, { }); underAnyKey && underAnyKey->isReadUnderAnyKey)
            addEdge(stored, underAnyKey->stored);
        if (m_prototypesAboveLostLayouts.get(layout.index()) || mayBeNamedByNumber(key))
            addEdge(stored, foundBesideFieldsUnder(key));
    }
    if (escapedLayouts.get(layout.index())) {
        arrive(stored, LinkType::anything().byEscapeAlone());
        addEdge(stored, unseen());
    }
    return record;
}

FlowNodeID LinkFlow::storedOnPrototypeOf(LayoutID layout, PropertyKeyID key)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    RELEASE_ASSERT(key);
    uint32_t record = recordOfNameOnPrototype(layout, key);
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
        if (isOfConstructorFunction(current)) {
            uint32_t record = recordOfNameOnPrototype(current, key);
            m_namesOnPrototypes[record].isPassedByLookups = true;
            m_prototypesPassedByLookups.set(current.index());
            functor(record);

            bool mayFindAccessor = m_prototypesWithDefinitions.get(current.index());
            if (mayFindAccessor && key) {
                const NameOnPrototype* underSomeKey = findNameOnPrototype(current, { });
                mayFindAccessor = m_namesOnPrototypes[record].isDefinedByDescriptor || (underSomeKey && underSomeKey->isDefinedByDescriptor);
            }
            if (mayFindAccessor)
                deferEscape(current);
        }
        if (key && !std::ranges::equal_range(membersOf(current, ClassSide::Prototype), key, { }, &MemberOfClass::key).empty())
            break;
    }
    settle();
}

FlowNodeID LinkFlow::foundOnPrototypesAbove(LayoutID layout, PropertyKeyID key)
{
    if (!m_layoutsBelowConstructorFunctions.get(layout.index()))
        return { };
    return nodeAskedOnceByLane(MadeOnDemand::FoundOnPrototypesAbove, static_cast<uint64_t>(layout.index()) << 32 | key.bits(), [&]() -> FlowNodeID {
        auto added = m_nodesFoundOnPrototypesAbove.add(wordOfNameInLinkPrototypes(layout, key), UINT32_MAX);
        if (!added.isNewEntry)
            return FlowNodeID(added.iterator->value);
        FlowNodeID found = makeNode();
        added.iterator->value = found.index();

        forEachPrototypePassedAbove(layout, key, [&](uint32_t record) {
            NameOnPrototype passed = m_namesOnPrototypes[record];
            if (!key && !std::exchange(m_namesOnPrototypes[record].isReadUnderAnyKey, true)) {
                for (size_t other = 0; other < m_namesOnPrototypes.size(); ++other) {
                    NameOnPrototype name = m_namesOnPrototypes[other];
                    if (name.layout == passed.layout && name.key)
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
    if (!m_layoutsBelowConstructorFunctions.get(layout.index()))
        return { };
    passPrototypesAboveUnderNumbers(layout);
    return foundBesideFieldsUnderAnyNumber();
}

void LinkFlow::passPrototypesAboveUnderNumbers(LayoutID layout)
{
    if (!m_layoutsBelowConstructorFunctions.get(layout.index()))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!isOfConstructorFunction(current) || m_prototypesPassedByNumbers.set(current.index()) || !m_prototypesWithDefinitions.get(current.index()))
            continue;
        bool mayFindAccessor = std::ranges::any_of(m_namesOnPrototypes, [&](const NameOnPrototype& name) {
            return name.layout == current && name.isDefinedByDescriptor && mayBeNamedByNumber(name.key);
        });
        if (mayFindAccessor)
            deferEscape(current);
    }
    settle();
}

void LinkFlow::lookForSetterOnPrototypesAbove(LayoutID layout, PropertyKeyID key)
{
    if (!m_layoutsBelowConstructorFunctions.get(layout.index()))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    forEachPrototypePassedAbove(layout, key, [](uint32_t) { });
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

void LinkFlow::defineOnPrototypeOf(LayoutID layout, PropertyKeyID key)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    uint32_t record = recordOfNameOnPrototype(layout, key);
    m_namesOnPrototypes[record].isDefinedByDescriptor = true;
    m_prototypesWithDefinitions.set(layout.index());
    arrive(m_namesOnPrototypes[record].stored, LinkType::anything());

    bool isFoundByLookups = m_prototypesPassedByLookups.get(layout.index());
    if (isFoundByLookups && key) {
        const NameOnPrototype* underAnyKey = findNameOnPrototype(layout, { });
        isFoundByLookups = m_namesOnPrototypes[record].isPassedByLookups || (underAnyKey && underAnyKey->isPassedByLookups);
    }
    isFoundByLookups |= m_prototypesPassedByNumbers.get(layout.index()) && mayBeNamedByNumber(key);
    if (isFoundByLookups)
        deferEscape(layout);
    settle();
}

void LinkFlow::tiePrototypesAboveLostLayout(LayoutID lost)
{
    if (!m_layoutsBelowConstructorFunctions.get(lost.index()))
        return;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = lost; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!isOfConstructorFunction(current) || m_prototypesAboveLostLayouts.set(current.index()))
            continue;
        for (size_t record = 0; record < m_namesOnPrototypes.size(); ++record) {
            NameOnPrototype name = m_namesOnPrototypes[record];
            if (name.layout == current && name.key)
                addEdge(name.stored, foundBesideFieldsUnder(name.key));
        }
    }
}

bool LinkFlow::letPrototypesFoundThroughLostEscape()
{
    bool hasDeferred = false;
    for (size_t record = 0; record < m_namesOnPrototypes.size(); ++record) {
        NameOnPrototype name = m_namesOnPrototypes[record];
        if (!name.isDefinedByDescriptor || !m_prototypesAboveLostLayouts.get(name.layout.index()) || escapedLayouts.get(name.layout.index()))
            continue;
        bool isLookedUp = m_anyFieldIsReadThroughLost || (name.key ? m_keysLookedUpThroughLost.contains(name.key.index()) : !m_keysLookedUpThroughLost.isEmpty());
        if (isLookedUp)
            hasDeferred |= deferEscape(name.layout);
    }
    settle();
    return hasDeferred;
}

void LinkFlow::openPrototypeOf(LayoutID layout)
{
    if (!isOfConstructorFunction(layout))
        return;
    for (size_t record = 0; record < m_namesOnPrototypes.size(); ++record) {
        if (m_namesOnPrototypes[record].layout != layout)
            continue;
        FlowNodeID stored = m_namesOnPrototypes[record].stored;
        arrive(stored, LinkType::anything().byEscapeAlone());
        addEdge(stored, unseen());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
