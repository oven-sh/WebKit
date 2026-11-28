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

TestRunsInLink::TestRunsInLink(LinkFlow& flow)
    : m_flow(flow)
    , m_isTestRun(flow.program.admitsReplacedBuiltins)
    , m_mocks(findMocksOfProgram(flow.program))
{
    const Program& program = flow.program;
    const BuiltinTable& builtins = program.builtins();
    for (BuiltinMark mark : { BuiltinMark::FakesTimers, BuiltinMark::ExtendsMatchers }) {
        for (const BuiltinMarkedEntry& marked : builtins.marked(mark)) {
            BuiltinName name = builtins.at(marked.builtin).name;
            m_writersOfRunner.append(WriterOfRunner { builtins.canonicalID(marked.builtin), name ? program.keyOf(name) : PropertyKeyID() });
        }
    }
    if (!m_isTestRun)
        return;
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
        if (writer.hasWritten || !writer.isInReach || (m_isTestRun && !writer.isNamed))
            continue;
        writer.hasWritten = true;
        hasNoted |= writeWhatRunnerWrites(writer.function);
    }
    for (SpyOutOfSight& spy : m_spiesOutOfSight) {
        LinkType holder = m_flow.typeAt(spy.holder);
        LayoutID layout = holder.objects == LinkType::Objects::Layout || holder.objects == LinkType::Objects::Set ? holder.layout : LayoutID();
        if (holder.builtin == spy.builtin && layout == spy.layout && static_cast<uint8_t>(holder.objects) == spy.kindOfObjects && holder.holdsLostBuiltins == spy.holdsLostBuiltins && holder.hasUnknownObjects == spy.hasUnknownObjects)
            continue;
        spy.builtin = holder.builtin;
        spy.layout = layout;
        spy.kindOfObjects = static_cast<uint8_t>(holder.objects);
        spy.holdsLostBuiltins = holder.holdsLostBuiltins;
        spy.hasUnknownObjects = holder.hasUnknownObjects;
        noteHolderOfSpy(holder, { }, spy.caller, spy.site);
        hasNoted = true;
    }
    if (hasNoted)
        letFlowGoRoundOnceMore();
}

bool TestRunsInLink::mayBeAddedInTestRuns(PropertyKeyID key) const
{
    return m_flow.program.at(key).kind == PropertyKeyKind::Text && !mayBeElementKey(m_flow.nameClassOf(key));
}

FlowNodeID TestRunsInLink::makeNodeOfNameAdded(PropertyKeyID key)
{
    if (!m_isTestRun || !key || !mayBeAddedInTestRuns(key))
        return { };
    return m_flow.nodeAskedOnceByLane(LinkFlow::MadeOnDemand::NameAddedByTests, key.bits(), [&] {
        auto added = m_nodesOfNamesAdded.add(key.bits(), UINT32_MAX);
        if (added.isNewEntry)
            added.iterator->value = m_flow.makeSharedNode().index();
        return FlowNodeID(added.iterator->value);
    });
}

FlowNodeID TestRunsInLink::nodeOfSomeNameAdded()
{
    if (!m_isTestRun)
        return { };
    return m_flow.nodeAskedOnceByLane(LinkFlow::MadeOnDemand::NameAddedByTests, uint64_t(1) << 32, [&] {
        if (!m_nodeOfSomeNameAdded)
            m_nodeOfSomeNameAdded = m_flow.makeSharedNode();
        return m_nodeOfSomeNameAdded;
    });
}

bool TestRunsInLink::isNameAdded(FlowNodeID nodeOfNameAdded) const
{
    return !m_flow.typeAt(nodeOfNameAdded).isNever();
}

BuiltinID TestRunsInLink::prototypeAboveClosuresOf(FunctionID function) const
{
    if (!m_isTestRun)
        return { };
    const Function& closed = m_flow.program.at(function);
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
    if (FlowNodeID added = nodeOfNameAdded(key)) {
        m_flow.arrive(added, LinkType::anything());
        m_flow.arrive(nodeOfSomeNameAdded(), LinkType::anything());
    }
}

void TestRunsInLink::noteWriteOn(const LinkType& subject, PropertyKeyID key, FunctionID writer)
{
    if (!m_isTestRun || !key)
        return;
    bool addsAsInExecutable = isFileOfProductInTestRun(m_flow.program, writer);
    if (subject.builtin && !addsAsInExecutable)
        noteWriteOfNewName(m_flow.program.builtins().canonicalID(subject.builtin), key);
    if (subject.holdsLostBuiltins && !addsAsInExecutable) {
        if (FlowNodeID added = nodeOfNameAdded(key)) {
            m_flow.arrive(added, LinkType::anything());
            m_flow.arrive(nodeOfSomeNameAdded(), LinkType::anything());
        }
    }
}

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
    for (BuiltinID member : builtins.membersOf(holder)) {
        PropertyKeyID name = program.keyOf(builtins.at(member).name);
        if (name && !program.isLockLeftToProgram(member))
            m_flow.note(LinkFlow::Note::WriteOnBuiltin, name.bits(), holder.index());
    }
    return true;
}

BuiltinID TestRunsInLink::memberThatMayStandAt(const BodyIndex& index, ValueID value) const
{
    while (value && inputPassThroughOpcodes.contains(index.opcodeOf(value)))
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

void TestRunsInLink::noteCallHandedObjectAndName(FunctionID caller, ValueID site, FlowNodeID firstArgument)
{
    if (!m_isTestRun || isFileOfProductInTestRun(m_flow.program, caller) || !keyWrittenAtSpy(caller, site))
        return;
    LinkFlow::LockerOfWhatIsShared locker(m_flow);
    if (m_callsHandedObjectAndName.add(static_cast<uint64_t>(caller.index()) << 32 | site.index()).isNewEntry)
        m_spiesOutOfSight.append(SpyOutOfSight { caller, site, firstArgument });
}

PropertyKeyID TestRunsInLink::keyWrittenAtSpy(FunctionID caller, ValueID site) const
{
    const Program& program = m_flow.program;
    const BodyIndex* index = caller && site ? bodiesLookedAt(m_flow.looks).find(caller) : nullptr;
    if (!index || index->isParameter(site) || index->opcodeOf(site) != Opcode::Call)
        return { };
    static constexpr uint32_t operandOfName = 3;
    ValueID name;
    index->value(site).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex == operandOfName)
            name = operand;
    });
    name = name ? index->stripCasts(name) : ValueID();
    if (!name || index->isParameter(name) || index->opcodeOf(name) != Opcode::String)
        return { };
    std::optional<PropertyKeyID> spelled = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = index->value(name).as<Opcode::String>().string() });
    return spelled ? *spelled : PropertyKeyID();
}

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
    if (!m_isTestRun)
        return;
    PropertyKeyID key = keyOrNull ? keyOrNull : keyWrittenAtSpy(caller, site);
    m_flow.noteWriteOn(holder, key, caller);
    if (holder.objects != LinkType::Objects::Layout && holder.objects != LinkType::Objects::Set)
        return;
    m_flow.forEachLayoutOf(holder, [&](LayoutID member) {
        const Layout& layout = m_flow.program.at(member);
        if (layout.kind == LayoutKind::ModuleNamespace && layout.namespaceOf)
            noteSpyOnExports(layout.namespaceOf, key);
    });
}

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
