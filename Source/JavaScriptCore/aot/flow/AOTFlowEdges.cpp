/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowCoroutines.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

static bool carriesProgramValues(OpaqueKind kind)
{
    switch (kind) {
    case OpaqueKind::IteratorState:
    case OpaqueKind::Exception:
        return true;
    case OpaqueKind::Enumerator:
    case OpaqueKind::ClassToken:
    case OpaqueKind::StringBuilder:
    case OpaqueKind::Cursor:
    case OpaqueKind::KeyOrder:
    case OpaqueKind::Guard:
    case OpaqueKind::FlatString:
    case OpaqueKind::CodeAddress:
    case OpaqueKind::BuilderCursor:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayThrowAsTyped(const Program& program, const Function& function, const Value& value)
{
    EffectSummaries unknownCallees;
    return effectsOf(EffectContext { program, unknownCallees }, function, value).mayThrow();
}

FlowSummaryInputs::FlowSummaryInputs(const Program& program, BuildContext& build)
    : program(program)
    , build(build)
    , hasModulesWithoutSideEffects(program.modules.containsIf([](const Module& module) {
        return module.flags.contains(ModuleFlags::HasNoSideEffects);
    }))
{
}

FlowSummary buildFlowSummary(const FlowSummaryInputs& inputs, FunctionID function)
{
    RELEASE_ASSERT(inputs.program.at(function).hasBody());
    return FlowSummaryBuilder(inputs, function).build();
}

// This decides how a function is analyzed, not what is proven about it, so a prediction is as good as a type.
static bool holdsNoObject(const Program& program, const DeclaredType& declared)
{
    if (!mayBeReference(program.at(declared.type)))
        return true;
    return declared.predicted && !mayBeReference(program.at(declared.predicted));
}

static BitVector entryParametersWithoutObjects(const Program& program, const Function& function)
{
    unsigned numberOfEntryParameters = function.numberOfEntryParameters();
    BitVector result(numberOfEntryParameters);
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        const DeclaredType* declared = function.declaredEntryParameter(index);
        if (function.entryParameterKind(index) == EntryParameter::ArgumentCount || (declared && holdsNoObject(program, *declared)))
            result.quickSet(index);
    }
    return result;
}

FlowSummaryBuilder::FlowSummaryBuilder(const FlowSummaryInputs& inputs, FunctionID id)
    : m_program(inputs.program)
    , m_build(inputs.build)
    , m_hasModulesWithoutSideEffects(inputs.hasModulesWithoutSideEffects)
    , m_id(id)
    , m_function(m_program.at(id))
    , m_cfg(m_function)
    , m_cycles(m_cfg)
    , m_uses(m_function, m_cfg)
    , m_assembler(FlowSummaryShape { id, m_function.numberOfEntryParameters(), static_cast<uint32_t>(m_function.results.size()), static_cast<uint32_t>(m_function.values.size()), entryParametersWithoutObjects(m_program, m_function) })
    , m_block(entryBlock)
    , m_caughtLocals(m_function.blocks.size())
{
}

FlowSummary FlowSummaryBuilder::build()
{
    addLocals();
    addEntry();
    addCoroutine();
    for (BlockID block : m_cfg.reversePostOrder())
        addBlock(block);
    if (m_function.isModuleBody())
        addSyntheticExportCaptures();
    addUsesOfModules();
    return m_assembler.finish();
}

void FlowSummaryBuilder::addLocals()
{
    auto addLocalIfNeeded = [&](ValueID id) {
        if (needsLocal(m_function.at(id)))
            m_assembler.addLocalFor(id);
    };
    for (BlockID id : m_cfg.reversePostOrder()) {
        const Block& block = m_function.at(id);
        if (id != entryBlock) {
            for (ValueID parameter : block.parameters)
                addLocalIfNeeded(parameter);
        }
        for (ValueID value : block.values)
            addLocalIfNeeded(value);
    }
}

void FlowSummaryBuilder::addEntry()
{
    const Block& entry = m_function.at(entryBlock);
    RELEASE_ASSERT(entry.parameters.size() == m_function.numberOfEntryParameters());
    for (uint32_t index = 0; index < entry.parameters.size(); ++index)
        m_assembler.bind(entry.parameters[index], m_assembler.entryParameter(index));
    addRestSite();
}

void FlowSummaryBuilder::addRestSite()
{
    std::optional<unsigned> restParameter = m_function.hiddenRestParameterIndex();
    if (!restParameter && m_function.has(FunctionFlags::HasRestParameter))
        restParameter = m_function.firstParameterIndex() + m_function.numberOfPositionalParameters();
    if (!restParameter)
        return;

    uint32_t site = m_assembler.addSite({
        .kind = FlowSiteKind::Instance,
        .origin = FlowSiteOrigin::RestParameter,
        .numberOfPositions = static_cast<uint16_t>(m_build.limit(LimitID::MaxPositionalParts)),
        .isCreatedDense = true,
        .builtinClass = BuiltinClasses::Array,
    });
    m_assembler.seed(FlowSeed::site(m_assembler.entryParameter(*restParameter), site));
    m_assembler.add(FlowAllocation { ValueID(), site, FlowLocalID(), false });
    m_assembler.setRestSite(site);
}

void FlowSummaryBuilder::addCoroutine()
{
    std::optional<CoroutineKind> kind = coroutineKindOf(m_function);
    if (!kind)
        return;

    FlowCoroutineSummary coroutine;
    coroutine.resultSite = m_assembler.addSite({
        .kind = FlowSiteKind::Instance,
        .origin = FlowSiteOrigin::CoroutineResult,
        .builtinClass = resultClassOf(*kind),
    });
    switch (*kind) {
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncGenerator:
        coroutine.yielded = m_assembler.addTemporary();
        coroutine.sent = m_assembler.addTemporary();
        break;
    case CoroutineKind::AsyncFunction:
        break;
    }
    m_assembler.setCoroutine(coroutine);
}

void FlowSummaryBuilder::addBlock(BlockID id)
{
    const Block& block = m_function.at(id);
    m_block = id;
    if (id != entryBlock)
        addBlockParameters(id);
    for (ValueID value : block.values)
        addValue(value);
    addTerminatorOf(block);
}

void FlowSummaryBuilder::addValue(ValueID id)
{
    const Value& value = m_function.at(id);
    m_value = id;
    if (mayThrow(value.opcode))
        noteMayThrow(id);

    switch (value.opcode) {
#define ADD_FLOW_EDGES(opcode) \
    case Opcode::opcode: \
        add<Opcode::opcode>(id, value); \
        return;
        FOR_EACH_AOT_OPCODE(ADD_FLOW_EDGES)
#undef ADD_FLOW_EDGES
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryBuilder::addTerminatorOf(const Block& block)
{
    const Terminator& terminator = block.terminator;
    m_value = ValueID();
    if (declaredBoundOf(terminator.kind).mayThrow)
        noteMayThrow(ValueID());

    switch (terminator.kind) {
#define ADD_FLOW_EDGES(kind) \
    case TerminatorKind::kind: \
        addTerminator<TerminatorKind::kind>(terminator); \
        return;
        FOR_EACH_AOT_TERMINATOR_KIND(ADD_FLOW_EDGES)
#undef ADD_FLOW_EDGES
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FlowSummaryBuilder::needsLocal(const Value& value) const
{
    if (!value.type)
        return false;
    ResultRule rule = resultRuleOf(value.opcode);
    if (rule == ResultRule::None || rule == ResultRule::Results)
        return false;
    std::optional<OpaqueKind> opaque = m_program.at(value.type).opaque;
    return !opaque || carriesProgramValues(*opaque);
}

bool FlowSummaryBuilder::mayHoldReference(ValueID id) const
{
    return localOf(id) && mayBeReference(typeOf(id));
}

FlowReadUse FlowSummaryBuilder::readUseOf(ValueID id) const
{
    // No code is emitted for a model: native code reads what it reads, and no pass turns its calls into direct calls.
    if (m_function.role == FunctionRole::Model)
        return FlowReadUse::Value;

    Vector<ValueID, 4> worklist { id };
    while (!worklist.isEmpty()) {
        std::span<const Use> uses = m_uses.usesOf(worklist.takeLast());
        if (uses.empty())
            return FlowReadUse::Value;
        for (const Use& use : uses) {
            if (use.kind != Use::Kind::Operand)
                return FlowReadUse::Value;
            Opcode user = m_function.at(use.user).opcode;
            OperandUses operandUses = operandUsesOf(user, use.operandIndex);
            if (isCast(user) && operandUses.contains(OperandUse::Returns)) {
                worklist.append(use.user);
                continue;
            }
            if (!operandUses.contains(OperandUse::Calls) || operandUses.containsAny({ OperandUse::Retains, OperandUse::Returns }))
                return FlowReadUse::Value;
        }
    }
    return FlowReadUse::CalleeOnly;
}

bool FlowSummaryBuilder::isTailCall(ValueID id) const
{
    std::optional<Use> use = m_uses.singleUse(id);
    return use && use->kind == Use::Kind::TerminatorOperand && m_function.at(use->block).terminator.kind == TerminatorKind::ReturnCall;
}

Vector<FlowLocalID, 8> FlowSummaryBuilder::localsOf(std::span<const ValueID> values) const
{
    Vector<FlowLocalID, 8> locals;
    locals.reserveInitialCapacity(values.size());
    for (ValueID value : values)
        locals.append(localOf(value));
    return locals;
}

void FlowSummaryBuilder::seedTypeAsRead(ValueID id)
{
    if (FlowLocalID local = localOf(id))
        m_assembler.seed(FlowSeed::typeAsRead(local, m_function.at(id).type));
}

void FlowSummaryBuilder::seedPrimitives(FlowLocalID local, uint16_t primitives)
{
    if (local)
        m_assembler.seed(FlowSeed::primitives(local, primitives));
}

void FlowSummaryBuilder::seedInteger(FlowLocalID local, int64_t integer)
{
    if (local)
        m_assembler.seed(FlowSeed::integers(local, m_assembler.addRange(Range::one(integer))));
}

void FlowSummaryBuilder::filter(FlowLocalID from, FlowLocalID to, TypeID type, FlowFilterKind kind)
{
    if (from && to)
        m_assembler.add(FlowFilter { from, to, type, kind });
}

FlowNewSite FlowSummaryBuilder::addSiteFor(ValueID id, const FlowSiteDescription& description, FlowLocalID prototype)
{
    FlowSiteDescription siteDescription = description;
    siteDescription.value = id;
    uint32_t site = m_assembler.addSite(siteDescription);
    FlowLocalID local = localOf(id);
    m_assembler.seed(FlowSeed::site(local, site));
    m_assembler.add(FlowAllocation { id, site, prototype, isInCycle() });
    return { site, local };
}

FlowNewSite FlowSummaryBuilder::addInstanceSiteFor(ValueID id, BuiltinClassID builtinClass, uint16_t numberOfPositions, bool isCreatedDense)
{
    return addSiteFor(id, {
        .kind = FlowSiteKind::Instance,
        .origin = FlowSiteOrigin::Operation,
        .numberOfPositions = numberOfPositions,
        .isCreatedDense = isCreatedDense,
        .builtinClass = builtinClass,
    });
}

void FlowSummaryBuilder::storeIntoSite(ValueID, const FlowExternalNode& part, FlowLocalID value)
{
    if (value)
        m_assembler.copy(value, m_assembler.external(part));
}

uint16_t FlowSummaryBuilder::numberOfPositionsFor(size_t numberOfElements) const
{
    if (!m_build.exceeds(LimitID::MaxPositionalParts, numberOfElements, { .function = m_id, .value = m_value }))
        return static_cast<uint16_t>(numberOfElements);
    return static_cast<uint16_t>(m_build.limit(LimitID::MaxPositionalParts));
}

FlowLocalID FlowSummaryBuilder::thrownTarget()
{
    const Block& block = m_function.at(m_block);
    return block.handler ? caughtLocalOf(block.handler->block) : m_assembler.thrown();
}

FlowLocalID FlowSummaryBuilder::caughtLocalOf(BlockID handler)
{
    FlowLocalID& local = m_caughtLocals[handler.index()];
    if (!local)
        local = m_assembler.addTemporary();
    return local;
}

void FlowSummaryBuilder::noteMayThrow(ValueID origin)
{
    FlowLocalID target = thrownTarget();
    // The failure of a module is an object of the host, such as a ResolveMessage, and no instance of Error.
    constexpr OpcodeSet opcodesThrowingObjectsOfHost { Opcode::ResolveDynamic, Opcode::ThrowModuleFailure };
    if (origin && opcodesThrowingObjectsOfHost.contains(m_function.at(origin).opcode)) {
        FlowLocalID unknownValue = m_assembler.addTemporary();
        m_assembler.seed(FlowSeed::unknown(unknownValue));
        m_assembler.copy(unknownValue, target);
    }
    if (m_localsWithRuntimeError.get(target.index()))
        return;
    if (origin && !mayThrowAsTyped(m_program, m_function, m_function.at(origin)))
        return;
    m_localsWithRuntimeError.set(target.index());
    m_assembler.add(FlowOperandUse { .origin = origin, .thrown = target });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
