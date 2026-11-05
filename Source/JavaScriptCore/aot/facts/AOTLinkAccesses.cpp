/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLinkStrings.h"
#include "AOTLinkExceptions.h"
#include "AOTNameClass.h"
#include "AOTPrimitives.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static LinkType primitivesForLinkAccesses(uint16_t primitives)
{
    LinkType type;
    type.primitives = LinkType::closed(primitives);
    return type;
}

static LinkType instanceForLinkAccesses(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Layout;
    type.layout = layout;
    return type;
}

static LinkType familyForLinkAccesses(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Family;
    type.family = family;
    return type;
}

static LinkType prototypeForLinkAccesses(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototype;
    type.layout = layout;
    return type;
}

static LinkType prototypesForLinkAccesses(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototypes;
    type.family = family;
    return type;
}

static LinkType constructorsForLinkAccesses(FamilyID family)
{
    LinkType type;
    type.functions = LinkType::Functions::Constructors;
    type.constructorsOf = family;
    return type;
}

static LinkType closureForLinkAccesses(FunctionID function)
{
    LinkType type;
    type.functions = LinkType::Functions::Closure;
    type.function = function;
    return type;
}

static LinkType instancesAmongForLinkAccesses(const LinkType& type)
{
    LinkType instances;
    instances.classes = type.classes;
    instances.builtinClass = type.builtinClass;
    instances.site = type.site;
    return instances;
}

static bool holdsObjectsForLinkAccesses(const LinkType& type)
{
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.holdsUnseen || type.builtin;
}

struct SharedAccessInLinkAccesses {
    enum class Subjects : uint8_t { PrototypeObjects, ClassObjects };

    Subjects subjects { Subjects::PrototypeObjects };
    FunctionID firstFunction;
    SummaryAccess access;
    bool keyIsOnlyNumbers { false };
    bool keyMayNamePrototypeLink { false };
    FlowNodeID subject;
    FlowNodeID value;
    FlowNodeID thrown;
};

struct MemberOfSharedAccessInLinkAccesses {
    uint32_t sharedAccess { 0 };
    LayoutID layout;
};

class StateOfLinkAccesses {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkAccesses);
    WTF_MAKE_NONCOPYABLE(StateOfLinkAccesses);
public:
    StateOfLinkAccesses() = default;

    Vector<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> lookups;
    Vector<uint8_t> codeFoundAboveClosures;

    HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> indexesOfSharedAccesses;
    Vector<std::unique_ptr<SharedAccessInLinkAccesses>> sharedAccesses;
    Vector<MemberOfSharedAccessInLinkAccesses> membersOfSharedAccesses;
};

void DeleterOfStateOfLinkAccesses::operator()(StateOfLinkAccesses* state) const
{
    delete state;
}

static bool isAboveHomeObjectOfClassForLinkAccesses(const LinkType& type)
{
    bool namesOnlyPrototypes = type.objects == LinkType::Objects::None || type.objects == LinkType::Objects::Prototype || type.objects == LinkType::Objects::Prototypes;
    bool namesOnlyClassObjects = type.functions == LinkType::Functions::None || type.functions == LinkType::Functions::Closure || type.functions == LinkType::Functions::Constructors;
    return namesOnlyPrototypes && namesOnlyClassObjects && type.classes == LinkType::Classes::None && !type.holdsUnseen && !type.builtin && !(type.primitives & ~Primitive::UndefinedOrNull);
}

namespace {

enum class LookupInLinkAccesses : uint8_t { Absent, Found, Unknown };

struct PrivatePartsInLinkAccesses {
    const LinkType& typeObservedBefore;
    const Vector<uint32_t>& firstFieldOfLayout;
    StateOfLinkAccesses& state;
};

class AccessInLinkAccesses {
public:
    AccessInLinkAccesses(LinkFlow&, const PrivatePartsInLinkAccesses&, const FlowObserver&);
    void goByKey(PropertyKeyID key)
    {
        m_access.key = key;
        m_nameClass = m_flow.nameClassOf(key);
    }
    AccessInLinkAccesses(LinkFlow&, const PrivatePartsInLinkAccesses&, const SharedAccessInLinkAccesses&, uint32_t member, bool laysObservers);

    void resolve();
    void resolveMemberOfSharedAccess();

private:
    using Kind = SummaryAccess::Kind;
    using SharedAccess = SharedAccessInLinkAccesses;

    bool isRead() const { return m_access.kind == Kind::Read; }
    bool isNamed() const { return !!m_access.key; }
    bool isTakenByLoader() const { return isRead() && m_function && isLibraryCode(m_program, m_program.at(m_function)) && std::ranges::contains(m_flow.summaries.at(m_function).namesThatLoaderTakes, m_access.value, &ThinSummary::NameThatLoaderTakes::read); }
    bool readKey();

    void give(const LinkType&);
    void giveAnything() { give(LinkType::anything()); }
    void givePrimitives(uint16_t primitives) { give(primitivesForLinkAccesses(primitives)); }
    void giveFrom(FlowNodeID);
    void giveWhatIsHeldAt(FlowNodeID);
    void giveWhatIsEstablishedOf(const DeclaredType&);
    void storeInto(FlowNodeID);
    bool storesTextNumberOrBooleanThatIsWritten() const;
    void storeIntoUnseen() { storeInto(m_flow.unseen()); }
    void mayThrow();
    enum class UnknownIs : uint8_t { ByEscapeAlone, InSight };
    void cannotTell(UnknownIs = UnknownIs::InSight);
    void cannotTellWhatRuns();

    void noteChange(LayoutID layout, LayoutChange change)
    {
        if (!m_flow.possibleChanges[layout.index()].contains(change))
            m_flow.note(LinkFlow::Note::ChangeOfLayout, layout.index(), static_cast<uint32_t>(change));
    }
    void noteWrittenAfterConstruction(FieldReference reference)
    {
        uint32_t cell = m_parts.firstFieldOfLayout[reference.layout.index()] + reference.field;
        if (!m_flow.fieldsWrittenAfterConstruction.get(cell))
            m_flow.note(LinkFlow::Note::FieldWrittenAfterConstruction, cell);
    }
    template<typename Functor> void forEachLayoutUpTheBases(LayoutID, const Functor&) const;

    FlowNodeID foundBesideFieldsOf(LayoutID id) { return m_flow.foundBesideFieldsOf(id); }
    FlowNodeID foundBesideFieldsUnderKey() { return m_flow.foundBesideFieldsUnder(m_access.key); }
    void storeBesideFieldsOf(LayoutID);
    bool resolveOnWhatStandsAbove(FlowNodeID above, unsigned stepsLeft);
    bool mayStoreIntoPrototypeLink() const { return m_access.kind == Kind::Store && m_keyMayNamePrototypeLink; }
    bool namesOnlyElements() const { return !isNamed() && m_keyIsOnlyNumbers && indexesAreAbsentFrom(Builtins::ObjectPrototype); }
    bool namesNothingOnPlainClosures() const;

    void resolveUnderPrivateName();
    void resolveOnLayout(LayoutID, const LinkType& pooled);
    void resolvePrototypeLinkOfLayout(LayoutID, const Layout&);
    void resolveOnNamespace(const Layout&);
    void giveExport(ModuleID, uint32_t index);
    bool wasNamedBefore(LayoutID) const;
    void resolveOnSet(const LinkType& subject);
    void resolveOnFamily(FamilyID);
    void resolveOnLostObjects(LayoutID member);
    void resolveOnPrototype(LayoutID);
    void resolveOnPrototypes(FamilyID);
    bool resolveThroughSharedAccess(SharedAccess::Subjects, FamilyID);
    FlowObserver observerOfWhatWasLookedUp() const { return FlowObserver { m_shared ? FlowObserver::Kind::MemberOfSharedAccess : FlowObserver::Kind::MemberOfAccess, m_function, m_record }; }
    FlowNodeID thrownNode() const { return m_shared ? m_shared->thrown : nodeTakingWhatIsThrown(m_flow, m_function, m_access.thrownTo); }
    void letPrototypeEscape(LayoutID id) { m_flow.escapeAllOf(prototypeForLinkAccesses(id)); }

    void resolveOnClosure(FunctionID);
    void resolvePrototypePropertyOf(FunctionID, LayoutID constructed);
    void giveConstructorsOfRowAbove(LayoutID);
    void resolveOnClassObject(FunctionID, LayoutID classLayout);
    void resolveOnConstructors(FamilyID);
    void resolveOnLostFunctions();
    void letClassObjectEscape(FunctionID, LayoutID classLayout);
    bool mayRunCodeFoundAboveClosures();
    bool findsCodeToRunAboveClosures();

    static bool findsGetterAmong(const LinkFlow::FoundMembers& members) { return std::ranges::contains(members, MethodKind::Getter, &LinkFlow::FoundMember::kind); }
    void enterAccessorAt(FlowNodeID member);
    void enterAccessor(FunctionID);
    void layOnReceiverUnlessAccessorAnswers(const LinkFlow::FoundMembers&);

    void resolveOnPrimitives(uint16_t);
    void resolveOnClasses(const LinkType& instances);
    void resolveUnderNameOnClass(const LinkType& instances);
    template<typename Functor> void forEachListOfElements(const LinkType& instances, const Functor&);
    void resolveContents(const LinkType& subject);
    void resolveContentsOfInstances(const LinkType& instances);
    bool settlesPromiseThatCallHasJustMade();
    void handOn(const LinkType&);
    void resolveOnBuiltin(BuiltinID);
    BuiltinID prototypeOfInstancesOf(BuiltinClassID) const;
    bool indexesAreAbsentFrom(BuiltinID start) const;
    bool membersOfObjectPrototypeAreWhatTableLists();
    LookupInLinkAccesses lookUpFrom(BuiltinID start, BuiltinID& member) const;
    LookupInLinkAccesses lookUpInTableFrom(BuiltinID start, BuiltinID& member) const;
    bool giveWhatIsFoundFrom(BuiltinID start);
    bool giveWhatLightFormServes(BuiltinID);
    bool mayRunCodeFoundFrom(BuiltinID start);
    bool nativeSetterFoundFromKeepsReceiver(BuiltinID start);
    bool holdsWhatTableLists(BuiltinID member);
    LinkType typeHeldAtLocation(BuiltinID member);
    LinkType typeHeldAtLocationThatRealmHas(BuiltinID member);
    LinkType typeAnsweredByReadsOf(BuiltinID member);
    LinkType typeWrittenBy(BuiltinTypeID);
    LinkType typeWrittenOfLightForm(BuiltinTypeID, BuiltinID canonical) const;

    LinkFlow& m_flow;
    const PrivatePartsInLinkAccesses& m_parts;
    const Program& m_program;
    const BuiltinTable& m_table;
    FunctionID m_function;
    uint32_t m_record;
    const SharedAccess* m_shared { nullptr };
    SummaryAccess m_access;
    FlowNodeID m_subject;
    FlowNodeID m_receiver;
    FlowNodeID m_value;
    LinkType m_before;
    NameClass m_nameClass { NameClass::Ordinary };
    bool m_laysObservers;
    bool m_keyIsWritten { true };
    void tellAgainOnceClimbs(FlowNodeID);
    bool m_keyIsOnlyNumbers { false };
    bool m_keyIsSymbolAsFarAsIsSeen { false };
    bool m_keyMayNamePrototypeLink { false };
    bool m_hasThrown { false };
    bool m_wasAskedToThrow { false };
    bool m_hasLeanedOnMemberTakenForConstant { false };
};

} // anonymous namespace

AccessInLinkAccesses::AccessInLinkAccesses(LinkFlow& flow, const PrivatePartsInLinkAccesses& parts, const FlowObserver& observer)
    : m_flow(flow)
    , m_parts(parts)
    , m_program(flow.program)
    , m_table(flow.program.builtins())
    , m_function(observer.function)
    , m_record(observer.record)
    , m_access(flow.summaries.at(observer.function).accesses[observer.record])
    , m_subject(flow.nodeOf(observer.function, m_access.subject))
    , m_receiver(m_access.receiver == noSummaryNode ? m_subject : flow.nodeOf(observer.function, m_access.receiver))
    , m_laysObservers(observer.kind == FlowObserver::Kind::SubjectOfAccess || (observer.kind == FlowObserver::Kind::KeyOfAccess && !m_access.isToContents()))
    , m_keyIsWritten(m_access.key)
{
    if (observer.kind == FlowObserver::Kind::SubjectOfAccess)
        m_before = parts.typeObservedBefore;
    if (m_access.value != noSummaryNode) {
        m_value = flow.nodeOf(observer.function, m_access.value);
        flow.prefetchTypeAt(m_value);
    }
    if (m_access.key)
        m_nameClass = flow.nameClassOf(m_access.key);
}

AccessInLinkAccesses::AccessInLinkAccesses(LinkFlow& flow, const PrivatePartsInLinkAccesses& parts, const SharedAccess& shared, uint32_t member, bool laysObservers)
    : m_flow(flow)
    , m_parts(parts)
    , m_program(flow.program)
    , m_table(flow.program.builtins())
    , m_function(shared.firstFunction)
    , m_record(member)
    , m_shared(&shared)
    , m_access(shared.access)
    , m_subject(shared.subject)
    , m_receiver(shared.subject)
    , m_value(shared.value)
    , m_laysObservers(laysObservers)
    , m_keyIsOnlyNumbers(shared.keyIsOnlyNumbers)
    , m_keyMayNamePrototypeLink(shared.keyMayNamePrototypeLink)
{
    if (m_access.key)
        m_nameClass = flow.nameClassOf(m_access.key);
}

void AccessInLinkAccesses::tellAgainOnceClimbs(FlowNodeID node)
{
    if (m_laysObservers)
        m_flow.observe(node, observerOfWhatWasLookedUp());
}

void AccessInLinkAccesses::give(const LinkType& type)
{
    if (isRead() && m_value)
        m_flow.arriveFromSource(m_value, type);
}

void AccessInLinkAccesses::giveFrom(FlowNodeID from)
{
    if (isRead() && m_value)
        m_flow.addEdge(from, m_value);
}

void AccessInLinkAccesses::giveWhatIsHeldAt(FlowNodeID from)
{
    if (isRead() && m_value)
        m_flow.addEdge(from, m_value, TypeTable::anything);
}

void AccessInLinkAccesses::giveWhatIsEstablishedOf(const DeclaredType& declared)
{
    std::optional<ProvenInEveryVersion> established = declared.established();
    if (!established)
        return;
    if (TypeID type = *established)
        give(m_flow.fromTypeID(type));
}

void AccessInLinkAccesses::storeInto(FlowNodeID to)
{
    if (!isRead() && m_value)
        m_flow.addEdge(m_value, to);
}

bool AccessInLinkAccesses::storesTextNumberOrBooleanThatIsWritten() const
{
    if (m_shared || m_access.value == noSummaryNode)
        return false;
    const SummaryNode& stored = m_flow.summaries.at(m_function).nodes[m_access.value];
    if (stored.kind != SummaryNode::Kind::Constant)
        return false;
    LinkType type = m_flow.fromTypeID(TypeID::fromBits(stored.first));
    return !holdsObjectsForLinkAccesses(type) && !(type.primitives & ~LinkType::closed(Primitive::AnyString | Primitive::Number | Primitive::Boolean));
}

void AccessInLinkAccesses::mayThrow()
{
    m_wasAskedToThrow = true;
    if (!std::exchange(m_hasThrown, true)) {
        LinkFlow::UnknownByEscapeAlone unknownInSight(false);
        m_flow.arrive(thrownNode(), LinkType::anything());
    }
}

void AccessInLinkAccesses::cannotTell(UnknownIs unknownIs)
{
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(unknownIs == UnknownIs::ByEscapeAlone, LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
    bool isToldOf = isRead() && (m_shared || !m_flow.rowsSpeakFor(m_function));
    if (isToldOf && isNamed())
        m_flow.note(LinkFlow::Note::NameRead, m_access.key.bits(), !m_flow.codeAtWorkIsOfLibrary());
    else if (isToldOf && !m_keyIsOnlyNumbers)
        m_flow.note(LinkFlow::Note::ReadUnderKeyNobodyKnows, 0);
    mayThrow();
    giveAnything();
    storeIntoUnseen();
    if (m_receiver != m_subject)
        m_flow.addEdge(m_receiver, m_flow.unseen());
}

void AccessInLinkAccesses::cannotTellWhatRuns()
{
    m_flow.addEdge(m_subject, m_flow.unseen());
    cannotTell();
}

template<typename Functor>
void AccessInLinkAccesses::forEachLayoutUpTheBases(LayoutID first, const Functor& stopsAt) const
{
    size_t steps = 0;
    for (LayoutID current = first; current && steps < m_program.layouts.size(); current = m_program.at(current).base, ++steps) {
        if (stopsAt(current))
            return;
    }
}

void AccessInLinkAccesses::storeBesideFieldsOf(LayoutID id)
{
    storeInto(m_flow.appendedField(id, m_access.key));
}

bool AccessInLinkAccesses::resolveOnWhatStandsAbove(FlowNodeID above, unsigned stepsLeft)
{
    tellAgainOnceClimbs(above);
    const LinkType type = m_flow.typeAt(above);
    bool setterTakesStore = false;
    auto lookAt = [&](LayoutID holder) {
        std::optional<FieldReference> field = findField(m_program, holder, m_access.key);
        if (!field) {
            if (stepsLeft && m_program.at(holder).has(LayoutFlags::IsOfCreatedObject))
                setterTakesStore |= resolveOnWhatStandsAbove(m_flow.whatStandsAbove(holder), stepsLeft - 1);
            return;
        }
        if (isRead())
            giveWhatIsHeldAt(m_flow.field(*field));
        else if (m_access.kind == Kind::Store && m_flow.hasSetterWritten(*field)) {
            storeInto(m_flow.field(*field));
            setterTakesStore = true;
        }
    };
    switch (type.objects) {
    case LinkType::Objects::Layout:
        lookAt(type.layout);
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(type))
            lookAt(member);
        break;
    case LinkType::Objects::Family:
        m_program.at(m_flow.families.layoutsOf(type.family)).forEach(lookAt);
        break;
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        break;
    }
    if (type.classes == LinkType::Classes::Site && stepsLeft)
        setterTakesStore |= resolveOnWhatStandsAbove(m_flow.whatStandsAbove(type.site), stepsLeft - 1);
    return setterTakesStore;
}

bool AccessInLinkAccesses::readKey()
{
    bool hasNode = m_access.keyNode != noSummaryNode;
    FlowNodeID node = hasNode ? m_flow.nodeOf(m_function, m_access.keyNode) : FlowNodeID();
    LinkType key = hasNode ? m_flow.typeAt(node) : LinkType::anything();
    key.primitives &= LinkType::closed(Primitive::All);
    if (key.isNothing())
        return false;
    if (hasNode && !m_access.isToContents())
        m_flow.noteKeyOfAccessInLibrary(m_function, m_record, m_access);
    if (PropertyKeyID symbol = m_flow.keyOf(m_function, m_access)) {
        m_access.key = symbol;
        m_nameClass = m_flow.nameClassOf(symbol);
        return true;
    }

    bool holdsObjects = holdsObjectsForLinkAccesses(key);
    if (holdsObjects) {
        if (hasNode)
            m_flow.addEdge(node, m_flow.unseen());
        mayThrow();
    }
    m_keyIsOnlyNumbers = !holdsObjects && !(key.primitives & ~LinkType::closed(Primitive::Number));
    m_keyIsSymbolAsFarAsIsSeen = hasNode && LinkFlow::isSymbolAsFarAsIsSeen(key);
    m_keyMayNamePrototypeLink = (holdsObjects || key.primitives & Primitive::AnyString) && keyThatBeginsSoMayNamePrototypeLink(m_program, m_access);
    return true;
}

void AccessInLinkAccesses::resolve()
{
    const LinkType subject = m_flow.typeAt(m_subject);
    if (subject.isNothing())
        return;
    if (m_access.isToContents()) {
        resolveContents(subject);
        return;
    }
    if (isPrivateNameInLink(m_program, m_access.key)) {
        if (!isRead() && m_access.kind != Kind::Has && subject.classes != LinkType::Classes::None)
            m_flow.arrive(m_flow.contentsOf(subject, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
        resolveUnderPrivateName();
        return;
    }
    if (!isNamed() && !readKey())
        return;

    if (m_access.kind == Kind::Has) {
        mayThrow();
        if (m_value)
            m_flow.arrive(m_value, primitivesForLinkAccesses(Primitive::Boolean));
        return;
    }

    if (!isRead() || subject.primitives & Primitive::UndefinedOrNull)
        mayThrow();
    if (m_receiver != m_subject && !isAboveHomeObjectOfClassForLinkAccesses(subject))
        m_flow.addEdge(m_receiver, m_flow.unseen());
    if (!isRead() && (isNamed() || !m_keyIsOnlyNumbers)) {
        m_flow.noteWriteOn(subject, m_access.key, m_function, storesTextNumberOrBooleanThatIsWritten() ? LinkFlow::WhatIsStored::TextsNumbersAndBooleans : LinkFlow::WhatIsStored::Anything);
        if (!m_shared)
            m_flow.noteSiteOfWrite(m_function, m_access.site, m_subject);
    }
    if (!isNamed() && !m_keyIsOnlyNumbers && m_access.kind != Kind::Define && !membersOfObjectPrototypeAreWhatTableLists())
        m_flow.addEdge(m_subject, m_flow.unseen());
    if ((subject.holdsUnseen && !m_before.holdsUnseen) || (subject.holdsUnseenInSight() && !m_before.holdsUnseenInSight()))
        cannotTell(subject.holdsUnseenInSight() ? UnknownIs::InSight : UnknownIs::ByEscapeAlone);
    if (isRead() && m_value && subject.holdsLostBuiltins && isNamed()) {
        m_flow.note(LinkFlow::Note::ReadThroughLostBuiltins, m_access.key.bits());
        LinkType found = LinkType::anything();
        found.holdsLostBuiltins = true;
        give(found);
    }
    bool hasSaidAllOfPrimitives = subject.holdsUnseen && (m_keyIsWritten || !m_before.holdsUnseen);
    if (!hasSaidAllOfPrimitives)
        resolveOnPrimitives(static_cast<uint16_t>(subject.primitives & ~m_before.primitives));

    if (subject.objects != m_before.objects || subject.layout != m_before.layout || subject.family != m_before.family) {
        switch (subject.objects) {
        case LinkType::Objects::None:
            break;
        case LinkType::Objects::Layout:
            resolveOnLayout(subject.layout, subject);
            break;
        case LinkType::Objects::Set:
            resolveOnSet(subject);
            break;
        case LinkType::Objects::Prototype:
            resolveOnPrototype(subject.layout);
            break;
        case LinkType::Objects::Prototypes:
            resolveOnPrototypes(subject.family);
            break;
        case LinkType::Objects::Family:
            resolveOnFamily(subject.family);
            break;
        case LinkType::Objects::Lost:
            resolveOnLostObjects(subject.layout);
            break;
        }
    }

    auto holdsLostFunctions = [](const LinkType& type) {
        return type.functions == LinkType::Functions::Set || type.functions == LinkType::Functions::Lost;
    };
    bool functionsAreNew = subject.functions != m_before.functions || subject.function != m_before.function || subject.constructorsOf != m_before.constructorsOf;
    if (functionsAreNew && !(holdsLostFunctions(subject) && holdsLostFunctions(m_before))) {
        switch (subject.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            resolveOnClosure(subject.function);
            break;
        case LinkType::Functions::Constructors:
            resolveOnConstructors(subject.constructorsOf);
            break;
        case LinkType::Functions::Set:
        case LinkType::Functions::Lost:
            resolveOnLostFunctions();
            break;
        }
    }

    if (subject.classes != LinkType::Classes::None && (subject.classes != m_before.classes || subject.builtinClass != m_before.builtinClass || subject.site != m_before.site))
        resolveOnClasses(instancesAmongForLinkAccesses(subject));

    if (subject.builtin && subject.builtin != m_before.builtin)
        resolveOnBuiltin(subject.builtin);
}

void AccessInLinkAccesses::resolveUnderPrivateName()
{
    if (!m_before.isNothing())
        return;
    mayThrow();
    if (m_access.kind == Kind::Has) {
        if (m_value)
            m_flow.arrive(m_value, primitivesForLinkAccesses(Primitive::Boolean));
        return;
    }
    LayoutID owner = m_program.at(m_access.key).owner;
    std::optional<FieldReference> field = owner ? findField(m_program, owner, m_access.key) : std::nullopt;
    if (!field || !fieldAt(m_program, *field).form.isEmpty()) {
        cannotTellWhatRuns();
        return;
    }
    if (isRead()) {
        giveWhatIsHeldAt(m_flow.field(*field));
        giveWhatIsEstablishedOf(fieldAt(m_program, *field).declared);
        return;
    }
    storeInto(m_flow.field(*field));
    if (!m_access.isPartOfConstruction)
        noteWrittenAfterConstruction(*field);
}

void AccessInLinkAccesses::resolveOnLayout(LayoutID id, const LinkType& pooled)
{
    const Layout& layout = m_program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        break;
    case LayoutKind::ModuleNamespace:
        resolveOnNamespace(layout);
        return;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        m_flow.escape(id);
        cannotTell();
        return;
    }

    if (m_flow.escapedLayouts.get(id.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    if (!isNamed()) {
        if (mayStoreIntoPrototypeLink())
            m_flow.escape(id);
        mayThrow();
        if (isRead())
            giveWhatIsHeldAt(m_flow.anyFieldReadThrough(pooled.objects, pooled.layout, pooled.family, namesOnlyElements()));
        else if (m_keyIsSymbolAsFarAsIsSeen) {
            m_flow.escape(id);
            storeIntoUnseen();
        } else
            storeInto(m_flow.anyFieldStoredThrough(pooled.objects, pooled.layout, pooled.family, m_keyIsOnlyNumbers));
        return;
    }
    if (m_nameClass == NameClass::Proto) {
        resolvePrototypeLinkOfLayout(id, layout);
        return;
    }

    bool isDefinition = m_access.kind == Kind::Define;
    if (!isDefinition && m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        m_flow.escape(id);
        cannotTell();
    }

    std::optional<FieldReference> field = findField(m_program, id, m_access.key);
    if (field && fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor)) {
        m_flow.escape(id);
        cannotTell();
        return;
    }

    LinkFlow::FoundMembers members = m_flow.findMembers(id, ClassSide::Prototype, m_access.key);
    bool findsAccessor = false;
    bool findsGetter = findsGetterAmong(members);
    for (const LinkFlow::FoundMember& found : members) {
        FlowNodeID member = m_flow.member(found.classLayout, ClassSide::Prototype, found.index);
        switch (found.kind) {
        case MethodKind::Method:
            if (isRead())
                giveFrom(member);
            else if (!field)
                storeInto(member);
            break;
        case MethodKind::Getter:
            findsAccessor = true;
            if (isRead())
                enterAccessorAt(member);
            break;
        case MethodKind::Setter:
            findsAccessor = true;
            if (m_access.kind == Kind::Store)
                enterAccessorAt(member);
            if (!findsGetter)
                givePrimitives(Primitive::Undefined);
            break;
        }
    }

    bool isPrivateName = m_program.at(m_access.key).kind == PropertyKeyKind::PrivateName;
    if (!field && !isDefinition && !isPrivateName && layout.has(LayoutFlags::IsOfCreatedObject) && resolveOnWhatStandsAbove(m_flow.whatStandsAbove(id), LinkFlow::mostStepsAboveCreatedObject))
        return;
    bool goesOnAboveClasses = members.lookupGoesOnAboveClasses() && !isPrivateName;
    FlowNodeID foundOnPrototypes;
    if (isRead() && !isPrivateName)
        foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, m_access.key);
    else if (!isDefinition && !isPrivateName)
        m_flow.lookForSetterOnPrototypesAbove(id, m_access.key);

    if (isRead()) {
        giveFrom(foundBesideFieldsOf(id));
        if (foundOnPrototypes)
            giveFrom(foundOnPrototypes);
        if (field) {
            giveWhatIsHeldAt(m_flow.field(*field));
            giveWhatIsEstablishedOf(fieldAt(m_program, *field).declared);
        } else
            giveFrom(m_flow.appendedField(id, m_access.key));
        if (!goesOnAboveClasses)
            return;
        if (field && field->layout == id && !m_flow.mayReadInFrontOfDefinition(m_function, m_access, id) && m_flow.isAbsentAboveClasses(m_access.key)) {
            giveFrom(m_flow.foundWhereFieldIsEmpty(*field));
            return;
        }
        bool mayFindClassAbove = m_nameClass == NameClass::Constructor && m_flow.isBelowConstructorFunction(id);
        if (mayFindClassAbove)
            giveConstructorsOfRowAbove(id);
        if (m_nameClass == NameClass::Constructor && layout.kind == LayoutKind::Instance && layout.constructor && !mayFindClassAbove)
            give(closureForLinkAccesses(layout.constructor));
        else if (!giveWhatIsFoundFrom(Builtins::ObjectPrototype))
            m_flow.escape(id);
        return;
    }

    if (m_access.kind == Kind::Store && goesOnAboveClasses && mayRunCodeFoundFrom(Builtins::ObjectPrototype)) {
        m_flow.escape(id);
        storeIntoUnseen();
    }
    if (field) {
        storeInto(m_flow.field(*field));
        if (!m_flow.isPartOfConstructionOf(id, m_function, m_access))
            noteWrittenAfterConstruction(*field);
        return;
    }
    if (findsAccessor && !isDefinition && !goesOnAboveClasses)
        return;
    if (findsAccessor && isDefinition)
        m_flow.escape(id);
    storeBesideFieldsOf(id);
}

void AccessInLinkAccesses::resolvePrototypeLinkOfLayout(LayoutID id, const Layout& layout)
{
    switch (m_access.kind) {
    case Kind::Read:
        if (!m_flow.findsPrototypeLinkOfSubject(id, ClassSide::Prototype, m_access.key, m_receiver != m_subject)) {
            m_flow.escape(id);
            cannotTell();
            return;
        }
        if (layout.kind == LayoutKind::Instance)
            give(prototypeForLinkAccesses(id));
        else
            giveAnything();
        giveFrom(foundBesideFieldsOf(id));
        giveFrom(m_flow.appendedField(id, m_access.key));
        return;
    case Kind::Store:
        noteChange(id, LayoutChange::ChangesPrototype);
        m_flow.escape(id);
        cannotTell();
        return;
    case Kind::Define:
        storeBesideFieldsOf(id);
        return;
    case Kind::Has:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveOnNamespace(const Layout& layout)
{
    mayThrow();
    if (!isRead()) {
        storeIntoUnseen();
        return;
    }
    if (!layout.namespaceOf) {
        giveAnything();
        return;
    }
    if (isNamed()) {
        if (std::optional<uint32_t> field = layout.findField(m_access.key))
            giveExport(layout.namespaceOf, *field);
        else
            giveAnything();
        return;
    }
    uint32_t numberOfFields = static_cast<uint32_t>(layout.fields.size());
    for (uint32_t field = 0; field < numberOfFields; ++field)
        giveExport(layout.namespaceOf, field);
    giveAnything();
}

void AccessInLinkAccesses::giveExport(ModuleID module, uint32_t index)
{
    m_flow.showExport(module, index);
    for (size_t steps = 0; steps <= m_program.modules.size(); ++steps) {
        const auto& exports = m_program.at(module).exports;
        if (index >= exports.size())
            break;
        const ModuleExport& entry = exports[index];
        if (m_flow.testRuns().mayComeThroughMock(module, index)) {
            giveAnything();
            return;
        }
        if (isNamed() && entry.isFromStarExport && m_program.at(module).requests.containsIf([](const ModuleRequest& request) { return request.isDynamicStarExport; }))
            givePrimitives(Primitive::Undefined);
        switch (entry.kind) {
        case ModuleExportKind::Variable:
            if (entry.variable)
                giveWhatIsHeldAt(m_flow.variable(entry.variable));
            else
                giveAnything();
            return;
        case ModuleExportKind::Namespace: {
            LayoutID whole = entry.module && entry.phase == ImportPhase::Evaluation ? m_program.layoutOfWholeNamespace(entry.module) : LayoutID();
            if (whole)
                give(instanceForLinkAccesses(whole));
            else
                giveAnything();
            return;
        }
        case ModuleExportKind::Indirect: {
            if (!entry.module || !entry.importName) {
                giveAnything();
                return;
            }
            const auto& exportsOfTarget = m_program.at(entry.module).exports;
            uint32_t found = 0;
            while (found < exportsOfTarget.size() && exportsOfTarget[found].name != entry.importName)
                ++found;
            module = entry.module;
            index = found;
            break;
        }
        }
    }
    giveAnything();
}

bool AccessInLinkAccesses::wasNamedBefore(LayoutID layout) const
{
    switch (m_before.objects) {
    case LinkType::Objects::Layout:
        return m_before.layout == layout;
    case LinkType::Objects::Set:
        return std::ranges::contains(m_flow.layoutsOfSet(m_before), layout);
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveOnSet(const LinkType& subject)
{
    std::span<const LayoutID> members = m_flow.layoutsOfSet(subject);
    bool poolOfSetWouldLetNamespaceEscape = std::ranges::any_of(members, [&](LayoutID member) {
        return m_program.at(member).kind == LayoutKind::ModuleNamespace;
    });
    for (LayoutID member : members) {
        if (!wasNamedBefore(member))
            resolveOnLayout(member, poolOfSetWouldLetNamespaceEscape ? instanceForLinkAccesses(member) : subject);
    }
}

void AccessInLinkAccesses::resolveOnFamily(FamilyID family)
{
    mayThrow();
    if (isNamed()) {
        if (isRead() && m_access.isPartOfConstruction) {
            bool mayComeInFrontOfDefinition = false;
            m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID member) {
                mayComeInFrontOfDefinition |= m_flow.mayReadInFrontOfDefinition(m_function, m_access, member);
            });
            if (mayComeInFrontOfDefinition)
                givePrimitives(Primitive::Undefined);
        }
        if (isRead())
            giveWhatIsHeldAt(m_flow.fieldReadThrough(family, m_access.key));
        else
            storeInto(m_flow.fieldStoredThrough(family, m_access.key, m_flow.isPartOfConstructionOfAllOf(family, m_function, m_access)));
        return;
    }
    if (isRead()) {
        giveWhatIsHeldAt(m_flow.anyFieldReadThrough(LinkType::Objects::Family, LayoutID(), family, namesOnlyElements()));
        return;
    }
    if (mayStoreIntoPrototypeLink() || m_keyIsSymbolAsFarAsIsSeen)
        m_flow.escapeAllOf(familyForLinkAccesses(family));
    if (m_keyIsSymbolAsFarAsIsSeen) {
        storeIntoUnseen();
        return;
    }
    storeInto(m_flow.anyFieldStoredThrough(LinkType::Objects::Family, LayoutID(), family, m_keyIsOnlyNumbers));
}

void AccessInLinkAccesses::resolveOnLostObjects(LayoutID member)
{
    mayThrow();
    if (isNamed()) {
        if (isRead() && m_access.isPartOfConstruction)
            givePrimitives(Primitive::Undefined);
        if (isRead())
            giveWhatIsHeldAt(m_flow.fieldReadThroughLost(member, m_access.key));
        else
            storeInto(m_flow.fieldStoredThroughLost(member, m_access.key));
    } else if (isRead())
        giveWhatIsHeldAt(m_flow.anyFieldReadThrough(LinkType::Objects::Lost, member, FamilyID(), namesOnlyElements()));
    else if (m_keyIsSymbolAsFarAsIsSeen) {
        LinkType lost;
        lost.objects = LinkType::Objects::Lost;
        lost.layout = member;
        m_flow.escapeAllOf(lost);
        storeIntoUnseen();
    }
    else
        storeInto(m_flow.anyFieldStoredThrough(LinkType::Objects::Lost, member, FamilyID()));
}

void AccessInLinkAccesses::resolveOnPrototype(LayoutID id)
{
    const Layout& layout = m_program.at(id);
    tellAgainOnceClimbs(foundBesideFieldsOf(id));
    if (m_flow.escapedLayouts.get(id.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    if (!isNamed() && m_access.kind == Kind::Define && m_value == m_flow.unseen() && m_flow.isOfConstructorFunction(id)) {
        m_flow.defineOnPrototypeOf(id, { });
        return;
    }
    if (!isNamed()) {
        letPrototypeEscape(id);
        cannotTell();
        return;
    }

    if (m_nameClass == NameClass::Proto) {
        if (isRead() && !m_flow.findsPrototypeLinkOfSubject(id, ClassSide::Prototype, m_access.key, m_receiver != m_subject)) {
            letPrototypeEscape(id);
            cannotTell();
            return;
        }
        if (isRead()) {
            if (layout.base) {
                give(prototypeForLinkAccesses(layout.base));
                giveFrom(foundBesideFieldsOf(id));
            }
            if (!layout.base || m_flow.mayNotBeLinkedYetBelow(id, layout.base))
                giveAnything();
            return;
        }
        noteChange(id, LayoutChange::ChangesPrototype);
        letPrototypeEscape(id);
        cannotTell();
        return;
    }

    bool isDefinition = m_access.kind == Kind::Define;
    if (!isDefinition && m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        letPrototypeEscape(id);
        cannotTell();
    }

    LinkFlow::FoundMembers members = m_flow.findMembers(id, ClassSide::Prototype, m_access.key);
    layOnReceiverUnlessAccessorAnswers(members);
    bool isPrivateName = m_program.at(m_access.key).kind == PropertyKeyKind::PrivateName;
    if (isRead() && !isPrivateName) {
        if (FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, m_access.key))
            giveFrom(foundOnPrototypes);
    } else if (!isDefinition && !isPrivateName)
        m_flow.lookForSetterOnPrototypesAbove(id, m_access.key);
    if (!isRead() && !isPrivateName && m_receiver == m_subject && m_flow.isOfConstructorFunction(id)) {
        bool findsAccessor = std::ranges::any_of(members, [](const LinkFlow::FoundMember& found) {
            return found.kind != MethodKind::Method;
        });
        if (!isDefinition && (findsAccessor || (members.lookupGoesOnAboveClasses() && mayRunCodeFoundFrom(Builtins::ObjectPrototype)))) {
            letPrototypeEscape(id);
            storeIntoUnseen();
            return;
        }
        if (isDefinition && m_value == m_flow.unseen())
            m_flow.defineOnPrototypeOf(id, m_access.key);
        else
            storeInto(m_flow.storedOnPrototypeOf(id, m_access.key));
        return;
    }
    bool goesOnAboveClasses = members.lookupGoesOnAboveClasses();
    if (goesOnAboveClasses && isRead())
        giveFrom(foundBesideFieldsOf(id));
    if (goesOnAboveClasses && isRead() && !isPrivateName) {
        if (m_nameClass == NameClass::Constructor && m_flow.isBelowConstructorFunction(id))
            giveConstructorsOfRowAbove(id);
        if (m_nameClass != NameClass::Constructor || m_flow.isBelowConstructorFunction(id)) {
            if (!giveWhatIsFoundFrom(Builtins::ObjectPrototype))
                letPrototypeEscape(id);
        } else if (layout.constructor)
            give(closureForLinkAccesses(layout.constructor));
        else
            giveAnything();
    } else if (goesOnAboveClasses && !isRead()) {
        letPrototypeEscape(id);
        storeIntoUnseen();
    }

    bool findsGetter = findsGetterAmong(members);
    for (const LinkFlow::FoundMember& found : members) {
        FlowNodeID member = m_flow.member(found.classLayout, ClassSide::Prototype, found.index);
        switch (found.kind) {
        case MethodKind::Method:
            giveFrom(member);
            storeInto(member);
            break;
        case MethodKind::Getter:
            if (isRead())
                enterAccessorAt(member);
            break;
        case MethodKind::Setter:
            if (m_access.kind == Kind::Store)
                enterAccessorAt(member);
            if (!findsGetter)
                givePrimitives(Primitive::Undefined);
            break;
        }
        if (isDefinition && found.kind != MethodKind::Method) {
            letPrototypeEscape(id);
            letPrototypeEscape(found.classLayout);
            storeIntoUnseen();
        }
    }
}

void AccessInLinkAccesses::resolveOnPrototypes(FamilyID family)
{
    if (resolveThroughSharedAccess(SharedAccess::Subjects::PrototypeObjects, family))
        return;
    m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID layout) {
        resolveOnPrototype(layout);
    });
}

bool AccessInLinkAccesses::resolveThroughSharedAccess(SharedAccess::Subjects subjects, FamilyID family)
{
    if (m_receiver != m_subject)
        return false;
    LinkFlow::LockerOfWhatIsShared locker(m_flow);

    StateOfLinkAccesses& state = m_parts.state;
    bool valueIsUnseen = m_value == m_flow.unseen();
    std::pair<uint64_t, uint32_t> place {
        static_cast<uint64_t>(family.index()) << 32 | (isNamed() ? m_access.key.index() : 0),
        static_cast<uint32_t>(subjects) | static_cast<uint32_t>(m_access.kind) << 1 | isNamed() << 3 | !!m_value << 4 | valueIsUnseen << 5
            | m_access.storesObjectMadeForIt << 6 | m_keyIsOnlyNumbers << 7 | m_keyMayNamePrototypeLink << 8 | isLibraryCode(m_program, m_program.at(m_function)) << 9,
    };
    auto found = state.indexesOfSharedAccesses.add(place, static_cast<uint32_t>(state.sharedAccesses.size()));
    uint32_t index = found.iterator->value;
    if (found.isNewEntry) {
        auto made = makeUniqueWithoutFastMallocCheck<SharedAccess>();
        made->subjects = subjects;
        made->firstFunction = m_function;
        made->access.kind = m_access.kind;
        made->access.key = m_access.key;
        made->access.storesObjectMadeForIt = m_access.storesObjectMadeForIt;
        made->keyIsOnlyNumbers = m_keyIsOnlyNumbers;
        made->keyMayNamePrototypeLink = m_keyMayNamePrototypeLink;
        made->subject = m_flow.makeSharedNode();
        if (m_value)
            made->value = valueIsUnseen ? m_flow.unseen() : m_flow.makeSharedNode();
        made->thrown = m_flow.makeSharedNode();
        state.sharedAccesses.append(WTF::move(made));
        const SharedAccess& shared = *state.sharedAccesses[index];
        bool isOnClassObjects = subjects == SharedAccess::Subjects::ClassObjects;
        m_flow.arrive(shared.subject, isOnClassObjects ? constructorsForLinkAccesses(family) : prototypesForLinkAccesses(family));

        uint32_t firstMember = static_cast<uint32_t>(state.membersOfSharedAccesses.size());
        m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID layout) {
            FunctionID constructor = m_program.at(layout).constructor;
            if (!isOnClassObjects || (constructor && m_flow.classOfConstructor(constructor) == layout))
                state.membersOfSharedAccesses.append(MemberOfSharedAccessInLinkAccesses { index, layout });
        });
        uint32_t endOfMembers = static_cast<uint32_t>(state.membersOfSharedAccesses.size());
        for (uint32_t member = firstMember; member < endOfMembers; ++member)
            AccessInLinkAccesses(m_flow, m_parts, shared, member, true).resolveMemberOfSharedAccess();
    }

    const SharedAccess& shared = *state.sharedAccesses[index];
    if (!valueIsUnseen) {
        giveFrom(shared.value);
        storeInto(shared.value);
    }
    m_flow.addEdge(shared.thrown, thrownNode());
    return true;
}

void AccessInLinkAccesses::resolveMemberOfSharedAccess()
{
    LayoutID layout = m_parts.state.membersOfSharedAccesses[m_record].layout;
    switch (m_shared->subjects) {
    case SharedAccess::Subjects::PrototypeObjects:
        resolveOnPrototype(layout);
        return;
    case SharedAccess::Subjects::ClassObjects:
        resolveOnClosure(m_program.at(layout).constructor);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveOnClosure(FunctionID function)
{
    const Function& record = m_program.at(function);
    LayoutID classLayout = record.constructedClass;
    if (m_nameClass == NameClass::Prototype && isNamed() && isRead() && classLayout && isClassConstructorOf(record.kind)) {
        give(prototypeForLinkAccesses(classLayout));
        return;
    }
    tellAgainOnceClimbs(m_flow.foundOnClosuresOf(function));
    if (m_flow.escapedFunctions.get(function.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    if (!isNamed()) {
        bool findsNoMember = isRead() && (!classLayout || m_flow.isOfConstructorFunction(classLayout)) && namesNothingOnPlainClosures();
        if (!findsNoMember)
            letClassObjectEscape(function, classLayout);
        cannotTell();
        return;
    }

    if (m_nameClass == NameClass::Prototype) {
        if (LayoutID constructed = m_flow.layoutOfConstructorFunction(function)) {
            resolvePrototypePropertyOf(function, constructed);
            return;
        }
        m_flow.escape(function);
        if (classLayout) {
            if (!isRead())
                noteChange(classLayout, LayoutChange::ChangesPrototype);
            m_flow.escape(classLayout);
        }
        cannotTell();
        return;
    }

    if (m_nameClass == NameClass::Proto) {
        LayoutID classOfClassObject = isClassConstructorOf(record.kind) ? classLayout : LayoutID();
        if (!isRead() || !m_flow.findsPrototypeLinkOfSubject(classOfClassObject, ClassSide::Static, m_access.key, m_receiver != m_subject)) {
            letClassObjectEscape(function, classLayout);
            cannotTell();
            return;
        }
        LayoutID base = classOfClassObject ? m_program.at(classOfClassObject).base : LayoutID();
        if (base && m_program.at(base).constructor) {
            give(closureForLinkAccesses(m_program.at(base).constructor));
            giveFrom(foundBesideFieldsOf(classLayout));
        } else
            giveAnything();
        return;
    }

    if (classLayout && !m_flow.isOfConstructorFunction(classLayout)) {
        resolveOnClassObject(function, classLayout);
        return;
    }
    if (isTakenByLoader())
        giveFrom(foundBesideFieldsUnderKey());
    if (mayRunCodeFoundAboveClosures())
        m_flow.escape(function);
    bool nameIsFollowed = m_access.kind != Kind::Has && m_flow.followsNamesOnClosuresOf(function, m_access.key);
    if (!nameIsFollowed)
        storeInto(foundBesideFieldsUnderKey());
    if (nameIsFollowed) {
        FlowNodeID laid = m_flow.laidOnClosuresOf(function, m_access.key);
        if (!isRead()) {
            storeInto(laid);
            return;
        }
        giveFrom(laid);
        if (BuiltinID member; lookUpFrom(Builtins::FunctionPrototype, member) == LookupInLinkAccesses::Absent) {
            givePrimitives(Primitive::Undefined);
            return;
        }
    }
    cannotTell();
}

void AccessInLinkAccesses::giveConstructorsOfRowAbove(LayoutID id)
{
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = id; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
        if (FunctionID constructor = m_program.at(current).constructor)
            give(closureForLinkAccesses(constructor));
    }
}

void AccessInLinkAccesses::resolvePrototypePropertyOf(FunctionID function, LayoutID constructed)
{
    if (isRead()) {
        give(prototypeForLinkAccesses(constructed));
        giveFrom(foundBesideFieldsOf(constructed));
        return;
    }
    if (m_access.storesObjectMadeForIt)
        return;
    if (m_value && m_laysObservers)
        m_flow.observe(m_value, observerOfWhatWasLookedUp());
    if (m_value && m_flow.typeAt(m_value).isNothing())
        return;
    noteChange(constructed, LayoutChange::ChangesPrototype);
    m_flow.escape(function);
    m_flow.escape(constructed);
    storeIntoUnseen();
}

void AccessInLinkAccesses::resolveOnClassObject(FunctionID function, LayoutID classLayout)
{
    bool isDefinition = m_access.kind == Kind::Define;
    if (!isDefinition && m_flow.namesMayBeIncomplete(classLayout, ClassSide::Static))
        cannotTellWhatRuns();

    bool findsField = false;
    bool isHiddenByEveryClass = classHolderIsMadeWithName(m_program, ClassSide::Static, m_access.key);
    forEachLayoutUpTheBases(classLayout, [&](LayoutID current) {
        m_program.at(current).forEachStaticFieldThatMayMatch(m_access.key, [&](const StaticField& field) {
            findsField |= !!field.key;
            giveWhatIsHeldAt(m_flow.variable(field.variable));
            storeInto(m_flow.variable(field.variable));
        });
        return findsField || isHiddenByEveryClass;
    });

    LinkFlow::FoundMembers members = m_flow.findMembers(classLayout, ClassSide::Static, m_access.key);
    layOnReceiverUnlessAccessorAnswers(findsField ? LinkFlow::FoundMembers() : members);
    bool findsAccessor = false;
    bool findsGetter = findsGetterAmong(members);
    for (const LinkFlow::FoundMember& found : members) {
        FlowNodeID member = m_flow.member(found.classLayout, ClassSide::Static, found.index);
        switch (found.kind) {
        case MethodKind::Method:
            giveFrom(member);
            if (!findsField)
                storeInto(member);
            break;
        case MethodKind::Getter:
            findsAccessor = true;
            if (isRead())
                enterAccessorAt(member);
            break;
        case MethodKind::Setter:
            findsAccessor = true;
            if (m_access.kind == Kind::Store)
                enterAccessorAt(member);
            if (!findsGetter)
                givePrimitives(Primitive::Undefined);
            break;
        }
    }

    if (findsField) {
        givePrimitives(Primitive::Undefined);
        return;
    }
    if (members.lookupGoesOnAboveClasses() && mayRunCodeFoundAboveClosures())
        m_flow.escape(function);
    if (isRead()) {
        if (members.lookupGoesOnAboveClasses())
            cannotTell();
        return;
    }
    if (findsAccessor && !isDefinition)
        return;
    if (findsAccessor)
        m_flow.escape(classLayout);
    storeInto(foundBesideFieldsUnderKey());
    storeIntoUnseen();
}

void AccessInLinkAccesses::resolveOnConstructors(FamilyID family)
{
    if (isRead() && m_nameClass == NameClass::Prototype) {
        give(prototypesForLinkAccesses(family));
        return;
    }
    if (resolveThroughSharedAccess(SharedAccess::Subjects::ClassObjects, family))
        return;
    m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID layout) {
        FunctionID constructor = m_program.at(layout).constructor;
        if (constructor && m_flow.classOfConstructor(constructor) == layout)
            resolveOnClosure(constructor);
    });
}

void AccessInLinkAccesses::resolveOnLostFunctions()
{
    if (isRead() && namesNothingOnPlainClosures()) {
        cannotTell();
        return;
    }
    if (!isNamed() || m_nameClass == NameClass::Prototype || mayRunCodeFoundAboveClosures())
        m_flow.addEdge(m_subject, m_flow.unseen());
    if (isNamed())
        storeInto(foundBesideFieldsUnderKey());
    cannotTell();
}

bool AccessInLinkAccesses::namesNothingOnPlainClosures() const
{
    if (isNamed() || !m_keyIsOnlyNumbers)
        return false;
    const BuiltinClassID classesOfFunctions[] { BuiltinClasses::Function, BuiltinClasses::AsyncFunction, BuiltinClasses::GeneratorFunction, BuiltinClasses::AsyncGeneratorFunction };
    return std::ranges::all_of(classesOfFunctions, [&](BuiltinClassID functionClass) {
        BuiltinID prototype = prototypeOfInstancesOf(functionClass);
        return prototype && indexesAreAbsentFrom(prototype);
    });
}

bool AccessInLinkAccesses::mayRunCodeFoundAboveClosures()
{
    static constexpr uint8_t isKnown = 1 << 0;
    static constexpr uint8_t mayRunCode = 1 << 1;
    static constexpr uint8_t lookupMayThrow = 1 << 2;

    if (m_access.kind == Kind::Define)
        return false;
    if (m_flow.testRuns().isTestRun()) {
        if (FlowNodeID added = m_flow.testRuns().nodeOfNameAdded(m_access.key)) {
            tellAgainOnceClimbs(added);
            if (m_flow.testRuns().isNameAdded(added))
                return true;
        }
        return findsCodeToRunAboveClosures();
    }
    Vector<uint8_t>& answers = m_parts.state.codeFoundAboveClosures;
    size_t place = 2 * static_cast<size_t>(m_access.key.index()) + isRead();
    if (uint8_t answer = answers[place]) {
        if (answer & lookupMayThrow)
            mayThrow();
        return answer & mayRunCode;
    }

    bool wasAskedToThrow = std::exchange(m_wasAskedToThrow, false);
    bool hadLeaned = std::exchange(m_hasLeanedOnMemberTakenForConstant, false);
    bool finds = findsCodeToRunAboveClosures();
    if (finds || !m_hasLeanedOnMemberTakenForConstant)
        answers[place] = isKnown | (finds ? mayRunCode : 0) | (m_wasAskedToThrow ? lookupMayThrow : 0);
    m_wasAskedToThrow |= wasAskedToThrow;
    m_hasLeanedOnMemberTakenForConstant |= hadLeaned;
    return finds;
}

bool AccessInLinkAccesses::findsCodeToRunAboveClosures()
{
    BuiltinName name = m_program.builtinNameOf(m_access.key);
    const BuiltinClassID classesOfFunctions[] { BuiltinClasses::Function, BuiltinClasses::AsyncFunction, BuiltinClasses::GeneratorFunction, BuiltinClasses::AsyncGeneratorFunction };
    for (BuiltinClassID functionClass : classesOfFunctions) {
        if (name && m_table.findOwnPropertyOfInstances(functionClass, name))
            continue;
        BuiltinID prototype = prototypeOfInstancesOf(functionClass);
        if (!prototype || mayRunCodeFoundFrom(prototype))
            return true;
    }
    return false;
}

void AccessInLinkAccesses::letClassObjectEscape(FunctionID function, LayoutID classLayout)
{
    m_flow.escape(function);
    if (!classLayout)
        return;
    m_flow.escape(classLayout);
    forEachLayoutUpTheBases(classLayout, [&](LayoutID current) {
        for (const StaticField& field : m_program.at(current).staticFields) {
            FlowNodeID variable = m_flow.variable(field.variable);
            m_flow.addEdge(variable, m_flow.unseen());
            m_flow.addEdge(m_flow.unseen(), variable);
        }
        return false;
    });
}

void AccessInLinkAccesses::enterAccessorAt(FlowNodeID member)
{
    if (m_laysObservers)
        m_flow.observe(member, observerOfWhatWasLookedUp());

    const LinkType held = m_flow.typeAt(member);
    if (held.primitives)
        givePrimitives(Primitive::Undefined);
    if (held.holdsUnseen || held.builtin || held.objects != LinkType::Objects::None || held.classes != LinkType::Classes::None)
        cannotTellWhatRuns();

    switch (held.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        enterAccessor(held.function);
        break;
    case LinkType::Functions::Constructors:
        m_flow.addEdge(member, m_flow.unseen());
        cannotTellWhatRuns();
        break;
    case LinkType::Functions::Set:
    case LinkType::Functions::Lost:
        mayThrow();
        m_flow.noteCallOfLostFunctions(held);
        m_flow.addEdge(m_receiver, m_flow.receiverOfLostFunctions());
        giveFrom(m_flow.resultOfLostFunctions());
        storeInto(m_flow.parameterOfLostFunctions(0));
        break;
    }
}

void AccessInLinkAccesses::layOnReceiverUnlessAccessorAnswers(const LinkFlow::FoundMembers& members)
{
    if (isRead() || m_receiver == m_subject)
        return;
    bool findsAccessor = std::ranges::any_of(members, [](const LinkFlow::FoundMember& member) {
        return member.kind != MethodKind::Method;
    });
    if (findsAccessor && !members.lookupGoesOnAboveClasses())
        return;
    m_flow.addEdge(m_receiver, m_flow.unseen());
    storeIntoUnseen();
}

void AccessInLinkAccesses::enterAccessor(FunctionID id)
{
    const Function& callee = m_program.at(id);
    if (callee.kind != FunctionKind::Getter && callee.kind != FunctionKind::Setter)
        m_flow.escape(id);
    if (!callee.hasBody()) {
        cannotTellWhatRuns();
        return;
    }

    m_flow.reach(id);
    m_flow.addEdge(m_flow.thrown(id), thrownNode());

    uint32_t numberOfParameters = callee.numberOfEntryParameters();
    uint32_t firstParameter = callee.firstParameterIndex();
    uint32_t numberOfPositionalParameters = callee.numberOfPositionalParameters();
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        FlowNodeID parameter = m_flow.parameter(id, index);
        switch (callee.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
            m_flow.addEdge(m_receiver, parameter);
            break;
        case EntryParameter::Callee:
            m_flow.arrive(parameter, closureForLinkAccesses(id));
            break;
        case EntryParameter::NewTarget:
            m_flow.arrive(parameter, primitivesForLinkAccesses(Primitive::Undefined));
            break;
        case EntryParameter::ArgumentCount:
            m_flow.arrive(parameter, primitivesForLinkAccesses(Primitive::Integer));
            break;
        case EntryParameter::Parameter:
            if (index - firstParameter >= numberOfPositionalParameters)
                m_flow.arrive(parameter, LinkType::anything());
            else if (!isRead() && index == firstParameter)
                storeInto(parameter);
            else
                m_flow.arrive(parameter, primitivesForLinkAccesses(Primitive::Undefined));
            break;
        case EntryParameter::HiddenRest:
            m_flow.arrive(parameter, LinkType::anything());
            break;
        }
    }

    if (!isRead()) {
        if (!numberOfPositionalParameters)
            storeIntoUnseen();
        return;
    }
    if (callee.isCoroutine() || callee.results.size() != 1)
        giveAnything();
    else
        giveFrom(m_flow.result(id, 0));
}

void AccessInLinkAccesses::resolveOnPrimitives(uint16_t primitives)
{
    primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent | Primitive::Unspecified);
    if (!primitives)
        return;
    if (!isRead()) {
        cannotTell();
        return;
    }

    static constexpr uint16_t kinds[] { Primitive::AnyString, Primitive::AnySymbol, Primitive::BigInt, Primitive::Number, Primitive::Boolean };
    for (uint16_t kind : kinds) {
        if (!(primitives & LinkType::closed(kind)))
            continue;
        BuiltinClassID builtinClass = primitiveClassOf(kind);
        BuiltinID prototype = prototypeOfInstancesOf(builtinClass);
        if (!prototype) {
            cannotTell();
            continue;
        }
        if ((isNamed() ? mayBeElementKey(m_nameClass) : m_keyIsOnlyNumbers) && m_table.definesCharactersOnNewInstances(builtinClass)) {
            givePrimitives(Primitive::String);
            if (indexesAreAbsentFrom(prototype))
                givePrimitives(Primitive::Undefined);
            else
                cannotTell();
            continue;
        }
        if (!isNamed()) {
            cannotTell();
            continue;
        }

        BuiltinName name = m_program.builtinNameOf(m_access.key);
        if (name && m_table.isRedefinedBelow(builtinClass, name))
            cannotTell();
        else if (BuiltinID own = name ? m_table.findOwnPropertyOfInstances(builtinClass, name) : BuiltinID()) {
            m_flow.note(LinkFlow::Note::BuiltinRead, own.index());
            give(typeAnsweredByReadsOf(own));
        } else
            giveWhatIsFoundFrom(prototype);
    }
}

template<typename Functor>
void AccessInLinkAccesses::forEachListOfElements(const LinkType& instances, const Functor& functor)
{
    if (instances.classes == LinkType::Classes::Mixed) {
        functor(m_flow.contentsOf(instances, ContentsSlot::Element));
        return;
    }
    for (BuiltinClassID current = instances.builtinClass; current; current = m_table.at(current).parent) {
        BuiltinSlotID slot = m_table.findSlot(current, ContentsSlot::Element);
        if (slot && m_table.at(slot).heap == AbstractHeap::Elements) {
            functor(m_flow.contentsOf(instances, ContentsSlot::Element));
            return;
        }
    }
}

void AccessInLinkAccesses::resolveOnClasses(const LinkType& instances)
{
    bool isOneClass = instances.classes != LinkType::Classes::Mixed;
    BuiltinClassID builtinClass = instances.builtinClass;
    if (m_nameClass == NameClass::Proto && !isRead()) {
        forEachListOfElements(instances, [&](FlowNodeID elements) {
            m_flow.addEdge(m_flow.unseen(), elements);
        });
        m_flow.addEdge(m_subject, m_flow.unseen());
        cannotTell();
        return;
    }

    bool mayBeElement = !isNamed() || mayBeElementKey(m_nameClass);
    bool isElement = isNamed() ? mayBeElement : m_keyIsOnlyNumbers;
    if (!mayBeElement) {
        if (!isRead() && m_access.key == m_program.keyOf(BuiltinNames::length) && !m_access.storesSurelyIndex)
            m_flow.arrive(m_flow.contentsOf(instances, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
        resolveUnderNameOnClass(instances);
        return;
    }
    if (!isElement)
        m_flow.addEdge(m_subject, m_flow.unseen());
    if (isRead() && !isElement)
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underAnyName()));

    TypedArrayType typedArray = isOneClass ? m_table.typedArrayTypeOf(builtinClass) : NotTypedArray;
    if (typedArray != NotTypedArray && typedArray != TypeDataView) {
        give(m_flow.fromTypeID(elementTypeOf(typedArray)));
        givePrimitives(Primitive::Undefined);
        storeIntoUnseen();
        if (!isElement)
            cannotTell();
        return;
    }

    bool hasElements = false;
    forEachListOfElements(instances, [&](FlowNodeID elements) {
        hasElements = true;
        giveWhatIsHeldAt(elements);
        storeInto(elements);
    });
    if (!isRead() && (isNamed() ? m_nameClass != NameClass::Index : !m_access.keyIsSurelyIndex))
        m_flow.arrive(m_flow.contentsOf(instances, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
    BuiltinID prototype = isOneClass && isElement && hasElements ? prototypeOfInstancesOf(builtinClass) : BuiltinID();
    if (prototype && indexesAreAbsentFrom(prototype))
        givePrimitives(Primitive::Undefined);
    else
        cannotTell();
}

void AccessInLinkAccesses::resolveUnderNameOnClass(const LinkType& instances)
{
    BuiltinClassID builtinClass = instances.builtinClass;
    if (isRead())
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::under(m_access.key)));

    BuiltinName name = m_program.builtinNameOf(m_access.key);
    if (BuiltinID own = isRead() && builtinClass && name ? m_table.findOwnPropertyOfInstances(builtinClass, name) : BuiltinID()) {
        m_flow.note(LinkFlow::Note::BuiltinRead, own.index());
        give(typeAnsweredByReadsOf(own));
        return;
    }

    if (BuiltinLayoutID record = isRead() && name && instances.classes == LinkType::Classes::Site ? m_flow.layoutOfRecordsOfFamilyOf(instances.site) : BuiltinLayoutID()) {
        bool isListed = std::ranges::any_of(m_table.at(record).fields, [&](const BuiltinLayoutField& field) {
            return field.name == name && !field.isOptional;
        });
        if (isListed) {
            giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
            return;
        }
    }

    BuiltinID prototype = builtinClass ? prototypeOfInstancesOf(builtinClass) : BuiltinID();
    bool tableSpeaksOfObject = prototype && m_nameClass != NameClass::Proto && m_table.mayHavePristineInstances(builtinClass) && !m_table.mayDefineOnNewInstances(builtinClass, name);
    if (tableSpeaksOfObject && name) {
        for (BuiltinClassID other : m_table.classesWithInstanceMember(name))
            tableSpeaksOfObject &= other == builtinClass || !m_table.isSameOrSubclassOf(other, builtinClass);
    }
    if (isRead() && tableSpeaksOfObject) {
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
        if (!giveWhatIsFoundFrom(prototype))
            m_flow.addEdge(m_subject, m_flow.unseen());
        return;
    }

    if (!isRead() && builtinClass && name && m_table.findOwnPropertyOfInstances(builtinClass, name)) {
        mayThrow();
        storeIntoUnseen();
        return;
    }

    if (isRead()) {
        if (!prototype || mayRunCodeFoundFrom(prototype))
            m_flow.addEdge(m_subject, m_flow.unseen());
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
        cannotTell();
        for (BuiltinID holder = name ? prototype : BuiltinID(); holder; holder = m_table.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? m_table.canonicalID(m_table.at(holder).inheritsFrom) : BuiltinID()) {
            BuiltinID member = m_table.findOwnMember(holder, name);
            if (!member)
                continue;
            BuiltinID canonical = m_table.canonicalID(member);
            if (m_table.at(member).scope == BuiltinScope::Realm && isObjectKind(m_table.at(canonical).kind))
                give(m_flow.anythingBeside(canonical, m_function));
            break;
        }
        return;
    }

    mayThrow();
    if (m_access.kind == Kind::Store && tableSpeaksOfObject && nativeSetterFoundFromKeepsReceiver(prototype))
        return;
    if (m_access.kind != Kind::Define && (!prototype || mayRunCodeFoundFrom(prototype))) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        storeIntoUnseen();
    }
    storeInto(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
}

void AccessInLinkAccesses::handOn(const LinkType& type)
{
    if (m_access.whatHoldsNothing != noSummaryNode)
        m_flow.arriveFromSource(m_flow.nodeOf(m_function, m_access.whatHoldsNothing), type);
}

void AccessInLinkAccesses::resolveContents(const LinkType& subject)
{
    bool settles = m_access.whatHoldsNothing != noSummaryNode;
    if (uint16_t primitives = (subject.primitives & ~m_before.primitives) | (subject.primitivesSeen() & ~m_before.primitivesSeen())) {
        LinkType handed = primitivesForLinkAccesses(primitives);
        handed.primitivesByEscapeAlone = handed.primitives & ~LinkType::closed(subject.primitivesSeen());
        handOn(handed);
    }

    LinkType others = subject;
    others.primitives = 0;
    others.classes = LinkType::Classes::None;
    others.builtinClass = { };
    others.site = { };
    LinkType othersBefore = m_before;
    othersBefore.primitives = 0;
    othersBefore.classes = LinkType::Classes::None;
    othersBefore.builtinClass = { };
    othersBefore.site = { };
    if (!others.isNothing() && (others != othersBefore || others.holdsUnseenInSight() != othersBefore.holdsUnseenInSight())) {
        handOn(others);
        if (!settles || others.holdsUnseen) {
            LinkType unseenAlone;
            unseenAlone.holdsUnseen = true;
            LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(others == unseenAlone && !others.holdsUnseenInSight(), LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
            giveAnything();
            storeIntoUnseen();
        }
    }

    if (subject.classes != LinkType::Classes::None && (subject.classes != m_before.classes || subject.builtinClass != m_before.builtinClass || subject.site != m_before.site))
        resolveContentsOfInstances(instancesAmongForLinkAccesses(subject));

    if (settles && m_access.keyNode != noSummaryNode) {
        const LinkType found = m_flow.typeAt(m_flow.nodeOf(m_function, m_access.keyNode));
        if (holdsObjectsForLinkAccesses(found))
            giveAnything();
    }
}

bool AccessInLinkAccesses::settlesPromiseThatCallHasJustMade()
{
    if (!m_access.standsRightBehindCall)
        return false;
    const ThinSummary& summary = m_flow.summaries.at(m_function);
    const SummaryNode& subject = summary.nodes[m_access.subject];
    if (subject.kind != SummaryNode::Kind::ResultOfCall)
        return false;
    const SummaryCall& call = summary.calls[subject.first];
    if (call.isConstruction)
        return false;
    auto makesItsPromise = [&](FunctionID function) {
        const Function& record = m_program.at(function);
        return record.has(FunctionFlags::IsAsync) && !record.has(FunctionFlags::IsGenerator);
    };
    auto isClosureOfSuchFunctionAt = [&](FlowNodeID node) {
        if (m_laysObservers)
            m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
        LinkType callee = m_flow.typeAt(node);
        callee.primitives = 0;
        return callee.functions == LinkType::Functions::Closure && callee == closureForLinkAccesses(callee.function) && makesItsPromise(callee.function);
    };
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        return makesItsPromise(call.function);
    case SummaryCall::Callee::Value:
        return call.subject != noSummaryNode && isClosureOfSuchFunctionAt(m_flow.nodeOf(m_function, call.subject));
    case SummaryCall::Callee::Name: {
        if (call.receiver == noSummaryNode || !call.receiver || !call.key || call.isMadeByProtocol)
            return false;
        FlowNodeID receiverNode = m_flow.nodeOf(m_function, call.receiver);
        if (m_laysObservers)
            m_flow.observe(receiverNode, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
        LinkType receiver = m_flow.typeAt(receiverNode);
        receiver.primitives &= ~Primitive::UndefinedOrNull;
        LinkType objects;
        objects.objects = LinkType::Objects::Layout;
        objects.layout = receiver.layout;
        if (receiver.objects != LinkType::Objects::Layout || receiver != objects)
            return false;
        LayoutID layout = receiver.layout;
        if (m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype) || m_flow.escapedLayouts.get(layout.index()) || findField(m_program, layout, call.key))
            return false;
        LinkFlow::FoundMembers members = m_flow.findMembers(layout, ClassSide::Prototype, call.key);
        return members.size() == 1 && members[0].kind == MethodKind::Method && !members.lookupGoesOnAboveClasses() && isClosureOfSuchFunctionAt(m_flow.member(members[0].classLayout, ClassSide::Prototype, members[0].index));
    }
    case SummaryCall::Callee::Builtin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveContentsOfInstances(const LinkType& instances)
{
    bool settles = m_access.whatHoldsNothing != noSummaryNode;
    bool isOneClass = instances.classes != LinkType::Classes::Mixed;

    if (!isOneClass && m_access.slot.indexOfInternalField()) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        giveAnything();
        storeIntoUnseen();
        return;
    }

    if (settles) {
        bool mayBePromise = !isOneClass || m_table.isSameOrSubclassOf(instances.builtinClass, BuiltinClasses::Promise);
        if (!isOneClass || !mayBePromise)
            handOn(instances);
        if (!mayBePromise)
            return;
    }

    TypedArrayType typedArray = isOneClass ? m_table.typedArrayTypeOf(instances.builtinClass) : NotTypedArray;
    if (typedArray != NotTypedArray && typedArray != TypeDataView && m_access.slot == SlotOfContents(ContentsSlot::Element)) {
        give(m_flow.fromTypeID(elementTypeOf(typedArray)));
        return;
    }

    for (SlotOfContents slot : m_flow.slotsBehind(instances.builtinClass, m_access.slot)) {
        FlowNodeID held = m_flow.contentsOf(instances, slot);
        giveFrom(held);
        storeInto(held);
    }
    if (!settles)
        return;

    auto mayHoldSomething = [&](FlowNodeID node) {
        if (m_laysObservers)
            m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
        return !m_flow.typeAt(node).isNothing();
    };
    bool hasJustBeenMade = settlesPromiseThatCallHasJustMade();
    if (mayHoldSomething(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames())) && !hasJustBeenMade) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        giveAnything();
    }

    for (BuiltinClassID current = isOneClass ? instances.builtinClass : BuiltinClassID(BuiltinClasses::Promise); current; current = m_table.at(current).parent) {
        for (BuiltinSlotID id : m_table.at(current).slots) {
            if (SlotOfContents other = m_flow.slotOf(id); other != m_access.slot)
                m_flow.addEdge(m_flow.contentsOf(instances, other), m_flow.thrown(m_function));
        }
    }
}

void AccessInLinkAccesses::resolveOnBuiltin(BuiltinID builtin)
{
    if (isRead() && isNamed()) {
        if (!giveWhatLightFormServes(builtin))
            giveWhatIsFoundFrom(builtin);
        return;
    }
    if (isRead())
        m_flow.noteLossOf(builtin);
    cannotTell();
}

bool AccessInLinkAccesses::giveWhatLightFormServes(BuiltinID builtin)
{
    const BuiltinCondition* light = m_flow.lightFormOf(builtin);
    BuiltinName name = light ? m_program.builtinNameOf(m_access.key) : BuiltinName();
    if (!name || (!std::ranges::contains(light->valuesServed, name) && !std::ranges::contains(light->methodsServed, name)))
        return false;
    mayThrow();
    giveAnything();
    return true;
}

BuiltinID AccessInLinkAccesses::prototypeOfInstancesOf(BuiltinClassID builtinClass) const
{
    BuiltinID prototype;
    for (BuiltinClassID current = builtinClass; current && !prototype; current = m_table.at(current).parent)
        prototype = m_table.at(current).prototype;
    return prototype;
}

bool AccessInLinkAccesses::membersOfObjectPrototypeAreWhatTableLists()
{
    if (!m_program.admitsReplacedBuiltins)
        return true;
    for (BuiltinID member : m_table.membersOf(m_table.canonicalID(Builtins::ObjectPrototype))) {
        if (m_program.isUnlocked(member) || m_program.isUnlocked(m_table.canonicalID(member)))
            return false;
        if (!m_program.isLockLeftToProgram(member))
            continue;
        if (!m_flow.takeForConstant(member))
            return false;
        tellAgainOnceClimbs(m_flow.writtenAtMember(member));
    }
    return true;
}

bool AccessInLinkAccesses::indexesAreAbsentFrom(BuiltinID start) const
{
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        if (!m_table.closesIndexKeys(holder) || !m_table.hasLockedPrototypeLink(holder))
            return false;
        const Builtin& object = m_table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

LookupInLinkAccesses AccessInLinkAccesses::lookUpFrom(BuiltinID start, BuiltinID& member) const
{
    if (FlowNodeID added = m_flow.testRuns().nodeOfNameAdded(m_access.key)) {
        if (m_laysObservers)
            m_flow.observe(added, observerOfWhatWasLookedUp());
        if (m_flow.testRuns().isNameAdded(added))
            return LookupInLinkAccesses::Unknown;
    }
    auto& lookups = m_parts.state.lookups[m_flow.indexOfLaneAtWork()];
    uint64_t question = static_cast<uint64_t>(start.index()) << 32 | m_access.key.index();
    if (auto known = lookups.find(question); known != lookups.end()) {
        member = BuiltinID(known->value & 0xffff);
        return static_cast<LookupInLinkAccesses>(known->value >> 16);
    }
    LookupInLinkAccesses outcome = lookUpInTableFrom(start, member);
    lookups.add(question, static_cast<uint32_t>(outcome) << 16 | member.index());
    return outcome;
}

LookupInLinkAccesses AccessInLinkAccesses::lookUpInTableFrom(BuiltinID start, BuiltinID& member) const
{
    MembersUnderKey members(m_program, m_access.key);
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        if (compiledModuleWithObject(m_program, holder) || m_table.at(m_table.rootOf(holder)).scope != BuiltinScope::Realm)
            return LookupInLinkAccesses::Unknown;
        member = members.findOwnMember(holder);
        if (member)
            return LookupInLinkAccesses::Found;
        if (!listsEveryMemberUnder(m_program, holder, m_access.key) || !members.isClosedKey(holder) || !m_table.hasLockedPrototypeLink(holder))
            return LookupInLinkAccesses::Unknown;
        const Builtin& object = m_table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return LookupInLinkAccesses::Unknown;
        case BuiltinPrototypeLink::Null:
            return LookupInLinkAccesses::Absent;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

bool AccessInLinkAccesses::giveWhatIsFoundFrom(BuiltinID start)
{
    BuiltinID member;
    switch (lookUpFrom(start, member)) {
    case LookupInLinkAccesses::Absent:
        givePrimitives(Primitive::Undefined);
        return true;
    case LookupInLinkAccesses::Found: {
        m_flow.note(LinkFlow::Note::BuiltinRead, member.index());
        LinkType held = typeHeldAtLocation(member);
        give(held);
        return !held.holdsUnseen;
    }
    case LookupInLinkAccesses::Unknown:
        cannotTell();
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool AccessInLinkAccesses::mayRunCodeFoundFrom(BuiltinID start)
{
    BuiltinID member;
    switch (lookUpFrom(start, member)) {
    case LookupInLinkAccesses::Absent:
        return false;
    case LookupInLinkAccesses::Found:
        return (!isRead() && m_table.at(member).kind == BuiltinKind::Accessor) || typeHeldAtLocation(member).holdsUnseen;
    case LookupInLinkAccesses::Unknown:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool AccessInLinkAccesses::nativeSetterFoundFromKeepsReceiver(BuiltinID start)
{
    BuiltinID member;
    if (!m_value || lookUpFrom(start, member) != LookupInLinkAccesses::Found || m_table.at(member).kind != BuiltinKind::Accessor)
        return false;
    bool hasSetter = false;
    for (const BuiltinOverload& overload : m_table.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Set)
            continue;
        static constexpr OperandUses usesThatLeaveReceiverOtherThanMade {
            OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
            OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype,
        };
        static constexpr OperandUses usesThatLeaveReceiverAsMade {
            OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly,
            OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::RetainsContents, OperandUse::MovesElements, OperandUse::PassesToUserCode,
            OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
        };
        static_assert(everyOperandUseIsInExactlyOneOf({ usesThatLeaveReceiverOtherThanMade, usesThatLeaveReceiverAsMade }));
        const BuiltinParameter* receiver = m_table.parameterFor(overload, BuiltinOperand::receiver());
        if (overload.isUndeclared() || (receiver && receiver->uses.containsAny(usesThatLeaveReceiverOtherThanMade)))
            return false;
        hasSetter = true;
    }
    if (!hasSetter || !holdsWhatTableLists(member))
        return false;
    tellAgainOnceClimbs(m_value);
    if (holdsObjectsForLinkAccesses(m_flow.typeAt(m_value)))
        return false;
    m_flow.note(LinkFlow::Note::BuiltinRead, member.index());
    return true;
}

LinkType AccessInLinkAccesses::typeHeldAtLocation(BuiltinID member)
{
    LinkType held = typeHeldAtLocationThatRealmHas(member);
    if (m_table.has(BuiltinMark::MayBeAbsent, member))
        held.primitives |= Primitive::Undefined;
    return held;
}

bool AccessInLinkAccesses::holdsWhatTableLists(BuiltinID member)
{
    const Builtin& entry = m_table.at(member);
    bool isLockedByTable = entry.mutability == BuiltinMutability::AlwaysLocked || m_table.isLocationOfLockedObject(member);
    if (entry.scope != BuiltinScope::Realm || entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances)
        || m_program.isUnlocked(member) || m_program.isUnlocked(m_table.canonicalID(member)) || (!isLockedByTable && !m_flow.takeForConstant(member)))
        return false;
    if (!isLockedByTable) {
        m_hasLeanedOnMemberTakenForConstant = true;
        tellAgainOnceClimbs(m_flow.writtenAtMember(member));
    }
    return true;
}

LinkType AccessInLinkAccesses::typeHeldAtLocationThatRealmHas(BuiltinID member)
{
    const Builtin& entry = m_table.at(member);
    BuiltinID canonical = m_table.canonicalID(member);
    if (!holdsWhatTableLists(member)) {
        mayThrow();
        return m_flow.anythingBeside(entry.scope == BuiltinScope::Realm && isObjectKind(m_table.at(canonical).kind) ? canonical : BuiltinID(), m_function);
    }

    switch (entry.kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method: {
        if (compiledModuleWithObject(m_program, canonical))
            return LinkType::anything();
        LinkType type;
        type.builtin = canonical;
        return type;
    }
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        return typeAnsweredByReadsOf(member);
    case BuiltinKind::Constant:
        if (std::optional<double> number = m_table.numberOf(member)) {
            uint16_t primitive = static_cast<uint16_t>(primitiveOfNumber(*number));
            return primitivesForLinkAccesses(LinkType::primitivesOf(primitive, primitive == Primitive::Integer ? Range::one(static_cast<int64_t>(*number)) : Range::empty()));
        }
        if (m_table.nameOfSymbol(member))
            return primitivesForLinkAccesses(Primitive::Symbol);
        if (m_table.stringOf(member))
            return primitivesForLinkAccesses(Primitive::String);
        return LinkType::anything();
    case BuiltinKind::NativeBinding:
        return LinkType::anything();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LinkType AccessInLinkAccesses::typeAnsweredByReadsOf(BuiltinID member)
{
    BuiltinID canonical = m_table.canonicalID(member);
    bool standsForLightForm = m_flow.lightFormOf(canonical);
    if (standsForLightForm) {
        tellAgainOnceClimbs(m_flow.writtenAtMember(canonical));
        standsForLightForm = !m_flow.isAskedForOtherNames(canonical);
    }
    LinkType answered;
    bool hasRow = false;
    for (const BuiltinOverload& overload : m_table.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Get)
            continue;
        if (overload.throws())
            mayThrow();
        if (overload.isUndeclared() || overload.resultIsDeclaredOnly())
            return LinkType::anything();
        answered = m_flow.joinOf(answered, standsForLightForm ? typeWrittenOfLightForm(overload.result.type, canonical) : typeWrittenBy(overload.result.type));
        hasRow = true;
    }
    return hasRow ? answered : LinkType::anything();
}

LinkType AccessInLinkAccesses::typeWrittenOfLightForm(BuiltinTypeID id, BuiltinID canonical) const
{
    if (!id || m_table.at(id).isAnything())
        return LinkType::anything();
    const BuiltinType& written = m_table.at(id);
    LinkType type = primitivesForLinkAccesses(LinkType::primitivesOf(primitivesOfBuiltinType(written), integerRangeWrittenBy(written)));
    if (!written.stringLiterals.empty())
        type.primitives |= Primitive::String;
    type.builtin = canonical;
    return type;
}

LinkType AccessInLinkAccesses::typeWrittenBy(BuiltinTypeID id)
{
    if (!id)
        return LinkType::anything();
    const BuiltinType& written = m_table.at(id);
    if (written.isAnything() || written.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }))
        return LinkType::anything();

    LinkType type = primitivesForLinkAccesses(LinkType::primitivesOf(primitivesOfBuiltinType(written), integerRangeWrittenBy(written)));
    if (!written.stringLiterals.empty())
        type.primitives |= Primitive::String;
    for (const BuiltinTypePart& part : written.parts) {
        if (part.kind == BuiltinTypePartKind::Operand && part.operand->isReceiver() && isRead() && m_value) {
            giveFrom(m_receiver);
            continue;
        }
        if (part.kind == BuiltinTypePartKind::Instance) {
            LinkType instances = LinkType::anything();
            instances.holdsLostBuiltins = m_flow.unseenObjectsBelow(m_flow.prototypeOfInstancesOf(part.builtinClass), m_function).holdsLostBuiltins;
            return instances;
        }
        if (part.kind != BuiltinTypePartKind::Builtin)
            return LinkType::anything();
        BuiltinID builtin = m_table.canonicalID(part.builtin);
        if (type.builtin && type.builtin != builtin)
            return LinkType::anything();
        type.builtin = builtin;
    }
    return type;
}

PropertyKeyID LinkFlow::keyOf(FunctionID function, const SummaryAccess& access) const
{
    if (access.key || access.keyNode == noSummaryNode || access.isToContents())
        return access.key;
    FlowNodeID node = nodeOf(function, access.keyNode);
    LinkType oneSymbol;
    oneSymbol.primitives = LinkType::oneSymbol;
    LinkType held = typeAt(node);
    held.primitives &= LinkType::closed(ViewAtWork::current() ? Primitive::All & ~Primitive::UndefinedOrNull : Primitive::All);
    if (held != oneSymbol)
        return { };
    PropertyKeyID symbol = symbolAt(node);
    if (!symbol || !isNameInTextOf(program, symbol, function))
        return { };
    if (ViewAtWork::current()) {
        if (!m_symbolsInSight.areNames && isSymbolOfLibrary(program, symbol) && m_symbolsInSight.hasPreparation(symbol))
            return { };
        return symbol;
    }
    if (!m_symbolsWithPreparationAreNames && isSymbolOfLibrary(program, symbol) && hasPreparation(symbol))
        return { };
    return symbol;
}

PropertyKeyID LinkFlow::keySeenOf(FunctionID function, const SummaryAccess& access) const
{
    if (access.key || access.keyNode == noSummaryNode || access.isToContents())
        return { };
    FlowNodeID node = nodeOf(function, access.keyNode);
    const LinkType& held = typeAt(node);
    if (!isSymbolAsFarAsIsSeen(held) || !(held.primitivesSeen() & LinkType::oneSymbol) || !(held.primitives & LinkType::closed(Primitive::All) & ~LinkType::oneSymbol))
        return { };
    PropertyKeyID symbol = symbolNotedAt(node);
    if (!symbol || !isNameInTextOf(program, symbol, function))
        return { };
    if (!m_symbolsInSight.areNames && isSymbolOfLibrary(program, symbol) && m_symbolsInSight.hasPreparation(symbol))
        return { };
    return symbol;
}

void LinkFlow::noteKeyOfAccessInLibrary(FunctionID function, uint32_t record, const SummaryAccess& access)
{
    if (PropertyKeyID seen = symbolSeenAt(nodeOf(function, access.keyNode)); seen && isSymbolOfLibrary(program, seen) && isLibraryCode(program, program.at(function)) && m_symbolsInSight.areNames && !keyOf(function, access) && !keySeenOf(function, access)) {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        if (!m_symbolsInSight.besideOtherKeys.contains(seen.bits()))
            m_symbolsInSight.notedBesideOtherKeys.append(seen);
    }
    PropertyKeyID symbol = symbolAt(nodeOf(function, access.keyNode));
    if (!symbol || !isSymbolOfLibrary(program, symbol) || !isLibraryCode(program, program.at(function)))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (keyOf(function, access)) {
        if (m_accessesUnderSymbolsOfLibraryListed.add(static_cast<uint64_t>(function.index()) << 32 | record).isNewEntry)
            m_accessesUnderSymbolsOfLibrary.append({ symbol, FlowObserver { FlowObserver::Kind::KeyOfAccess, function, record } });
        return;
    }
    if (!m_symbolsWithPreparationAreNames)
        return;
    if (!m_symbolsOfLibraryBesideOtherKeys.contains(symbol.bits()))
        m_symbolsNotedBesideOtherKeys.append(symbol);
}

bool LinkFlow::takeInWhatWasNotedOfSymbols()
{
    {
        SymbolsWithPreparationInSight& inSight = m_symbolsInSight;
        for (PropertyKeyID symbol : std::exchange(inSight.notedBesideOtherKeys, { }))
            inSight.besideOtherKeys.add(symbol.bits());
        for (PropertyKeyID symbol : std::exchange(inSight.notedWithPreparation, { }))
            inSight.withPreparation.add(symbol.bits());
        if (std::exchange(inSight.unknownSymbolWasNoted, false))
            inSight.everySymbolOfLibraryMayHaveOne = true;
        if (inSight.areNames) {
            bool someSymbolWithPreparationLiesBesideOtherKeys = inSight.everySymbolOfLibraryMayHaveOne;
            for (uint32_t bits : inSight.besideOtherKeys)
                someSymbolWithPreparationLiesBesideOtherKeys |= inSight.hasPreparation(PropertyKeyID::fromBits(bits));
            if (someSymbolWithPreparationLiesBesideOtherKeys)
                inSight.areNames = false;
        }
    }
    if (m_symbolsNotedBesideOtherKeys.isEmpty() && m_symbolsNotedWithPreparation.isEmpty() && !m_unknownSymbolWasNotedWithPreparation)
        return false;
    bool hasGrown = false;
    for (PropertyKeyID symbol : std::exchange(m_symbolsNotedBesideOtherKeys, { }))
        hasGrown |= m_symbolsOfLibraryBesideOtherKeys.add(symbol.bits()).isNewEntry;
    for (PropertyKeyID symbol : std::exchange(m_symbolsNotedWithPreparation, { }))
        hasGrown |= m_symbolsWithPreparation.add(symbol.bits()).isNewEntry;
    if (std::exchange(m_unknownSymbolWasNotedWithPreparation, false))
        hasGrown |= !std::exchange(m_everySymbolOfLibraryMayHavePreparation, true);
    if (!hasGrown)
        return false;
    if (m_symbolsWithPreparationAreNames) {
        bool someSymbolWithPreparationLiesBesideOtherKeys = m_everySymbolOfLibraryMayHavePreparation;
        for (uint32_t bits : m_symbolsOfLibraryBesideOtherKeys)
            someSymbolWithPreparationLiesBesideOtherKeys |= hasPreparation(PropertyKeyID::fromBits(bits));
        if (!someSymbolWithPreparationLiesBesideOtherKeys)
            return true;
        m_symbolsWithPreparationAreNames = false;
    }
    tellAccessesUnderSymbolsWithPreparation();
    return true;
}

void LinkFlow::tellAccessesUnderSymbolsWithPreparation()
{
    for (size_t index = 0; index < m_accessesUnderSymbolsOfLibrary.size(); ++index) {
        auto [symbol, observer] = m_accessesUnderSymbolsOfLibrary[index];
        if (hasPreparation(symbol))
            tell(observer, typeAt(nodeOf(observer.function, summaries.at(observer.function).accesses[observer.record].keyNode)), LinkType { });
    }
}

void LinkFlow::noteUnknownSymbolHandedToPreparation(UnknownSymbolIs unknownSymbolIs)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (unknownSymbolIs != UnknownSymbolIs::UnknownByEscapeAlone)
        m_symbolsInSight.unknownSymbolWasNoted = true;
    if (unknownSymbolIs != UnknownSymbolIs::UnknownInSightAlone && !m_everySymbolOfLibraryMayHavePreparation)
        m_unknownSymbolWasNotedWithPreparation = true;
}

void LinkFlow::noteSymbolHandedToPreparation(FunctionID caller, FlowNodeID node)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_nodesHandedToPreparation.of(*this).add(static_cast<uint32_t>(node.index())).isNewEntry)
        observe(node, FlowObserver { FlowObserver::Kind::SymbolOfPreparation, caller, static_cast<uint32_t>(node.index()) });
}

void LinkFlow::preparationObserved(const FlowObserver& observer, const LinkType& now)
{
    LinkType held = now;
    held.primitives &= LinkType::closed(Primitive::All);
    if (held.isNothing())
        return;
    LinkType oneSymbol;
    oneSymbol.primitives = LinkType::oneSymbol;
    if (LinkType seen = held.asSeen(); !seen.isNothing()) {
        PropertyKeyID symbolSeen = seen == oneSymbol ? symbolSeenAt(FlowNodeID(observer.record)) : PropertyKeyID();
        if (!symbolSeen)
            noteUnknownSymbolHandedToPreparation(UnknownSymbolIs::UnknownInSightAlone);
        else {
            LockerOfWhatIsShared lockerOfWhatIsShared(*this);
            if (!m_symbolsInSight.withPreparation.contains(symbolSeen.bits()))
                m_symbolsInSight.notedWithPreparation.append(symbolSeen);
        }
    }
    PropertyKeyID symbol = held == oneSymbol ? symbolAt(FlowNodeID(observer.record)) : PropertyKeyID();
    if (!symbol) {
        noteUnknownSymbolHandedToPreparation(UnknownSymbolIs::UnknownByEscapeAlone);
        return;
    }
    if (!m_symbolsWithPreparation.contains(symbol.bits()))
        m_symbolsNotedWithPreparation.append(symbol);
}

void LinkFlow::accessObserved(const FlowObserver& observer, const LinkType&)
{
    PrivatePartsInLinkAccesses parts { typeObservedBefore(), m_firstFieldOfLayout, stateOfLinkAccesses() };
    if (observer.kind == FlowObserver::Kind::MemberOfSharedAccess) {
        const SharedAccessInLinkAccesses& shared = *parts.state.sharedAccesses[parts.state.membersOfSharedAccesses[observer.record].sharedAccess];
        AccessInLinkAccesses(*this, parts, shared, observer.record, false).resolveMemberOfSharedAccess();
        return;
    }
    AccessInLinkAccesses(*this, parts, observer).resolve();
    if (PropertyKeyID symbol = keySeenOf(observer.function, summaries.at(observer.function).accesses[observer.record])) {
        AccessInLinkAccesses underSymbol(*this, parts, observer);
        underSymbol.goByKey(symbol);
        underSymbol.resolve();
    }
}

StateOfLinkAccesses& LinkFlow::stateOfLinkAccesses()
{
    if (!m_stateOfLinkAccesses) {
        m_stateOfLinkAccesses.reset(new StateOfLinkAccesses);
        m_stateOfLinkAccesses->lookups.grow(numberOfLanes());
        m_stateOfLinkAccesses->codeFoundAboveClosures.fill(0, 2 * static_cast<size_t>(program.types.numberOfPropertyKeys()));
    }
    return *m_stateOfLinkAccesses;
}

void LinkFlow::prepareAccessesForSeveralThreads()
{
    stateOfLinkAccesses();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
