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

// `anything` speaks only of what has escaped, not of what is lost. So wherever a read gives it, whatever may lie there has flowed into unseen(), or flows into the result beside it.
// An access only ever lays edges, lets types arrive and sets bits. Told by its subject it resolves the parts of the type that are not as they were. Told by its key or by a member it resolves them all.

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

// What this file keeps in LinkFlow. The table of built-ins answers a question about a key the same way as long as Link runs, and every access under the key asks it again.
// WHAT MANY ACCESSES DO ALIKE IS DONE ONCE, as with calls (LinkFlow::calleesOfSharedCalls). The objects of a family have an aggregate for each key. Its prototype objects and its class objects have this:
// one access for each (which of the two, family, key, kind and form), with nodes of its own, which does member by member what an access on that one member does.
// Each access that goes by the family lays an edge to its value and one from what it throws: members + accesses, where each access by itself makes members x accesses.
struct SharedAccessInLinkAccesses {
    enum class Subjects : uint8_t { PrototypeObjects, ClassObjects };

    Subjects subjects { Subjects::PrototypeObjects };
    // Whoever came first: an observer names a function. Who that is hangs on the order, so nothing may go by it but what is true of all that go by the shared access: they are code of the library, or none of them is.
    FunctionID firstFunction;
    // The kind, the key and the flags count. It has no nodes of a summary.
    SummaryAccess access;
    bool keyIsOnlyNumbers { false };
    bool keyMayNamePrototypeLink { false };
    // It holds all the prototype objects or all the class objects of the family.
    FlowNodeID subject;
    // Null, or LinkFlow::unseen(), where that is so of the accesses that go by it.
    FlowNodeID value;
    FlowNodeID thrown;
};

// FlowObserver::record of Kind::MemberOfSharedAccess. A climb is looked at for that member alone.
struct MemberOfSharedAccessInLinkAccesses {
    uint32_t sharedAccess { 0 };
    LayoutID layout;
};

class StateOfLinkAccesses {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkAccesses);
    WTF_MAKE_NONCOPYABLE(StateOfLinkAccesses);
public:
    StateOfLinkAccesses() = default;

    // AccessInLinkAccesses::lookUpFrom(). The number of the built-in at which the lookup starts above the number of the key. The outcome above the number of the member.
    // By the lane at work: each thread keeps what it has asked.
    Vector<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> lookups;
    // AccessInLinkAccesses::mayRunCodeFoundAboveClosures(). By twice the number of the key, and one more for a read.
    Vector<uint8_t> codeFoundAboveClosures;

    // The family above the key, and the rest of what tells shared accesses apart.
    HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> indexesOfSharedAccesses;
    Vector<std::unique_ptr<SharedAccessInLinkAccesses>> sharedAccesses;
    Vector<MemberOfSharedAccessInLinkAccesses> membersOfSharedAccesses;
};

void DeleterOfStateOfLinkAccesses::operator()(StateOfLinkAccesses* state) const
{
    delete state;
}

// All that it can be are prototype objects and class objects of the program. Null stands above a class that extends null, and a lookup from there throws.
static bool isAboveHomeObjectOfClassForLinkAccesses(const LinkType& type)
{
    bool namesOnlyPrototypes = type.objects == LinkType::Objects::None || type.objects == LinkType::Objects::Prototype || type.objects == LinkType::Objects::Prototypes;
    bool namesOnlyClassObjects = type.functions == LinkType::Functions::None || type.functions == LinkType::Functions::Closure || type.functions == LinkType::Functions::Constructors;
    return namesOnlyPrototypes && namesOnlyClassObjects && type.classes == LinkType::Classes::None && !type.holdsUnseen && !type.builtin && !(type.primitives & ~Primitive::UndefinedOrNull);
}

namespace {

enum class LookupInLinkAccesses : uint8_t { Absent, Found, Unknown };

// What LinkFlow keeps to itself. accessObserved() is a member, and hands it over.
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
    // ThinSummary::namesThatLoaderTakes.
    bool isTakenByLoader() const { return isRead() && m_function && std::ranges::contains(m_flow.summaries.at(m_function).namesThatLoaderTakes, m_access.value, &ThinSummary::NameThatLoaderTakes::read); }
    bool readKey();

    void give(const LinkType&);
    void giveAnything() { give(LinkType::anything()); }
    void givePrimitives(uint16_t primitives) { give(primitivesForLinkAccesses(primitives)); }
    void giveFrom(FlowNodeID);
    // Without the mark of an empty cell, which no read by name finds.
    void giveWhatIsHeldAt(FlowNodeID);
    void giveWhatIsEstablishedOf(const DeclaredType&);
    void storeInto(FlowNodeID);
    void storeIntoUnseen() { storeInto(m_flow.unseen()); }
    void mayThrow();
    enum class UnknownIs : uint8_t { ByEscapeAlone, InSight };
    void cannotTell(UnknownIs = UnknownIs::InSight);
    // Code that nobody has seen may run with the subject as `this`.
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
    bool mayStoreIntoPrototypeLink() const { return m_access.kind == Kind::Store && m_keyMayNamePrototypeLink; }
    bool namesOnlyElements() const { return !isNamed() && m_keyIsOnlyNumbers && indexesAreAbsentFrom(Builtins::ObjectPrototype); }
    bool namesNothingOnPlainClosures() const;

    void resolveUnderPrivateName();
    void resolveOnLayout(LayoutID);
    void resolvePrototypeLinkOfLayout(LayoutID, const Layout&);
    void resolveOnNamespace(const Layout&);
    void giveExport(ModuleID, uint32_t index);
    void resolveOnFamily(FamilyID);
    void resolveOnLostObjects(LayoutID member);
    void resolveOnPrototype(LayoutID);
    void resolveOnPrototypes(FamilyID);
    // False: this one goes member by member itself.
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

    // A getter and a setter under one key are one property: a read of it runs the getter. One with a setter alone reads as undefined.
    static bool findsGetterAmong(const Vector<LinkFlow::FoundMember, 2>& members) { return std::ranges::contains(members, MethodKind::Getter, &LinkFlow::FoundMember::kind); }
    void enterAccessorAt(FlowNodeID member);
    void enterAccessor(FunctionID);
    // `super.x = v`, where the lookup from the subject upward comes to no accessor: the property is laid on the receiver, which nobody follows from here.
    void layOnReceiverUnlessAccessorAnswers(const Vector<LinkFlow::FoundMember, 2>&);

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
    LookupInLinkAccesses lookUpFrom(BuiltinID start, BuiltinID& member) const;
    LookupInLinkAccesses lookUpInTableFrom(BuiltinID start, BuiltinID& member) const;
    // Whether it could tell.
    bool giveWhatIsFoundFrom(BuiltinID start);
    bool mayRunCodeFoundFrom(BuiltinID start);
    LinkType typeHeldAtLocation(BuiltinID member);
    LinkType typeHeldAtLocationThatRealmHas(BuiltinID member);
    LinkType typeAnsweredByReadsOf(BuiltinID member);
    LinkType typeWrittenBy(BuiltinTypeID) const;

    LinkFlow& m_flow;
    const PrivatePartsInLinkAccesses& m_parts;
    const Program& m_program;
    const BuiltinTable& m_table;
    FunctionID m_function;
    // Of a shared access: the member that is looked at, in StateOfLinkAccesses::membersOfSharedAccesses.
    uint32_t m_record;
    const SharedAccess* m_shared { nullptr };
    // As the summary has it. Where all that lies at the node of the key is one symbol, `key` is the key of that symbol.
    SummaryAccess m_access;
    FlowNodeID m_subject;
    // What an accessor gets as `this`: SummaryAccess::receiver.
    FlowNodeID m_receiver;
    FlowNodeID m_value;
    // What the subject held when this access was last told by it. Nothing: every part is resolved.
    LinkType m_before;
    NameClass m_nameClass { NameClass::Ordinary };
    // Told by a node at which it has put an observer itself, a member that is an accessor among them, it finds nothing to watch that it has not found.
    bool m_laysObservers;
    bool m_keyIsWritten { true };
    // WHOEVER HAS LOOKED SOMETHING UP IS TOLD AGAIN WHEN IT CHANGES: of all the parts of its subject, once the node climbs.
    void tellAgainOnceClimbs(FlowNodeID);
    bool m_keyIsOnlyNumbers { false };
    bool m_keyMayNamePrototypeLink { false };
    // `anything` has arrived at what the function throws. It stays there.
    bool m_hasThrown { false };
    // mayThrow() was asked, whether or not it had been before.
    bool m_wasAskedToThrow { false };
    // typeHeldAtLocationThatRealmHas() has answered by a member that is taken for a constant until a write is followed to it.
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
        __builtin_prefetch(&flow.typeAt(m_value));
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

void AccessInLinkAccesses::mayThrow()
{
    m_wasAskedToThrow = true;
    if (!std::exchange(m_hasThrown, true)) {
        LinkFlow::UnknownByEscapeAlone unknownInSight(false);
        m_flow.arrive(thrownNode(), LinkType::anything());
    }
}

// A subject that nobody can tell anything of has escaped. The receiver of `super.x` need not have.
void AccessInLinkAccesses::cannotTell(UnknownIs unknownIs)
{
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(unknownIs == UnknownIs::ByEscapeAlone, LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
    // An access that several functions share is told for all of them.
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

// `class A extends B {}` beside `class B extends A {}` compiles, so the row may be a ring.
template<typename Functor>
void AccessInLinkAccesses::forEachLayoutUpTheBases(LayoutID first, const Functor& stopsAt) const
{
    size_t steps = 0;
    for (LayoutID current = first; current && steps < m_program.layouts.size(); current = m_program.at(current).base, ++steps) {
        if (stopsAt(current))
            return;
    }
}

// The value is followed: whoever reads the key where no field answers finds it, and it escapes when the layout does. Whether the name gets a slot or the layout gains a property is said behind the flow, so it opens nothing.
void AccessInLinkAccesses::storeBesideFieldsOf(LayoutID id)
{
    storeInto(m_flow.appendedField(id, m_access.key));
}

bool AccessInLinkAccesses::readKey()
{
    bool hasNode = m_access.keyNode != noSummaryNode;
    FlowNodeID node = hasNode ? m_flow.nodeOf(m_function, m_access.keyNode) : FlowNodeID();
    LinkType key = hasNode ? m_flow.typeAt(node) : LinkType::anything();
    // A key is a value of JavaScript: the mark of an empty cell never comes this far. A variable holds it in front of whatever is stored there.
    key.primitives &= LinkType::closed(Primitive::All);
    if (key.isNothing())
        return false;
    if (hasNode && !m_access.isToContents())
        m_flow.noteKeyOfAccessInLibrary(m_function, m_record, m_access);
    // `o[name]` is `o.name`. What the access lays down stands when a second key comes: then it is told, and goes on as an access under any key.
    if (PropertyKeyID symbol = m_flow.keyOf(m_function, m_access)) {
        m_access.key = symbol;
        m_nameClass = m_flow.nameClassOf(symbol);
        return true;
    }

    bool holdsObjects = holdsObjectsForLinkAccesses(key);
    if (holdsObjects) {
        // It is converted to a key, which runs its own code.
        if (hasNode)
            m_flow.addEdge(node, m_flow.unseen());
        mayThrow();
    }
    // Whatever number it is, it is printed as an index or as another canonical numeric string: mayBeElementKey(). A counter that goes round a loop is an integer or a whole number to Link.
    m_keyIsOnlyNumbers = !holdsObjects && !(key.primitives & ~LinkType::closed(Primitive::Number));
    m_keyMayNamePrototypeLink = (holdsObjects || key.primitives & Primitive::AnyString) && keyThatBeginsSoMayNamePrototypeLink(m_program, m_access);
    return true;
}

void AccessInLinkAccesses::resolve()
{
    // A copy: the list of types moves when an aggregate is made.
    const LinkType subject = m_flow.typeAt(m_subject);
    if (subject.isNothing())
        return;
    if (m_access.isToContents()) {
        resolveContents(subject);
        return;
    }
    if (isPrivateNameInLink(m_program, m_access.key)) {
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
    // Only the members of the classes of the program are entered with the receiver: enterAccessor(). An aggregate enters what it finds with the objects of its own layouts, and a row speaks of the object that is looked at.
    if (m_receiver != m_subject && !isAboveHomeObjectOfClassForLinkAccesses(subject))
        m_flow.addEdge(m_receiver, m_flow.unseen());
    // A number names an element and never a member.
    if (!isRead() && (isNamed() || !m_keyIsOnlyNumbers)) {
        m_flow.noteWriteOn(subject, m_access.key, m_function);
        if (!m_shared)
            m_flow.noteSiteOfWrite(m_function, m_access.site, m_subject);
    }
    if ((subject.holdsUnseen && !m_before.holdsUnseen) || (subject.holdsUnseenInSight() && !m_before.holdsUnseenInSight()))
        cannotTell(subject.holdsUnseenInSight() ? UnknownIs::InSight : UnknownIs::ByEscapeAlone);
    // What the read finds on one of them is lost in turn.
    if (isRead() && m_value && subject.holdsLostBuiltins && isNamed()) {
        m_flow.note(LinkFlow::Note::ReadThroughLostBuiltins, m_access.key.bits());
        LinkType found = LinkType::anything();
        found.holdsLostBuiltins = true;
        give(found);
    }
    // What a primitive answers is a primitive, a built-in or anything, it may throw, and what is stored on one goes to unseen code. cannotTell() has said all that: just now, or when the subject first held what is unseen.
    // An access under a computed key may have been told of that while nothing had arrived at its key.
    bool hasSaidAllOfPrimitives = subject.holdsUnseen && (m_keyIsWritten || !m_before.holdsUnseen);
    if (!hasSaidAllOfPrimitives)
        resolveOnPrimitives(static_cast<uint16_t>(subject.primitives & ~m_before.primitives));

    if (subject.objects != m_before.objects || subject.layout != m_before.layout || subject.family != m_before.family) {
        switch (subject.objects) {
        case LinkType::Objects::None:
            break;
        case LinkType::Objects::Layout:
            resolveOnLayout(subject.layout);
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

    if (subject.functions != m_before.functions || subject.function != m_before.function || subject.constructorsOf != m_before.constructorsOf) {
        switch (subject.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure:
            resolveOnClosure(subject.function);
            break;
        case LinkType::Functions::Constructors:
            resolveOnConstructors(subject.constructorsOf);
            break;
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

// Whatever the subject is, what lies under the name was laid there by an access under the name. All of them meet at the field that the class declares, which the layouts below share:
// an object whose layout lacks the field has the name only from a definition that `super()` answered with it, and keeps the value where nobody else finds it.
// Nothing above the object is asked, no code of the program runs, and neither the object nor the value goes anywhere else. So it does not matter what else arrives at the subject.
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

// ---- Objects of the program

void AccessInLinkAccesses::resolveOnLayout(LayoutID id)
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
        // What is stored under `__proto__` answers every lookup from then on, and what is found there runs with the object as `this`.
        if (mayStoreIntoPrototypeLink())
            m_flow.escape(id);
        mayThrow();
        if (isRead())
            giveWhatIsHeldAt(m_flow.anyFieldReadThrough(LinkType::Objects::Layout, id, FamilyID(), namesOnlyElements()));
        else
            storeInto(m_flow.anyFieldStoredThrough(LinkType::Objects::Layout, id, FamilyID()));
        return;
    }
    if (m_nameClass == NameClass::Proto) {
        resolvePrototypeLinkOfLayout(id, layout);
        return;
    }

    bool isDefinition = m_access.kind == Kind::Define;
    // What nobody lists may stand in front of whatever the name finds, and an accessor among it gets the object as `this`. A definition looks at the object alone.
    if (!isDefinition && m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        m_flow.escape(id);
        cannotTell();
    }

    std::optional<FieldReference> field = findField(m_program, id, m_access.key);
    if (field && fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor)) {
        // The slot holds the pair of its functions, and no node says which they are.
        m_flow.escape(id);
        cannotTell();
        return;
    }

    Vector<LinkFlow::FoundMember, 2> members = m_flow.findMembers(id, ClassSide::Prototype, m_access.key);
    bool findsAccessor = false;
    bool findsGetter = findsGetterAmong(members);
    for (const LinkFlow::FoundMember& found : members) {
        FlowNodeID member = m_flow.member(found.classLayout, ClassSide::Prototype, found.index);
        switch (found.kind) {
        case MethodKind::Method:
            // A store gives the object a property of its own that hides the method. The member stands for both, for the calls by name.
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

    // A private name is found on the object itself, or the access throws.
    bool isPrivateName = m_program.at(m_access.key).kind == PropertyKeyKind::PrivateName;
    bool goesOnAboveClasses = (members.isEmpty() || m_flow.mayNotBeLinkedYetBelow(id, members[0].classLayout)) && !isPrivateName;
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
        // The object may not have the field yet, or no more, and then the read goes on upward.
        if (!goesOnAboveClasses)
            return;
        // Nothing answers there, so that gives undefined, and only where some construction leaves the field empty. A field that is deleted holds undefined from then on.
        if (field && field->layout == id && !m_flow.mayReadInFrontOfDefinition(m_function, m_access, id) && m_flow.isAbsentAboveClasses(m_access.key)) {
            giveFrom(m_flow.foundWhereFieldIsEmpty(*field));
            return;
        }
        // An object that was made to stand under `prototype` of a function has the name only if the program has stored it there, so the lookup may go on to any level.
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
        if (!m_access.isPartOfConstruction)
            noteWrittenAfterConstruction(*field);
        return;
    }
    if (findsAccessor && !isDefinition)
        return;
    if (findsAccessor)
        m_flow.escape(id);
    storeBesideFieldsOf(id);
}

void AccessInLinkAccesses::resolvePrototypeLinkOfLayout(LayoutID id, const Layout& layout)
{
    switch (m_access.kind) {
    case Kind::Read:
        if (layout.kind == LayoutKind::Instance)
            give(prototypeForLinkAccesses(id));
        else
            giveAnything();
        giveFrom(foundBesideFieldsOf(id));
        giveFrom(m_flow.appendedField(id, m_access.key));
        return;
    case Kind::Store:
        // From now on a name is looked for in an object that nobody tracks.
        noteChange(id, LayoutChange::ChangesPrototype);
        m_flow.escape(id);
        storeIntoUnseen();
        return;
    case Kind::Define:
        storeBesideFieldsOf(id);
        return;
    case Kind::Has:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The fields of such a layout stand for the exports of the module, in their order. The values lie in variables.
void AccessInLinkAccesses::resolveOnNamespace(const Layout& layout)
{
    // A store is refused.
    if (!isRead())
        return;
    mayThrow();
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
    // An export may hand on that of another module, and that one of a third.
    for (size_t steps = 0; steps <= m_program.modules.size(); ++steps) {
        const auto& exports = m_program.at(module).exports;
        if (index >= exports.size())
            break;
        const ModuleExport& entry = exports[index];
        if (m_flow.testRuns().mayComeThroughMock(module, index)) {
            giveAnything();
            return;
        }
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

// One edge. The aggregate does for each member what resolveOnLayout() does for its one layout: it enters the accessors, with the objects of the member as `this`.
void AccessInLinkAccesses::resolveOnFamily(FamilyID family)
{
    mayThrow();
    if (isNamed()) {
        // The aggregate speaks for those who are not making the object.
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
            storeInto(m_flow.fieldStoredThrough(family, m_access.key, m_access.isPartOfConstruction));
        return;
    }
    if (isRead()) {
        giveWhatIsHeldAt(m_flow.anyFieldReadThrough(LinkType::Objects::Family, LayoutID(), family, namesOnlyElements()));
        return;
    }
    // As on one layout.
    if (mayStoreIntoPrototypeLink())
        m_flow.escapeAllOf(familyForLinkAccesses(family));
    storeInto(m_flow.anyFieldStoredThrough(LinkType::Objects::Family, LayoutID(), family));
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
    else
        storeInto(m_flow.anyFieldStoredThrough(LinkType::Objects::Lost, member, FamilyID()));
}

// A node that holds a prototype object does not climb when the class escapes. The Member nodes do, and so does the node beside the fields of the instances: whoever lets the prototype object go lets the layout go.
void AccessInLinkAccesses::resolveOnPrototype(LayoutID id)
{
    const Layout& layout = m_program.at(id);
    tellAgainOnceClimbs(foundBesideFieldsOf(id));
    if (m_flow.escapedLayouts.get(id.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    // What a descriptor lays under a key that nobody knows: the change beside this access has let the descriptor go.
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
        if (isRead()) {
            // The node beside the fields holds anything once the class has escaped or the link may have changed. Nothing else would tell this read.
            if (layout.base) {
                give(prototypeForLinkAccesses(layout.base));
                giveFrom(foundBesideFieldsOf(id));
            } else
                giveAnything();
            return;
        }
        noteChange(id, LayoutChange::ChangesPrototype);
        letPrototypeEscape(id);
        storeIntoUnseen();
        return;
    }

    bool isDefinition = m_access.kind == Kind::Define;
    if (!isDefinition && m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        letPrototypeEscape(id);
        cannotTell();
    }

    Vector<LinkFlow::FoundMember, 2> members = m_flow.findMembers(id, ClassSide::Prototype, m_access.key);
    layOnReceiverUnlessAccessorAnswers(members);
    bool isPrivateName = m_program.at(m_access.key).kind == PropertyKeyKind::PrivateName;
    if (isRead() && !isPrivateName) {
        if (FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, m_access.key))
            giveFrom(foundOnPrototypes);
    } else if (!isDefinition && !isPrivateName)
        m_flow.lookForSetterOnPrototypesAbove(id, m_access.key);
    // The object that a constructor function was made with takes the name as a property of its own, unless the store finds an accessor above it.
    if (!isRead() && !isPrivateName && m_receiver == m_subject && m_flow.isOfConstructorFunction(id)) {
        bool findsAccessor = std::ranges::any_of(members, [](const LinkFlow::FoundMember& found) {
            return found.kind != MethodKind::Method;
        });
        if (!isDefinition && (findsAccessor || (members.isEmpty() && mayRunCodeFoundFrom(Builtins::ObjectPrototype)))) {
            letPrototypeEscape(id);
            storeIntoUnseen();
            return;
        }
        // A definition that brings nobody knows what is one by a descriptor.
        if (isDefinition && m_value == m_flow.unseen())
            m_flow.defineOnPrototypeOf(id, m_access.key);
        else
            storeInto(m_flow.storedOnPrototypeOf(id, m_access.key));
        return;
    }
    if (members.isEmpty()) {
        if (isRead()) {
            giveFrom(foundBesideFieldsOf(id));
            if (isPrivateName)
                return;
            if (m_nameClass == NameClass::Constructor && m_flow.isBelowConstructorFunction(id))
                giveConstructorsOfRowAbove(id);
            if (m_nameClass != NameClass::Constructor || m_flow.isBelowConstructorFunction(id)) {
                if (!giveWhatIsFoundFrom(Builtins::ObjectPrototype))
                    letPrototypeEscape(id);
            } else if (layout.constructor)
                give(closureForLinkAccesses(layout.constructor));
            else
                giveAnything();
            return;
        }
        // Nobody tracks a member that no DefineMethod made.
        letPrototypeEscape(id);
        storeIntoUnseen();
        return;
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
    // `super.x`: an accessor gets the receiver of this one access as `this`.
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

// ---- Functions of the program

void AccessInLinkAccesses::resolveOnClosure(FunctionID function)
{
    const Function& record = m_program.at(function);
    LayoutID classLayout = record.constructedClass;
    // The property of a class can be neither written nor defined anew, so whoever else has the class object changes nothing of it.
    if (m_nameClass == NameClass::Prototype && isNamed() && isRead() && classLayout && isClassConstructorOf(record.kind)) {
        give(prototypeForLinkAccesses(classLayout));
        return;
    }
    // The node holds anything once the function has escaped. The node of the closure does not climb then.
    tellAgainOnceClimbs(m_flow.foundOnClosuresOf(function));
    if (m_flow.escapedFunctions.get(function.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    if (!isNamed()) {
        // A closure takes any name, and nobody tracks what lies under one on which closure. A read under a number finds that or nothing, and runs no code.
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
        // Of any other plain function the object under the property leads back to the function, and nobody follows it.
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
        if (!isRead()) {
            letClassObjectEscape(function, classLayout);
            storeIntoUnseen();
            return;
        }
        LayoutID base = classLayout && isClassConstructorOf(record.kind) ? m_program.at(classLayout).base : LayoutID();
        // As on a prototype object: a class object that escapes takes its layout along.
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
    // A closure takes any name, and nobody tracks what lies under one on which closure.
    // `import { EventEmitter } from "node:events"`: the value of the module is a function, and its classes hang on it. What some store has laid under the name beside the fields of anything is held to lie there (Facts::held()).
    // It stands beside the unknown. Of the reads of the loader alone: of every read off a closure the constructors of the library's own classes met, and linked() lost the lists of their keys.
    if (isTakenByLoader())
        giveFrom(foundBesideFieldsUnderKey());
    if (mayRunCodeFoundAboveClosures())
        m_flow.escape(function);
    storeInto(foundBesideFieldsUnderKey());
    if (m_access.kind != Kind::Has && m_flow.followsNamesOnClosuresOf(function, m_access.key)) {
        FlowNodeID laid = m_flow.laidOnClosuresOf(function, m_access.key);
        if (!isRead()) {
            storeInto(laid);
            return;
        }
        giveFrom(laid);
        // It may not lie there yet, and then the lookup goes on upward. What stands there under the name is nobody's to hand out here: `f.constructor` is Function, and whoever is seen to hold that is taken to run text.
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

// The property can be stored into. What `new` makes from then on stands below what was stored, or below Object.prototype where that is a primitive, and nobody follows either:
// the layout escapes, and with that the node beside its fields holds anything, which tells the reads.
void AccessInLinkAccesses::resolvePrototypePropertyOf(FunctionID function, LayoutID constructed)
{
    if (isRead()) {
        give(prototypeForLinkAccesses(constructed));
        giveFrom(foundBesideFieldsOf(constructed));
        return;
    }
    // Objects::Prototype of the layout stands for that object too: whatever the body stores into it, it stores into what it reads here.
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

    // A class object inherits from that of its base. What is stored below a field of the base flows into that field: every read from below finds it there.
    bool findsField = false;
    // `class A { static length = "a text" }  class B extends A {}`: B has a `length` of its own.
    bool isHiddenByEveryClass = classHolderIsMadeWithName(m_program, ClassSide::Static, m_access.key);
    forEachLayoutUpTheBases(classLayout, [&](LayoutID current) {
        for (const StaticField& field : m_program.at(current).staticFields) {
            if (field.key && field.key != m_access.key)
                continue;
            findsField |= !!field.key;
            giveWhatIsHeldAt(m_flow.variable(field.variable));
            storeInto(m_flow.variable(field.variable));
        }
        return findsField || isHiddenByEveryClass;
    });

    Vector<LinkFlow::FoundMember, 2> members = m_flow.findMembers(classLayout, ClassSide::Static, m_access.key);
    layOnReceiverUnlessAccessorAnswers(findsField ? Vector<LinkFlow::FoundMember, 2>() : members);
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
    if (members.isEmpty() && mayRunCodeFoundAboveClosures())
        m_flow.escape(function);
    if (isRead()) {
        if (members.isEmpty())
            giveAnything();
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
    // Of a class the property is fixed.
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

// A function that has a class escapes when it joins the lost ones (LinkFlow::becomeLost()). Of the others the object under `prototype` leads back to the function.
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

// An accessor that the name finds above a closure gets the closure as `this`.
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
            if (m_flow.testRuns().isNameAdded(m_access.key))
                return true;
        }
        // Only the first asker of the list below watches the members that the answer went by. In a test run those above a closure are the program's to lock, so each asker looks and watches for itself.
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
    // ONLY WHAT HANGS ON THE TABLE ALONE IS REMEMBERED. "No code" that leans on a member taken for a constant holds until a write is followed to the member: each asker looks and watches for itself, and asks again when it is told.
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

// ---- Accessors. LinkFlow::enter() wants a call of a summary, and an access is none.

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
    case LinkType::Functions::Lost:
        mayThrow();
        m_flow.addEdge(m_receiver, m_flow.receiverOfLostFunctions());
        giveFrom(m_flow.resultOfLostFunctions());
        storeInto(m_flow.parameterOfLostFunctions(0));
        break;
    }
}

void AccessInLinkAccesses::layOnReceiverUnlessAccessorAnswers(const Vector<LinkFlow::FoundMember, 2>& members)
{
    if (isRead() || m_receiver == m_subject)
        return;
    bool findsAccessor = std::ranges::any_of(members, [](const LinkFlow::FoundMember& member) {
        return member.kind != MethodKind::Method;
    });
    if (findsAccessor)
        return;
    m_flow.addEdge(m_receiver, m_flow.unseen());
    storeIntoUnseen();
}

void AccessInLinkAccesses::enterAccessor(FunctionID id)
{
    const Function& callee = m_program.at(id);
    // Only of an accessor do the tables know that something other than a call enters it.
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

// ---- Primitives, the engine's classes, built-ins

void AccessInLinkAccesses::resolveOnPrimitives(uint16_t primitives)
{
    primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent | Primitive::Unspecified);
    if (!primitives)
        return;
    // A store finds a setter above the value, or is refused.
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

// What lies under an index is an element only where the class keeps its elements there: what a set holds is no property.
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
        // What a hole reads as is found in an object that nobody tracks.
        forEachListOfElements(instances, [&](FlowNodeID elements) {
            m_flow.addEdge(m_flow.unseen(), elements);
        });
        // A call by name goes by what lies under unknown names.
        m_flow.addEdge(m_subject, m_flow.unseen());
        cannotTell();
        return;
    }

    // A whole number below zero is a name. It goes with the elements, whether it is written as a number or as a text.
    bool mayBeElement = !isNamed() || mayBeElementKey(m_nameClass);
    bool isElement = isNamed() ? mayBeElement : m_keyIsOnlyNumbers;
    if (!mayBeElement) {
        resolveUnderNameOnClass(instances);
        return;
    }
    // The key may be any name, and what is stored hides what the class has under it.
    if (!isRead() && !isElement)
        m_flow.addEdge(m_subject, m_flow.unseen());
    // What a row has laid under a name has not escaped.
    if (isRead() && !isElement)
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underAnyName()));

    TypedArrayType typedArray = isOneClass ? m_table.typedArrayTypeOf(builtinClass) : NotTypedArray;
    if (typedArray != NotTypedArray && typedArray != TypeDataView) {
        // The class fixes what an element is, and a store converts, which runs the code of what is stored.
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
    BuiltinID prototype = isOneClass && isElement && hasElements ? prototypeOfInstancesOf(builtinClass) : BuiltinID();
    if (prototype && indexesAreAbsentFrom(prototype))
        givePrimitives(Primitive::Undefined);
    else
        cannotTell();
}

// What a store lays under a name lies under unknown names, and what a row lays there in the slot under the name: both hang on the family of the site. Whatever else may stand in front of what the class has lets the object reach unseen code, which opens what lies under unknown names.
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

    // A record that a row makes has each field of its layout as a property of its own, so the lookup ends there. A store over it lies under unknown names, and so does whatever unseen code has done.
    if (BuiltinLayoutID record = isRead() && name && instances.classes == LinkType::Classes::Site ? m_flow.layoutOfRecordsOfFamilyOf(instances.site) : BuiltinLayoutID()) {
        bool isListed = std::ranges::any_of(m_table.at(record).fields, [&](const BuiltinLayoutField& field) {
            return field.name == name && !field.isOptional;
        });
        if (isListed) {
            giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
            return;
        }
    }

    // The object may be of a class below, and of some classes no instance is as the table says.
    BuiltinID prototype = builtinClass ? prototypeOfInstancesOf(builtinClass) : BuiltinID();
    bool tableSpeaksOfObject = isRead() && prototype && m_nameClass != NameClass::Proto && m_table.mayHavePristineInstances(builtinClass) && !m_table.mayDefineOnNewInstances(builtinClass, name);
    if (tableSpeaksOfObject && name) {
        for (BuiltinClassID other : m_table.classesWithInstanceMember(name))
            tableSpeaksOfObject &= other == builtinClass || !m_table.isSameOrSubclassOf(other, builtinClass);
    }
    if (tableSpeaksOfObject) {
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
        if (!giveWhatIsFoundFrom(prototype))
            m_flow.addEdge(m_subject, m_flow.unseen());
        return;
    }

    // The table lists the property for every object of the class, and native code takes the store: `list.length = 0`. It converts what is stored, which runs the code of an object.
    if (!isRead() && builtinClass && name && m_table.findOwnPropertyOfInstances(builtinClass, name)) {
        mayThrow();
        storeIntoUnseen();
        return;
    }

    if (isRead()) {
        // An accessor that the name finds above the object gets the object as `this`.
        if (!prototype || mayRunCodeFoundFrom(prototype))
            m_flow.addEdge(m_subject, m_flow.unseen());
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
        cannotTell();
        // What the table lists above the object may well be what the read finds: `pick(unknown, new WeakRef(o)).constructor`.
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

    // A setter that the name finds above the object gets the object as `this`, and what is stored.
    mayThrow();
    if (m_access.kind != Kind::Define && (!prototype || mayRunCodeFoundFrom(prototype))) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        storeIntoUnseen();
    }
    // From here on the object is not as its class makes it.
    storeInto(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
}

// ---- Contents. No key is looked up and nobody's code runs.

// What is no promise is what the operation goes on with.
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

    // `class A extends Map`: no site stands for such an object, so what is stored into one escapes, here and in CallInLinkCalls::storeInSlot().
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
            // All that is there is something unseen, and that by escape alone.
            LinkType unseenAlone;
            unseenAlone.holdsUnseen = true;
            LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(others == unseenAlone && !others.holdsUnseenInSight(), LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
            giveAnything();
            storeIntoUnseen();
        }
    }

    if (subject.classes != LinkType::Classes::None && (subject.classes != m_before.classes || subject.builtinClass != m_before.builtinClass || subject.site != m_before.site))
        resolveContentsOfInstances(instancesAmongForLinkAccesses(subject));

    // A function that `then` finds is handed the two functions that settle the wait, and what it hands to either nobody sees.
    if (settles && m_access.keyNode != noSummaryNode) {
        const LinkType found = m_flow.typeAt(m_flow.nodeOf(m_function, m_access.keyNode));
        if (holdsObjectsForLinkAccesses(found))
            giveAnything();
    }
}

// A call of an async function makes its promise, and the body has no name for it. Where the wait stands right behind the call nobody has had it in hand: no store of the program, and no listener that the host hands it to,
// which it does between two jobs. A function that is not async may answer with a promise that it has kept.
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
        // A call of a primitive throws.
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
        // Off undefined and null the read throws.
        receiver.primitives &= ~Primitive::UndefinedOrNull;
        LinkType objects;
        objects.objects = LinkType::Objects::Layout;
        objects.layout = receiver.layout;
        if (receiver.objects != LinkType::Objects::Layout || receiver != objects)
            return false;
        LayoutID layout = receiver.layout;
        if (m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype) || m_flow.escapedLayouts.get(layout.index()) || findField(m_program, layout, call.key))
            return false;
        Vector<LinkFlow::FoundMember, 2> members = m_flow.findMembers(layout, ClassSide::Prototype, call.key);
        return members.size() == 1 && members[0].kind == MethodKind::Method && isClosureOfSuchFunctionAt(m_flow.member(members[0].classLayout, ClassSide::Prototype, members[0].index));
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

    // Which slot of the table an internal field is hangs on the class.
    if (!isOneClass && m_access.slot.indexOfInternalField()) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        giveAnything();
        storeIntoUnseen();
        return;
    }

    // A map has values too, and is no promise.
    if (settles) {
        bool mayBePromise = !isOneClass || m_table.isSameOrSubclassOf(instances.builtinClass, BuiltinClasses::Promise);
        if (!isOneClass || !mayBePromise)
            handOn(instances);
        if (!mayBePromise)
            return;
    }

    // The class fixes what an element is.
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

    // Native code takes the value of a promise without asking it for anything, unless the promise has been given a `constructor` or a `then` of its own. That one is asked like any other object.
    auto mayHoldSomething = [&](FlowNodeID node) {
        if (m_laysObservers)
            m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
        return !m_flow.typeAt(node).isNothing();
    };
    if (mayHoldSomething(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames())) && !settlesPromiseThatCallHasJustMade()) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        giveAnything();
    }

    // What a promise holds beside its value is what it was rejected with, which the operation throws.
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
        giveWhatIsFoundFrom(builtin);
        return;
    }
    // Any member may be what the read gives.
    if (isRead())
        m_flow.noteLossOf(builtin);
    cannotTell();
}

BuiltinID AccessInLinkAccesses::prototypeOfInstancesOf(BuiltinClassID builtinClass) const
{
    BuiltinID prototype;
    for (BuiltinClassID current = builtinClass; current && !prototype; current = m_table.at(current).parent)
        prototype = m_table.at(current).prototype;
    return prototype;
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
        if (m_flow.testRuns().isNameAdded(m_access.key))
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

// Each object that the lookup passes by can never get the key, and stays below what it stands below.
LookupInLinkAccesses AccessInLinkAccesses::lookUpInTableFrom(BuiltinID start, BuiltinID& member) const
{
    MembersUnderKey members(m_program, m_access.key);
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        // Compiled code makes the object of such a module, and each module has its own of an object of that scope: the table speaks of neither.
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

// Other code than the native code behind a row that says what a read answers.
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

// A lock holds a name that the realm lacks as it holds any other: nobody can lay anything there.
LinkType AccessInLinkAccesses::typeHeldAtLocation(BuiltinID member)
{
    LinkType held = typeHeldAtLocationThatRealmHas(member);
    if (m_table.has(BuiltinMark::MayBeAbsent, member))
        held.primitives |= Primitive::Undefined;
    return held;
}

LinkType AccessInLinkAccesses::typeHeldAtLocationThatRealmHas(BuiltinID member)
{
    const Builtin& entry = m_table.at(member);
    BuiltinID canonical = m_table.canonicalID(member);
    bool isLockedByTable = entry.mutability == BuiltinMutability::AlwaysLocked || m_table.isLocationOfLockedObject(member);
    if (entry.scope != BuiltinScope::Realm || entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances)
        || m_program.isUnlocked(member) || m_program.isUnlocked(canonical) || (!isLockedByTable && !m_flow.takeForConstant(member))) {
        mayThrow();
        // What the table lists may well lie there.
        return m_flow.anythingBeside(entry.scope == BuiltinScope::Realm && isObjectKind(m_table.at(canonical).kind) ? canonical : BuiltinID(), m_function);
    }
    if (!isLockedByTable) {
        m_hasLeanedOnMemberTakenForConstant = true;
        tellAgainOnceClimbs(m_flow.writtenAtMember(member));
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
    LinkType answered;
    bool hasRow = false;
    for (const BuiltinOverload& overload : m_table.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Get)
            continue;
        if (overload.throws())
            mayThrow();
        if (overload.isUndeclared() || overload.resultIsDeclaredOnly())
            return LinkType::anything();
        answered = m_flow.joinOf(answered, typeWrittenBy(overload.result.type));
        hasRow = true;
    }
    return hasRow ? answered : LinkType::anything();
}

// Primitives and built-ins as they are written. Of an object that native code makes nobody has seen the contents.
LinkType AccessInLinkAccesses::typeWrittenBy(BuiltinTypeID id) const
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
        // Nobody follows the object. What its class has above it is followed to the writes: `Object.defineProperty(Object.getPrototypeOf(controller.signal), "aborted", d)`.
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
    // A key is a value of JavaScript: the mark of an empty cell never comes this far.
    LinkType held = typeAt(node);
    held.primitives &= LinkType::closed(Primitive::All);
    if (held != oneSymbol)
        return { };
    PropertyKeyID symbol = symbolAt(node);
    if (!symbol || !isNameInTextOf(program, symbol, function))
        return { };
    if (s_viewAtWork) {
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
    if (!held.primitivesByEscapeAlone && !held.unseenIsByEscapeAlone)
        return { };
    if (held.objects != LinkType::Objects::None || held.functions != LinkType::Functions::None || held.classes != LinkType::Classes::None || held.builtin || held.holdsLostBuiltins || held.holdsUnseenInSight())
        return { };
    if ((held.primitivesSeen() & LinkType::closed(Primitive::All)) != LinkType::oneSymbol)
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
    // Something else lies at the node too.
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
    // What is told lists nothing: none of them goes by a name any more.
    for (size_t index = 0; index < m_accessesUnderSymbolsOfLibrary.size(); ++index) {
        auto [symbol, observer] = m_accessesUnderSymbolsOfLibrary[index];
        if (hasPreparation(symbol))
            tell(observer, typeAt(nodeOf(observer.function, summaries.at(observer.function).accesses[observer.record].keyNode)), LinkType { });
    }
}

void LinkFlow::stopNamingSymbolsWithPreparation()
{
    if (!std::exchange(m_symbolsWithPreparationAreNames, false))
        return;
    tellAccessesUnderSymbolsWithPreparation();
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
    // WHAT THE HELD READING WOULD DO: the key is one symbol but for what an escape has brought. What the access lays down under it stands beside what it lays down under any key.
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
