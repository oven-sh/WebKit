/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTID.h"
#include "AOTMocksOfProgram.h"
#include "AOTVector.h"
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BodyIndex;
class LinkFlow;
class Program;
struct FactTables;
struct LinkTables;
struct LinkType;
using FlowNodeID = ID<struct FlowNodeIDTag>;

// WHAT THE BUILD KNOWS OF A TEST RUN (`bun test --aot`): Program::admitsReplacedBuiltins. NOTHING STARTS OVER, so each decision has one decider, and whoever comes earlier may only leave things open.
// Exports and modules are the front end's: the engine reads the flags, proves nothing past a mock (AOTMocksOfProgram.h) and says what it finds beyond them.
// Members of built-ins are the engine's: the table of the build leaves the lock of every member that a test may replace to the program (BuiltinTableOfBuild::unlockForTestRun()),
// and from there on it is the machinery of every build: BuiltinsInReach::takeForConstant(), LinkFlow::writtenAtMember(), the locks that are laid at the start.
// AN EXECUTABLE PAYS NOTHING: no node, no field of a node or a type, and every question below answers at its first line.

struct NameAddedByTests {
    // Canonical.
    BuiltinID holder;
    StringID name;

    friend constexpr auto operator<=>(const NameAddedByTests&, const NameAddedByTests&) = default;
};

// AOTLinkTestRuns.cpp. What Link learns of a test run while it flows. One for each LinkFlow, so both readings have their own. Outside a test run every question answers the default and every note does nothing.
class TestRunsInLink {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TestRunsInLink);
    WTF_MAKE_NONCOPYABLE(TestRunsInLink);
public:
    explicit TestRunsInLink(LinkFlow&);

    bool isTestRun() const { return m_isTestRun; }
    // Also in an executable: the flags are the wire's.
    bool mayComeThroughMock(ModuleID exporter, uint32_t indexOfExport) const { return m_mocks.isAnyModuleMocked && mayComeThroughMockOfProgram(exporter, indexOfExport); }

    // WHAT THE RUNNER WRITES BY ITSELF. BuiltinMark::FakesTimers: each member with IsReplacedByFakeTimers. ExtendsMatchers: any name on each holder of matchers.
    // `import { test } from "bun:test"` lets the object of the module go with all that hangs below it, so such a function is in reach in every test run: that alone writes nothing.
    // It writes once somebody names it too: Link finds the function itself, or its name is read or called off a value that nobody knows.
    // enterLibraryInReach() tells each built-in that has come into reach, canonical. LinkFlow::takeNote() tells the other two. Each on one thread.
    void noteCameIntoReach(BuiltinID canonical);
    void noteFound(BuiltinID);
    void noteNameFoundOffUnknownValue(PropertyKeyID);
    // enterLibraryInReach(), behind what has come into reach, each time the worklist has run dry. On one thread.
    void lookAtWhatHasSettled();

    // LinkFlow::noteWriteOn() tells each write that the flow has followed, in front of what it notes itself. A null key: nobody can read it.
    void noteWriteOn(const LinkType& subject, PropertyKeyID, FunctionID writer);
    // The write is made by a file of the product, which finds the built-ins as an executable has them. True: all that it opens has been noted here, and the caller notes nothing of the built-in.
    bool takesWriteOnBuiltin(BuiltinID holder, PropertyKeyID, FunctionID writer);
    // The holder has no own member under the key. Text keys alone. A holder that BuiltinTable::canTakeNameInTestRuns() refuses is passed by.
    void noteWriteOfNewName(BuiltinID canonicalHolder, PropertyKeyID);
    // Made when first asked for, holds nothing. `anything` arrives with the first write of a new name under the key, on whichever object. Null outside a test run, and for a key that no test run opens.
    // WHOEVER GOES BY WHAT A LOOKUP THROUGH BUILT-INS FINDS, OR BY ITS FINDING NOTHING, WATCHES IT AND ASKS isNameAdded(): a name that is laid on an object on the way stands in front of whatever lies above.
    FlowNodeID nodeOfNameAdded(PropertyKeyID key) { return m_isTestRun ? makeNodeOfNameAdded(key) : FlowNodeID(); }
    bool isNameAdded(PropertyKeyID) const;
    // For LinkFlow::builtinsAbove(): `Object.getPrototypeOf(function* () { })`. Only the choice of locks and names goes by it. Null outside a test run, and for a class object.
    BuiltinID prototypeAboveClosuresOf(FunctionID) const;

    // The holder operand of a row with BuiltinMark::IsSpy, by the parts of its type. On a built-in a spy is a write. On the object of a module it opens nothing: the front end has folded by its own word.
    // It lays no edge. A null key: the name is read off the call where it is written there.
    void noteHolderOfSpy(const LinkType& holder, PropertyKeyID keyOrNull, FunctionID caller, ValueID site);
    // `import { spyOn } from "bun:test"`, `jest.spyOn`: the loader fills the variable, so no type names the callee and the call never comes to a row.
    // CallInLinkCalls::callUnseen() tells each call that leaves sight, once, with the node of its first argument. Where the operations of the body read the callee out of such a variable,
    // lookAtWhatHasSettled() hands what stands at the node to noteHolderOfSpy(), each time it has climbed.
    void noteCallOutOfSight(FunctionID caller, ValueID site, FlowNodeID firstArgument);

    // Sorted, each once. Behind run().
    Vector<NameAddedByTests> takeNamesAdded();
    Vector<VariableID> takeExportsSpiedOnThatWireLeavesShut();

private:
    struct WriterOfRunner {
        // Canonical.
        BuiltinID function;
        PropertyKeyID name;
        bool isInReach { false };
        bool isNamed { false };
        bool hasWritten { false };
    };
    struct SpyOutOfSight {
        FunctionID caller;
        ValueID site;
        FlowNodeID holder;
        // The parts of the type at the node that noteHolderOfSpy() goes by, as it was last handed them.
        BuiltinID builtin { };
        LayoutID layout { };
        bool holdsLostBuiltins { false };
        bool holdsUnseen { false };
    };

    bool mayComeThroughMockOfProgram(ModuleID exporter, uint32_t indexOfExport) const;
    FlowNodeID makeNodeOfNameAdded(PropertyKeyID);
    void letFlowGoRoundOnceMore();
    bool writeWhatRunnerWrites(BuiltinID canonical);
    bool mayBeAddedInTestRuns(PropertyKeyID) const;
    BuiltinID memberThatMayStandAt(const BodyIndex&, ValueID) const;
    PropertyKeyID keyWrittenAtSpy(FunctionID caller, ValueID site) const;
    void noteSpyOnExports(ModuleID, PropertyKeyID);

    LinkFlow& m_flow;
    const bool m_isTestRun;
    MocksOfProgram m_mocks;
    // Both are filled by the constructor, in a test run alone. The second: by the number of a variable that the loader fills out of the object of a module of the host, the member that the table lists there.
    Vector<WriterOfRunner, 4> m_writersOfRunner;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_membersThatLoaderFillsIn;
    Vector<SpyOutOfSight> m_spiesOutOfSight;
    // By the bits of the key.
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfNamesAdded;
    // The number of the holder above the number of the name.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesListed;
    Vector<NameAddedByTests> m_namesAdded;
    Vector<VariableID> m_exportsSpiedOn;
};

// ---- AOTTestRunsOfProgram.cpp

// A text that is no index, under which the object has no member of its own and which BuiltinTable::canTakeNameInTestRuns() lets it take.
bool holderTakesNameInTestRun(const Program&, BuiltinID canonicalHolder, PropertyKeyID);
// Behind both lanes of computeFacts(), in front of computeFactsOfProgram(): the names go into Program::openedNames, so that AOTBuiltinMembers.cpp, the plan, the walk and the image see them by the road that is there.
void openNamesThatTestsAdd(Program&, Vector<NameAddedByTests>&& ofLinked, Vector<NameAddedByTests>&& ofHeld);
// The one call of computeFacts(): what the proven reading has listed, and each name that it has followed to a write through built-ins that have met (LookupsPastProgramObjects::lostBuiltins) on each of those that takes it.
// TWO KINDS OF NAMES ARE SAID (TS100083) AND NOT OPENED, so that the run time refuses the write aloud:
// a name on Object.prototype, because the scan and Link's aggregates have gone by what the wire opens there, and a name that the held reading alone has found, because no lookup of the proven reading has heard of it.
void openNamesFoundByLink(Program&, BuildContext&, LinkTables& linked, LinkTables& held);
// Behind computeFactsOfProgram(): FactTables::builtinMembersThatTestsReplace out of builtinMembersLeftOpen, and TS100083 for the exports.
void finishFactsOfTestRun(const Program&, BuildContext&, FactTables&, Vector<VariableID>&& exportsSpiedOnThatWireLeavesShut);

// The function stands in a module with ModuleFlags::FindsBuiltinsReadOnly: a file that an executable holds too. False outside a test run.
bool isFileOfProductInTestRun(const Program&, FunctionID writer);
// And Program::isLockLeftToProgram(member): the run time refuses the write as an executable does, so it opens nothing. False outside a test run: the write counts as it does today.
bool writeOpensNothingInTestRun(const Program&, FunctionID writer, BuiltinID member);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
