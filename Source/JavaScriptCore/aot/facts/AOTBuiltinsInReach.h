/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFactRecords.h"
#include <memory>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BuilderOfProgramFacts;
class Program;
class ThinSummaries;
struct FactTables;
struct InputsOfProgramFacts;
struct LinkTables;
struct LookupsPastProgramObjects;

class LooksAtBodies;
struct DeleterOfLooksAtBodies {
    void operator()(LooksAtBodies*) const;
};
using OwnedLooksAtBodies = std::unique_ptr<LooksAtBodies, DeleterOfLooksAtBodies>;
OwnedLooksAtBodies lookAtBodies(const Program&, const BodyIndexTable&, const ThinSummaries&, BuildContext&);
const BodyIndexTable& bodiesLookedAt(const LooksAtBodies&);
class FieldsOfNumbers;
struct LinkTables;
const FieldsOfNumbers& fieldsReadAsIntegersIn(const LooksAtBodies&);
const FieldsOfNumbers& fieldsReadForArithmeticIn(const LooksAtBodies&);
const FieldsOfNumbers& fieldsStoredWhatIsNoInt32In(const LooksAtBodies&);
FieldsOfNumbers fieldsStoredCountsIn(const LooksAtBodies&, const Program&, const LinkTables& linked);
const BitVector& keysStoredWhatIsNoInt32In(const LooksAtBodies&);

class BuiltinsInReach {
    WTF_MAKE_NONCOPYABLE(BuiltinsInReach);
public:
    BuiltinsInReach(const Program&, const LooksAtBodies&, const ThinSummaries&, BuildContext&);
    ~BuiltinsInReach();

    void noteFindingsIn(FunctionID);
    void noteWhatBodyMakes(FunctionID);
    bool hasNoFindingsIn(FunctionID) const;
    bool knowsWhatBodyMakes(FunctionID) const;
    void noteEvaluated(ModuleID);
    void noteNativeCodeMayHoldObjects();
    void noteNameRead(PropertyKeyID);
    void noteNameCalled(PropertyKeyID);
    void noteNameSpelledByProgram(PropertyKeyID);
    void noteReadUnderKeyNobodyKnows();
    void noteRead(BuiltinID);
    void noteCalled(BuiltinID);
    void noteLetGo(BuiltinID);
    void noteCalledOutOfSight(BuiltinID);

    void settle(Vector<BuiltinID>& cameIntoReach, Vector<BuiltinID>& cameOutOfSight);
    Vector<BuiltinID> takeMembersWrittenAfterAll() { return std::exchange(m_membersWrittenAfterAll, { }); }

    bool mayBeHandedWhatIsNoTextNumberOrBoolean(BuiltinID canonical) const;
    bool loaderOfHostMayRun() const;
    bool unseenCodeCanRun() const;
    bool objectsMayHaveMethodCalledWithStartupLoads() const;

    bool takeForConstant(BuiltinID member);

    void noteWrite(BuiltinID holder, PropertyKeyID);
    void noteHandedWhatIsNoTextNumberOrBoolean(BuiltinID holder);
    void noteWriteThroughLostBuiltins(PropertyKeyID);
    void noteWriteThroughUnseenByProgram(PropertyKeyID);
    void noteNameWrittenThroughUnseenByLibrary(PropertyKeyID);
    void noteReadThroughLostBuiltins(PropertyKeyID);
    void noteClimbFromLostBuiltins();
    void noteClimbFromUnseenObjects();
    void noteLost(BuiltinID);
    void noteAtUnseen(BuiltinID);
    void noteOptionThatRunsCodePassed(BuiltinID);
    void noteLostBuiltinsReachUnseen();
    void noteInstanceAtUnseen(BuiltinClassID);
    bool isWorthFollowingToWrites(BuiltinID canonical) const { return m_builtinsWorthFollowingToWrites.quickGet(canonical.index()); }

    LookupsPastProgramObjects takeLookups();

private:
    std::unique_ptr<FactTables> m_tables;
    std::unique_ptr<LinkTables> m_linked;
    std::unique_ptr<InputsOfProgramFacts> m_inputs;
    std::unique_ptr<BuilderOfProgramFacts> m_builder;
    BitVector m_builtinsWorthFollowingToWrites;
    Vector<BuiltinID> m_membersWrittenAfterAll;
    bool m_wasToldSomethingNew { true };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
