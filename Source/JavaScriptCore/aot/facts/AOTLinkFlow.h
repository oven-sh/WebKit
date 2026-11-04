/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinsInReach.h"
#include "AOTLinkTables.h"
#include "AOTLinkKeyOrders.h"
#include "AOTNameClass.h"
#include "AOTTestRuns.h"
#include "AOTUnitedFamilies.h"
#include <memory>
#include <span>
#include <utility>
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/SpinBackoff.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>
#include <wtf/SegmentedVector.h>

namespace JSC { namespace AOT {

struct BuiltinSlot;

namespace Typed {

// LINK IS ONE FLOW OF TYPES OVER ONE GRAPH, ON ONE WORKLIST. Reach, the call graph and the types grow together, upward from nothing:
// a function is reached when a call is found to enter it, a call finds its callees when a closure or a layout arrives at its subject,
// an access finds its fields when a layout arrives at its subject. It is what GraalVM Native Image and Dart's type flow analysis do.
//
// IT IS LINEAR IN THE EDGES BECAUSE A TYPE CAN ONLY CLIMB A SHORT LADDER, and a node goes on the worklist only when its type has climbed:
//   objects of the program:  none > one layout > a declared family > a larger declared family > Lost
//   prototype objects of its classes: none > that of one layout > those of a declared family > of a larger one > escaped
//   functions of the program: none > the closures of one function > the constructors of a declared family > of a larger one > Lost
//   objects of the engine's classes: none > a family of allocation sites of one class > a family of sites of several classes
//   primitives: sixteen bits, each set once. No ranges, no sets of literals. Of integers: none > those that fit int32 > all. Of symbols: none > one that is made once > all.
// There is no number in any of that, and nothing is ever taken back.

// A place at which objects of one of the engine's classes are made: a ClassInstance node of a summary, a part of the result of a row at one call, the list of a rest parameter. Numbered as Link comes to them.
using SiteID = ID<struct SiteIDTag>;

// A type as Link sees it. Fixed size, joined in constant time. It becomes a TypeID only when the tables are written.
struct LinkType {
    enum class Objects : uint8_t {
        None,
        Layout, // `layout`
        Prototype, // the prototype object that the instances of `layout` inherit from
        Prototypes, // the prototype objects of the layouts of `family`
        Family, // `family`
        Lost, // the family of layouts that have met (LinkFlow::unitedLayouts) of which `layout` is a member, whichever. No layout: no object of the program's.
    };
    enum class Functions : uint8_t {
        None,
        Closure, // `function`
        Constructors, // the class objects (the closures of the constructors) of the layouts of `constructorsOf`
        Lost,
    };
    enum class Classes : uint8_t {
        None,
        Site, // the family of `site`. Whatever of it lies here is of `builtinClass`.
        Mixed, // the family of `site`. Objects of several classes lie here.
    };

    // One more member of `primitives`, beside those of Primitive::. An integer that fits int32. Primitive::Integer stands for the integers that do not, and never stands without this one.
    // So a set with this bit and without Integer says int32, whatever is ORed into a set takes the claim away where it must, and Primitive::Number written by hand means every number once it is closed.
    // ONLY THESE CLAIM IT: a type of the program's table whose integers fit (a constant, the result rule of a bit operation, a length or an index that a row writes, an established position). Link adds no ranges: a sum of two of them is an integer.
    static constexpr uint16_t int32 = 1 << 14;
    static_assert(!(int32 & Primitive::Defined));
    // The same for symbols: one symbol that is made once. Primitive::Symbol stands for all the others, and never stands without this one. So a set with this bit and without Symbol says one symbol.
    // WHICH ONE IT IS THE NODE SAYS AT WHICH THE TYPE LIES (LinkFlow::symbolAt()), and it goes along the edges beside the type. A type in hand does not say it: whoever lets such a type arrive without the name lets any symbol arrive.
    // ONLY THIS CLAIMS IT: a call of Symbol() that runs once, in whose place the walk puts the symbol that the image holds under the key (SummaryCall::key).
    static constexpr uint16_t oneSymbol = 1 << 15;
    static_assert(!(oneSymbol & Primitive::Defined));
    // Of a set or a mask that was written with the names of Primitive::. The join closes what it makes. WHOEVER CUTS `primitives` WITH A MASK CLOSES THE MASK FIRST: without that the integers that fit int32 and the one symbol are cut away.
    static constexpr uint16_t closed(uint16_t primitives)
    {
        if (primitives & Primitive::Integer)
            primitives |= int32;
        if (primitives & Primitive::Symbol)
            primitives |= oneSymbol;
        return primitives;
    }
    static constexpr bool holdsOneSymbol(uint16_t primitives) { return (primitives & (Primitive::Symbol | oneSymbol)) == oneSymbol; }
    // Of the primitives and the integers of a type of the program's table.
    static constexpr uint16_t primitivesOf(uint16_t primitivesOfType, Range integers)
    {
        if (!(primitivesOfType & Primitive::Integer))
            return primitivesOfType;
        return static_cast<uint16_t>(integers.isSubsetOf(Range::int32()) ? (primitivesOfType & ~Primitive::Integer) | int32 : primitivesOfType | int32);
    }
    // As Primitive:: has them: Integer for either kind of integer, Symbol for either kind of symbol.
    static constexpr uint16_t inNamesOfPrimitive(uint16_t primitives)
    {
        // A Set and the keys of a Map keep +0 for -0, and the flow hands on what went in. -0 is a whole number that is no integer, +0 is an integer: a type that names the one names the other.
        if (primitives & Primitive::WholeNumber)
            primitives |= int32;
        if (primitives & int32)
            primitives = static_cast<uint16_t>((primitives & ~int32) | Primitive::Integer);
        if (primitives & oneSymbol)
            primitives = static_cast<uint16_t>((primitives & ~oneSymbol) | Primitive::Symbol);
        return primitives;
    }
    static constexpr Range integersOf(uint16_t primitives)
    {
        if (primitives & Primitive::Integer)
            return Range::full();
        return primitives & (int32 | Primitive::WholeNumber) ? Range::int32() : Range::empty();
    }
    // The primitives alone. It needs no arena. There are no literals among them.
    TypeID primitivesAsType() const { return *tryMakeTypeOfPrimitives(inNamesOfPrimitive(primitives), integersOf(primitives)); }

    // In an order, and the small ones in as many bits, that leaves no gap: LinkFlow::Node.
    uint16_t primitives { 0 };
    Objects objects : 3 { Objects::None };
    Functions functions : 2 { Functions::None };
    Classes classes : 2 { Classes::None };
    // Beside all that: objects and functions that code nobody has seen has made. Nothing is known of them.
    bool holdsUnseen : 1 { false };
    // Beside what is unseen: some of the built-in objects that a type has stopped naming (LookupsPastProgramObjects::lostBuiltins). See below.
    bool holdsLostBuiltins : 1 { false };
    // THE SHADOWS. No rule of the flow asks for them and operator== passes them by. What the held reading never lets in (what unseen code may lay into an object, a parameter, a member that has escaped) arrives marked,
    // and so does whatever a rule lays down because of something unknown that is marked. held() is the type less what is marked: LinkFlow::hideWhatUnseenCodeBrought().
    // Of holdsUnseen.
    bool unseenIsByEscapeAlone : 1 { false };
    // Among `primitives`. A bit that is set here and not there says nothing.
    uint16_t primitivesByEscapeAlone { 0 };
    uint16_t primitivesSeen() const { return primitives & ~primitivesByEscapeAlone; }
    bool holdsUnseenInSight() const { return holdsUnseen && !unseenIsByEscapeAlone; }
    bool hasSameShadowsAs(const LinkType& other) const { return primitivesSeen() == other.primitivesSeen() && holdsUnseenInSight() == other.holdsUnseenInSight(); }
    // Less what is marked.
    LinkType asSeen() const
    {
        LinkType seen = *this;
        seen.primitives = primitivesSeen();
        seen.holdsUnseen = holdsUnseenInSight();
        seen.primitivesByEscapeAlone = 0;
        seen.unseenIsByEscapeAlone = false;
        return seen;
    }
    // All of it marked.
    LinkType byEscapeAlone() const
    {
        LinkType marked = *this;
        marked.primitivesByEscapeAlone = primitives;
        marked.unseenIsByEscapeAlone = holdsUnseen;
        return marked;
    }
    BuiltinClassID builtinClass;
    // The one built-in that is all the built-ins of the type. Two become holdsUnseen and holdsLostBuiltins.
    BuiltinID builtin;
    LayoutID layout;
    FamilyID family;
    FunctionID function;
    FamilyID constructorsOf;
    SiteID site;

    void dump(PrintStream&) const;

    bool isNothing() const { return *this == LinkType { }; }
    static LinkType anything();
    bool isAnything() const { return *this == anything(); }

    friend bool operator==(const LinkType& first, const LinkType& second)
    {
        return first.primitives == second.primitives && first.objects == second.objects && first.functions == second.functions && first.classes == second.classes
            && first.holdsUnseen == second.holdsUnseen && first.holdsLostBuiltins == second.holdsLostBuiltins && first.builtinClass == second.builtinClass && first.builtin == second.builtin
            && first.layout == second.layout && first.family == second.family && first.function == second.function && first.constructorsOf == second.constructorsOf && first.site == second.site;
    }
};

// LOST IS A FAMILY WHOSE MEMBERS ARE ADDED AS LINK GOES. Where two layouts meet that no declared family holds, neither is forgotten: both become members of Lost, and so do the members of a family that meets a stranger.
// A value of Objects::Lost is an object of some member. An access through it reads or stores the field of that name in the members, and in no other layout.
// A LOST LAYOUT HAS NOT ESCAPED: every writer of its fields is still known. Functions::Lost is the same for closures: a call of one enters the members, and their parameters get what such calls pass.
// A LOST BUILT-IN HAS NOT ESCAPED EITHER. Where a built-in meets another one, or what is unseen, the type stops naming it and says holdsLostBuiltins, which goes wherever the value goes. All of them are one family.
// Of a read and a call through such a value nothing is known (holdsUnseen stands beside it). A WRITE THROUGH IT IS A WRITE ON EACH OF THEM: `pick(Math, Date).now = f`, `for (const holder of [Date, performance]) holder[name] = f`,
// and what a read through it finds on one of them is lost in turn: `pick(unknown, new WeakRef(o)).constructor.prototype[name] = f`. Only the choice of locks goes by it: it claims nothing of any value.
// An object that nobody follows and that the table puts below one of them says it too: a read through it finds what lies there.
// NO NODE OF A FUNCTION OF THE LIBRARY SAYS IT, and what a type stops naming there is not one of them: all that the program prints or hands to an emitter meets there, and the library replaces no member of a built-in through such a value.
// ONLY AN EDGE INTO UNSEEN CODE LETS ANYTHING ESCAPE: a host function without a row, code from disk, eval. The fields of an escaped layout and the parameters of an escaped function hold anything, and what they held escapes in turn.
// WHAT AN OBJECT OF ONE OF THE ENGINE'S CLASSES HOLDS HANGS ON ITS ALLOCATION SITE. Two sites that meet are united with each other, and with nobody else (UnitedFamilies): a type keeps the number of any member.
// A family has one node a slot, at its representative. The nodes of two families that merge are tied to each other, both ways. So an access through a value reads and stores the contents of the sites of its family, and of no other site. A SITE THAT IS UNITED WITH ANOTHER HAS NOT ESCAPED.
// A TYPE DOES NOT CLIMB WHEN ITS FAMILY GROWS. So an observer may lean on what the type says (the class) and on what lies at the nodes of the family, which it watches, and on nothing else that it knows of one site.
// A family of which one object reaches unseen code holds anything in every slot, and what it held escapes.

using FlowNodeID = ID<struct FlowNodeIDTag>;

// Who must be told when the type at a node climbs.
struct FlowObserver {
    enum class Kind : uint8_t { SubjectOfCall, ReceiverOfCall, SubjectOfAccess, KeyOfAccess, SubjectOfChange, OtherOfChange, OperandOfOperator, Inspected, MemberOfSharedCall, MemberOfSharedAccess, MemberOfAccess, SymbolOfPreparation, ArgumentOfWaitingCall };

    Kind kind { Kind::SubjectOfCall };
    FunctionID function;
    // Into ThinSummary::calls, accesses, changes or operators of `function`. Of Inspected: a step of an inspection (AOTLinkCalls.cpp), and `function` is whoever asked for it first.
    // Of MemberOfSharedCall: a member of a shared call (AOTLinkCalls.cpp), and `function` is whoever came to the shared call first. MemberOfSharedAccess: the same of a shared access (AOTLinkAccesses.cpp).
    uint32_t record { 0 };

    void dump(PrintStream&) const;

    friend bool operator==(const FlowObserver&, const FlowObserver&) = default;
};

// THE FLOW ENDS CLOSED: behind run() no node climbs and no set grows when every edge is gone along and every observer is told once more, as one that has never been told.
// An edge or an observer that is new then is no breach: it is gone along, or told, at once, and where that brings nothing what it asks for holds already. Families that were united since lay such edges.
// WHOEVER HAS LOOKED SOMETHING UP OBSERVES WHAT IT LOOKED AT (a member of a class, what lies above, what a prototype object holds, whether a name is hidden), and is told again when that changes.
// An observer that decides on the state at the time of its telling leaves a node with too little in it, and a table with a claim that does not hold.
struct BreachOfClosure {
    // The node that climbs. Null: a set grows.
    FlowNodeID node;
    LinkType before;
    LinkType after;
    // The set of the flow that grows.
    ASCIILiteral set;
    // WHO BRINGS IT: the observer that was told once more, at `goneOver`. Without a function: an edge out of `goneOver`. Without a node: what follows from the breaches in front of it.
    FlowObserver told;
    FlowNodeID goneOver;
};

// AOTLinkReach.cpp.
class StateOfLinkReach;
struct DeleterOfStateOfLinkReach {
    void operator()(StateOfLinkReach*) const;
};

// AOTLinkCalls.cpp.
class StateOfLinkCalls;
struct DeleterOfStateOfLinkCalls {
    void operator()(StateOfLinkCalls*) const;
};

// AOTLinkAccesses.cpp.
class StateOfLinkAccesses;
struct DeleterOfStateOfLinkAccesses {
    void operator()(StateOfLinkAccesses*) const;
};

// AOTLinkOperators.cpp.
class StateOfLinkOperators;
struct DeleterOfStateOfLinkOperators {
    void operator()(StateOfLinkOperators*) const;
};

// AOTLinkLayouts.cpp.
class WriterOfLinkLayouts;
struct DeleterOfWriterOfLinkLayouts {
    void operator()(WriterOfLinkLayouts*) const;
};

// What summarizeCallees() found and has not typed: it runs beside a reading that is at the table of types.
struct TypesFoundOfCallees {
    struct ResultOfCallOfSeveral {
        // In LinkKnownEffects::callsOfSeveral.
        uint32_t place { 0 };
        LinkType type;
    };
    Vector<ResultOfCallOfSeveral> resultsOfCallsOfSeveral;
    // Ascending.
    Vector<uint32_t> variablesInitializedAtEveryRead;
    // Places in LinkTables::namesOnPrototypes, ascending: the store that lays the name stands in front of every entry of the function.
    Vector<uint32_t> namesLaidAheadOfEveryEntry;
    // Layouts of constructor functions, ascending: the one store of an object made to be the prototype object, and the link of the prototype object below that of the base, stand in front of every entry of the function and of those below it.
    Vector<uint32_t> prototypeObjectsStoredAheadOfEveryEntry;
    Vector<uint32_t> linksLaidAheadOfEveryEntry;
    // Places in LinkTables::namesOnFunctions, ascending: the store stands in front of whatever reads or calls the name off the function.
    Vector<uint32_t> namesOnFunctionsLaidAheadOfEveryRead;
};

// `#x`. Only the body of the class that declares it can name it, and text that is evaluated there (Program::evalScopes). So whoever gets hold of an object gets hold of nothing under such a name:
// a layout that escapes, is lost or is written under a key that nobody knows keeps the field to itself.
bool isPrivateNameInLink(const Program&, PropertyKeyID);

class LinkFlow {
    WTF_MAKE_NONCOPYABLE(LinkFlow);
public:
    LinkFlow(Program&, const ThinSummaries&, const Families&, const LooksAtBodies&, Mechanisms, BuildContext&, OrderOfWorklist = OrderOfWorklist::NewestFirst);
    ~LinkFlow();

    Program& program;
    const ThinSummaries& summaries;
    const Families& families;
    const LooksAtBodies& looks;
    Mechanisms mechanisms;
    BuildContext& build;
    const ParallelLoops& loops;

    // ---- AOTLinkFlow.cpp: the graph and the worklist.
    // The one node of the whole program for what is seen from outside functions.
    FlowNodeID parameter(FunctionID, uint32_t entryParameter) const;
    FlowNodeID result(FunctionID, uint32_t index) const;
    FlowNodeID thrown(FunctionID) const;
    FlowNodeID capture(FunctionID, uint32_t index) const;
    FlowNodeID variable(VariableID) const;
    FlowNodeID field(FieldReference) const;
    FlowNodeID member(LayoutID classLayout, ClassSide, uint32_t index) const;
    // ---- Sites, and what the objects that are made at them hold.
    enum class MakerOfSite : uint8_t {
        NodeOfSummary, // the number of the ClassInstance node in the flow
        PartOfRow, // the number of the call in calleesOfCalls, and which part of which type of the table
        WrapperAtCall, // the number of the call: native code makes an object of a primitive
        PairAtCall, // the number of the call: the pairs that an iterator made there hands out
        ListOfArguments, // the function whose entry gathers what no parameter takes in a list
    };
    // The one site of that maker and class. Made when first asked for.
    SiteID siteOf(MakerOfSite, uint32_t number, uint32_t part, BuiltinClassID);
    // Under the lock of what is shared.
    SiteID makeSite(std::pair<uint64_t, uint32_t> words, BuiltinClassID);
    // The list of the rest parameter, written or hidden. Whoever enters the function stores what the list gathers into its elements.
    SiteID listOfArgumentsOf(FunctionID);
    BuiltinClassID classOf(SiteID site) const
    {
        return m_sites[site.index()].builtinClass;
    }
    // The objects of the family are records of that layout of the table: each has every field that the layout lists as its own property from the start. Null: nobody has said so.
    // A family that comes to hold anything else beside such records escapes, which tells whoever has leaned on this.
    BuiltinLayoutID layoutOfRecordsOfFamilyOf(SiteID site) const
    {
        // Without the lock: a site never moves; it is written right behind siteOf(), by whoever has made the site, and where families merge, which is never while several threads are at work.
        return m_sites[m_familiesOfSites.representativeOf(site.index())].layoutOfRecords;
    }
    // Right behind siteOf(), while the site is alone.
    void noteRecordsAreMadeAt(SiteID, BuiltinLayoutID);
    // The code of the library makes a record as a cell of a compiled layout, which is no cell of the class that the site names. It holds of the family however it grows.
    bool mayHoldCellsOfCompiledLayouts(SiteID site) const
    {
        LockerOfWhatIsShared locker(*this);
        return m_sites[m_familiesOfSites.representativeOf(site.index())].mayHoldCellsOfCompiledLayouts;
    }
    size_t numberOfSites() const { return m_sites.size(); }
    static LinkType typeOf(SiteID, BuiltinClassID);
    LinkType typeOf(SiteID site) const { return typeOf(site, classOf(site)); }
    // Of the family of the site. Made when first asked for.
    FlowNodeID contents(SiteID, SlotOfContents);
    // The node of a summary that stands for the contents of a site of that summary. It is the family's node for the slot where there is none yet, else it is tied to that one.
    void takeAsContents(SiteID, SlotOfContents, FlowNodeID);
    // Null: the type holds no object of the engine's classes.
    FlowNodeID contentsOf(const LinkType& type, SlotOfContents slot) { return type.classes == LinkType::Classes::None ? FlowNodeID() : contents(type.site, slot); }
    // Null too where nobody has asked for the node.
    FlowNodeID findContentsOf(const LinkType& type, SlotOfContents slot) const { return type.classes == LinkType::Classes::None ? FlowNodeID() : findContents(m_familiesOfSites.representativeOf(type.site.index()), slot); }
    // The slot of the table, as a slot of contents.
    static SlotOfContents slotOf(const BuiltinSlot&);
    SlotOfContents slotOf(BuiltinSlotID) const;
    // An internal field may be a slot that the table lists under another number: the value of a promise. Each slot that the field of an object of the class may be.
    Vector<SlotOfContents, 2> slotsBehind(BuiltinClassID, SlotOfContents) const;
    void unite(SiteID, SiteID);
    // With its family.
    void escape(SiteID);
    bool hasEscaped(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].hasEscaped; }
    // Unseen code: what flows in escapes, what flows out is anything.
    FlowNodeID unseen() const;
    // A node of a summary. Those seen from outside answer with the nodes above, the local ones have numbers of their own behind the function's base.
    FlowNodeID nodeOf(FunctionID function, uint32_t nodeInSummary) const
    {
        uint32_t base = m_nodesOfFunctions[function.index()].base;
        RELEASE_ASSERT(nodeInSummary < m_nodesOfFunctions[function.index() + 1].firstParameter - base);
        return FlowNodeID(m_nodesOfSummaries[base - m_nodesOfFunctions[0].firstParameter + nodeInSummary]);
    }
    // What nodeOf() answers for each node of the summary, by its number there: for whoever goes through all of them.
    std::span<const uint32_t> nodesOfSummaryOf(FunctionID function) const LIFETIME_BOUND
    {
        uint32_t base = m_nodesOfFunctions[function.index()].base;
        return m_nodesOfSummaries.span().subspan(base - m_nodesOfFunctions[0].firstParameter, m_nodesOfFunctions[function.index() + 1].firstParameter - base);
    }
    // Whoever is about to go through the summary of the function asks for its nodes in time. It changes nothing.
    void prefetchNodesOf(FunctionID) const;

    // WHAT A NAME FINDS ON AN OBJECT OF A LAYOUT. A class's layout lists data fields only; its methods, getters and setters are known from SummaryMember alone.
    // The table is made once, in the constructor, from the summaries of ALL functions, reached or not: a member that is never defined holds nothing, which is sound.
    struct FoundMember {
        LayoutID classLayout;
        uint32_t index { 0 };
        MethodKind kind { MethodKind::Method };
    };
    // The member that `key` names on an instance of the layout (Prototype) or on its class object (Static): its own class's, else the nearest base's (Layout::base). A getter and a setter of one key are two members.
    Vector<FoundMember, 2> findMembers(LayoutID, ClassSide, PropertyKeyID) const;
    // The class or a base has a member under a computed key, or a base that is no class of the program: a lookup by name that finds nothing may still find something at run time.
    // So too, on the side of the instances, where the layout lists a field under a computed key (Field::computedKey). Such a field answers to a name that nobody knows: it holds anything, and what is stored there escapes.
    bool namesMayBeIncomplete(LayoutID, ClassSide) const;
    // A read under a number off an instance may find something in the classes above: `get 0() {}`, `1() {}`, or a name that nobody lists.
    bool classesAnswerReadsUnderNumbers(LayoutID layout) const { return m_layoutsBelowMembersUnderNumbers.get(layout.index()) || namesMayBeIncomplete(layout, ClassSide::Prototype); }
    // A read of `key` off an instance sees the data field if the object has it by then, else the member: where a layout has both, both flow.
    // Neither Object.prototype nor what a test run adds to a built-in answers to the key: a lookup that the classes of the program do not answer finds nothing.
    bool isAbsentAboveClasses(PropertyKeyID) const;
    // The class whose class object the closures of the function are. Null: it is no constructor of a class.
    LayoutID classOfConstructor(FunctionID) const;
    // Program::nameClassOf(), which every access and every call by name asks, out of one list.
    NameClass nameClassOf(PropertyKeyID key) const { return m_nameClassesOfKeys[key.index()]; }

    // ---- AOTLinkPrototypes.cpp. A FUNCTION THAT IS CONSTRUCTED GIVES ITS OBJECTS WHAT A CLASS GIVES (LayoutFlags::IsConstructorFunction).
    // Its closures stand where the class object stands, and Objects::Prototype of its layout is the object that a closure is made with under `prototype`.
    // No DefineMethod says what lies on that object: the program stores it there under names, when it pleases. So it is followed as it flows.
    // IT MAY NOT LIE THERE YET WHEN A LOOKUP COMES BY (`new F` may run in front of `F.prototype.m = f`): a lookup takes what is stored and goes on upward. Nobody may claim that such a member is found.
    // The layout whose objects `new` of the function makes. Null: the function is no such function.
    LayoutID layoutOfConstructorFunction(FunctionID) const;
    bool isOfConstructorFunction(LayoutID layout) const { return m_layoutsOfConstructorFunctions.get(layout.index()); }
    // What the program stores under the key on the prototype object. ASKING SAYS THAT SUCH A STORE EXISTS: only a store asks.
    FlowNodeID storedOnPrototypeOf(LayoutID, PropertyKeyID);
    // What a lookup under the key finds on the prototype objects of the constructor functions that it comes by on its way up from an object of the layout, or from the prototype object of the layout:
    // those of the row of bases, as far as the nearest class that has a member under the key. A null key: under any key. Null: it comes by none.
    // ASKING SAYS THAT SUCH A LOOKUP EXISTS.
    FlowNodeID foundOnPrototypesAbove(LayoutID, PropertyKeyID);
    // The same under any number: where whatever the program stores under such a name on a prototype object arrives. An accessor that a descriptor lays there lets the objects escape. Null: it comes by none.
    FlowNodeID foundOnPrototypesAboveUnderNumbers(LayoutID);
    // A store comes by the same objects and takes nothing.
    void lookForSetterOnPrototypesAbove(LayoutID, PropertyKeyID);
    bool isBelowConstructorFunction(LayoutID layout) const { return m_layoutsBelowConstructorFunctions.get(layout.index()); }
    // What stands above the prototype object of a constructor function is put there by a statement, which may not have run yet. So a lookup from `from` upward may not come to the class, which is at or above it.
    bool mayNotBeLinkedYetBelow(LayoutID from, LayoutID classAbove) const;
    // `class D extends F {}`: the entry of F that makes `this` makes an object of D, at no node of any summary. The node that holds the objects of the layout that are made so.
    // And of the layout of a constructor function below which layouts are written: what a construct call makes that has the function for its own new.target.
    FlowNodeID objectsMadeForClassBelowConstructorFunction(LayoutID);
    // A descriptor defines the key on the prototype object. A null key: some key. It may lay an accessor, which runs with whatever object finds it as `this`, or a value that refuses the stores below it.
    // Nothing is lost by that alone: the layout and those below it escape once a lookup under the key comes by.
    void defineOnPrototypeOf(LayoutID, PropertyKeyID);
    // The layout has escaped: unseen code has the prototype object.
    void openPrototypeOf(LayoutID);
    struct NameOnPrototype {
        LayoutID layout;
        // Null: any key.
        PropertyKeyID key;
        FlowNodeID stored;
        // What the stores of code in sight bring, and nothing else. It flows into `stored`, where whatever unseen code may lay there arrives too.
        FlowNodeID storedInSight;
        bool isStoredInto { false };
        bool isPassedByLookups { false };
        bool isDefinedByDescriptor { false };
        // Of the record with the null key: what is stored under the names of the layout flows into `stored` too.
        bool isReadUnderAnyKey { false };
    };
    // In the order in which they were first asked for. Behind run(), for those who write the tables.
    std::span<const NameOnPrototype> namesOnPrototypes() const LIFETIME_BOUND { return m_namesOnPrototypes.span(); }

    // WHAT THE PROGRAM LAYS UNDER A NAME ON A FUNCTION OF ITS OWN: `Ok.of = function (v) { .. }`. Of a plain function or a constructor function, under a name that no function object is made with.
    // A store by name through a value that holds the closures of that one function flows in, and into foundBesideFieldsUnder(key) as before. Whoever reads or calls the name off such a value takes what is here.
    // Whatever else may put something there lets the function escape (a key that nobody knows, a descriptor, a copy), and then every access is told again: foundOnClosuresOf(). A function that has met another is lost:
    // what is stored under the name through a lost value lies at foundBesideFieldsUnder(key), which flows in from then on, AND WHAT IS LAID GOES TO UNSEEN CODE, as all of it did once: a read or a call through a lost value gives anything.
    struct NameOnClosures {
        FunctionID function;
        PropertyKeyID key;
        FlowNodeID laid;
        // The next older name of the same function. UINT32_MAX: none.
        uint32_t next { UINT32_MAX };
    };
    bool followsNamesOnClosuresOf(FunctionID, PropertyKeyID) const;
    FlowNodeID laidOnClosuresOf(FunctionID, PropertyKeyID);
    void tieNamesOnLostClosuresOf(FunctionID);
    std::span<const NameOnClosures> namesOnClosures() const LIFETIME_BOUND { return m_namesOnClosures.span(); }
    // Null: nobody has asked for it.
    const NameOnPrototype* findNameOnPrototype(LayoutID, PropertyKeyID) const LIFETIME_BOUND;
    // Behind run(): the program stores or defines the key, or a key that nobody knows, on the prototype object of a constructor function at or above the layout.
    bool mayLieOnPrototypeAbove(LayoutID, PropertyKeyID, bool countsKeysThatNobodyKnows = true) const;

    // WHAT LIES ON AN OBJECT BESIDE THE FIELDS THAT ITS LAYOUT LISTS. A value stored under a name that the layout does not list has not escaped. It flows into storedBesideFieldsOf(layout) and into foundBesideFieldsUnder(key).
    // Whoever looks a name up on an object of a layout (a read, a call by name) takes what is at foundBesideFieldsOf(layout) too, which holds anything once the layout has escaped or changed, and, where no field answers, what is at foundBesideFieldsUnder(key).
    // A layout whose names may be incomplete escapes when either of its nodes is first asked for: an accessor that nobody lists gets the object as `this`.
    FlowNodeID foundBesideFieldsOf(LayoutID);
    FlowNodeID storedBesideFieldsOf(LayoutID);
    // The nodes with what an object of the layout has as its own: its fields, each by itself, and what lies beside them. Empty: nobody can say, or reading them out runs an accessor.
    Vector<FlowNodeID, 8> nodesWithOwnValuesOf(LayoutID);
    // The same, each field with its key. What lies beside the fields comes last, under no key.
    struct OwnValue {
        PropertyKeyID key;
        FlowNodeID node;
    };
    std::span<const OwnValue> ownValuesOf(LayoutID) LIFETIME_BOUND;
    FlowNodeID foundBesideFieldsUnder(PropertyKeyID);
    // A NAME THAT IS STORED ON AN OBJECT OF A LAYOUT THAT DOES NOT LIST IT IS A FIELD THAT LINK APPENDS: LinkTables::appendedKeys. Its node is made when a read or a store under the name first comes to the layout.
    // What arrives goes on into storedBesideFieldsOf(layout) and foundBesideFieldsUnder(key), so whoever looks at those two finds it there as before. Whoever reads the name off an object of that layout takes this node in place of foundBesideFieldsUnder(key).
    // Every store that may lay the name on such an object flows in: one by name, through the layout or through a family, and for a lost layout all that lies at foundBesideFieldsUnder(key). Whatever else may lay it there opens the layout: foundBesideFieldsOf().
    FlowNodeID appendedField(LayoutID, PropertyKeyID);
    // Null: nobody has asked for it.
    FlowNodeID findAppendedField(LayoutID, PropertyKeyID) const;
    void forEachAppendedField(const Invocable<void(PropertyKeyID, FlowNodeID)> auto& functor) const
    {
        for (const AppendedField& field : m_appendedFields)
            functor(field.key, field.node);
    }
    // AN OBJECT OF ONE OF THE ENGINE'S CLASSES takes any name too. What is stored under a name on one flows into contentsOf(type, SlotOfContents::underUnknownNames()): it hangs on the family of the site, like all that the object holds.
    // Whatever else may put something in front of what the table of built-ins lists for the instances (a store under a computed key that may be a name, a change, unseen code) lets the object flow into unseen(), and from then on that node holds anything.
    // A call by name takes what is there and in the slot under the key, beside the member that the table finds.
    // AND FOR A CLOSURE OF A FUNCTION OF THE PROGRAM, under a name that its class lists neither as a static field nor as a static member. It holds anything once the function has escaped.
    FlowNodeID foundOnClosuresOf(FunctionID);
    // The function has escaped.
    void openClosuresOf(FunctionID);

    // Aggregates, made when first asked for. Reading: what the key finds in every layout of the family (its data field, its class's member) flows into it. Storing: it flows into every such data field, and into the method that the new property hides.
    // An accessor that the key finds is entered by the aggregate itself, with the objects of the layout as `this`. Where nobody knows which function it is, the layout escapes. So an access through an aggregate owes nothing to the members one by one.
    // A member that joins later (Lost grows) is tied in by run(), which looks at lostLayouts each time the worklist is empty.
    // ASKING FOR A STORE AGGREGATE SAYS THAT SUCH A STORE EXISTS: the members gain a property, and their fields are written after construction unless `isPartOfConstruction`. Only a store asks for one.
    FlowNodeID fieldReadThrough(FamilyID, PropertyKeyID);
    FlowNodeID fieldStoredThrough(FamilyID, PropertyKeyID, bool isPartOfConstruction = false);
    FlowNodeID fieldReadThroughLost(LayoutID member, PropertyKeyID);
    FlowNodeID fieldStoredThroughLost(LayoutID member, PropertyKeyID);
    // A value of the family has flowed into unseen code: its members escape, and whoever is united with them later.
    void letLostFamilyEscape(LayoutID member);
    // Under a computed key: every field of every member.
    FlowNodeID anyFieldReadThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber = false);
    FlowNodeID anyFieldStoredThrough(LinkType::Objects, LayoutID, FamilyID);
    // What calls of a lost closure pass and get.
    FlowNodeID parameterOfLostFunctions(uint32_t positionalArgument);
    FlowNodeID receiverOfLostFunctions();
    FlowNodeID resultOfLostFunctions();

    // A node that was made while several threads tell observers gets its place behind them. Nothing has arrived there.
    const LinkType& typeAt(FlowNodeID id) const LIFETIME_BOUND
    {
        if (s_viewAtWork) [[unlikely]]
            return typeInViewAtWork(id);
        return id.index() < m_nodes.size() ? m_nodes[id.index()].type : s_nothing;
    }
    // Joins `type` in. If the node's type climbed, the node goes on the worklist. `symbol`: the key of the one symbol among `type` (LinkType::oneSymbol). Null: nobody says which it is, so it is any.
    void arrive(FlowNodeID, const LinkType&, PropertyKeyID symbol = { });
    // The key of the one symbol that is all the symbols at the node. Null: none lies there, or any may.
    PropertyKeyID symbolAt(FlowNodeID node) const { return LinkType::holdsOneSymbol(typeAt(node).primitives) ? symbolNotedAt(node) : PropertyKeyID(); }
    // Whatever an escape has brought beside it. It is what goes along the edges.
    PropertyKeyID symbolSeenAt(FlowNodeID node) const { return LinkType::holdsOneSymbol(typeAt(node).primitivesSeen()) ? symbolNotedAt(node) : PropertyKeyID(); }
    // keyOf(), of what was seen at the key. Nothing where keyOf() answers.
    PropertyKeyID keySeenOf(FunctionID, const SummaryAccess&) const;
    // Idempotent: the same edge is laid once. What is at `from` now arrives at `to` at once.
    void addEdge(FlowNodeID from, FlowNodeID to, TypeID passes = { });
    // It keeps every observer that it is handed. One that is put at a node which holds something by then is told by run(), once, with m_typeObservedBefore nothing, and never from here.
    void observe(FlowNodeID, const FlowObserver&);
    // Until the worklist is empty: along the edges of each node that climbed, and to its observers.
    void run();
    // Behind run(), in front of whoever writes tables. WHAT HAS ESCAPED IS AMONG WHAT IS UNSEEN. So beside `holdsUnseen` a type says nothing by naming it, and whether it does hangs on what came first:
    // a call that has entered a function and then comes to call unseen code has laid edges that one which calls unseen code at once never lays. Each type is brought into the one form that names only what has not escaped.
    // THE OBJECTS STAY NAMED AT THE NODES: a store under a name through a layout that has escaped still says which layout gets a field for the name (LinkTables::appendedKeys). Whoever writes a type out of a node asks withoutObjectsThatHaveEscaped().
    void settleTypes();
    LinkType withoutObjectsThatHaveEscaped(LinkType) const;
    // In place of run(), for the tests: the flow, and then everything gone over once more. What run() notes at its end is not noted: it is meant to move nothing.
    Vector<BreachOfClosure> findBreachesOfClosure();
    // `f3.parameter1`, `f3.node7`, `variable2`, `L4.field0`.
    void describe(PrintStream&, FlowNodeID) const;
    SizeOfLinkFlow size() const;
    // Behind run(), settleTypes() and settleSharedCalls(), for whoever goes on to the tables: the edges, the observers and what the lanes held are let go. Nothing flows from here on. size() answers as before.
    void forgetWaysOfFlow();
    // While it lives, whatever arrives with something unseen in it is marked: whoever resolves a call, an access, an operator or a change says so where all that is unknown of its operands is unknown by escape alone.
    class UnknownByEscapeAlone {
        WTF_MAKE_NONCOPYABLE(UnknownByEscapeAlone);
    public:
        // OfRule: what a rule lets arrive. OfEscape: that, and what comes along an edge out of what is unseen.
        enum class Marks : uint8_t { Nothing, OfRule, OfEscape };
        explicit UnknownByEscapeAlone(bool isSo, Marks = Marks::OfEscape);
        ~UnknownByEscapeAlone();
    private:
        Marks m_outer;
    };
    static bool isUnknownByEscapeAloneIfAtAll(const LinkType& type) { return !type.holdsUnseenInSight(); }

    // What summarizeCallees() leaves for writeTypesFoundOfCallees().
    mutable TypesFoundOfCallees typesFoundOfCallees;
    // What lookAtSummariesForLayoutsAndCells() leaves for writeLayoutsAndCells().
    mutable std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts> writerOfLayouts;

    // The only join. It calls becomeLost() for what it can no longer name.
    // The closures of the constructors of two classes (classOfConstructor()) that a declared family holds become Constructors of the nearest one. A closure that is no such constructor, or no common family: Lost.
    // The prototype objects of two layouts that a declared family holds become Prototypes of the nearest one. With none, or beside any other object: they escape.
    LinkType joinOf(const LinkType&, const LinkType&);
    LinkType passedBy(const LinkType&, TypeID test) const;
    // What it must make lost or let escape is queued, and done by the next arrive(), joinOf() or run().
    LinkType fromTypeID(TypeID) const;
    // With the two restrictions of FRESH.md. Lost, Prototype and Prototypes are anyObject, Functions::Lost and Constructors are anyFunction. Nothing is null.
    TypeID toTypeID(TypeArena&, const LinkType&) const;
    // ONLY WHAT THE PROVEN READING REACHES GETS CODE, and whoever tests for a function or calls it mentions its code. The held reading reaches more: it knows a built-in where the proven one has `anything`, and enters the library's code behind it.
    // So its tables name no function but these: a type says `function` in place of another, and a call lists no other. Null: every function may be named.
    bool tablesMayName(FunctionID function) const
    {
        const BitVector* functions = s_viewAtWork ? s_viewAtWork->functionsThatTablesMayName : nullptr;
        return !functions || functions->get(function.index());
    }

    // ---- AOTLinkReach.cpp: what grows beside the types.
    // The first time: its summary comes alive: sources arrive, edges are laid, observers are put at the subjects of its calls, accesses and changes.
    void reach(FunctionID);
    void becomeLost(LayoutID);
    void becomeLost(FamilyID);
    // The closures of a function that has a constructedClass escape at once: nobody who reads or stores on a lost closure is told which class objects are among them.
    void becomeLost(FunctionID);
    // With a layout go its class object, its prototype object and those above it, the layouts written below it, and the members of all those on both sides.
    void escape(LayoutID);
    void escape(FunctionID);
    // Whatever `type` names escapes. The observer of unseen(). A prototype object takes its layout along. An object of one of the engine's classes opens the contents of the family of its site.
    void escapeAllOf(const LinkType&);
    // Objects of exactly that layout are made at the node. A type that stays as it is tells nobody that the layout has escaped, so from then on unseen objects lie at the node too.
    // Whoever lays a layout down with arrive() alone must let it escape at once.
    void arriveFromSource(FlowNodeID, LayoutID);
    // Of any layout of the family.
    void arriveFromSource(FlowNodeID, FamilyID);
    // As arrive(), for a type that no edge brought and that may name a layout or a family.
    void arriveFromSource(FlowNodeID, const LinkType&);
    void noteAllocated(LayoutID);
    // The entries of the program: module bodies that are evaluated, worker entries, what the host enters.
    // NO CODE OF THE LIBRARY IS AN ENTRY. A function with a `runtimefunction` record and a module of the library are reached when something reached leads there.
    void reachEntries();
    // run() asks each time the worklist is empty. It enters what the rows of the table of built-ins name for each built-in that reached code can come by (BuiltinsInReach):
    // BuiltinOverload::code, calledFunctionsOf(), moduleLoadsOf(). True: it has entered something.
    //   An overload of the host that is undeclared brings what Builtins::UndeclaredNativeCode lists.
    //   Once an object has reached unseen() and an object may have a method that the host calls with what it loads at its start: the start-up rows.
    //   Once Facts::unseenCode() would have a record: moduleLoaderFunctions() and every module of the library, which a name made at run time may find.
    //   Once unseen code can run: every function that the host calls by its role (functionsCalledByHost()). Such code finds the realm, under names that nobody sees. No realm holds a function of the compiler.
    bool enterLibraryInReach();
    // What the observers tell BuiltinsInReach.
    BuiltinsInReach& builtinsInReach();
    // BuiltinsInReach::takeForConstant(). What it answers of a member stands while observers are told (a write that is followed to the member is taken behind the pass), so a lane that has asked knows until the pass ends.
    bool takeForConstant(BuiltinID member);
    // It never changes while Link runs.
    bool isWorthFollowingToWrites(BuiltinID canonical) const;

    // OBSERVERS ARE TOLD ON SEVERAL THREADS. The types, what has escaped, what is lost and what is united stand still meanwhile, and what an observer lays down is held back in the boxes of its thread.
    // All observers of one function are told by one thread, so what belongs to a call or an access belongs to that thread. WHATEVER ELSE IS WRITTEN, or is read where somebody may write (what is made on demand: a node, a site, an aggregate, a shared call;
    // what is noted of a built-in, a layout, a name), IS UNDER THIS ONE LOCK. It may be taken again by the thread that has it. With one thread it costs a test.
    // OBSERVERS OF CALLS, ACCESSES AND OPERATORS, AND bringAlive(), RUN ON SEVERAL THREADS: all of one function on one thread, those of different functions at once.
    // EVERY TABLE THAT A UNIT ADDS TO LinkFlow OR TO ONE OF ITS STATES, AND THAT SUCH CODE READS OR WRITES, IS A Shared<T>, OR IS KEPT BY LANE (a Vector by indexOfLaneAtWork()), OR IS WRITTEN BY note() ALONE.
    // of(flow) stops the build where several threads are at work and the caller does not hold LockerOfWhatIsShared. A plain member filled from there is a crash once in fifty runs (AOTLinkTestRuns.cpp, 11-03 15:33).
    void assertHasWhatIsSharedToItself() const { RELEASE_ASSERT((!m_severalThreadsTell && !s_laneAtWork) || s_depthOfLockOfWhatIsShared); }
    template<typename T>
    class Shared {
    public:
        T& of(const LinkFlow& flow)
        {
            flow.assertHasWhatIsSharedToItself();
            return m_table;
        }

    private:
        T m_table;
    };
    class LockerOfWhatIsShared {
        WTF_MAKE_NONCOPYABLE(LockerOfWhatIsShared);
    public:
        explicit LockerOfWhatIsShared(const LinkFlow& flow) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
            : m_flow(flow.m_severalThreadsTell ? &flow : nullptr)
        {
            if (m_flow && !s_depthOfLockOfWhatIsShared++)
                m_flow->lockWhatIsShared();
        }
        ~LockerOfWhatIsShared() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            if (m_flow && !--s_depthOfLockOfWhatIsShared)
                m_flow->m_whatIsSharedIsLocked.store(false, std::memory_order_release);
        }

    private:
        const LinkFlow* m_flow;
    };
    class BuiltinsInReachUnderLock {
    public:
        explicit BuiltinsInReachUnderLock(LinkFlow& flow)
            : m_locker(flow)
            , m_builtins(flow.builtinsInReach())
        {
        }
        BuiltinsInReach* operator->() { return &m_builtins; }

    private:
        LockerOfWhatIsShared m_locker;
        BuiltinsInReach& m_builtins;
    };
    BuiltinsInReachUnderLock builtinsInReachUnderLock() { return BuiltinsInReachUnderLock(*this); }
    // WHAT IS MADE ON DEMAND STAYS WHAT IT IS: whoever asks again with the same words gets the same node. So each thread keeps what it was given, and takes the lock once for each.
    enum class MadeOnDemand : uint8_t { Contents, ContentsOfFamily, SiteOfMaker, TakenForConstant, FieldReadThroughLost, FieldStoredThroughLost, FieldReadThrough, FieldStoredThrough, FoundOnPrototypesAbove, FoundBesideFieldsUnder, FoundBesideFieldsOf, StoredBesideFieldsOf, FoundWhereFieldIsEmpty, AppendedField, FoundOnClosuresOf, LaidOnClosuresOf, WrittenAtMember, ParameterOfLostFunctions, ReceiverOfLostFunctions, ResultOfLostFunctions, ResultOfOperator };
    static constexpr size_t numberOfKindsMadeOnDemand = static_cast<size_t>(MadeOnDemand::ResultOfOperator) + 1;
    uint32_t askedOnceByLane(MadeOnDemand kind, uint64_t words, const Invocable<uint32_t()> auto& make) const
    {
        if (!m_severalThreadsTell)
            return make();
        if (std::optional<uint32_t> known = findWhatLaneWasGiven(kind, words))
            return *known;
        LockerOfWhatIsShared locker(*this);
        uint32_t made = make();
        keepWhatLaneWasGiven(kind, words, made);
        return made;
    }
    FlowNodeID nodeAskedOnceByLane(MadeOnDemand kind, uint64_t words, const Invocable<FlowNodeID()> auto& make) const
    {
        return FlowNodeID::fromBits(askedOnceByLane(kind, words, [&]() -> uint32_t {
            return make().bits();
        }));
    }
    std::optional<uint32_t> findWhatLaneWasGiven(MadeOnDemand, uint64_t words) const;
    void keepWhatLaneWasGiven(MadeOnDemand, uint64_t words, uint32_t) const;
    // WHAT IS ONLY NOTED, and read when the observers are through, waits in a list of the thread.
    enum class Note : uint8_t { NameRead, NameCalled, ReadUnderKeyNobodyKnows, BuiltinRead, BuiltinCalled, BuiltinCalledOutOfSight, FunctionEnteredByPlainCall, FieldWrittenAfterConstruction, LayoutAllocated, ChangeOfLayout, WriteOnBuiltin, WriteThroughLostBuiltins, ReadThroughLostBuiltins, WriteThroughUnseenByProgram, NameWrittenThroughUnseenByLibrary, OptionThatRunsCodePassed };
    void note(Note, uint32_t number, uint32_t more = 0);
    unsigned numberOfLanes() const { return static_cast<unsigned>(m_lanes.size()); }
    // For StateOfLinkReach: each function by the thread that tells its observers. False: it does not pay, or what is laid down is not held back, and nothing was done.
    bool bringAliveOnLanes(std::span<const FunctionID>, const ScopedLambda<void(FunctionID)>& bringAlive);
    // While several threads tell nothing escapes, so a source that is listed behind the pass is listed in time. False: one thread is at work, and the caller lists it.
    bool listsSourceOfObjectsBehindPass(FlowNodeID, LayoutID);
    // BuiltinsInReach::noteFindingsIn(). False: one thread is at work, and the caller tells.
    bool tellsBuiltinsInReachBehindPass(FunctionID);
    void listSourceOfObjects(FlowNodeID, LayoutID);
    bool letsFunctionsWaitToBeBroughtAlive() const { return m_letsFunctionsWaitToBeBroughtAlive; }
    // The thread that tells the observers of the function, by the lane of its nodes.
    unsigned laneOfObserversOf(FunctionID function) const { return laneIndexOf(FlowNodeID(m_nodesOfFunctions[function.index()].firstParameter)); }
    // 0 where one thread works.
    unsigned indexOfLaneAtWork() const;
    const LinkType& typeObservedBefore() const LIFETIME_BOUND;
    // A TYPE THAT IS JOINED IN HAND IS THE LIBRARY'S WHILE CODE OF THE LIBRARY IS AT WORK: an observer of one of its functions is told, one of its calls is resolved, one of its functions is brought alive.
    class CodeAtWork {
        WTF_MAKE_NONCOPYABLE(CodeAtWork);
    public:
        CodeAtWork(LinkFlow& flow, FunctionID function)
            : m_flow(flow)
            , m_wasOfLibrary(flow.setCodeAtWorkIsOfLibrary(flow.m_isFunctionOfLibrary.quickGet(function.index())))
        {
        }
        ~CodeAtWork() { m_flow.setCodeAtWorkIsOfLibrary(m_wasOfLibrary); }

    private:
        LinkFlow& m_flow;
        bool m_wasOfLibrary;
    };
    // What it was.
    bool setCodeAtWorkIsOfLibrary(bool);
    bool codeAtWorkIsOfLibrary() const;
    // Code of the language's own library and of the compiler: RuntimeFunctionRole. The row of the built-in says what it looks up on its operands.
    bool rowsSpeakFor(FunctionID function) const { return m_isFunctionThatRowsSpeakFor.quickGet(function.index()); }
    // Something other than a primitive, and other than what unseen code has made, has arrived at unseen().
    bool objectsReachUnseen() const { return m_objectsReachUnseen; }
    // WITH NO CALL WRITTEN: the host hands a promise that was rejected with nobody waiting to the listeners of `unhandledRejection` and `rejectionHandled`. A listener has `anything` in hand,
    // and what it lays on that, a `then`, a `constructor`, another prototype, it lays on a promise of the program that has gone nowhere. So from the moment on at which somebody can be handed something from outside,
    // each site of the class Promise holds `anything` under unknown names. THAT MOMENT NAMES NO EVENT AND NO FUNCTION OF THE HOST: something of the program has reached unseen code or native memory (a listener has),
    // a row passes a function what nobody follows, or code that no build has seen can run. Until then nobody but the code in sight has a hand on a promise.
    void noteRowPassesWhatNobodyFollows() { LockerOfWhatIsShared locker(*this); m_rowPassesWhatNobodyFollows = true; }
    bool rowPassesWhatNobodyFollows() const { return m_rowPassesWhatNobodyFollows; }
    // enterLibraryInReach(), on one thread. True: something has arrived.
    bool letPromisesBeHandedOut();
    // A BUILT-IN THAT A READ FINDS IS FOLLOWED LIKE ANY VALUE (LinkType::builtin). Where the type stops naming it, it counts as let go.
    void noteLossOf(BuiltinID builtin) const
    {
        if (builtin)
            noteLossOfBuiltin(builtin);
    }
    Vector<BuiltinID> takeBuiltinsLost() { return std::exchange(m_builtinsLost, { }); }
    Vector<BuiltinID> takeBuiltinsLostInSight() { return std::exchange(m_builtinsLostInSight, { }); }
    Vector<BuiltinID> takeBuiltinsAtUnseen() { return std::exchange(m_builtinsAtUnseen, { }); }
    bool lostBuiltinsReachUnseen() const { return m_lostBuiltinsReachUnseen; }
    // `anything`, beside which the built-in may lie: it is lost from here on. Null, or a built-in that nobody follows to writes: `anything`.
    LinkType anythingBeside(BuiltinID canonical, FunctionID reader) const;
    // Objects that nobody follows, of which the table says what they stand below: a read through one finds what lies there.
    LinkType unseenObjectsBelow(BuiltinID canonicalPrototype, FunctionID maker) const;
    // A store, a definition, a deletion or a copy is made on such a value by code of that function. A null key: nobody can read it.
    void noteWriteOn(const LinkType& subject, PropertyKeyID, FunctionID writer);
    // Beside it, where an operation of the program makes the write: what lies at the node WHEN LINK IS THROUGH says whether the write is reported.
    void noteSiteOfWrite(FunctionID writer, ValueID site, FlowNodeID subject);
    // The same of `Object.assign(subject, source)` and `Object.defineProperties(subject, source)`.
    void noteSiteOfCopy(FunctionID writer, ValueID site, FlowNodeID subject, FlowNodeID source);
    Vector<WriteFollowedToBuiltin> takeWritesFollowedToBuiltins();
    // The names of the fields of the objects of the program among them. False: one of them may have a name more, or no object of the program is among them. FOR A REPORT ALONE: it claims nothing.
    bool appendNamesOfObjectsAmong(const LinkType&, Vector<PropertyKeyID>&) const;
    // What stands above such a value, as far as the table says it, for whoever asks for the prototype of an object. It is added to `anything`.
    LinkType builtinsAbove(const LinkType&, FunctionID asker);
    BuiltinID prototypeOfInstancesOf(BuiltinClassID) const;
    // The prototype of some instance of the family was asked for where instances of several classes lie.
    bool isClimbedFrom(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].isClimbedFrom; }
    // A MEMBER OF A BUILT-IN THAT A PROGRAM MAY REPLACE IS WHAT THE TABLE SAYS UNTIL A WRITE IS FOLLOWED TO IT (BuiltinsInReach::takeForConstant()). The node is made when first asked for and holds nothing.
    // `anything` arrives once such a write is found: enterLibraryInReach(). It carries no value: whoever has gone by the member observes it, is told, and asks again.
    FlowNodeID writtenAtMember(BuiltinID member);
    // Behind the flow. Null: nobody has gone by the member.
    FlowNodeID findWrittenAtMember(BuiltinID member) const;
    // ---- AOTLinkTestRuns.cpp: what a test run adds. Outside of one every question answers at its first line.
    TestRunsInLink& testRuns() { return *m_testRuns; }
    const TestRunsInLink& testRuns() const { return *m_testRuns; }

    // The constructor sizes every set and list that is public here, and fills firstCallOfFunction.
    BitVector reached;
    BitVector allocatedLayouts;
    BitVector lostLayouts;
    // STRANGERS THAT MEET ARE UNITED WITH EACH OTHER, NOT WITH EVERYBODY. By layout. A lost layout that has met nobody yet is alone in its family.
    UnitedFamilies unitedLayouts;
    BitVector lostFunctions;
    BitVector escapedLayouts;
    // By layout: something stands below the prototype object of the class by a link that the layouts do not show. notePrototypesLaidAboveOtherObjects() of AOTLinkUnions.cpp.
    BitVector prototypesLaidAboveOtherObjects;
    // findTaggedMakers(), once for the flow.
    Vector<TaggedMaker> taggedMakers;
    // AOTLinkLayouts.cpp: a copy is told again when something comes to lie beside the fields of its source. The node above the function and the record of the change.
    HashSet<std::pair<uint64_t, uint32_t>> copiesToldOfWhatLiesBesideFields;
    // By the function and the record of the change: the two sides as the copy last looked at them. What a copy does follows from them and from whether anything lies beside the fields of its source.
    struct CopyLookedAt {
        LinkType subject;
        LinkType other;
        // Something lies there, or nobody lists what the source has: whatever else arrives beside the fields changes nothing.
        bool waitsForNothingBesideFields { false };
    };
    HashMap<uint64_t, CopyLookedAt, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> copiesLookedAt;
    BitVector escapedFunctions;
    // At some node the layout has met another one that a declared family holds with it. Of any other layout that is neither lost nor escaped every object is known by its layout, wherever it goes: no value that is known by a family is one.
    mutable BitVector layoutsMetInFamilies;
    bool mayBeKnownByFamily(LayoutID layout) const { return layoutsMetInFamilies.get(layout.index()) || lostLayouts.get(layout.index()); }
    // Unseen code stores into them. One that it only reads is not among them.
    BitVector escapedVariables;
    // Nobody ever finds in them what they held at first: AOTLinkReach.cpp, findVariablesStoredFirst().
    BitVector variablesStoredFirst;
    OrderOfScopes orderOfScopes;
    // A row of the table of built-ins has led to the function. The walk and the run time name it where no summary shows it.
    BitVector libraryFunctionsEntered;
    // Some call that is no construction enters the function: `F()`, `F.call(o)`, a callback. What unseen code and lost calls do is not noted here: escapedFunctions and lostFunctions say that.
    BitVector functionsEnteredByPlainCalls;

    // ---- The observers, a file each. Each is told that the type at the node has climbed, and looks at what it is NOW. What it did for the lower type stands.
    void callObserved(const FlowObserver&, const LinkType&); // AOTLinkCalls.cpp
    // Each call that was told in this round is resolved once, however many of its operands have climbed. True: some were.
    bool resolveCallsThatWereTold(); // AOTLinkCalls.cpp
    void inspectionObserved(const FlowObserver&, const LinkType&); // AOTLinkCalls.cpp
    void accessObserved(const FlowObserver&, const LinkType&); // AOTLinkAccesses.cpp
    // AOTLinkAccesses.cpp. The key that the access of the function goes by, as things stand: the one that is written, or that of the one symbol which is all that lies at the node of its key. Null: any key.
    PropertyKeyID keyOf(FunctionID, const SummaryAccess&) const;
    // ---- BuiltinMark::RegistersPreparationOfHolders. AOTLinkAccesses.cpp.
    // The library keeps the fields under such a symbol in a form of its own, and a look by value changes the form of the holder. Its text counts on its own form wherever it writes an access under one of them.
    // So those accesses are accesses by name all together, or none of them is: one that may also go under another key goes by value, and then so do all.
    // What is at the node is handed to such a built-in as the symbol.
    void noteSymbolHandedToPreparation(FunctionID caller, FlowNodeID);
    // Nobody knows what is.
    enum class UnknownSymbolIs : uint8_t { UnknownInSightToo, UnknownByEscapeAlone, UnknownInSightAlone };
    void noteUnknownSymbolHandedToPreparation(UnknownSymbolIs = UnknownSymbolIs::UnknownInSightToo);
    void preparationObserved(const FlowObserver&, const LinkType&);
    // The observer's `record` is the node.
    void noteKeyOfAccessInLibrary(FunctionID, uint32_t record, const SummaryAccess&);
    void changeObserved(const FlowObserver&, const LinkType&); // AOTLinkLayouts.cpp
    void waitingCallObserved(const FlowObserver&, const LinkType&); // AOTLinkNarrowing.cpp
    void operatorObserved(const FlowObserver&, const LinkType&); // AOTLinkOperators.cpp

    // AOTLinkCalls.cpp. Written as calls find their callees. By function: where its calls begin, in the order of ThinSummary::calls. One more entry than there are functions.
    struct CalleesOfCall {
        // The functions of the program that run because of the call. Of a call by Builtin: those that the native code calls, so none of them is the callee.
        Vector<FunctionID, 1> listed;
        // Of a call by Name or by Value: the built-ins that it may find, by their canonical IDs. It enters their native code, of which the rows speak, so that is no unseen code. What the native code calls is among those listed.
        Vector<BuiltinID, 1> builtins;
        // Into calleesOfSharedCalls. Whatever one of those lists, finds or may enter, this call does too. A shared call has none.
        Vector<uint32_t, 1> shared;
        // Beside all those it may enter lost closures or unseen code.
        bool mayEnterLost { false };
        bool mayEnterUnseen { false };

        void add(BuiltinID builtin)
        {
            if (!builtins.contains(builtin))
                builtins.append(builtin);
        }
    };
    Vector<uint32_t> firstCallOfFunction;
    Vector<CalleesOfCall> calleesOfCalls;
    // WHAT MANY CALLS DO ALIKE IS DONE ONCE. A call that goes by a family (a name looked up on its objects, its prototype objects or its class objects, or one of its class objects called) may enter something of every member.
    // One shared call does that, member by member, with nodes of its own for the operands and the result, and each such call lays an edge for each of its operands to it: members + calls, where each call by itself makes members x calls.
    Vector<CalleesOfCall> calleesOfSharedCalls;
    // WHAT held()'s WRITERS READ IN THE PLACE OF WHAT THE FLOW HOLDS. They run beside linked()'s writers and the summary, for which the flow itself is settled meanwhile. A thread that has the view at work is handed what lies here
    // by typeAt(), reading(), escapedLayoutsAsRead(), calleesOf..AsSettled(), tablesMayName(), keyOf(): whoever writes tables asks those, never the members.
    struct ViewOfHeldReading {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ViewOfHeldReading);
        ~ViewOfHeldReading();
        // By node: LinkType::asSeen(). Both lists are filled by all threads at once, each place once.
        std::span<LinkType> types;
        // None.
        BitVector escapedLayouts;
        std::span<CalleesOfCall> calleesOfCalls;
        Vector<CalleesOfCall> calleesOfSharedCalls;
        const BitVector* functionsThatTablesMayName { nullptr };
        std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts> writerOfLayouts;
        // Those of the lane in which the writers run.
        const ParallelLoops* loops { nullptr };
    };
    // Behind run(), in front of settleTypes().
    std::unique_ptr<ViewOfHeldReading> makeViewOfHeldReading();
    class ViewAtWork {
        WTF_MAKE_NONCOPYABLE(ViewAtWork);
    public:
        explicit ViewAtWork(ViewOfHeldReading* view)
            : m_outer(std::exchange(s_viewAtWork, view))
        {
        }
        ~ViewAtWork() { s_viewAtWork = m_outer; }
        // For whoever hands work to other threads: each of them puts it to work in turn.
        static ViewOfHeldReading* current() { return s_viewAtWork; }
    private:
        ViewOfHeldReading* m_outer;
    };
    const ParallelLoops& loopsOfReading() const { return s_viewAtWork ? *s_viewAtWork->loops : loops; }
    // THE FLOW RUNS IN ONE WAY AND IS READ IN TWO. Which of them a writer of tables writes.
    WhatUnseenCodeDoes reading() const { return s_viewAtWork ? WhatUnseenCodeDoes::OnlyReadsAndCalls : WhatUnseenCodeDoes::Anything; }
    const BitVector& escapedLayoutsAsRead() const { return s_viewAtWork ? s_viewAtWork->escapedLayouts : escapedLayouts; }
    std::span<const CalleesOfCall> calleesOfCallsAsSettled() const { return s_viewAtWork ? std::span<const CalleesOfCall>(s_viewAtWork->calleesOfCalls) : calleesOfCalls.span(); }
    const Vector<CalleesOfCall>& calleesOfSharedCallsAsSettled() const { return s_viewAtWork ? s_viewAtWork->calleesOfSharedCalls : calleesOfSharedCalls; }
    std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts>& writerOfLayoutsOfReading() const { return s_viewAtWork ? s_viewAtWork->writerOfLayouts : writerOfLayouts; }
    // Beside it. In which order shared calls are made hangs on the order of the worklist. What they stand for does not.
    struct SubjectOfSharedCall {
        FamilyID family;
        // Null: the class objects themselves are called.
        PropertyKeyID key;
        // What of the family, and how many operands in which form.
        uint32_t form { 0 };

        friend constexpr auto operator<=>(const SubjectOfSharedCall&, const SubjectOfSharedCall&) = default;
    };
    Vector<SubjectOfSharedCall> subjectsOfSharedCalls;
    // Of a shared call, or of a shared access (AOTLinkAccesses.cpp).
    FlowNodeID makeSharedNode() { return makeNode(); }
    // Behind run(). A call whose subject climbed from a family to a larger one has gone by the shared calls of both, or of the larger alone, as the worklist had it: it keeps the larger, which does all that the smaller does.
    // The built-ins that a shared call finds, which are few, and whether it may enter lost closures or unseen code are written to each call that goes by it. What it lists stays with it, and with it alone.
    void settleSharedCalls() { settleSharedCallsAs(WhatUnseenCodeDoes::Anything); }
    void settleSharedCallsAs(WhatUnseenCodeDoes);
    // The call enters the function: arguments to parameters, results and what is thrown back, and reach(). Idempotent.
    void enter(FunctionID caller, uint32_t callInSummary, FunctionID callee);

    // AOTLinkLayouts.cpp. By layout. It starts empty and grows. An escaped layout has them all.
    // ONLY WHAT IS DONE THROUGH ONE LAYOUT, A FAMILY OR A PROTOTYPE OBJECT. What is done through a lost value is gathered behind the flow, when every member of Lost is known: behind run(), ask LinkTables::layouts[].possibleChanges.
    Vector<OptionSet<LayoutChange>> possibleChanges;
    // What a row of a built-in that was applied in the flow has listed the keys of, and what an inspection came to: AOTLinkKeyOrders.h. It only grows.
    KeysListedInLink keysListed;
    // By layKnownAccessorInLinkLayouts(), in whatever order the flow came there: whoever reads it sorts it.
    Vector<AccessorLaidInField> accessorsLaidInFields;
    // An accessor whose function is known is laid on objects of the layout, in the place of a field: what the getter returns flows into the field, and what is stored into the field flows into the setter.
    // So nothing is opened. The tables say GetsAccessor all the same: the slot of such a field is empty.
    BitVector layoutsWithKnownAccessors;
    // SummaryChange::descriptorIsInSight has come to an object of the layout. Written by the observer of changes alone.
    BitVector layoutsDefinedOnUnderWrittenKeys;
    // The fields in whose place they are laid. With repeats, in the order of the worklist: whoever reads it sorts.
    Vector<FieldReference> fieldsWithKnownAccessors;
    // By LinkTables::firstFieldOfLayout + field: some store that is no part of the construction reaches the field.
    BitVector fieldsWrittenAfterConstruction;
    // What the proving reading alone says of layouts and fields: what an escape allows, and what a store through a lost value adds. Whoever writes tables asks possibleChangesOf() and isWrittenAfterConstruction().
    BitVector layoutsThatUnseenCodeMayChange;
    BitVector layoutsThatGainPropertiesThroughLost;
    BitVector fieldsThatUnseenCodeMayWrite;
    OptionSet<LayoutChange> possibleChangesOf(size_t layout) const
    {
        OptionSet<LayoutChange> changes = layout < possibleChanges.size() ? possibleChanges[layout] : OptionSet<LayoutChange> { };
        if (reading() != WhatUnseenCodeDoes::Anything)
            return changes;
        if (layoutsThatUnseenCodeMayChange.get(layout))
            changes.add({ LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor });
        if (layoutsThatGainPropertiesThroughLost.get(layout))
            changes.add(LayoutChange::GainsProperty);
        return changes;
    }
    bool isWrittenAfterConstruction(size_t cell) const { return fieldsWrittenAfterConstruction.get(cell) || (reading() == WhatUnseenCodeDoes::Anything && fieldsThatUnseenCodeMayWrite.get(cell)); }

    // AOTLinkLayouts.cpp. A FIELD IS FILLED WHERE EVERY CONSTRUCTION FILLS IT. By LinkTables::firstFieldOfLayout + field: some object of the layout may be seen from outside its construction while the field is not defined.
    // It starts empty and grows, as functions are reached. What takes a property away later (a deletion, a definition by a descriptor, unseen code) is not noted here: possibleChanges and escapedLayouts say that.
    BitVector fieldsThatMayBeEmpty;
    // AOTLinkBirthsInlines.h. For the mark of the cell, no proof. By LinkTables::firstFieldOfLayout + field: some construction in sight is through with an object of the layout while the field is not defined. It starts empty and grows.
    BitVector fieldsNotOfBirth;
    // ThinSummary::halvesOfAccessorsNotLaid of the functions that are reached. By LinkTables::firstFieldOfLayout + field. They start empty and grow.
    BitVector gettersThatMayBeAbsent;
    BitVector settersThatMayBeAbsent;
    void noteBirthsOf(FunctionID);
    // The summary has come alive: what its allocations leave empty. Of the constructor of a layout also what the row of constructors, from the class that allocates down to this one, leaves empty in the objects of that layout.
    // That row is as the classes are written. Whoever finds that a class object has another parent lets the class escape, and so does whoever hands a new.target on in any other way than `super()` does.
    void noteConstructionsOf(FunctionID);
    // For a read of a field that the layout lists itself, under a key that nothing above the classes answers to (isAbsentAboveClasses()), by a body that is not making the object: it holds undefined once the field may be empty, and nothing before.
    FlowNodeID foundWhereFieldIsEmpty(FieldReference);
    // Of a read by name in the summary of `reader`, where an object of the layout arrives at its subject: the body may be making that object, and may not have defined the name yet.
    bool mayReadInFrontOfDefinition(FunctionID reader, const SummaryAccess&, LayoutID) const;

private:
    struct Edge {
        FlowNodeID to;
        TypeID passes;
        // Into m_edges. UINT32_MAX: the last.
        uint32_t next { UINT32_MAX };
    };

    // What an arrival asks of a node, in one line of memory.
    struct Node {
        LinkType type;
        // Of a node that stands for a position of the program: TypeTable::provenTypeOf(). It holds of every value that is ever there, so whatever arrives has passed it. Null: no bound.
        // Only the compiler leaves a position empty. A flow knows no order: without this the mark of an empty cell would go on out of a cell of which the compiler knows that no read comes in front of the first store.
        TypeID typeOfPosition;
    };
    static_assert(sizeof(Node) == 36);
    // Into m_edges and m_observers. UINT32_MAX: none.
    struct ListsOfNode {
        uint32_t firstEdge { UINT32_MAX };
        uint32_t firstObserver { UINT32_MAX };
    };
    Vector<Node> m_nodes;
    using SetOfNumbers = HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    bool hasPreparation(PropertyKeyID symbol) const { return m_everySymbolOfLibraryMayHavePreparation || m_symbolsWithPreparation.contains(symbol.bits()); }
    // THE SAME IN SIGHT: as it stands of what was seen at the nodes, whatever an escape has brought there. keySeenOf() goes by it, and held()'s writers: exchangeWhatIsHeldOfSymbolsWithWhatIsInSight().
    struct SymbolsWithPreparationInSight {
        Vector<PropertyKeyID> notedBesideOtherKeys;
        Vector<PropertyKeyID> notedWithPreparation;
        bool unknownSymbolWasNoted { false };
        SetOfNumbers withPreparation;
        bool everySymbolOfLibraryMayHaveOne { false };
        bool areNames { true };
        SetOfNumbers besideOtherKeys;
        bool hasPreparation(PropertyKeyID symbol) const { return everySymbolOfLibraryMayHaveOne || withPreparation.contains(symbol.bits()); }
    };
    SymbolsWithPreparationInSight m_symbolsInSight;
    void stopNamingSymbolsWithPreparation();
    // WHETHER A SYMBOL IS A NAME STANDS STILL WHILE OBSERVERS ARE TOLD, like the types: what they note of symbols is taken in behind the last of them. True: something was.
    bool takeInWhatWasNotedOfSymbols();
    Vector<PropertyKeyID> m_symbolsNotedBesideOtherKeys;
    Vector<PropertyKeyID> m_symbolsNotedWithPreparation;
    bool m_unknownSymbolWasNotedWithPreparation { false };
    void tellAccessesUnderSymbolsWithPreparation();
    Shared<SetOfNumbers> m_nodesHandedToPreparation;
    // The bits of keys.
    SetOfNumbers m_symbolsWithPreparation;
    bool m_everySymbolOfLibraryMayHavePreparation { false };
    bool m_symbolsWithPreparationAreNames { true };
    // The symbols of the library that lie at the key of an access in its text beside something else.
    SetOfNumbers m_symbolsOfLibraryBesideOtherKeys;
    // The accesses in the text of the library that have gone by the name of one of its symbols.
    Vector<std::pair<PropertyKeyID, FlowObserver>> m_accessesUnderSymbolsOfLibrary;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessesUnderSymbolsOfLibraryListed;
    // By node, of those at which one symbol has arrived: the bits of its key.
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_symbolsAtNodes;
    Vector<ListsOfNode> m_listsOfNodes;
    std::optional<SizeOfLinkFlow> m_sizeWhenWaysWereForgotten;
    // By node: an edge that passes everything leads from it into unseen(). Half of all edges that are asked for are such, so nobody looks through a list for one.
    BitVector m_flowsIntoUnseen;
    // By node: its edges are in m_edgesOfBusyNodes, but those that m_flowsIntoUnseen knows.
    BitVector m_hasEdgesInSet;
    // THE FLOW GOES IN ROUNDS. First along the edges alone, until nothing climbs. Then whoever observes a node that has climbed is told, and ALL WHO ARE TOLD IN A ROUND LOOK AT TYPES THAT STAND STILL:
    // what they let arrive, the edges that they lay and the observers that they put are held back, and taken in behind the last of them. So a round gives one answer in whatever order its steps are taken, and by however many threads.
    // ALONG THE EDGES IT GOES STEP BY STEP TOO. In one step every node that has climbed sends its type along its edges, and no type is written. In the next all that was sent arrives, and nothing is sent.
    // So what arrives at a node in a step is a set that hangs on no order. Where several layouts or functions meet in a step, they are set aside and joined as a set behind the step: foldObjects(), foldFunctions().
    // THE NODES ARE DEALT OUT TO LANES, block by block, so that the nodes of a function lie in one lane. Only its lane writes the type of a node, its lists and its bits.
    struct ArrivalHeldBack {
        FlowNodeID node;
        PropertyKeyID symbol;
        LinkType type;
    };
    struct EdgeLaid {
        FlowNodeID from;
        FlowNodeID to;
        TypeID passes;
    };
    struct ObserverPut {
        FlowNodeID node;
        FlowObserver observer;
    };
    struct Lane;
    Vector<std::unique_ptr<Lane>> m_lanes;
    unsigned m_log2OfNodesInBlock { 12 };
    unsigned m_maskOfLanes { 0 };
    unsigned laneIndexOf(FlowNodeID node) const { return (node.index() >> m_log2OfNodesInBlock) & m_maskOfLanes; }
    Lane& laneOf(FlowNodeID node) { return *m_lanes[laneIndexOf(node)]; }
    // Of the thread: the lane that it works for while several are at work. Null: one thread has the flow to itself.
    static thread_local Lane* s_laneAtWork;
    static thread_local ViewOfHeldReading* s_viewAtWork;
    const LinkType& typeInViewAtWork(FlowNodeID id) const LIFETIME_BOUND { return id.index() < s_viewAtWork->types.size() ? s_viewAtWork->types[id.index()] : s_nothing; }
    // Of the thread: the lane whose nodes it sends from.
    static thread_local Lane* m_laneThatSends;
    bool m_holdsBack { false };
    // WHAT HAS ESCAPED, WHAT IS LOST AND WHAT IS UNITED STAND STILL WHILE OBSERVERS ARE TOLD, like the types: whatever an observer lets escape waits for settle() behind the last of them.
    bool m_tellsObservers { false };
    bool m_severalThreadsTell { false };
    // IT IS HELD FOR A FEW HUNDRED NANOSECONDS AT A TIME. Whoever waits looks again after one pause, and gives way to the system only when whoever holds it must have been taken off its core.
    // A wait that doubles (SpinBackoff) sleeps through ten turns of the lock and then asks the system: 12 microseconds a wait, measured, where the lock was free 60 of 100 of the time.
    alignas(128) mutable std::atomic<bool> m_whatIsSharedIsLocked { false };
    void lockWhatIsShared() const
    {
        static constexpr unsigned pausesBeforeGivingWay = 4096;
        unsigned pauses = 0;
        while (m_whatIsSharedIsLocked.load(std::memory_order_relaxed) || m_whatIsSharedIsLocked.exchange(true, std::memory_order_acquire)) {
            if (++pauses < pausesBeforeGivingWay)
                simde_mm_pause();
            else
                Thread::yield();
        }
    }
    static thread_local unsigned s_depthOfLockOfWhatIsShared;
    static const LinkType s_nothing;
    // With those that have no place in m_nodes yet.
    std::atomic<size_t> m_numberOfNodesMade { 0 };
    // While several threads tell.
    FlowNodeID contentsFoundOrMadeByLane(SiteID, SlotOfContents);
    void makeContentsInLane(uint32_t family, SlotOfContents, FlowNodeID);
    void listContentsThatLanesMade();
    static bool isToldByOneThread(FlowObserver::Kind);
    void tellOnLanes();
    void prepareAccessesForSeveralThreads();
    void prepareOperatorsForSeveralThreads();
    void sortTellsOfLane(unsigned lane);
    void tellInLane(unsigned lane);
    void takeOverWhatLanesDidWhileTelling();
    void noteFunctionToReach(FunctionID);
    void reachAll(std::span<const FunctionID>);
public:
    // The layout has become lost just now: tieInLostLayouts().
    void noteLayoutToTie(LayoutID layout) { m_layoutsToTie.append(layout); }
private:
    void holdBackWhile(const ScopedLambda<void()>&);
    void takeNote(Note, uint32_t number, uint32_t more);
    bool resolveCallsThatWereToldIn(unsigned lane);
    // While what was sent arrives.
    bool m_setsMeetingsAside { false };
    // The lane into whose boxes the thread writes what it holds back.
    Lane& laneThatHoldsBack() const { return s_laneAtWork ? *s_laneAtWork : *m_lanes[0]; }
    BitVector m_isToBeTold;
    uint64_t m_numberOfRounds { 0 };
    void flow();
    void goAlongEdges();
    void sendFromLane(unsigned lane);
    void receiveInLane(unsigned lane);
    void foldObjects(LinkType& into, std::span<const LinkType> met) const;
    void foldFunctions(LinkType& into, std::span<const LinkType> met) const;
    bool severalThreadsPayFor(size_t numberOfSteps) const;
    void takeInWhatWasHeldBack();
    void layWhatWasHeldBackFor(unsigned lane);
    void gatherWhatLanesNoted();
    void runOnLanes(const Invocable<void(unsigned lane)> auto&);
    bool layEdge(Lane&, FlowNodeID from, FlowNodeID to, TypeID passes);
    void putObserver(Lane&, FlowNodeID, const FlowObserver&);
    void send(FlowNodeID to, const LinkType&, PropertyKeyID symbol);
    // Nothing is held back, and the type holds something.
    void receive(FlowNodeID, const LinkType&, PropertyKeyID symbol);
    // Whoever is at work has the lists of the node to itself: they are read along the edges and where observers are sorted, never while observers are told.
    bool hasListsToItself(FlowNodeID node) const { return node.index() < m_listsOfNodes.size() && (!m_severalThreadsTell || s_laneAtWork == m_lanes[laneIndexOf(node)].get()); }
    bool wouldNameSomethingNewAtUnseen(const LinkType&) const;
    void noteLossOfBuiltin(BuiltinID) const;
    void noteMetInFamily(LayoutID) const;
    // A VALUE THAT IS KNOWN BY A FAMILY IS AN OBJECT OF A MEMBER THAT HAS MET ANOTHER ONE, OR THAT IS LOST. So what befalls the family befalls those members, and each further one when it comes to be such.
    // A type that climbs from a layout to a family to Lost then lets go what it let go before, and more: whatever reaches unseen code early does nothing that its last type would not do.
    bool hasEscapedAsFamily(FamilyID) const;
    void catchUpWithFamiliesAbove(LayoutID) const;
    PropertyKeyID symbolNotedAt(FlowNodeID) const;
    std::pair<FlowNodeID, LinkType> takeNode(Lane&);
    std::pair<FlowObserver, FlowNodeID> takeNewObserver();
    uint64_t nextScatteredNumber();
    mutable Lock m_lockOfSymbolsAtNodes;
    const OrderOfWorklist m_order;
    // Where the oldest is taken first: those in front have been taken.
    size_t m_firstOfNewObservers { 0 };
    uint64_t m_scatteredNumber { 88172645463325252ull };
    // Set while findBreachesOfClosure() is at work.
    Vector<BreachOfClosure>* m_breachesOfClosure { nullptr };
    FlowObserver m_observerToldOnceMore;
    FlowNodeID m_nodeGoneOver;
    void noteBreachOfClosure(BreachOfClosure&&);
    // settleTypes(). By family, and by member of a family of lost layouts.
    BitVector m_familiesThatHaveEscaped;
    BitVector m_lostFamiliesThatHaveEscaped;
    BitVector m_isOnWorklist;
    uint64_t m_numberOfNodesTakenOffWorklist { 0 };
    uint64_t m_numberOfObserversTold { 0 };
    // What belongs to one function lies side by side: whoever is at one of its nodes is soon at another. Its entry parameters, its results, what it throws, then a node for each node of its summary.
    struct NodesOfFunction {
        uint32_t firstParameter { 0 };
        uint32_t firstResult { 0 };
        uint32_t thrown { 0 };
        uint32_t base { 0 };
    };
    // One more entry than there are functions.
    Vector<NodesOfFunction> m_nodesOfFunctions;
    // What nodeOf() answers. By NodesOfFunction::base + the node in the summary, less the first node of the first function.
    Vector<uint32_t> m_nodesOfSummaries;
    Vector<NameClass> m_nameClassesOfKeys;

    // ---- AOTLinkReach.cpp alone. Made when first asked for.
    StateOfLinkReach& stateOfLinkReach();
    bool fillParametersThatUnseenCodeAlonePasses();
    std::unique_ptr<StateOfLinkReach, DeleterOfStateOfLinkReach> m_stateOfLinkReach;

    // ---- AOTLinkCalls.cpp alone. Made when first asked for.
    StateOfLinkCalls& stateOfLinkCalls();
    std::unique_ptr<StateOfLinkCalls, DeleterOfStateOfLinkCalls> m_stateOfLinkCalls;

    // ---- AOTLinkAccesses.cpp alone. Made when first asked for.
    StateOfLinkAccesses& stateOfLinkAccesses();
    std::unique_ptr<StateOfLinkAccesses, DeleterOfStateOfLinkAccesses> m_stateOfLinkAccesses;

    // ---- AOTLinkOperators.cpp alone. Made when first asked for.
    std::unique_ptr<StateOfLinkOperators, DeleterOfStateOfLinkOperators> m_stateOfLinkOperators;

    // ---- AOTLinkTestRuns.cpp.
    std::unique_ptr<TestRunsInLink> m_testRuns;

    // ---- For the observers too.
    // While run() tells an observer: the type at its node when the observers of that node were last told. Nothing: this observer has never been told. Outside of that it is Nothing.
    // It is never above what the observer last saw. So an observer lays the edges of a part of the type (objects, functions, classes, the built-in, holdsUnseen) only where that part is not as it is here.
    LinkType m_typeObservedBefore;
    // LinkTables::firstFieldOfLayout.
    Vector<uint32_t> m_firstFieldOfLayout;

    // ---- AOTLinkLayouts.cpp alone.
    void noteFieldMayBeEmpty(FieldReference);
    // By LinkTables::firstFieldOfLayout + field. UINT32_MAX: not made. Empty: none is.
    Vector<uint32_t> m_nodesFoundWhereFieldsAreEmpty;

    // ---- AOTLinkPrototypes.cpp alone.
    void indexConstructorFunctions();
    uint32_t recordOfNameOnPrototype(LayoutID, PropertyKeyID);
    void forEachPrototypePassedAbove(LayoutID, PropertyKeyID, const Invocable<void(uint32_t record)> auto&);
    void tiePrototypesAboveLostLayout(LayoutID);
    bool letPrototypesFoundThroughLostEscape();
    BitVector m_layoutsOfConstructorFunctions;
    // Such a layout, or one with such a layout above it.
    BitVector m_layoutsBelowConstructorFunctions;
    // By layout: some record of it has the flag.
    BitVector m_prototypesPassedByLookups;
    BitVector m_prototypesWithDefinitions;
    BitVector m_prototypesAboveLostLayouts;
    Vector<NameOnPrototype> m_namesOnPrototypes;
    // By the layout above the key. Into m_namesOnPrototypes, and the nodes of foundOnPrototypesAbove().
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_recordsOfNamesOnPrototypes;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_nodesFoundOnPrototypesAbove;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfObjectsMadeForClassesBelow;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfMembersWritten;

    // ---- AOTLinkFlow.cpp alone.
    enum class NodeOnDemand : uint8_t { ReadThroughFamily, StoredThroughFamily, StoredInConstructionThroughFamily, AnyFieldReadOfLayout, AnyFieldStoredOfLayout, AnyFieldReadOfFamily, AnyFieldStoredOfFamily, FoundBesideFieldsUnderKey, LaidOnClosuresUnderKey };
    enum class AggregateAccess : uint8_t { Read, Store };

    struct MemberOfClass {
        LayoutID classLayout;
        ClassSide side { ClassSide::Prototype };
        PropertyKeyID key;
        MethodKind kind { MethodKind::Method };
        uint32_t index { 0 };
        // Null: what is defined is no closure made in the body that defines it.
        FunctionID function;
    };

    // What a join or an arrival at unseen() calls for. It is done when the type is written, and never from inside another of these: nothing here recurses over the program.
    struct Deferred {
        enum class Kind : uint8_t { LayoutIsLost, FamilyIsLost, LayoutsAreUnited, LostFamilyEscapes, FunctionIsLost, ConstructorsAreLost, SitesAreUnited, LayoutEscapes, FunctionEscapes, ConstructorsEscape, PrototypesEscape, SiteEscapes, TypeEscapes };

        Kind kind { Kind::TypeEscapes };
        uint32_t number { 0 };
        LinkType type;
        // LayoutsAreUnited: the other one.
        uint32_t other { 0 };
    };

    // One for each family of lost layouts and key that a member has or an access names.
    struct LostKey {
        PropertyKeyID key;
        uint32_t read { UINT32_MAX };
        uint32_t stored { UINT32_MAX };
        // Into m_holdersOfLostKeys: the members that have a field or a member of their class under the key.
        uint32_t firstHolder { UINT32_MAX };
        uint32_t lastHolder { UINT32_MAX };
        // Some member of the family.
        uint32_t family { 0 };
        // Into m_lostKeys: the next one of the family.
        uint32_t next { UINT32_MAX };
        // Object.prototype may answer to the key, so a member without it counts too.
        bool tiesEveryMember { false };
        bool isListedAsTyingEveryMember { false };
        // Its family was united with one that had a record for the key, which stands for both now. The nodes stay, tied to those of that record.
        bool hasBeenMerged { false };
    };

    // What hangs on the representative of a family of lost layouts.
    struct LostFamily {
        // What the nodes beside the fields of the members bring, and what is handed on to them.
        uint32_t foundBesideFields { UINT32_MAX };
        uint32_t storedBesideFields { UINT32_MAX };
        uint32_t anyFieldRead { UINT32_MAX };
        // Into m_lostKeys.
        uint32_t firstKey { UINT32_MAX };
        Vector<uint32_t> keysThatTieEveryMember;
        bool reachesUnseen { false };
        // By declared family: how many of the members it holds. loseFamiliesOfMembersThatMeet().
        HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersInDeclaredFamilies;
    };

    struct HolderOfLostKey {
        LayoutID layout;
        uint32_t next { UINT32_MAX };
    };

    void indexMembersOfClasses();
    void numberNodes();
    FlowNodeID findNodeOf(FunctionID, uint32_t nodeInSummary) const;
    FlowNodeID makeNode();
    std::span<const MemberOfClass> membersOf(LayoutID, ClassSide) const LIFETIME_BOUND;
    // As findMembers().
    std::span<const MemberOfClass> membersFoundUnder(LayoutID, ClassSide, PropertyKeyID) const LIFETIME_BOUND;

    LinkType joined(const LinkType&, const LinkType&) const;
    // True: the type that stops naming it says holdsLostBuiltins.
    bool loseInSight(BuiltinID canonical) const;
    Vector<BuiltinID>& builtinsLostInSightByThread() const;
    bool isNodeOfLibrary(size_t index) const { return index < m_isNodeOfLibrary.size() && m_isNodeOfLibrary.quickGet(index); }
    // As passedBy(). What a test asks of the table of types is asked once.
    LinkType passedByTestOfEdge(const LinkType&, TypeID test);
    void joinObjects(LinkType& into, const LinkType&) const;
    void joinFunctions(LinkType& into, const LinkType&) const;
    void joinClasses(LinkType& into, const LinkType&) const;
    bool standsOutsideFamilies(const LinkType&) const;
    void deferLoss(LayoutID) const;
    void deferUnion(LayoutID, LayoutID) const;
    LayoutID memberStandingFor(const LinkType&) const;
    void uniteLost(LayoutID, LayoutID);
    void loseFamiliesOfMembersThatMeet(uint32_t kept, uint32_t absorbed);
    void mergeLostFamilies(uint32_t kept, uint32_t absorbed);
    Vector<LayoutID> membersOfLostFamily(uint32_t representative) const;
    void deferLoss(FamilyID) const;
    void deferLoss(FunctionID) const;
    void deferUnion(SiteID, SiteID) const;
    void deferLossOfConstructors(FamilyID) const;
    bool deferEscape(LayoutID) const;
    bool deferEscape(FunctionID) const;
    void deferEscape(SiteID) const;
    void noteReachesUnseen(const LinkType&) const;
    void settle();
    void tell(const FlowObserver&, const LinkType& now, const LinkType& before);

    bool tieInWhatGrew();
    bool tieInLostLayouts();
    void tieInLostLayout(LayoutID);
    bool tieInLostFunctions();
    void tieInLostFunction(FunctionID);
    void open(FlowNodeID contents);
    FlowNodeID findContents(uint32_t family, SlotOfContents) const;
    void addContents(uint32_t family, SlotOfContents, FlowNodeID);
    void listContents(uint32_t site, SlotOfContents, FlowNodeID);
    bool openLayoutsThatChanged();
    bool letMembersOfLostEscape();
    bool tieAppendedFieldsOfLostLayouts();
    void noteLostLayoutsGainingProperties();

    std::pair<FlowNodeID, bool> nodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID);
    Vector<NameOnClosures> m_namesOnClosures;
    // By function: its newest name. Empty until the first name is asked for.
    Vector<uint32_t> m_newestNameOnClosuresOf;
    void makeNodesBesideFieldsOf(LayoutID);
    void open(LayoutID);
    LinkType whatLiesAboveClasses(LayoutID, PropertyKeyID) const;
    // Both answer whether the layout lists a data field under the key.
    bool tieRead(FlowNodeID aggregate, LayoutID, PropertyKeyID);
    bool tieStore(FlowNodeID aggregate, LayoutID, PropertyKeyID, bool isPartOfConstruction);
    void tieReadOfAnyField(FlowNodeID aggregate, LayoutID, bool keyIsNumber = false);
    void tieStoreOfAnyField(FlowNodeID aggregate, LayoutID);
    void tieAccessor(FlowNodeID aggregate, LayoutID receiver, const MemberOfClass&, AggregateAccess);
    void tieAccessorThatNobodyKnows(FlowNodeID aggregate, LayoutID receiver, FlowNodeID holder, AggregateAccess);
    void tieToUnseen(FlowNodeID aggregate, LayoutID, AggregateAccess);
    uint32_t recordOfLostKey(uint32_t representative, PropertyKeyID);
    FlowNodeID fieldThroughLost(LayoutID member, PropertyKeyID, AggregateAccess);
    FlowNodeID argumentOfLostFunctions(uint32_t positionalArgument);
    FlowNodeID gatheredByLostFunctions();

    // Put at a node that held something by then.
    Vector<std::pair<FlowObserver, FlowNodeID>> m_newObservers;
    mutable Vector<Deferred> m_deferred;
    bool m_isSettling { false };
    // While settle() has something deferred.
    bool m_letsFunctionsWaitToBeBroughtAlive { false };
    void bringAliveWhatSettlingReached();

    uint32_t m_firstVariable { 0 };
    // By m_firstFieldOfLayout + field. A field that a layout has from the layout above has the node of its declaration: the constructor above stores into objects of either layout.
    Vector<uint32_t> m_nodeOfField;
    // By 2 * layout + side, one more entry than that.
    Vector<uint32_t> m_firstMemberNodeOfSide;
    Vector<uint32_t> m_firstMemberOfSide;
    // Ascending by class, side, key, kind and index. Those under computed keys are left out.
    Vector<MemberOfClass> m_membersOfClasses;
    // By side: the keys under which some class has a member. A store to `this.x` in a row of classes asks for each class below, and each would go up the whole row to find nothing.
    std::array<BitVector, 2> m_keysOfMembers;
    BitVector m_sidesWithComputedKeys;
    // By 2 * layout + side: what namesMayBeIncomplete() finds on the way up the row of bases.
    BitVector m_sidesBelowIncompleteNames;
    void findSidesBelowIncompleteNames();
    // The class or a base has a getter or a method under a name that a number may print as. Few programs have one.
    BitVector m_layoutsBelowMembersUnderNumbers;
    void findLayoutsBelowMembersUnderNumbers();
    BitVector m_layoutsWithFieldsUnderComputedKeys;

    // What has been handed to m_deferred once.
    mutable BitVector m_layoutsToldLost;
    mutable BitVector m_familiesToldLost;
    mutable BitVector m_functionsToldLost;
    mutable BitVector m_layoutsToldToEscape;
    mutable BitVector m_functionsToldToEscape;
    mutable BitVector m_layoutsAtUnseen;
    mutable BitVector m_prototypesAtUnseen;
    mutable BitVector m_familiesAtUnseen;
    mutable BitVector m_functionsAtUnseen;
    // By family.
    mutable BitVector m_constructorsToldLost;
    mutable BitVector m_constructorsAtUnseen;
    mutable BitVector m_prototypesOfFamiliesAtUnseen;
    mutable bool m_lostFunctionsReachUnseen { false };
    mutable bool m_objectsReachUnseen { false };
    bool m_rowPassesWhatNobodyFollows { false };
    bool m_promisesMayBeHandedOut { false };
    // The flow no longer says where the object of the built-in is. enterLibraryInReach() empties the list.
    mutable Vector<BuiltinID> m_builtinsLost;
    mutable Vector<BuiltinID> m_builtinsLostInSight;
    struct SiteOfWrite {
        FunctionID writer;
        ValueID site;
        FlowNodeID subject;
        FlowNodeID source { };
    };
    Shared<Vector<SiteOfWrite>> m_sitesOfWritesOnBuiltins;
    mutable Vector<BuiltinID> m_builtinsAtUnseen;
    // By node, as far as the nodes of functions go.
    BitVector m_isNodeOfLibrary;
    // By function.
    BitVector m_isFunctionOfLibrary;
    // A type that is joined in hand meanwhile is the library's.
    bool m_tellsObserverOfLibrary { false };
    BitVector m_isFunctionThatRowsSpeakFor;
    mutable bool m_lostBuiltinsReachUnseen { false };

    // By the bits of the type.
    struct TestOfEdge {
        enum class Passes : uint8_t { Everything, PrimitivesAlone, EveryObject, SomeObjects };

        Passes passes { Passes::SomeObjects };
        uint16_t primitives { 0 };
    };
    HashMap<uint32_t, TestOfEdge, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_testsOfEdges;

    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_nodesOnDemand;
    // By 2 * layout: what a read finds on an object of the layout where unseen code or a change may have put something beside the fields. Then: what is stored there. UINT32_MAX: not made.
    Vector<uint32_t> m_nodesBesideFields;
    // By layout, once m_layoutsWhoseOwnValuesAreListed has it: ownValuesOf().
    Vector<Vector<OwnValue>> m_ownValuesOfLayouts;
    BitVector m_layoutsWhoseOwnValuesAreListed;
    Vector<OwnValue, 8> listOwnValuesOf(LayoutID);
    Vector<LayoutID> m_layoutsWithNodesBesideFields;
    BitVector m_openLayouts;
    struct AppendedField {
        PropertyKeyID key;
        FlowNodeID node;
        // Into m_appendedFields: the one that was made in front of it for the same layout. UINT32_MAX: none.
        uint32_t next { UINT32_MAX };
    };
    Vector<AppendedField> m_appendedFields;
    // A READ THROUGH A FAMILY OF MANY RECORDS. Nearly every member is without the key. A member with nothing above it that does not list the key answers with undefined, with what was appended to it under the key
    // and with what lies beside its fields. So only the members that list the key are tied one by one. One node stands for what lies beside the fields of them all, and the node of an appended field is tied to
    // the reads when somebody else asks for it: appendedField(). What a store through a lost value brings lies at foundBesideFieldsUnder(key): the read takes it once a member without the key is lost.
    struct ReadThroughFamily {
        PropertyKeyID key;
        FlowNodeID aggregate;
    };
    struct SortedMembersOfFamily {
        Vector<LayoutID> tiedOneByOne;
        // The rest, by the bits of each key that they list.
        HashMap<uint32_t, Vector<LayoutID, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersWithNothingAboveByKey;
        uint32_t numberWithNothingAbove { 0 };
        FlowNodeID foundBesideTheirFields;
        // Those of them that m_lostLayoutsWhoseAppendedFieldsAreTied has and that may have appended fields.
        uint32_t numberLost { 0 };
        Vector<ReadThroughFamily> readsWithoutLostMember;
    };
    // No base, no member of a class, no constructor function: what a lookup finds above the fields goes by the key alone.
    bool hasNothingAbove(LayoutID);
    BitVector m_layoutsWithNothingAbove;
    bool m_hasListedLayoutsWithNothingAbove { false };
    // The number of the family above the key: the nodes of fields appended to such members that no read through the family has asked for yet.
    HashMap<uint64_t, Vector<FlowNodeID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_appendedFieldsThatWaitForRead;
    SortedMembersOfFamily& sortedMembersOf(FamilyID);
    bool noteLostMemberWithNothingAbove(FamilyID, LayoutID);
    void tieReadThroughSortedMembers(FlowNodeID aggregate, FamilyID, PropertyKeyID);
    FlowNodeID findNodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID) const;
    HashMap<uint32_t, std::unique_ptr<SortedMembersOfFamily>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_sortedMembersOfFamilies;
    // The number of the layout above the key.
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_placesOfAppendedFields;
    // By layout, into m_appendedFields. UINT32_MAX: none.
    Vector<uint32_t> m_newestAppendedFieldOfLayout;
    BitVector m_lostLayoutsWhoseAppendedFieldsAreTied;
    struct Site {
        BuiltinClassID builtinClass;
        // ---- Of the family, where the site is its representative.
        BuiltinLayoutID layoutOfRecords;
        bool hasEscaped { false };
        // It has been handed to m_deferred.
        bool isToldToEscape { false };
        bool isClimbedFrom { false };
        bool mayHoldCellsOfCompiledLayouts { false };
        // Into m_contentsOfSites. UINT32_MAX: none. Of a site that is no representative any more: the nodes that it had, and those that it has been given since.
        uint32_t newestContents { UINT32_MAX };
        uint32_t numberOfContents { 0 };
    };
    struct ContentsOfSite {
        SlotOfContents slot;
        FlowNodeID node;
        uint32_t next { UINT32_MAX };
    };
    // Sites are made under the lock while several threads tell observers, and read without it. So a site never moves: the blocks double, and there are as many as a number has bits.
    // Whoever knows the number of a site has it from the lock, or from a type that arrived in an earlier round: the site was whole by then.
    class ListOfSites {
        WTF_MAKE_NONCOPYABLE(ListOfSites);
    public:
        ListOfSites() = default;
        size_t size() const { return m_size; }
        Site& operator[](size_t index)
        {
            size_t position = index + (static_cast<size_t>(1) << log2OfFirstBlock);
            unsigned block = static_cast<unsigned>(std::bit_width(position)) - 1 - log2OfFirstBlock;
            return m_blocks[block][position - (static_cast<size_t>(1) << (block + log2OfFirstBlock))];
        }
        void append(const Site& site)
        {
            size_t position = m_size + (static_cast<size_t>(1) << log2OfFirstBlock);
            unsigned block = static_cast<unsigned>(std::bit_width(position)) - 1 - log2OfFirstBlock;
            if (!m_blocks[block])
                m_blocks[block] = std::make_unique<Site[]>(static_cast<size_t>(1) << (block + log2OfFirstBlock));
            ++m_size;
            (*this)[m_size - 1] = site;
        }

    private:
        static constexpr unsigned log2OfFirstBlock = 10;
        std::array<std::unique_ptr<Site[]>, 33 - log2OfFirstBlock> m_blocks;
        size_t m_size { 0 };
    };
    mutable ListOfSites m_sites;
    UnitedFamilies m_familiesOfSites;
    Vector<ContentsOfSite> m_contentsOfSites;
    // Of the families with more slots than are looked through: the representative above the number of the slot. A large family is asked under every name that is called on a list.
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_contentsOfFamiliesWithManySlots;
    HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_sitesOfMakers;
    // By function. UINT32_MAX: not made.
    Vector<uint32_t> m_nodesFoundOnClosures;
    bool m_hasStoresThroughLost { false };

    // The representative above the key.
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_recordsOfLostKeys;
    // By layout. Only that of a representative counts.
    Vector<LostFamily> m_lostFamilies;
    // Through whichever family.
    bool m_anyFieldIsReadThroughLost { false };
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysLookedUpThroughLost;
    Vector<LostKey> m_lostKeys;
    // By key: whatLiesAboveClasses().
    mutable HashMap<uint32_t, BuiltinID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsOnObjectPrototype;
    Vector<HolderOfLostKey> m_holdersOfLostKeys;
    Vector<LayoutID> m_layoutsToTie;
    // becomeLost() has been asked since tieInLostLayouts() last looked.
    bool m_lostLayoutsMayHaveGrown { false };

    Vector<uint32_t> m_argumentsOfLostFunctions;
    // From this positional argument on, some lost function gathers what is passed in a list.
    uint32_t m_firstArgumentGatheredByLostFunctions { UINT32_MAX };
    // What those arguments bring. It flows into the list of each lost function that gathers. UINT32_MAX: not made.
    uint32_t m_gatheredByLostFunctions { UINT32_MAX };
    BitVector m_lostFunctionsTied;
    size_t m_numberOfLostFunctionsTied { 0 };
    bool m_lostFunctionsAreCalled { false };

    // A NUMBER NAMES AN ELEMENT AND NEVER A MEMBER. Printed it is an index or another canonical numeric string: mayBeElementKey(). So a read under a key that is only numbers comes by the fields, members and accessors under such names, and by no other.
    bool mayBeNamedByNumber(PropertyKeyID key) const { return !key || mayBeElementKey(nameClassOf(key)); }
    FlowNodeID foundBesideFieldsUnderAnyNumber();
    std::pair<FlowNodeID, bool> aggregateOfReadsUnderNumbers(Vector<uint32_t>& nodes, size_t numberOfHolders, uint32_t holder);
    // An accessor that a descriptor lays under such a name on the prototype object of a constructor function gets the object as `this`.
    void passPrototypesAboveUnderNumbers(LayoutID);
    uint32_t m_foundBesideFieldsUnderAnyNumber { UINT32_MAX };
    // By the number of the layout, of the family, of the representative of the lost family.
    Vector<uint32_t> m_elementsReadThroughLayouts;
    Vector<uint32_t> m_elementsReadThroughFamilies;
    Vector<uint32_t> m_elementsReadThroughLostFamilies;
    BitVector m_prototypesPassedByNumbers;
};

// AOTLink.cpp. reachEntries(), run(), then the tables are written: types become TypeIDs . a function whose closures neither escaped nor were lost has every caller known .
// a call with one listed callee that can enter nothing else is listed . referrers . then summarizeCallees().
// With DirectCalls and TypedOperations both withheld: conservativeLinkTables() with reach alone filled in.
// ONE FLOW, TWO READINGS, WRITTEN SIDE BY SIDE: link(). A TYPE IS NUMBERED AS IT ARRIVES IN THE PROGRAM's TABLE. So no two of the steps that make types run at once, and they come in one order whatever the threads do:
//     the tables of the proving reading | the tables of the held one | what summarizeCallees() left to type | what the held reading takes from the proofs.
// The numbers are then the same from run to run and at every width. What makes no type goes on beside a step that does: summarizeCallees(), the look at the summaries, whatever of WriterOfLink makes none.
// The first two steps are link()'s, where the held reading waits for the proving one, never the other way round: runLanes() promises a later lane no thread until the first has ended.
// The last two are finishReadingsOfLink()'s, behind the lanes.
struct TurnsOfReadings {
    LinkTables held;
    BinarySemaphore provingTablesAreWritten;
    // Kept for writeTypesFoundOfCallees().
    std::unique_ptr<LinkFlow> provingFlow;
};
// It answers the proving reading. `turns`: the held one is left there. Null: nobody asks for it, and link() does all of the proving one.
LinkTables link(Program&, const ThinSummaries&, const Families&, const LooksAtBodies&, Mechanisms, BuildContext&, OrderOfWorklist = OrderOfWorklist::NewestFirst, TurnsOfReadings* = nullptr);
// On one thread, behind both. IT MAKES TYPES.
void finishReadingsOfLink(Program&, Mechanisms, TurnsOfReadings&, LinkTables& proven, LinkTables& held);
// For the tests. The flow in each OrderOfWorklist, and a line for each BreachOfClosure. Nothing: it ends closed in all of them.
void printBreachesOfClosureOfLink(PrintStream&, Program&, const ThinSummaries&, const Families&, const LooksAtBodies&, Mechanisms, BuildContext&);
// AOTLinkEffects.cpp. Behind the flow, callees first: effects, which parameters escape, which results are fresh.
// IT ADDS NOTHING TO THE PROGRAM's TABLE OF TYPES: TurnsOfReadings. What is to be typed it leaves in LinkFlow::typesFoundOfCallees.
void summarizeCallees(const LinkFlow&, LinkTables&);
void writeTypesFoundOfCallees(const LinkFlow&, LinkTables&);
// BEHIND BOTH READINGS, on the thread that began them: neither lane waits for the other's graph.
// AOTLink.cpp. Code may test for a function that the held tables name, and mentions it then. So the held reading's graph of referrers is laid over the proven one's. Nobody asks the held tables for theirs: they are left as conservativeLinkTables() has them.
void layReferrersOfHeldReadingOver(LinkTables& proven, LinkTables& held, const ParallelLoops&);
// AOTLinkEffects.cpp. The cycles of referrers and their ranks, from LinkTables::referrers alone. link() leaves them as conservativeLinkTables() has them.
void rankReferrersFirst(LinkTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
