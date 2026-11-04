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

// All of it in AOTFactsOfProgram.cpp.

// What each body does with the built-ins that it names, the modules that it loads and the workers that it starts. One look at each body, in parallel, ahead of Link.
class LooksAtBodies;
struct DeleterOfLooksAtBodies {
    void operator()(LooksAtBodies*) const;
};
using OwnedLooksAtBodies = std::unique_ptr<LooksAtBodies, DeleterOfLooksAtBodies>;
OwnedLooksAtBodies lookAtBodies(const Program&, const BodyIndexTable&, const ThinSummaries&, BuildContext&);
const BodyIndexTable& bodiesLookedAt(const LooksAtBodies&);
class FieldsOfNumbers;
struct LinkTables;
// Out of the program's own code, by the layout that declares the field as the written type of the receiver leads to it: what a read of the field gives is used as an integer . something that no int32 slot holds is stored into it in sight.
// By key: the same store on a receiver of which nothing is written. True of the text; whoever goes by it guesses.
const FieldsOfNumbers& fieldsReadAsIntegersIn(const LooksAtBodies&);
const FieldsOfNumbers& fieldsReadForArithmeticIn(const LooksAtBodies&);
const FieldsOfNumbers& fieldsStoredWhatIsNoInt32In(const LooksAtBodies&);
FieldsOfNumbers fieldsStoredCountsIn(const LooksAtBodies&, const Program&, const LinkTables& linked);
const BitVector& keysStoredWhatIsNoInt32In(const LooksAtBodies&);

// Which built-ins the program can come by, while Link still finds what is reached. What is reached uses built-ins, and the code of the library behind a built-in that is used is reached: the two grow together.
// The names that find members are not those of the summaries: Link says which lookups the objects of the program do not answer. A name that only a protocol or native code looks up finds nothing until native code may hold an object.
// Facts::builtinUses() is made by the same rules from the same lookups (LinkTables::lookups): the facts of the program and what is reached say the same.
class BuiltinsInReach {
    WTF_MAKE_NONCOPYABLE(BuiltinsInReach);
public:
    BuiltinsInReach(const Program&, const LooksAtBodies&, const ThinSummaries&, BuildContext&);
    ~BuiltinsInReach();

    // In two halves: what lookAtBodies() has found in the body . of which classes of the engine it makes objects.
    void noteReached(FunctionID);
    void noteFindingsIn(FunctionID);
    void noteWhatBodyMakes(FunctionID);
    // The half would add nothing. They read alone, and may be asked by a thread while another tells something: what is told only adds, so a no that comes too early costs one call.
    bool hasNoFindingsIn(FunctionID) const;
    bool knowsWhatBodyMakes(FunctionID) const;
    void noteEvaluated(ModuleID);
    void noteNativeCodeMayHoldObjects();
    // A lookup may go on to an object of the engine or of the host: what it is made on may be one, stand below one, or be a value that nobody knows.
    void noteNameRead(PropertyKeyID);
    void noteNameCalled(PropertyKeyID);
    // Beside either of the two: the lookup stands in text of the program.
    void noteNameSpelledByProgram(PropertyKeyID);
    void noteReadUnderKeyNobodyKnows();
    // Link has found the built-in itself. It follows what a read gives, and says where it loses it. A call enters the built-in with operands that Link sees, unless it says otherwise.
    void noteRead(BuiltinID);
    void noteCalled(BuiltinID);
    void noteLetGo(BuiltinID);
    void noteCalledOutOfSight(BuiltinID);

    // It appends each built-in, under the entry that stands for the object, that has come into reach since the last call: it has a use other than BuiltinUse::Tested.
    // The second list: somebody may call it with operands that Link does not see: native code, unseen code, a call that goes by no row, a call that the scan has not bound to it. Each is in the first list too, then or earlier.
    // It costs nothing where nothing new was told since.
    void settle(Vector<BuiltinID>& cameIntoReach, Vector<BuiltinID>& cameOutOfSight);
    // Behind settle(): takeForConstant() has said yes of each of these, and says no from now on.
    Vector<BuiltinID> takeMembersWrittenAfterAll() { return std::exchange(m_membersWrittenAfterAll, { }); }

    // As they stand behind the last settle(). The first: Facts::unseenCode() would have a record, be it of a load of a module of the image.
    // BuiltinCondition::objectNobodyKnowsIsWritten, of the object that the entry stands for. It never turns false again.
    bool mayBeWritten(BuiltinID canonical) const;
    bool loaderOfHostMayRun() const;
    bool unseenCodeCanRun() const;
    // The host calls a method that it finds under a name of namesOfHostMethodsCallingStartupLoads() with what it loads at its start. Such a name is a registered symbol. An object has a method under it
    // where the table says so of what a built-in in use makes (BuiltinMark::InstancesHaveMethodCalledWithStartupLoads, a member under the name), or where code that has the symbol has laid one:
    // and the symbol comes out of a built-in whose row may give a symbol that is not new, or out of a read that no row answers: `Bun.inspect.custom`. What the table does not list may be any value that the host has.
    bool objectsMayHaveMethodCalledWithStartupLoads() const;

    // Of a location that the table does not lock. Whoever is about to go by what the table says lies there asks this, last of all its questions.
    // True: a program may replace the member (BuiltinMutability::ConstantUnlessWritten), and no write has been followed to it SO FAR: WritesOnBuiltinsInProgramFacts::reasonToLeaveOpen().
    // WHOEVER GETS A YES WATCHES LinkFlow::writtenAtMember(member) AND ASKS AGAIN WHEN IT CLIMBS: settle() lists the member once a write is followed to it after all, and from then on the answer is no.
    // Because the last answer was yes the program locks the member when it starts, so it holds whoever writes: text that is evaluated, code from disk, native code. A script that declares the name of such a global is refused.
    bool takeForConstant(BuiltinID member);

    // THE FLOW HAS BROUGHT A VALUE TO A WRITE: LinkFlow::noteWriteOn(). A null key: nobody can read it.
    void noteWrite(BuiltinID holder, PropertyKeyID);
    void noteWriteThroughLostBuiltins(PropertyKeyID);
    void noteWriteThroughUnseenByProgram(PropertyKeyID);
    void noteNameWrittenThroughUnseenByLibrary(PropertyKeyID);
    void noteReadThroughLostBuiltins(PropertyKeyID);
    void noteClimbFromLostBuiltins();
    void noteClimbFromUnseenObjects();
    // LinkType::holdsLostBuiltins. Under the entry that stands for the object.
    void noteLost(BuiltinID);
    void noteAtUnseen(BuiltinID);
    void noteOptionThatRunsCodePassed(BuiltinID);
    void noteLostBuiltinsReachUnseen();
    void noteInstanceAtUnseen(BuiltinClassID);
    // The object has a member that a lock can be laid on, or leads to one that has. Of any other nobody asks where it goes once a type stops naming it.
    bool isWorthFollowingToWrites(BuiltinID canonical) const { return m_builtinsWorthFollowingToWrites.quickGet(canonical.index()); }

    // All that it was told of lookups, for LinkTables::lookups.
    LookupsPastProgramObjects takeLookups();

private:
    // The builder writes its lists into the first, which nobody reads. The second holds the lookups.
    std::unique_ptr<FactTables> m_tables;
    std::unique_ptr<LinkTables> m_linked;
    std::unique_ptr<InputsOfProgramFacts> m_inputs;
    std::unique_ptr<BuilderOfProgramFacts> m_builder;
    BitVector m_membersThatNoBodyWrites;
    BitVector m_builtinsWorthFollowingToWrites;
    Vector<BuiltinID> m_membersWrittenAfterAll;
    bool m_wasToldSomethingNew { true };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
