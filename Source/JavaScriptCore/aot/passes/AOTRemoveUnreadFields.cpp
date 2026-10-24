/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnreadFields.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTPassRunner.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct UnreadFieldCandidateTable {
    std::optional<uint32_t> find(FieldReference field) const
    {
        auto found = std::ranges::lower_bound(list, field);
        if (found == list.end() || *found != field)
            return std::nullopt;
        return static_cast<uint32_t>(found - list.begin());
    }

    std::optional<uint32_t> find(VariableID variable) const
    {
        if (!variables.get(variable.index()))
            return std::nullopt;
        return static_cast<uint32_t>(list.size() + variable.index());
    }

    void forEachCandidateOf(LayoutID layout, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        size_t candidate = std::ranges::lower_bound(list, layout, { }, &FieldReference::layout) - list.begin();
        for (; candidate < list.size() && list[candidate].layout == layout; ++candidate)
            functor(static_cast<uint32_t>(candidate));
    }

    bool isEmpty() const { return list.isEmpty() && variables.isEmpty(); }
    size_t size() const { return list.size() + variables.size(); }

    Vector<FieldReference> list;
    BitVector variables;
};

class LiveCandidates {
public:
    explicit LiveCandidates(size_t numberOfCandidates)
        : m_isLive(FillWith { }, numberOfCandidates, static_cast<uint8_t>(0))
    {
    }

    size_t size() const { return m_isLive.size(); }
    bool isLive(uint32_t candidate) { return WTF::atomicLoad(&m_isLive[candidate], std::memory_order_relaxed); }

    void markLive(uint32_t candidate)
    {
        if (!isLive(candidate))
            WTF::atomicStore(&m_isLive[candidate], static_cast<uint8_t>(1), std::memory_order_relaxed);
    }

private:
    Vector<uint8_t> m_isLive;
};

struct CandidateSet : WTF::UnionFind<CandidateSet> { };

struct RemovableAccess {
    BlockID block;
    ValueID access;
    Vector<uint32_t, 1> candidates;
};

struct FunctionAccesses {
    FunctionID function;
    Vector<RemovableAccess> accesses;
};

struct ListedValue {
    BlockID block;
    ValueID id;
    const Value& value;
};

struct ReachedLayouts {
    std::span<const LayoutID> members;
    bool isComplete { false };
};

struct UnreadFieldScanContext {
    std::optional<uint32_t> candidateOf(LayoutID layout, uint32_t field) const
    {
        return candidates.find(declarationOf(program, { layout, field }));
    }

    const Program& program;
    const EffectSummaries& effects;
    const BuildContext& build;
    const UnreadFieldCandidateTable& candidates;
    LiveCandidates& live;
    OpcodeSet opcodesNamingLayouts;
    OpcodeSet opcodesStoringThroughOperands;
    OpcodeSet scannedOpcodes;
    BitVector keysOfCandidates;
};

class UnreadFieldScanner {
public:
    UnreadFieldScanner(const UnreadFieldScanContext& context, FunctionID id)
        : m_context(context)
        , m_id(id)
        , m_function(context.program.at(id))
        , m_canChangeFunction(context.build.isEnabled(PassID::RemoveUnreadFields, id))
    {
    }

    Vector<RemovableAccess> run();

private:
    void scan(const ListedValue&);
    void scanNamedFields(const ListedValue&);
    void scanVariableAccess(const ListedValue&);
    void scanAccessByKey(const ListedValue&);
    void scanStoresThroughOperands(const Value&);
    ReachedLayouts layoutsReachedBy(const Value& access) const;
    bool isUnusedSlotRead(const ListedValue&);
    void addRemovableAccess(const ListedValue&, Vector<uint32_t, 1>&& candidates);
    void markLive(std::span<const uint32_t> candidates);

    const UnreadFieldScanContext& m_context;
    FunctionID m_id;
    const Function& m_function;
    bool m_canChangeFunction;
    std::optional<BitVector> m_usedValues;
    Vector<RemovableAccess> m_removableAccesses;
};

} // anonymous namespace

static constexpr OperandUses keyedStoreUses { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor };

static bool supportsRemoval(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Environment:
        return true;
    case LayoutKind::Container:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isUnread(const BuildContext& build, const Layout& layout, const Field& field)
{
    constexpr OptionSet<FieldFlags> excludedFlags { FieldFlags::IsInherited, FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden };
    if (field.flags.containsAny(excludedFlags) || field.form.contains(FieldForm::IsAccessor))
        return false;
    if (!layout.isEnvironment() && !field.key)
        return false;
    if (field.flow.location.writers != WriterState::AllKnown)
        return false;
    // It has no storage as it is, whatever flow says of the names of its stores. Only a candidate gets its storage back.
    if (hasNoAccesses(field.flow.location))
        return true;
    // A store that the scan cannot find would define the key at run time on an object without the field.
    if (field.flow.location.storeNaming != StoreNaming::EveryStoreNamesLocation)
        return false;
    switch (field.flow.location.reads) {
    case ReadState::OnlyCalled:
    case ReadState::NeverRead:
        return true;
    case ReadState::Unknown:
        return build.isSabotaged(Sabotage::RemovedFieldsHaveUnknownReaders);
    case ReadState::OnlyAccessed:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayHaveNamespaceObject(NamespaceUse use)
{
    switch (use) {
    case NamespaceUse::Unknown:
    case NamespaceUse::NamedReadsOnly:
        return true;
    case NamespaceUse::NeverCreated:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findVariablesReadOutsideBodies(const Program& program, const FlowResults& flow)
{
    BitVector variables(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            variables.quickSet(variable.index());
    };
    BitVector isReadByHost(program.modules.size());
    for (const Entry& entry : program.entries) {
        if (!entry.hostReads.isEmpty())
            isReadByHost.quickSet(entry.module.index());
    }
    for (unsigned index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        bool isEveryExportRead = isReadByHost.quickGet(index) || mayHaveNamespaceObject(flow.module(ModuleID(index)).namespaceUse);
        for (const ModuleExport& entry : module.exports) {
            if (entry.variable && (isEveryExportRead || program.at(entry.variable).flow.reads == ReadState::Unknown))
                add(entry.variable);
        }
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
        // The run time reads the variable that holds `module.exports` for whoever asks for the value of the module, takes its names from that value, or holds its module object.
        if (module.exportsVariable && program.at(module.exportsVariable).flow.reads == ReadState::Unknown)
            add(module.exportsVariable);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings)
            add(binding.variable);
    }
    return variables;
}

// Only GetModuleVariable reads a variable that no module, eval scope or class object names, so the scan sees every read of it.
static BitVector findCandidateVariables(const Program& program, const FlowResults& flow)
{
    BitVector isReadOutsideBodies = findVariablesReadOutsideBodies(program, flow);
    BitVector candidates(program.variables.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (!variable.owner && variable.flow.writers == WriterState::AllKnown && !isReadOutsideBodies.quickGet(index))
            candidates.quickSet(index);
    }
    return candidates;
}

static UnreadFieldCandidateTable findCandidateFields(const BuildContext& build, const Program& program)
{
    UnreadFieldCandidateTable candidates;
    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        LayoutID id(layoutIndex);
        const Layout& layout = program.at(id);
        if (!supportsRemoval(layout.kind) || layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (isUnread(build, layout, layout.fields[field]))
                candidates.list.append(FieldReference { id, field });
        }
    }
    return candidates;
}

// The keys under which a lookup finds a candidate, or a field that was inherited from one.
static BitVector findKeysOfCandidates(const Program& program, const ParallelLoops& loops, const UnreadFieldCandidateTable& candidates)
{
    Vector<PropertyKeyID> found = collectInParallel<PropertyKeyID>(loops, program.layouts.size(), [&](unsigned index, Vector<PropertyKeyID>& keys) {
        LayoutID layout(index);
        const auto& fields = program.at(layout).fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            if (fields[field].key && candidates.find(declarationOf(program, { layout, field })))
                keys.append(fields[field].key);
        }
    });
    BitVector keys;
    for (PropertyKeyID key : found)
        keys.set(key.index());
    return keys;
}

static OpcodeSet findOpcodesWithLayout()
{
    return Value::opcodesWithLayout;
}

static bool callsBuiltin(EffectRule rule)
{
    switch (rule) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static OpcodeSet findOpcodesStoringThroughOperands()
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool mayStore = callsBuiltin(effectRuleOf(opcode));
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex)
            mayStore |= operandUsesOf(opcode, operandIndex).containsAny(keyedStoreUses);
        if (mayStore)
            opcodes.add(opcode);
    }
    return opcodes;
}

static void markFieldsLive(const UnreadFieldScanContext& context, LayoutID id)
{
    context.candidates.forEachCandidateOf(id, [&](uint32_t candidate) {
        context.live.markLive(candidate);
    });

    const Layout& layout = context.program.at(id);
    for (uint32_t field = 0; field < layout.fields.size(); ++field) {
        if (!layout.fields[field].flags.contains(FieldFlags::IsInherited))
            continue;
        if (auto candidate = context.candidateOf(id, field))
            context.live.markLive(*candidate);
    }
}

static void markEvalBindingsLive(const UnreadFieldScanContext& context)
{
    for (const EvalScope& scope : context.program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!binding.layout || !binding.field)
                continue;
            if (auto candidate = context.candidateOf(binding.layout, *binding.field))
                context.live.markLive(*candidate);
        }
    }
}

static void markDataLayoutsLive(const UnreadFieldScanContext& context)
{
    BitVector isMarked(context.program.layouts.size());
    for (const Data& data : context.program.data) {
        forEachLayout(data, [&](LayoutID layout) {
            if (!isMarked.quickSet(layout.index()))
                markFieldsLive(context, layout);
        });
    }
}

static void markParsedLayoutsLive(const UnreadFieldScanContext& context)
{
    const Program& program = context.program;
    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        if (program.layouts[layoutIndex].flow.observedUses.contains(ObservedUse::ReachesTypedParse))
            markFieldsLive(context, LayoutID(layoutIndex));
    }
}

static LayoutID layoutInPayloadOf(const Value& value)
{
    return Value::opcodesWithLayout.contains(value.opcode) ? value.layout() : LayoutID();
}

static void forEachNamedField(const Value& value, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    for (const PayloadFieldInfo& field : payloadFieldsOf(value.opcode)) {
        if (field.kind == PayloadKind::Field) {
            if (auto index = decodePayloadWord<std::optional<uint32_t>>(value.payloadWordAt(field.firstWord)))
                functor(*index);
            continue;
        }
        if (field.kind != PayloadKind::List || field.elementKind != PayloadKind::Field)
            continue;
        for (uint32_t index : value.payloadListFrom(field.firstWord))
            functor(index);
    }
}

static bool isUnmodeledAllocation(Opcode opcode)
{
    if (opcode == Opcode::CloneAs)
        return true;
    return resultRuleOf(opcode) == ResultRule::Allocation && opcode != Opcode::NewObject;
}

static bool isRemovableFieldStore(Opcode opcode)
{
    return opcode == Opcode::NewObject || opcode == Opcode::PutEnvironmentField;
}

static constexpr bool isRemovableSlotStore(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::Slot && accessOf(opcode) == Access::Store && resultRuleOf(opcode) == ResultRule::None;
}

static_assert(isRemovableSlotStore(Opcode::PutSlot) && isRemovableSlotStore(Opcode::PutSlotOrAdd));

static constexpr bool isSlotRead(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::Slot && accessOf(opcode) == Access::Read;
}

static_assert(isSlotRead(Opcode::GetSlot) && isSlotRead(Opcode::GetDeferredClosureSlot));
static_assert(resultRuleOf(Opcode::PutEnvironmentField) == ResultRule::None);

static constexpr bool findsFieldByKey(Opcode opcode)
{
    return Value::opcodesWithReceivers.contains(opcode) && !Value::opcodesWithField.contains(opcode) && !Value::opcodesWithPlacedKeys.contains(opcode);
}

static BitVector findUsedValues(const Function& function)
{
    BitVector isUsed(function.values.size());
    auto markUsed = [&](ValueID value) {
        isUsed.quickSet(value.index());
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            function.at(id).forEachChild(markUsed);
        block.forEachChildOutsideValues(markUsed);
    }
    return isUsed;
}

Vector<RemovableAccess> UnreadFieldScanner::run()
{
    forEachListedValue(m_context.program, m_id, m_context.scannedOpcodes, [&](BlockID block, uint32_t, ValueID id, const Value& value) {
        scan(ListedValue { block, id, value });
    });
    return WTF::move(m_removableAccesses);
}

void UnreadFieldScanner::scan(const ListedValue& listed)
{
    Opcode opcode = listed.value.opcode;
    if (m_context.opcodesNamingLayouts.contains(opcode))
        scanNamedFields(listed);
    if (Value::opcodesWithVariable.contains(opcode))
        scanVariableAccess(listed);
    if (findsFieldByKey(opcode))
        scanAccessByKey(listed);
    if (m_context.opcodesStoringThroughOperands.contains(opcode))
        scanStoresThroughOperands(listed.value);
}

void UnreadFieldScanner::scanNamedFields(const ListedValue& listed)
{
    const Value& value = listed.value;
    LayoutID layout = layoutInPayloadOf(value);
    if (!layout)
        return;

    if (isUnmodeledAllocation(value.opcode))
        markFieldsLive(m_context, layout);

    forEachNamedField(value, [&](uint32_t field) {
        auto candidate = m_context.candidateOf(layout, field);
        if (!candidate)
            return;
        if (isRemovableFieldStore(value.opcode))
            addRemovableAccess(listed, Vector<uint32_t, 1> { *candidate });
        else
            m_context.live.markLive(*candidate);
    });
}

void UnreadFieldScanner::scanVariableAccess(const ListedValue& listed)
{
    auto candidate = m_context.candidates.find(listed.value.variable());
    if (!candidate)
        return;
    if (listed.value.opcode == Opcode::PutModuleVariable)
        addRemovableAccess(listed, Vector<uint32_t, 1> { *candidate });
    else
        m_context.live.markLive(*candidate);
}

ReachedLayouts UnreadFieldScanner::layoutsReachedBy(const Value& access) const
{
    const Program& program = m_context.program;
    if (LayoutSetID stamp = access.receivers()) {
        const LayoutSet& receivers = program.at(stamp);
        return { receivers.members.span(), receivers.isComplete };
    }

    std::optional<uint8_t> receiver = receiverOf(access.opcode);
    if (!receiver)
        return { };
    return { membersOf(program, program.typeOf(m_function, access.operand(*receiver))), false };
}

void UnreadFieldScanner::scanAccessByKey(const ListedValue& listed)
{
    const Program& program = m_context.program;
    const Value& value = listed.value;
    ReachedLayouts reached = layoutsReachedBy(value);

    PropertyKeyID key = Value::opcodesWithKey.contains(value.opcode) ? value.key() : PropertyKeyID();
    if (!key) {
        for (LayoutID member : reached.members)
            markFieldsLive(m_context, member);
        return;
    }

    // Most keys are the key of no candidate, and then no member has a candidate under the key.
    bool isKeyOfCandidate = m_context.keysOfCandidates.get(key.index());
    if (!isKeyOfCandidate)
        return;

    bool reachesOnlyCandidates = reached.isComplete;
    Vector<uint32_t, 1> found;
    for (LayoutID member : reached.members) {
        std::optional<uint32_t> candidate;
        if (auto field = findField(program, member, key, BaseChain::Proven))
            candidate = m_context.candidateOf(field->layout, field->field);
        if (candidate)
            found.append(*candidate);
        else
            reachesOnlyCandidates = false;
    }
    if (found.isEmpty())
        return;
    AOT::sort(found);
    removeRepeatedElements(found);

    if (reachesOnlyCandidates && (isRemovableSlotStore(value.opcode) || isUnusedSlotRead(listed)))
        addRemovableAccess(listed, WTF::move(found));
    else
        markLive(found.span());
}

// The read of a deferred closure fills the slot, so dead code elimination leaves it where nothing uses its value.
bool UnreadFieldScanner::isUnusedSlotRead(const ListedValue& listed)
{
    if (!isSlotRead(listed.value.opcode))
        return false;
    if (!m_usedValues)
        m_usedValues = findUsedValues(m_function);
    return !m_usedValues->quickGet(listed.id.index());
}

void UnreadFieldScanner::scanStoresThroughOperands(const Value& value)
{
    const Program& program = m_context.program;
    EffectContext effectContext { program, m_context.effects };
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        std::span<const LayoutID> members = membersOf(program, program.typeOf(m_function, operand));
        if (members.empty() || !summarizeOperand(effectContext, m_function, value, operandIndex).uses.containsAny(keyedStoreUses))
            return;
        for (LayoutID member : members)
            markFieldsLive(m_context, member);
    });
}

void UnreadFieldScanner::addRemovableAccess(const ListedValue& listed, Vector<uint32_t, 1>&& candidates)
{
    if (m_canChangeFunction)
        m_removableAccesses.append(RemovableAccess { listed.block, listed.id, WTF::move(candidates) });
    else
        markLive(candidates.span());
}

void UnreadFieldScanner::markLive(std::span<const uint32_t> candidates)
{
    for (uint32_t candidate : candidates)
        m_context.live.markLive(candidate);
}

// The opcodes for which FunctionScanner::scan() does something.
static OpcodeSet findScannedOpcodes(const OpcodeSet& opcodesNamingLayouts, const OpcodeSet& opcodesStoringThroughOperands)
{
    OpcodeSet opcodes = opcodesNamingLayouts | Value::opcodesWithVariable | opcodesStoringThroughOperands;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (findsFieldByKey(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}

static Vector<FunctionAccesses> scanFunctions(const UnreadFieldScanContext& context)
{
    const Program& program = context.program;
    return collectInParallel<FunctionAccesses>(context.build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FunctionAccesses>& collected) {
        FunctionID id(index);
        if (!mayListValueWith(program, id, context.scannedOpcodes) || !program.at(id).hasBody())
            return;
        Vector<RemovableAccess> accesses = UnreadFieldScanner(context, id).run();
        if (!accesses.isEmpty())
            collected.append(FunctionAccesses { id, WTF::move(accesses) });
    });
}

static void keepAccessesWhole(std::span<const FunctionAccesses> functions, LiveCandidates& live)
{
    Vector<CandidateSet> sets(live.size());
    bool hasSharedAccess = false;
    for (const FunctionAccesses& entry : functions) {
        for (const RemovableAccess& site : entry.accesses) {
            for (uint32_t candidate : site.candidates.span().subspan(1)) {
                sets[candidate].unify(&sets[site.candidates[0]]);
                hasSharedAccess = true;
            }
        }
    }
    if (!hasSharedAccess)
        return;

    auto rootOf = [&](uint32_t candidate) {
        return static_cast<size_t>(sets[candidate].find() - sets.span().data());
    };
    BitVector hasLiveMember(live.size());
    for (uint32_t candidate = 0; candidate < live.size(); ++candidate) {
        if (live.isLive(candidate))
            hasLiveMember.quickSet(rootOf(candidate));
    }
    for (uint32_t candidate = 0; candidate < live.size(); ++candidate) {
        if (hasLiveMember.quickGet(rootOf(candidate)))
            live.markLive(candidate);
    }
}

// A validating build tells the run time which names it took away, so that a lookup of one from outside compiled code ends the process.
static uint64_t recordRemovedFields(ProgramEditor& editor, const UnreadFieldCandidateTable& candidates, LiveCandidates& live, bool validates)
{
    uint64_t numberOfRemovedFields = 0;
    for (uint32_t candidate = 0; candidate < candidates.list.size(); ++candidate) {
        FieldReference reference = candidates.list[candidate];
        bool hadNoAccesses = hasNoAccesses(editor.program().at(reference.layout).fields[reference.field].flow.location);
        if (live.isLive(candidate) != hadNoAccesses)
            continue;
        Field& field = editor.at(reference);
        if (hadNoAccesses) {
            field.flow.location.stores = StoreCount::Unknown;
            continue;
        }
        field.flow.location.stores = StoreCount::Never;
        field.flow.location.reads = ReadState::NeverRead;
        // No allocation lists it any more: an object is finished without it.
        if (field.flow.occupancy == Occupancy::FilledAtBirth || field.flow.occupancy == Occupancy::FilledWithValueAtBirth)
            field.flow.occupancy = Occupancy::Unknown;
        if (validates)
            field.flags.add(FieldFlags::IsRemovedAsUnread);
        ++numberOfRemovedFields;
    }
    return numberOfRemovedFields;
}

static void recordRemovedVariables(ProgramEditor& editor, const UnreadFieldCandidateTable& candidates, LiveCandidates& live)
{
    for (size_t index : candidates.variables) {
        if (live.isLive(*candidates.find(VariableID(index))))
            continue;
        LocationFlow& flow = editor.at(VariableID(index)).flow;
        flow.stores = StoreCount::Never;
        flow.reads = ReadState::NeverRead;
    }
}

static uint64_t countVariableStores(const Program& program, std::span<const FunctionAccesses> functions)
{
    uint64_t numberOfStores = 0;
    for (const FunctionAccesses& entry : functions) {
        const Function& function = program.at(entry.function);
        for (const RemovableAccess& site : entry.accesses)
            numberOfStores += function.at(site.access).opcode == Opcode::PutModuleVariable;
    }
    return numberOfStores;
}

static void pruneAccessesOfLiveCandidates(Vector<FunctionAccesses>& functions, LiveCandidates& live)
{
    for (FunctionAccesses& entry : functions) {
        entry.accesses.removeAllMatching([&](const RemovableAccess& site) {
            return std::ranges::any_of(site.candidates, [&](uint32_t candidate) {
                return live.isLive(candidate);
            });
        });
    }
    functions.removeAllMatching([](const FunctionAccesses& entry) {
        return entry.accesses.isEmpty();
    });
}

static void rebuildAllocation(const UnreadFieldScanContext& context, Value& allocation)
{
    LayoutID layout = allocation.layout();
    std::span<const uint32_t> fields = allocation.fields();
    std::span<const ValueID> values = allocation.variadicOperands();
    RELEASE_ASSERT(fields.size() == values.size());

    Vector<uint32_t, 16> remainingFields;
    Vector<ValueID, 16> remainingValues;
    for (size_t index = 0; index < fields.size(); ++index) {
        auto candidate = context.candidateOf(layout, fields[index]);
        if (candidate && !context.live.isLive(*candidate))
            continue;
        remainingFields.append(fields[index]);
        remainingValues.append(values[index]);
    }
    allocation.setFields(remainingFields.span());
    allocation.setVariadicOperands(remainingValues.span());
}

static void removeAccessesInBlock(const UnreadFieldScanContext& context, IRBuilder& builder, BlockID block, std::span<const RemovableAccess> accesses)
{
    Function& function = builder.function();
    const BlockValues& values = function.at(block).values;
    BitVector erasedIndexes;
    for (uint32_t index = 0; index < values.size() && !accesses.empty(); ++index) {
        ValueID id = values[index];
        if (accesses.front().access != id)
            continue;
        while (!accesses.empty() && accesses.front().access == id)
            accesses = accesses.subspan(1);

        Value& access = function.edit(id);
        if (access.opcode == Opcode::NewObject)
            rebuildAllocation(context, access);
        else
            erasedIndexes.set(index);
    }
    RELEASE_ASSERT(accesses.empty());
    if (!erasedIndexes.isEmpty())
        builder.eraseValues(block, erasedIndexes);
}

static void removeAccesses(const UnreadFieldScanContext& context, IRBuilder& builder, std::span<const RemovableAccess> accesses)
{
    while (!accesses.empty()) {
        BlockID block = accesses.front().block;
        auto end = std::ranges::find_if(accesses, [&](const RemovableAccess& site) {
            return site.block != block;
        });
        size_t numberOfAccessesInBlock = end - accesses.begin();
        removeAccessesInBlock(context, builder, block, accesses.first(numberOfAccessesInBlock));
        accesses = accesses.subspan(numberOfAccessesInBlock);
    }
}

static void rewriteFunctions(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner, const UnreadFieldScanContext& context, std::span<const FunctionAccesses> functions)
{
    const Program& program = editor.program();
    BuildContext& build = runner.build();
    Vector<FunctionID> ids;
    ids.reserveInitialCapacity(functions.size());
    for (const FunctionAccesses& entry : functions)
        ids.append(entry.function);

    editFunctionsInParallel(editor, build, ids.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        {
            IRBuilder builder(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function, arenas.types);
            removeAccesses(context, builder, functions[index].accesses.span());
        }
        simplifyEditedFunction(runner, program, inputs, id, function, arenas);
    });
}

static bool areReceiverSetsEnabled(const Program& program, const BuildContext& build)
{
    for (uint32_t function = 0; function < program.numberOfFunctionsAsRead(); ++function) {
        if (!build.isEnabled(FlowResultKind::ReceiverSets, function))
            return false;
    }
    return true;
}

void removeUnreadFields(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    const Program& program = editor.program();
    BuildContext& build = runner.build();

    UnreadFieldCandidateTable candidates;
    if (areReceiverSetsEnabled(program, build))
        candidates = findCandidateFields(build, program);
    if (inputs.flow.hasResults())
        candidates.variables = findCandidateVariables(program, inputs.flow);
    if (candidates.isEmpty())
        return;

    LiveCandidates live(candidates.size());
    OpcodeSet opcodesNamingLayouts = findOpcodesWithLayout();
    OpcodeSet opcodesStoringThroughOperands = findOpcodesStoringThroughOperands();
    UnreadFieldScanContext context { program, inputs.effects, build, candidates, live, opcodesNamingLayouts, opcodesStoringThroughOperands, findScannedOpcodes(opcodesNamingLayouts, opcodesStoringThroughOperands), findKeysOfCandidates(program, build.parallelLoops(), candidates) };
    markEvalBindingsLive(context);
    markDataLayoutsLive(context);
    markParsedLayoutsLive(context);
    Vector<FunctionAccesses> functions = scanFunctions(context);
    keepAccessesWhole(functions.span(), live);

    build.statistics().count(StatisticID::UnreadFieldsRemoved, recordRemovedFields(editor, candidates, live, build.validates()));
    recordRemovedVariables(editor, candidates, live);
    pruneAccessesOfLiveCandidates(functions, live);
    build.statistics().count(StatisticID::UnreadVariableStoresRemoved, countVariableStores(program, functions.span()));
    rewriteFunctions(editor, inputs, runner, context, functions.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
