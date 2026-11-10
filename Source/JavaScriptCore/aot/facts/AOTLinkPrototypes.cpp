/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

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
    size_t numberOfLayouts = program.layouts.size();
    for (BitVector* byLayout : { &m_layoutsOfConstructorFunctions, &m_layoutsBelowConstructorFunctions, &m_prototypesPassedByLookups, &m_prototypesWithDefinitions, &m_prototypesAboveLostLayouts, &m_prototypesPassedByNumbers })
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
        if (underKey && (underKey->isStoredInto || underKey->mayHoldAccessor()))
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
        if (m_prototypesAboveLostLayouts.get(layout.index()) || mayBeNamedByNumber(key))
            addEdge(stored, foundBesideFieldsUnder(key));
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

FlowNodeID LinkFlow::storedOnPrototypeOf(LayoutID layout, PropertyKeyID key, bool isLaidByBodyOfFile)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    RELEASE_ASSERT(key);
    uint32_t record = recordOfNameOnPrototype(layout, key);
    if (!isLaidByBodyOfFile)
        m_namesOnPrototypes[record].isStoredIntoFromOutside = true;
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
    if (!isDefinedByDescriptorSomewhere(key) || !m_layoutsBelowConstructorFunctions.get(layout.index()))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!isOfConstructorFunction(current))
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
    if (!isDefinedByDescriptorSomewhere(key) || !m_layoutsBelowConstructorFunctions.get(layout.index()))
        return halves;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (isOfConstructorFunction(current)) {
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
    if (!std::exchange(m_namesOnPrototypes[record].isAccessorAsWritten, true))
        noteFieldsThatAccessorAboveMayLeaveEmpty(layout, key);
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
    if (!m_layoutsBelowConstructorFunctions.get(layout.index()))
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
    if (!m_layoutsBelowConstructorFunctions.get(layout.index()))
        return;
    if (accessors == AccessorsAbove::AreEnteredByNobody)
        letAccessorsOnPrototypesAboveEscape(layout, key);
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

FlowNodeID LinkFlow::objectsMadeThroughConstructorOf(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    auto added = m_nodesOfObjectsMadeThroughConstructors.add(layout.index(), UINT32_MAX);
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
    if (!m_layoutsBelowConstructorFunctions.get(lost.index()))
        return;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = lost; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        if (!isOfConstructorFunction(current) || m_prototypesAboveLostLayouts.set(current.index()))
            continue;
        for (uint32_t record = firstNameOnPrototypeOf(current); record != UINT32_MAX; record = m_namesOnPrototypes[record].nextOfLayout) {
            NameOnPrototype name = m_namesOnPrototypes[record];
            if (name.key)
                addEdge(name.stored, foundBesideFieldsUnder(name.key));
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
    if (!isOfConstructorFunction(layout))
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
