/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTCodeRole.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesThatLeaveCellToItsFramesInEnvironmentCells { OperandUse::Inspects, OperandUse::Returns };
static constexpr OperandUses usesOfCellThatResultHoldsInEnvironmentCells { OperandUse::Inspects, OperandUse::Returns, OperandUse::Retains };

static_assert(resultRuleOf(Opcode::MarkEnvironmentEscaped) == ResultRule::None && resultRuleOf(Opcode::ReleaseEnvironment) == ResultRule::None);
static_assert(!mayThrow(Opcode::MarkEnvironmentEscaped) && !mayThrow(Opcode::ReleaseEnvironment) && !maySuspend(Opcode::MarkEnvironmentEscaped) && !maySuspend(Opcode::ReleaseEnvironment));

static bool kindAdmitsEnvironmentInEnvironmentCells(OperandKind kind)
{
    return kind == OperandKind::Layout || kind == OperandKind::Declared || kind == OperandKind::Results || kind == OperandKind::Environment || kind == OperandKind::EvalOperand || kind == OperandKind::Convertible;
}

static bool looksAtUsesOfOperandsInEnvironmentCells(Opcode opcode)
{
    return !maySuspend(opcode) && opcode != Opcode::Results;
}

OpcodeSet operationsThatMayKeepEnvironments()
{
    return opcodesWhere([](Opcode opcode) {
        if (opcode == Opcode::CallDirect)
            return false;
        if (opcode == Opcode::NewObject || opcode == Opcode::ResultAt)
            return true;
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode);
        for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
            const OperandInfo& info = operandInfoOf(opcode, operandIndex);
            if (!kindAdmitsEnvironmentInEnvironmentCells(info.kind))
                continue;
            if (!looksAtUsesOfOperandsInEnvironmentCells(opcode) || !info.protocols.isEmpty() || !info.uses.containsOnly(usesThatLeaveCellToItsFramesInEnvironmentCells))
                return true;
        }
        return false;
    });
}

static bool cellsAreHandedBackInEnvironmentCells(const Plan& plan, LayoutID layout)
{
    const PlannedLayout& planned = plan.layout(layout);
    return planned.isEscapedOffset && !planned.liesOnlyInFrames;
}

static ValueID recordBehindNamesInEnvironmentCells(const Code& code, Reference reference)
{
    if (!reference || !reference.isValue())
        return { };
    ValueID root = rootOfCellInSweptCode(code, reference.id());
    return code.isConstant(root) ? ValueID() : root;
}

static LayoutID layoutHandedBackInEnvironmentCells(Walker& walker, ValueID record)
{
    TypeID type = walker.code().at(record).type;
    if (!type || holdsOnlyPrimitives(type))
        return { };
    LayoutID environment = walker.types().at(type).environment;
    return environment && cellsAreHandedBackInEnvironmentCells(walker.plan(), environment) ? environment : LayoutID();
}

static bool mayBeNothingInEnvironmentCells(Walker& walker, ValueID record)
{
    return walker.types().at(walker.code().at(record).type).primitives;
}

static ValueID cellMadeByUnitInEnvironmentCells(Walker& walker, Reference reference)
{
    ValueID root = recordBehindNamesInEnvironmentCells(walker.code(), reference);
    if (!root)
        return { };
    const Operation& made = walker.code().at(root);
    if (made.opcode != Opcode::NewObject || made.storage() != AllocationStorage::Heap || !cellsAreHandedBackInEnvironmentCells(walker.plan(), made.layout()))
        return { };
    return root;
}

static bool buildMarkInEnvironmentCells(Walker& walker, Reference environment, Origin origin)
{
    origin.isTailCall = false;
    Draft mark(Opcode::MarkEnvironmentEscaped, origin);
    mark.setEnvironmentOperand(environment);
    walker.build(WTF::move(mark));
    return !walker.isUnreachable();
}

bool isHandedEnvironmentWithByte(const Walker& walker, FunctionID function)
{
    for (const DeclaredCapture& capture : walker.program().at(function).captures) {
        TypeID written = capture.written();
        LayoutID environment = written && !holdsOnlyPrimitives(written) ? walker.program().at(written).environment : LayoutID();
        if (environment && cellsAreHandedBackInEnvironmentCells(walker.plan(), environment))
            return true;
    }
    return false;
}

void markEnvironmentsThatOperationKeeps(Walker& walker, const Draft& draft)
{
    Opcode opcode = draft.opcode;
    if (opcode == Opcode::ResultAt)
        return;
    if (opcode == Opcode::NewClosure && draft.closureForm() == ClosureForm::Deferred)
        return;

    bool mayHoldWithoutKeeping = opcode == Opcode::NewClosure ? !draft.isLent() : opcode == Opcode::NewObject && walker.program().at(draft.layout()).isEnvironment();
    bool resultLiesInFrame = mayHoldWithoutKeeping && draft.storage() == AllocationStorage::Frame && draft.isMadeForOneUse();
    bool looksAtUsesOfOperands = looksAtUsesOfOperandsInEnvironmentCells(opcode);
    uint32_t indexOfList = numberOfNamedOperandsOf(opcode);
    std::span<const Reference> operands = draft.operands();
    for (uint32_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex) {
        Reference operand = operands[operandIndex];
        if (!operand || !operand.isValue())
            continue;
        if (looksAtUsesOfOperands) {
            const OperandInfo& info = operandInfoOf(opcode, std::min(operandIndex, indexOfList));
            if (info.protocols.isEmpty() && info.uses.containsOnly(usesThatLeaveCellToItsFramesInEnvironmentCells))
                continue;
            if (resultLiesInFrame && info.protocols.isEmpty() && info.retainedIn == RetainedIn::Result && info.uses.containsOnly(usesOfCellThatResultHoldsInEnvironmentCells))
                continue;
        }
        if (!layoutHandedBackInEnvironmentCells(walker, operand.id()))
            continue;
        if (maySuspend(opcode) && cellMadeByUnitInEnvironmentCells(walker, operand))
            continue;
        if (opcode == Opcode::Results && cellMadeByUnitInEnvironmentCells(walker, operand))
            continue;
        if (mayBeNothingInEnvironmentCells(walker, operand.id())) {
            walker.fail("what may be no environment is kept, and nobody can tell whether it has a byte to set"_s);
            return;
        }
        if (opcode == Opcode::NewClosure)
            continue;
        if (!buildMarkInEnvironmentCells(walker, operand, draft.origin()))
            return;
    }
}

static void markWhatCoroutineIsHandedInEnvironmentCells(Walker& walker)
{
    BodyBlock entry = walker.currentBodyIndex().block(entryBlock);
    uint32_t numberOfCaptures = static_cast<uint32_t>(walker.program().at(walker.unit().function).captures.size());
    for (uint32_t capture = 0; capture < numberOfCaptures && capture < entry.numberOfParameters(); ++capture) {
        Reference handedOver = walker.referenceBoundTo(entry.parameter(capture));
        if (!handedOver || !handedOver.isValue() || !layoutHandedBackInEnvironmentCells(walker, handedOver.id()))
            continue;
        if (mayBeNothingInEnvironmentCells(walker, handedOver.id())) {
            walker.fail("what may be no environment is kept, and nobody can tell whether it has a byte to set"_s);
            return;
        }
        if (!buildMarkInEnvironmentCells(walker, handedOver, walker.originOfOperation()))
            return;
    }
}

void noteEnvironmentThatUnitMakes(Walker& walker, const Draft& allocation)
{
    Walker::EnvironmentCellsOfUnit& cells = walker.environmentCellsOfUnit();
    cells.hasMadeSome = true;
    cells.mayNameSome = true;
    ValueID name = allocation.inputValueGiven;
    if (!name || allocation.standsForNoValueOfInput)
        return;
    const BodyIndex& input = walker.currentBodyIndex();
    uint32_t copy = allocation.origin().inlineFrame;
    for (Walker::EnvironmentCellsOfUnit::Name& noted : cells.names) {
        if (noted.body == &input && noted.copy == copy && noted.name == name) {
            noted.hasSeveralRecords = true;
            return;
        }
    }
    BlockID making = input.blockOf(name);
    if (input.loopOf(making) || input.block(making).hasHandler() || input.hasIrreducibleFlow() || walker.program().at(walker.currentFunction()).isCoroutine())
        return;
    cells.names.append({ &input, copy, name, making, false });
}

void handBackEnvironmentsAtReturn(Walker& walker, BlockID block)
{
    if (walker.isUnreachable())
        return;
    const BodyIndex& input = walker.currentBodyIndex();
    Origin origin = walker.originOfOperation();
    origin.isTailCall = false;
    const auto& names = walker.environmentCellsOfUnit().names;
    for (size_t index = 0; index < names.size(); ++index) {
        const Walker::EnvironmentCellsOfUnit::Name& name = names[index];
        if (name.body != &input || name.copy != origin.inlineFrame || name.hasSeveralRecords || !input.dominates(name.making, block))
            continue;
        ValueID record = cellMadeByUnitInEnvironmentCells(walker, walker.referenceBoundTo(name.name));
        if (!record)
            continue;
        Draft release(Opcode::ReleaseEnvironment, origin);
        release.setEnvironmentOperand(Reference::value(record));
        walker.build(WTF::move(release));
        if (walker.isUnreachable())
            return;
    }
}

void markEnvironmentsThatCoroutineIsHanded(Walker& walker)
{
    if (!walker.isUnreachable() && isBodyOfCoroutine(walker.unit().role, walker.program().at(walker.unit().function).isCoroutine()))
        markWhatCoroutineIsHandedInEnvironmentCells(walker);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
