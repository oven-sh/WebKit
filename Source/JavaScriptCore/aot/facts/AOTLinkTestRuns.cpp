/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTOpcodeSets.h"
#include "AOTSort.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// Every note only sets bits, lists what a set has not listed, or lets `anything` arrive at a node of its own: told twice it does nothing new, and a larger type does all that a smaller one did.
// It goes by the parts of the type that it is handed and by the records of the program, never by what a set of the flow holds at the time.
// OBSERVERS ARE TOLD ON SEVERAL THREADS: whatever they may write here, or read where another may write, is under LinkFlow::LockerOfWhatIsShared, and what they tell BuiltinsInReach goes by LinkFlow::note().

TestRunsInLink::TestRunsInLink(LinkFlow& flow)
    : m_flow(flow)
    , m_isTestRun(flow.program.admitsReplacedBuiltins)
    , m_mocks(findMocksOfProgram(flow.program))
{
    if (!m_isTestRun)
        return;
    const Program& program = flow.program;
    const BuiltinTable& builtins = program.builtins();
    for (BuiltinMark mark : { BuiltinMark::FakesTimers, BuiltinMark::ExtendsMatchers }) {
        for (const BuiltinMarkedEntry& marked : builtins.marked(mark)) {
            BuiltinName name = builtins.at(marked.builtin).name;
            m_writersOfRunner.append(WriterOfRunner { builtins.canonicalID(marked.builtin), name ? program.keyOf(name) : PropertyKeyID() });
        }
    }
    for (const Module& module : program.modules) {
        BuiltinID object = module.runtimeModule ? builtins.namespaceOf(module.runtimeModule) : BuiltinID();
        if (!object)
            continue;
        for (const SyntheticExport& filled : module.syntheticExports) {
            BuiltinName name = filled.variable ? program.builtinName(filled.name) : BuiltinName();
            BuiltinID member = name ? builtins.findOwnMember(builtins.canonicalID(object), name) : BuiltinID();
            if (member)
                m_membersThatLoaderFillsIn.set(filled.variable.index(), member.index());
        }
    }
}

bool TestRunsInLink::mayComeThroughMockOfProgram(ModuleID exporter, uint32_t indexOfExport) const
{
    return Typed::mayComeThroughMock(m_flow.program, m_mocks, exporter, indexOfExport);
}

// ---- What the runner writes by itself

// The object that `extend` hangs on, and the prototype object of every class of the host whose instances it or a member of it answers: `expect(value)`, `expect.not`.
// A matcher is bound to a call only where a row says of which class the receiver is, so whatever prototype object a call can be bound through is among these.
static void forEachHolderOfMatchersInLinkTestRuns(const BuiltinTable& builtins, BuiltinID extend, const Invocable<void(BuiltinID)> auto& functor)
{
    if (!builtins.at(extend).holder)
        return;
    BuiltinID expect = builtins.canonicalID(builtins.at(extend).holder);
    functor(expect);
    auto visitPrototypesOfInstancesAnsweredBy = [&](BuiltinID entry) {
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(entry))) {
            if (!overload.result.type)
                continue;
            for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
                if (!part.builtinClass || builtins.at(part.builtinClass).origin != BuiltinOrigin::Host)
                    continue;
                if (BuiltinID prototype = builtins.at(part.builtinClass).prototype)
                    functor(builtins.canonicalID(prototype));
            }
        }
    };
    visitPrototypesOfInstancesAnsweredBy(expect);
    for (BuiltinID member : builtins.membersOf(expect)) {
        if (builtins.at(member).kind == BuiltinKind::Prototype)
            functor(builtins.canonicalID(member));
        visitPrototypesOfInstancesAnsweredBy(member);
    }
}

// BuiltinsInReach looks at what it was told when the worklist has next run dry. A node that climbs sees to it that it does.
void TestRunsInLink::letFlowGoRoundOnceMore()
{
    m_flow.arrive(m_flow.makeSharedNode(), LinkType::anything());
}

void TestRunsInLink::noteCameIntoReach(BuiltinID canonical)
{
    for (WriterOfRunner& writer : m_writersOfRunner)
        writer.isInReach |= writer.function == canonical;
}

void TestRunsInLink::noteFound(BuiltinID builtin)
{
    for (WriterOfRunner& writer : m_writersOfRunner)
        writer.isNamed |= writer.function == m_flow.program.builtins().canonicalID(builtin);
}

void TestRunsInLink::noteNameFoundOffUnknownValue(PropertyKeyID key)
{
    for (WriterOfRunner& writer : m_writersOfRunner)
        writer.isNamed |= key && writer.name == key;
}

// `const tools = [vi]; tools[0].useFakeTimers()`, `expect.extend(matchers)`: the rows say too little, and no operation of the program stores anything.
bool TestRunsInLink::writeWhatRunnerWrites(BuiltinID canonical)
{
    const Program& program = m_flow.program;
    const BuiltinTable& builtins = program.builtins();
    BuiltinsInReach& inReach = m_flow.builtinsInReach();
    bool hasWritten = false;
    if (builtins.has(BuiltinMark::FakesTimers, canonical)) {
        for (const BuiltinMarkedEntry& replaced : builtins.marked(BuiltinMark::IsReplacedByFakeTimers)) {
            const Builtin& member = builtins.at(replaced.builtin);
            PropertyKeyID key = member.holder && member.name ? program.keyOf(member.name) : PropertyKeyID();
            if (!key)
                continue;
            inReach.noteWrite(member.holder, key);
            hasWritten = true;
        }
    }
    if (builtins.has(BuiltinMark::ExtendsMatchers, canonical)) {
        forEachHolderOfMatchersInLinkTestRuns(builtins, canonical, [&](BuiltinID holder) {
            inReach.noteWrite(holder, { });
            hasWritten = true;
        });
    }
    return hasWritten;
}

void TestRunsInLink::lookAtWhatHasSettled()
{
    bool hasNoted = false;
    for (WriterOfRunner& writer : m_writersOfRunner) {
        if (writer.hasWritten || !writer.isInReach || !writer.isNamed)
            continue;
        writer.hasWritten = true;
        hasNoted |= writeWhatRunnerWrites(writer.function);
    }
    for (SpyOutOfSight& spy : m_spiesOutOfSight) {
        // A copy: what is noted may make a node.
        LinkType holder = m_flow.typeAt(spy.holder);
        LayoutID layout = holder.objects == LinkType::Objects::Layout ? holder.layout : LayoutID();
        if (holder.builtin == spy.builtin && layout == spy.layout && holder.holdsLostBuiltins == spy.holdsLostBuiltins && holder.holdsUnseen == spy.holdsUnseen)
            continue;
        spy.builtin = holder.builtin;
        spy.layout = layout;
        spy.holdsLostBuiltins = holder.holdsLostBuiltins;
        spy.holdsUnseen = holder.holdsUnseen;
        noteHolderOfSpy(holder, { }, spy.caller, spy.site);
        hasNoted = true;
    }
    if (hasNoted)
        letFlowGoRoundOnceMore();
}

// ---- Names that the flow brings to a built-in

bool TestRunsInLink::mayBeAddedInTestRuns(PropertyKeyID key) const
{
    return m_flow.program.at(key).kind == PropertyKeyKind::Text && !mayBeElementKey(m_flow.nameClassOf(key));
}

FlowNodeID TestRunsInLink::makeNodeOfNameAdded(PropertyKeyID key)
{
    if (!m_isTestRun || !key || !mayBeAddedInTestRuns(key))
        return { };
    // Every access by name of a test run asks.
    return m_flow.nodeAskedOnceByLane(LinkFlow::MadeOnDemand::NameAddedByTests, key.bits(), [&] {
        auto added = m_nodesOfNamesAdded.add(key.bits(), UINT32_MAX);
        if (added.isNewEntry)
            added.iterator->value = m_flow.makeSharedNode().index();
        return FlowNodeID(added.iterator->value);
    });
}

bool TestRunsInLink::isNameAdded(FlowNodeID nodeOfNameAdded) const
{
    return !m_flow.typeAt(nodeOfNameAdded).isNothing();
}

BuiltinID TestRunsInLink::prototypeAboveClosuresOf(FunctionID function) const
{
    if (!m_isTestRun)
        return { };
    const Function& closed = m_flow.program.at(function);
    // A class object stands below the class that it extends.
    if (closed.constructedClass && !m_flow.isOfConstructorFunction(closed.constructedClass))
        return { };
    BuiltinClassID classOfClosures = BuiltinClasses::Function;
    if (closed.has(FunctionFlags::IsGenerator) && closed.has(FunctionFlags::IsAsync))
        classOfClosures = BuiltinClasses::AsyncGeneratorFunction;
    else if (closed.has(FunctionFlags::IsGenerator))
        classOfClosures = BuiltinClasses::GeneratorFunction;
    else if (closed.has(FunctionFlags::IsAsync))
        classOfClosures = BuiltinClasses::AsyncFunction;
    return m_flow.prototypeOfInstancesOf(classOfClosures);
}

// `Object.getPrototypeOf(function* () { }).stored = 1`
void TestRunsInLink::noteWriteOfNewName(BuiltinID canonicalHolder, PropertyKeyID key)
{
    const Program& program = m_flow.program;
    if (!m_isTestRun || !key || !holderTakesNameInTestRun(program, canonicalHolder, key))
        return;
    StringID name = program.at(key).text;
    if (program.isOpenedSomewhere(name) && program.isOpened(canonicalHolder, name))
        return;
    if (m_namesListed.add(static_cast<uint64_t>(canonicalHolder.index()) << 32 | name.index()).isNewEntry)
        m_namesAdded.append(NameAddedByTests { canonicalHolder, name });
    if (FlowNodeID added = nodeOfNameAdded(key))
        m_flow.arrive(added, LinkType::anything());
}

void TestRunsInLink::noteWriteOn(const LinkType& subject, PropertyKeyID key, FunctionID writer)
{
    if (!m_isTestRun || !key)
        return;
    // A file of the product adds what an executable lets it add.
    bool addsAsInExecutable = isFileOfProductInTestRun(m_flow.program, writer);
    if (subject.builtin && !addsAsInExecutable)
        noteWriteOfNewName(m_flow.program.builtins().canonicalID(subject.builtin), key);
    // `for (const [, place] of places()) place.mark = 1`. Which of them take the name is asked when all of them are known: openNamesFoundByLink().
    // That goes by LookupsPastProgramObjects::namesWrittenThroughLostBuiltins, which has the key whoever wrote. So whoever wrote, the lookups are told.
    if (subject.holdsLostBuiltins) {
        if (FlowNodeID added = nodeOfNameAdded(key))
            m_flow.arrive(added, LinkType::anything());
    }
}

// ---- A write in a file of the product

bool TestRunsInLink::takesWriteOnBuiltin(BuiltinID holder, PropertyKeyID key, FunctionID writer)
{
    const Program& program = m_flow.program;
    if (!m_isTestRun || !isFileOfProductInTestRun(program, writer))
        return false;
    const BuiltinTable& builtins = program.builtins();
    holder = builtins.canonicalID(holder);
    if (key) {
        BuiltinID member = findOwnMember(program, holder, key);
        return member && program.isLockLeftToProgram(member);
    }
    // `Buffer[name] = f`: it may name whatever an executable leaves to the program, and nothing else.
    for (BuiltinID member : builtins.membersOf(holder)) {
        PropertyKeyID name = program.keyOf(builtins.at(member).name);
        if (name && !program.isLockLeftToProgram(member))
            m_flow.note(LinkFlow::Note::WriteOnBuiltin, name.bits(), holder.index());
    }
    return true;
}

// ---- Spies

// What the table lists at the place that the operations of the body read the value out of. Nothing proves that it still lies there.
BuiltinID TestRunsInLink::memberThatMayStandAt(const BodyIndex& index, ValueID value) const
{
    while (value && inputOperationsThatHandOn.contains(index.opcodeOf(value)))
        value = index.value(value).operand(0);
    if (!value)
        return { };
    BodyValue read = index.value(value);
    if (read.opcode() == Opcode::Builtin)
        return read.as<Opcode::Builtin>().builtin();
    if (read.opcode() == Opcode::GetModuleVariable) {
        VariableID variable = read.as<Opcode::GetModuleVariable>().variable();
        auto filled = variable ? m_membersThatLoaderFillsIn.find(variable.index()) : m_membersThatLoaderFillsIn.end();
        return filled == m_membersThatLoaderFillsIn.end() ? BuiltinID() : BuiltinID(filled->value);
    }
    if (read.opcode() != Opcode::PropGet)
        return { };
    auto access = read.as<Opcode::PropGet>();
    BuiltinID holder = access.key() ? memberThatMayStandAt(index, access.objectOperand()) : BuiltinID();
    return holder ? findOwnMember(m_flow.program, m_flow.program.builtins().canonicalID(holder), access.key()) : BuiltinID();
}

void TestRunsInLink::noteCallOutOfSight(FunctionID caller, ValueID site, FlowNodeID firstArgument)
{
    if (!m_isTestRun || !caller || !site)
        return;
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(caller);
    if (!index || index->opcodeOf(site) != Opcode::Call)
        return;
    const BuiltinTable& builtins = m_flow.program.builtins();
    BuiltinID callee = memberThatMayStandAt(*index, index->value(site).as<Opcode::Call>().calleeOperand());
    if (!callee || (!builtins.has(BuiltinMark::IsSpy, callee) && !builtins.has(BuiltinMark::IsSpy, builtins.canonicalID(callee))))
        return;
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    m_spiesOutOfSight.append(SpyOutOfSight { caller, site, firstArgument });
}

// `spyOn(object, "total")`: Link knows no texts, and the name stands in the call.
PropertyKeyID TestRunsInLink::keyWrittenAtSpy(FunctionID caller, ValueID site) const
{
    const Program& program = m_flow.program;
    const BodyIndex* index = caller && site ? bodiesLookedAt(m_flow.looks).find(caller) : nullptr;
    if (!index || index->isParameter(site) || index->opcodeOf(site) != Opcode::Call)
        return { };
    // The callee, `this`, the object, the name.
    static constexpr uint32_t operandOfName = 3;
    ValueID name;
    index->value(site).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex == operandOfName)
            name = operand;
    });
    name = name ? index->valueBehindCasts(name) : ValueID();
    if (!name || index->isParameter(name) || index->opcodeOf(name) != Opcode::String)
        return { };
    std::optional<PropertyKeyID> spelled = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = index->value(name).as<Opcode::String>().string() });
    return spelled ? *spelled : PropertyKeyID();
}

// No key: every export.
void TestRunsInLink::noteSpyOnExports(ModuleID module, PropertyKeyID key)
{
    const Program& program = m_flow.program;
    StringID name;
    if (key) {
        const PropertyKey& named = program.at(key);
        if (named.kind != PropertyKeyKind::Text)
            return;
        name = named.text;
    }
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    for (const ModuleExport& entry : program.at(module).exports) {
        if ((name && entry.name != name) || entry.kind != ModuleExportKind::Variable || !entry.variable)
            continue;
        if (!program.at(entry.variable).flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock }))
            m_exportsSpiedOn.append(entry.variable);
    }
}

void TestRunsInLink::noteHolderOfSpy(const LinkType& holder, PropertyKeyID keyOrNull, FunctionID caller, ValueID site)
{
    // An executable refuses a spy that its build has not seen: it opens no export and unlocks no member for one.
    if (!m_isTestRun)
        return;
    PropertyKeyID key = keyOrNull ? keyOrNull : keyWrittenAtSpy(caller, site);
    m_flow.noteWriteOn(holder, key, caller);
    if (holder.objects != LinkType::Objects::Layout)
        return;
    const Layout& layout = m_flow.program.at(holder.layout);
    if (layout.kind == LayoutKind::ModuleNamespace && layout.namespaceOf)
        noteSpyOnExports(layout.namespaceOf, key);
}

// ---- Behind run()

Vector<NameAddedByTests> TestRunsInLink::takeNamesAdded()
{
    Vector<NameAddedByTests> names = std::exchange(m_namesAdded, { });
    m_namesListed.clear();
    AOT::sort(names);
    return names;
}

Vector<VariableID> TestRunsInLink::takeExportsSpiedOnThatWireLeavesShut()
{
    Vector<VariableID> exports = std::exchange(m_exportsSpiedOn, { });
    AOT::sort(exports);
    removeRepeatedElements(exports);
    return exports;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
