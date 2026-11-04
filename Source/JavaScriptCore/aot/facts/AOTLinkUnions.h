/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThinSummary.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;
struct LinkTables;
struct LinkType;

// ---- AOTLinkUnions.cpp

// StateOfLinkReach::bringAlive(), behind noteConstructionsOf().
void comeAliveForUnions(LinkFlow&, FunctionID);
// link(), behind writeLayoutsAndCells(): LinkTables::taggedMakers, usesThroughFamilies, LinkedLayout::onlyOwnObjectsStandBelowPrototype. It writes where unseen code does anything, and leaves the defaults in the other reading.
void writeTablesOfUnions(LinkFlow&, LinkTables&);
// AOTLinkUnionUses.cpp.
void countUsesThroughFamilies(const LinkFlow&, LinkTables&);
// Something that the layouts do not say stands below the prototype objects of the type: LinkFlow::prototypesLaidAboveOtherObjects.
void notePrototypesLaidAboveOtherObjects(LinkFlow&, const LinkType& above);
// For the scan, of a NewTemplateObject: what lies among the cooked texts.
TypeID typeOfElementsOfTemplateObject(const BodyValue& made);

// ---- AOTLinkNarrowing.cpp

// WHAT A TEST OF THE PROGRAM LETS THROUGH ON ONE OF ITS EDGES: the values of `first` and those of `second`. Each is a type that the scan can name without an arena, and that LinkFlow::passedBy() cuts by.
// Neither: the edge says nothing. It is never less than what passes in some run.
struct TypesPassedByTest {
    TypeID first;
    TypeID second;

    explicit operator bool() const { return !!first; }
};
// `test`: what the condition of a Branch is defined by. `other`: what the other operand of a comparison is defined by, behind casts. Null: a parameter, or the test has one operand.
TypesPassedByTest typesPassedByTest(const Program&, const BodyValue& test, uint32_t operandIndex, bool isOnTakenEdge, const BodyValue& other);
// Of the subject of a Switch or a SwitchString.
TypesPassedByTest typesPassedByCase(const BodyTerminator&, uint32_t targetIndex);
// Of an operation that hands its operand on or throws: RequireObjectCoercible, RequireObject, a claim of the library that the walk tests.
TypesPassedByTest typesPassedByCheck(const Program&, const BodyValue& check);

// ListInScan of AOTScan.cpp.
struct WordsOfListInScan {
    uint32_t first { 0 };
    uint32_t count { 0 };
};

// What the names ask of the scan.
class ScanAskedByNames {
public:
    virtual ~ScanAskedByNames() = default;
    // Null: a parameter.
    virtual BodyValue definitionOf(ValueID) = 0;
    virtual WordsOfListInScan listOf(ValueID) = 0;
    // False: the body is making the object, and goes by what it has seen of it.
    virtual bool mayGoUnderName(ValueID) = 0;
    virtual uint32_t newJoin() = 0;
    virtual void flowInto(WordsOfListInScan, uint32_t join, TypeID passes) = 0;
    virtual WordsOfListInScan listOfNode(uint32_t) = 0;
    // noSummaryNode: the list is not one node.
    virtual uint32_t onlyNodeOf(WordsOfListInScan) = 0;
};

// For the parameter that takes a proof of an edge, where several edges lead to its block: SummaryEdge::passes has room for one type. `test`: as for typesPassedByTest(), null of a Switch. Null: the edge cuts nothing, or two types pass.
TypeID typeThatAlonePassesOnEdge(ScanAskedByNames&, const Program&, const BodyTerminator&, const BodyValue& test, uint32_t operandIndex, uint32_t targetIndex);

// BEHIND A TEST THE BODY NAMES THE VALUE THAT WAS TESTED. `if (typeof x === "string") use(x)`: the arm reads `x` as the code in front of the test does, and no edge hands on a narrower value.
// So what has passed goes under a name of the scan's own in every block that only the edge leads to, and behind a check that throws in all that follows it.
// The names in force are a list that grows at its head. An edge hands on the list of its block, with what its test says in front. A block that several edges lead to goes on with the part that they all have,
// and with a name for each value that the last test in front of every one of them has cut: `typeof x === "string" || typeof x === "number"`.
// A NAME COSTS NOTHING UNTIL IT IS READ: then it is a Join, with an edge for each type that passes.
// The scan reads a block when it has read every edge that leads forward into it. An edge that leads back has to bring what its target went by. Where one does not, the body is read again without names.
class NamesBehindTestsInScan {
    WTF_MAKE_NONCOPYABLE(NamesBehindTestsInScan);
public:
    static constexpr uint32_t none = UINT32_MAX;

    NamesBehindTestsInScan() = default;

    void begin(uint32_t numberOfBlocks, uint32_t numberOfValues, bool follows);
    bool follows() const { return m_follows; }
    bool flowIsNotWhatNamesWentBy() const { return m_flowIsNotWhatNamesWentBy; }

    // In front of the arguments of an edge of a terminator: what the edge says of the operands of its test.
    void beginEdge(ScanAskedByNames&, const Program&, const BodyTerminator&, uint32_t targetIndex, const BodyTarget&);
    // Of every edge, that to a handler too.
    void leadEdge(uint32_t from, uint32_t to);
    // `numberOfEdgesRead`: how many edges into the block the scan has read.
    void enterBlock(const BodyBlock&, uint32_t numberOfEdgesRead);
    // Behind the operation, which has handed its operand on.
    void noteCheck(ScanAskedByNames&, const Program&, const BodyValue& check);
    void leaveBody() { standIn(none); }

    uint32_t nameOf(ValueID value) const
    {
        if (m_names.isEmpty())
            return none;
        if (value.index() == m_valueAskedAsAtRead)
            return m_nameAsAtRead;
        return m_nameOfValue[value.index()];
    }
    WordsOfListInScan listUnder(ScanAskedByNames&, uint32_t name);
    // The list is one Join that stands for a name. While nothing lies there, no run comes by.
    uint32_t joinBehindTest(ScanAskedByNames&, WordsOfListInScan) const;

    // The scan looks at a read by name when somebody asks for what it gives, which may be behind a test of the object. The read has run on all that came to it.
    void noteRead(uint32_t read, ValueID object);
    class NameAsAtRead {
        WTF_MAKE_NONCOPYABLE(NameAsAtRead);
    public:
        NameAsAtRead(NamesBehindTestsInScan&, uint32_t read, ValueID object);
        ~NameAsAtRead();

    private:
        NamesBehindTestsInScan& m_names;
        uint32_t m_valueBefore;
        uint32_t m_nameBefore;
    };

private:
    struct Name {
        uint32_t value { 0 };
        uint32_t parent { none };
        // As Walker::Frame::addNarrowerName() lays them: the list is climbed in a number of steps that grows with the logarithm of its length.
        uint32_t jump { none };
        uint32_t depth { 0 };
        // The block that the edge leads to, or in which the check stands. The names at the head of a list that have one group are what one test says.
        uint32_t group { 0 };
        // What the value went by when this name came into force.
        uint32_t hidden { none };
        uint32_t join { noSummaryNode };
        TypesPassedByTest passes;
        // What the value went by in front of the test. none: its own list.
        uint32_t nameInFrontOfTest { none };
        // Into m_parts. One part: another name of the same value. Several: what arrives under any of them.
        uint32_t firstPart { 0 };
        uint32_t numberOfParts { 0 };
        // The parameter of the target that takes the proof of the edge. none: the edge hands on no proof of it.
        uint32_t positionOfProof { none };
    };
    struct EdgeIntoBlock {
        uint32_t head { none };
        uint32_t next { none };
    };
    struct AtBlock {
        uint32_t names { none };
        uint32_t common { none };
        uint32_t firstEdge { none };
        uint32_t numberOfEdges { 0 };
        bool isEntered { false };
        bool namesItsParameters { false };
    };

    uint32_t depthOf(uint32_t name) const { return name == none ? 0 : m_names[name].depth; }
    uint32_t add(uint32_t parent, Name&&);
    uint32_t addOtherName(uint32_t parent, uint32_t value, uint32_t group, uint32_t sameAs);
    uint32_t nameValueAndWhatItWasCastFrom(ScanAskedByNames&, uint32_t parent, ValueID subject, TypesPassedByTest, uint32_t group, uint32_t positionOfProof);
    uint32_t climbTo(uint32_t name, uint32_t depth) const;
    uint32_t commonPartOf(uint32_t, uint32_t) const;
    uint32_t findInGroupAtHead(uint32_t head, uint32_t value, uint32_t depthOfCommonPart) const;
    uint32_t uniteWhatEveryEdgeHasCut(const AtBlock&, uint32_t block);
    WordsOfListInScan listWithoutName(ScanAskedByNames&, uint32_t value);
    uint32_t behindOtherNames(uint32_t name) const;
    void makeJoin(ScanAskedByNames&, uint32_t name, std::span<const WordsOfListInScan>);
    void standIn(uint32_t head);

    bool m_follows { false };
    bool m_flowIsNotWhatNamesWentBy { false };
    uint32_t m_numberOfValues { 0 };
    uint32_t m_inForce { none };
    std::optional<uint32_t> m_headOnEdge;
    uint32_t m_valueAskedAsAtRead { none };
    uint32_t m_nameAsAtRead { none };
    Vector<Name> m_names;
    Vector<uint32_t> m_parts;
    Vector<uint32_t> m_nameOfValue;
    Vector<AtBlock> m_blocks;
    Vector<EdgeIntoBlock> m_edges;
    Vector<uint32_t> m_path;
    // Ascending.
    Vector<uint32_t> m_joins;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_namesAtReads;
};

// A CALL BY BINDING WHOSE ARGUMENT HAS PASSED A TEST IS ENTERED WHEN SOMETHING PASSES: SummaryCall::waitsFor. True: the call waits, and LinkFlow::waitingCallObserved() enters it.
bool callWaitsForArgument(LinkFlow&, FunctionID caller, uint32_t indexOfCall);

// ---- AOTLinkUnionEffects.cpp

// ThinSummary::inspections: some operation of the body walks a chain or lists keys of what may be a proxy, or of what may stand below one.
bool someInspectionMayRunCode(const LinkFlow&, const LinkTables&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
