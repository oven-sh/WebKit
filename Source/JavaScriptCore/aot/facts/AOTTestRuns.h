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

struct NameAddedByTests {
    BuiltinID holder;
    StringID name;

    friend constexpr auto operator<=>(const NameAddedByTests&, const NameAddedByTests&) = default;
};

class TestRunsInLink {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(TestRunsInLink);
    WTF_MAKE_NONCOPYABLE(TestRunsInLink);
public:
    explicit TestRunsInLink(LinkFlow&);

    bool isTestRun() const { return m_isTestRun; }
    bool mayComeThroughMock(ModuleID exporter, uint32_t indexOfExport) const { return m_mocks.isAnyModuleMocked && mayComeThroughMockOfProgram(exporter, indexOfExport); }

    void noteCameIntoReach(BuiltinID canonical);
    void noteFound(BuiltinID);
    void noteNameFoundOffUnknownValue(PropertyKeyID);
    void lookAtWhatHasSettled();

    void noteWriteOn(const LinkType& subject, PropertyKeyID, FunctionID writer);
    bool takesWriteOnBuiltin(BuiltinID holder, PropertyKeyID, FunctionID writer);
    void noteWriteOfNewName(BuiltinID canonicalHolder, PropertyKeyID);
    FlowNodeID nodeOfNameAdded(PropertyKeyID key) { return m_isTestRun ? makeNodeOfNameAdded(key) : FlowNodeID(); }
    FlowNodeID nodeOfSomeNameAdded();
    bool isNameAdded(FlowNodeID nodeOfNameAdded) const;
    BuiltinID prototypeAboveClosuresOf(FunctionID) const;

    void noteHolderOfSpy(const LinkType& holder, PropertyKeyID keyOrNull, FunctionID caller, ValueID site);
    void noteCallOutOfSight(FunctionID caller, ValueID site, FlowNodeID firstArgument);
    void noteCallHandedObjectAndName(FunctionID caller, ValueID site, FlowNodeID firstArgument);

    Vector<NameAddedByTests> takeNamesAdded();
    Vector<VariableID> takeExportsSpiedOnThatWireLeavesShut();

private:
    struct WriterOfRunner {
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
        BuiltinID builtin { };
        LayoutID layout { };
        uint8_t kindOfObjects { 0 };
        bool holdsLostBuiltins { false };
        bool hasUnknownObjects { false };
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
    Vector<WriterOfRunner, 4> m_writersOfRunner;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_membersThatLoaderFillsIn;
    Vector<SpyOutOfSight> m_spiesOutOfSight;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_callsHandedObjectAndName;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfNamesAdded;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesListed;
    Vector<NameAddedByTests> m_namesAdded;
    FlowNodeID m_nodeOfSomeNameAdded;
    Vector<VariableID> m_exportsSpiedOn;
};

bool holderTakesNameInTestRun(const Program&, BuiltinID canonicalHolder, PropertyKeyID);
void openNamesThatTestsAdd(Program&, Vector<NameAddedByTests>&& ofLinked, Vector<NameAddedByTests>&& ofHeld);
void openNamesFoundByLink(Program&, BuildContext&, LinkTables& linked, LinkTables& held);
void finishFactsOfTestRun(const Program&, BuildContext&, FactTables&, Vector<VariableID>&& exportsSpiedOnThatWireLeavesShut);

bool isFileOfProductInTestRun(const Program&, FunctionID writer);
bool writeOpensNothingInTestRun(const Program&, FunctionID writer, BuiltinID member);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
