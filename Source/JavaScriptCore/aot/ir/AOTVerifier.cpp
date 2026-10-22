/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifier.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTFlowResults.h"
#include "AOTVerifierInternal.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

template<typename IDType>
static bool isInFlowTable(IDType id, const FlowResults* flow, unsigned (FlowResults::*numberOfRecords)() const)
{
    return id && (!flow || id.index() < (flow->*numberOfRecords)());
}

template<typename IDType>
static bool isInPlacedTable(IDType id, IRForm form, const PlacedTableSizes* sizes, unsigned PlacedTableSizes::* numberOfRecords)
{
    return id && (form != IRForm::Placed || id.index() < sizes->*numberOfRecords);
}

template<typename Entry>
static std::optional<InternalErrorKind> findStorageError(const Instruction& instruction, Entry entry)
{
    if (instruction.numberOfNamedOperands() != numberOfNamedOperandsOf(entry) || instruction.numberOfOperands() < instruction.numberOfNamedOperands())
        return InternalErrorKind::OperandMissing;
    if (instruction.inlinePayload().size() + instruction.outOfLinePayload().size() < defaultPayloadOf(entry).size())
        return InternalErrorKind::PayloadOutOfRange;
    return std::nullopt;
}

template<typename Entry>
static void verifyEntry(FunctionVerifier& verifier, BlockID block, ValueID id, const Instruction& instruction, Entry entry)
{
    if (!verifier.isLegalInForm(formsOf(entry))) {
        bool isLeftByPass = verifier.form() == IRForm::Prepared && removedByOf(entry) != PassID::None;
        verifier.fail(isLeftByPass ? InternalErrorKind::RequiredPassLeftOperation : InternalErrorKind::OpcodeNotInForm, block, id);
        return;
    }
    if (uint32_t undeclaredFlags = instruction.flags & ~declaredFlagsOf(entry)) {
        verifier.fail(InternalErrorKind::FlagNotDeclared, block, id, undeclaredFlags);
        return;
    }
    if (std::optional<InternalErrorKind> error = findStorageError(instruction, entry))
        verifier.fail(*error, block, id);
}

FunctionVerifier::FunctionVerifier(const Program& program, FunctionID id, const Function& function, IRForm form, BuildContext& build, const VerifierInputs& inputs)
    : m_program(program)
    , m_id(id)
    , m_function(function)
    , m_form(form)
    , m_build(build)
    , m_inputs(inputs)
{
    RELEASE_ASSERT(form != IRForm::Placed || inputs.placed);
}

FunctionVerifier::~FunctionVerifier() = default;

bool FunctionVerifier::run()
{
    if (!m_function.hasBody())
        return true;

    using Stage = void (FunctionVerifier::*)();
    static constexpr Stage stages[] = {
        &FunctionVerifier::verifyStructure,
        &FunctionVerifier::verifyEntries,
        &FunctionVerifier::verifyEdges,
        &FunctionVerifier::verifyTypes,
        &FunctionVerifier::verifyDominance,
        &FunctionVerifier::verifyGuards,
        &FunctionVerifier::verifyOperations,
    };
    for (Stage stage : stages) {
        (this->*stage)();
        if (hasFailed())
            return false;
    }
    return true;
}

bool FunctionVerifier::runStructure()
{
    if (!m_function.hasBody())
        return true;
    verifyStructure();
    if (!hasFailed())
        verifyEntries();
    return !hasFailed();
}

FunctionID findFunctionAsReadOfProvisional(const Program& program, const Function& function)
{
    FunctionID source = function.derivation.source;
    while (ProvisionalFunctions::contains(source))
        source = program.at(source).derivation.source;
    return findFunctionAsRead(&program, source);
}

Vector<InternalError> FunctionVerifier::takeErrors()
{
    // A provisional id means nothing to a reader of the diagnostics, and neither do the blocks and values of that body.
    if (ProvisionalFunctions::contains(m_id)) {
        FunctionID functionAsRead = findFunctionAsReadOfProvisional(m_program, m_function);
        for (InternalError& error : m_errors) {
            error.function = functionAsRead;
            error.block = { };
            error.value = { };
        }
    }
    return std::exchange(m_errors, { });
}

void FunctionVerifier::fail(InternalErrorKind kind, BlockID block, ValueID value, uint32_t count)
{
    m_errors.append(InternalError {
        .kind = kind,
        .function = m_id,
        .block = block,
        .value = value,
        .count = count,
        .location = m_hasValidStructure && isValid(value) ? m_function.locationOf(value) : noSourceLocation,
    });
}

bool FunctionVerifier::isLegalInForm(IRFormSet forms) const
{
    if (forms.contains(m_form))
        return true;
    return isOptimizing() && (forms.contains(IRForm::AsRead) || forms.contains(IRForm::Prepared));
}

bool FunctionVerifier::isValid(AccessSiteID id) const
{
    return isInFlowTable(id, m_inputs.flow, &FlowResults::numberOfAccessSites);
}

bool FunctionVerifier::isValid(AllocationSiteID id) const
{
    return isInFlowTable(id, m_inputs.flow, &FlowResults::numberOfAllocationSites);
}

bool FunctionVerifier::isValid(AllocationClassID id) const
{
    return isInFlowTable(id, m_inputs.flow, &FlowResults::numberOfAllocationClasses);
}

bool FunctionVerifier::isValid(FieldAccessID id) const
{
    return isInPlacedTable(id, m_form, m_inputs.placed, &PlacedTableSizes::fieldAccesses);
}

bool FunctionVerifier::isValid(AllocationID id) const
{
    return isInPlacedTable(id, m_form, m_inputs.placed, &PlacedTableSizes::allocations);
}

bool FunctionVerifier::isValid(RegExpRecordID id) const
{
    return isInPlacedTable(id, m_form, m_inputs.placed, &PlacedTableSizes::regExpRecords);
}

bool FunctionVerifier::isValid(GuardBitID id) const
{
    return isInPlacedTable(id, m_form, m_inputs.placed, &PlacedTableSizes::guardBits);
}

bool FunctionVerifier::isValidField(LayoutID layout, uint32_t field) const
{
    return isValid(layout) && field < m_program.at(layout).fields.size();
}

FunctionAnalyses& FunctionVerifier::analyses()
{
    RELEASE_ASSERT(m_hasValidStructure);
    if (!m_analyses)
        m_analyses.emplace(AnalysisContext { m_build, m_program, m_inputs.flow, m_inputs.effects, m_id }, m_function);
    return *m_analyses;
}

TypingContext FunctionVerifier::typing()
{
    if (!m_types)
        m_types.emplace(m_program.types);
    return { m_program, m_function, m_types->arena(), TypeLevel::Proven };
}

bool FunctionVerifier::need(InternalErrorKind kind, BlockID block, ValueID user, ValueID operand, const Type& needed)
{
    const Type& type = typeOf(operand);
    if (&type == &needed || type.isSubsetOf(needed, m_program))
        return true;
    fail(kind, block, user, operand.index());
    return false;
}

void FunctionVerifier::verifyStructure()
{
    BitVector isListed(m_function.values.size());
    for (unsigned blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        verifyBlockLists(block, isListed);
        if (hasFailed())
            return;

        const Block& contents = m_function.at(block);
        for (std::span<const ValueID> list : { contents.parameters.span(), contents.values.span() }) {
            for (ValueID id : list) {
                const Value& value = m_function.at(id);
                if (static_cast<unsigned>(value.opcode) >= numberOfEnumerators<Opcode>) {
                    fail(InternalErrorKind::OpcodeNotInForm, block, id);
                    return;
                }
                verifyInstructionIDs(block, id, value);
                if (hasFailed())
                    return;
            }
        }

        if (static_cast<unsigned>(contents.terminator.kind) >= numberOfEnumerators<TerminatorKind>) {
            fail(InternalErrorKind::OpcodeNotInForm, block);
            return;
        }
        verifyInstructionIDs(block, { }, contents.terminator);
        contents.forEachTarget([&](const Target& target) {
            if (!hasFailed())
                verifyTargetIDs(block, target);
        });
        if (!hasFailed() && contents.subject && !isValid(contents.subject))
            fail(InternalErrorKind::IdOutOfRange, block, { }, m_id.index());
        if (hasFailed())
            return;
    }

    verifyInlineFrames();
    if (hasFailed())
        return;

    if (m_form == IRForm::AsRead && !isOptimizing()) {
        verifyDetachedValues(isListed);
        if (hasFailed())
            return;
        verifyBlockOrder();
        if (hasFailed())
            return;
        verifyDefinitionsPrecedeUses();
        if (hasFailed())
            return;
    }
    m_hasValidStructure = true;
}

void FunctionVerifier::verifyBlockLists(BlockID block, BitVector& isListed)
{
    auto verifyList = [&](std::span<const ValueID> list, bool isParameterList) {
        for (ValueID id : list) {
            if (!isValid(id)) {
                fail(InternalErrorKind::IdOutOfRange, block, { }, m_id.index());
                return;
            }
            if (isListed.quickSet(id.index())) {
                fail(InternalErrorKind::ValueListedTwice, block, id);
                return;
            }
            if ((m_function.at(id).opcode == Opcode::Parameter) != isParameterList) {
                fail(InternalErrorKind::ParameterNotInList, block, id);
                return;
            }
        }
    };

    const Block& contents = m_function.at(block);
    verifyList(contents.parameters.span(), true);
    if (!hasFailed())
        verifyList(contents.values.span(), false);
}

void FunctionVerifier::verifyInstructionIDs(BlockID block, ValueID id, const Instruction& instruction)
{
    for (ValueID operand : instruction.operands()) {
        if (operand && !isValid(operand)) {
            fail(InternalErrorKind::IdOutOfRange, block, id, m_id.index());
            return;
        }
    }
    if (!isValidInlineFrame(instruction.inlineFrame))
        fail(InternalErrorKind::InvalidInlineFrame, block, id, instruction.inlineFrame);
}

void FunctionVerifier::verifyTargetIDs(BlockID block, const Target& target)
{
    if (!isValid(target.block)) {
        fail(InternalErrorKind::IdOutOfRange, block, { }, m_id.index());
        return;
    }
    if (target.block == entryBlock) {
        fail(InternalErrorKind::EdgeToEntryBlock, block);
        return;
    }
    for (ValueID argument : target.arguments) {
        if (!isValid(argument)) {
            fail(InternalErrorKind::IdOutOfRange, block, { }, m_id.index());
            return;
        }
    }
}

void FunctionVerifier::verifyInlineFrames()
{
    for (uint32_t index = 1; index < m_function.inlineFrames.size(); ++index) {
        const InlineFrame& frame = m_function.inlineFrames[index];
        bool isValidFrame = frame.parent < index
            && isValid(frame.callee)
            && frame.module == m_program.at(frame.callee).module;
        if (!isValidFrame) {
            fail(InternalErrorKind::InvalidInlineFrame, { }, { }, index);
            return;
        }
    }
}

void FunctionVerifier::verifyDetachedValues(const BitVector& isListed)
{
    size_t firstDetachedValue = isListed.findBit(0, false);
    if (firstDetachedValue < m_function.values.size())
        fail(InternalErrorKind::ValueNotListed, { }, ValueID(firstDetachedValue));
}

void FunctionVerifier::verifyBlockOrder()
{
    BitVector hasEarlierPredecessor(m_function.blocks.size());
    for (unsigned index = 0; index < m_function.blocks.size(); ++index) {
        BlockID block(index);
        if (block != entryBlock && !hasEarlierPredecessor.quickGet(index)) {
            fail(InternalErrorKind::BlockOrderInvalid, block);
            return;
        }
        m_function.at(block).forEachTarget([&](const Target& target) {
            if (target.block.index() > index)
                hasEarlierPredecessor.quickSet(target.block.index());
        });
    }
}

// Typing runs before dominance is verified and follows operands, so it needs a body without cycles of operands.
void FunctionVerifier::verifyDefinitionsPrecedeUses()
{
    BitVector isDefined(m_function.values.size());
    for (unsigned blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const Block& contents = m_function.at(block);
        auto verifyUse = [&](ValueID user, ValueID operand) {
            if (!hasFailed() && !isDefined.quickGet(operand.index()))
                fail(InternalErrorKind::UseNotDominated, block, user ? user : operand, operand.index());
        };

        for (ValueID parameter : contents.parameters)
            isDefined.quickSet(parameter.index());
        for (ValueID id : contents.values) {
            m_function.at(id).forEachChild([&](ValueID operand) {
                verifyUse(id, operand);
            });
            isDefined.quickSet(id.index());
        }
        contents.terminator.forEachOperand([&](ValueID operand, uint32_t) {
            verifyUse({ }, operand);
        });
        if (hasFailed())
            return;
    }
}

void FunctionVerifier::verifyEntries()
{
    for (unsigned blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const Block& contents = m_function.at(block);
        for (std::span<const ValueID> list : { contents.parameters.span(), contents.values.span() }) {
            for (ValueID id : list) {
                const Value& value = m_function.at(id);
                verifyEntry(*this, block, id, value, value.opcode);
                if (!hasFailed())
                    verifyFields(*this, block, id, value);
                // The result rule of a call in a form with several results asserts that the overload has that form.
                if (!hasFailed() && value.opcode == Opcode::CallBuiltin && !overloadHasForm(m_program.builtins(), value.overload(), value.builtinCallForm()))
                    fail(InternalErrorKind::InvalidBuiltinCall, block, id);
                if (!hasFailed() && !m_inputs.lacksEngineFields && Value::opcodesWithReadOrigin.contains(value.opcode) && !isValidInlineFrame(value.readOrigin().inlineFrame))
                    fail(InternalErrorKind::PayloadOutOfRange, block, id, value.readOrigin().inlineFrame);
                if (hasFailed())
                    return;
            }
        }

        const Terminator& terminator = contents.terminator;
        verifyEntry(*this, block, { }, terminator, terminator.kind);
        if (!hasFailed())
            verifyFields(*this, block, terminator);
        if (!hasFailed())
            verifyTargetCount(block, terminator);
        if (hasFailed())
            return;
    }
}

void FunctionVerifier::verifyTargetCount(BlockID block, const Terminator& terminator)
{
    size_t numberOfFixedTargets = std::ranges::count(targetInfosOf(terminator.kind), false, &TargetInfo::isVariadic);
    size_t numberOfTargets = terminator.targets.size();
    if (numberOfTargets != numberOfFixedTargets + terminator.numberOfCases())
        fail(InternalErrorKind::TargetCountMismatch, block, { }, static_cast<uint32_t>(numberOfTargets));
}

Vector<InternalError> findErrorsInFunction(const Program& program, FunctionID id, const Function& function, IRForm form, BuildContext& build, const VerifierInputs& inputs)
{
    FunctionVerifier verifier(program, id, function, form, build, inputs);
    if (verifier.run())
        return { };
    return verifier.takeErrors();
}

bool verifyFunction(const Program& program, FunctionID id, const Function& function, IRForm form, BuildContext& build, const VerifierInputs& inputs)
{
    Vector<InternalError> errors = findErrorsInFunction(program, id, function, form, build, inputs);
    if (errors.isEmpty())
        return true;
    build.diagnostics().append(WTF::move(errors));
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
