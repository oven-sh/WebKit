/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A CELL OF AN ENVIRONMENT THAT NOTHING HOLDS ANY MORE IS THE NEXT CELL OF ITS LAYOUT: PlannedLayout::isEscapedOffset.
// Nothing is proven. Whatever lets a cell of the heap, a saved frame or a caller hold such an environment sets a byte in it first, in every unit. A direct callee answers for what it keeps.
// Where the body as written has done with a cell that the unit has made, a ReleaseEnvironment is proposed. The sweep keeps the last on each way behind which no name of the cell lives, and its code asks the byte.
// At a suspension: each body of a coroutine marks at its entry what it is handed, and goes on holding what it has made itself. At a throw: nothing is handed back. Withheld: never, the plan gives the byte.

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

// Walker::build() shares and hoists only operations that have a result, and neither leaves the walk anywhere else.
static_assert(resultRuleOf(Opcode::MarkEnvironmentEscaped) == ResultRule::None && resultRuleOf(Opcode::ReleaseEnvironment) == ResultRule::None);
static_assert(!mayThrow(Opcode::MarkEnvironmentEscaped) && !mayThrow(Opcode::ReleaseEnvironment) && !maySuspend(Opcode::MarkEnvironmentEscaped) && !maySuspend(Opcode::ReleaseEnvironment));

// recordHasOperandKind() of ir/AOTOperandKinds.cpp lets an environment pass for no other kind, and Walker::build() asks it of every operand in every build.
static bool kindAdmitsEnvironmentInEnvironmentCells(OperandKind kind)
{
    return kind == OperandKind::Layout || kind == OperandKind::Declared || kind == OperandKind::Results || kind == OperandKind::Environment || kind == OperandKind::EvalOperand || kind == OperandKind::Convertible;
}

// What gives its operand back under another name gives it the type of an environment, by which the sweep follows it. The type of several results names none of them.
static bool looksAtUsesOfOperandsInEnvironmentCells(Opcode opcode)
{
    return !maySuspend(opcode) && opcode != Opcode::Results;
}

// By the rows alone. Every other operation is passed by without a look at an operand.
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

// Frames' flag wins over the byte: only a cell of the heap is handed back.
static bool cellsAreHandedBackInEnvironmentCells(const Plan& plan, LayoutID layout)
{
    const PlannedLayout& planned = plan.layout(layout);
    return planned.isEscapedOffset && !planned.liesOnlyInFrames;
}

// Behind the names that tests have given it and the forms that it was brought into. Null: the reference names no record that lies in a block.
static ValueID recordBehindNamesInEnvironmentCells(const Code& code, Reference reference)
{
    if (!reference || !reference.isValue())
        return { };
    ValueID root = rootOfCellInSweptCode(code, reference.id());
    return code.isConstant(root) ? ValueID() : root;
}

// Null: the value is no environment, or cells of its layout are not handed back.
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

// The NewObject. Null: the reference is not known to name a cell that this unit has made in the heap, or cells of the layout are not handed back.
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

// ---- The byte

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

// In front of the conversions of the operands: a conversion is no name that the type of an environment follows.
void markEnvironmentsThatOperationKeeps(Walker& walker, const Draft& draft)
{
    Opcode opcode = draft.opcode;
    const Plan& plan = walker.plan();
    if (opcode == Opcode::ResultAt) {
        Reference results = draft.resultsOperand();
        if (!results.isValue())
            return;
        const Operation& call = walker.code().at(results.id());
        if (call.opcode == Opcode::CallDirect && plan.conventions().handsBackPartsAtEveryReturn(call))
            walker.environmentCellsOfUnit().hasMadeSome = true;
        return;
    }
    // No cell is made of it: whoever calls it takes it apart, and names what it lists.
    if (opcode == Opcode::NewClosure && draft.closureForm() == ClosureForm::Deferred)
        return;

    // The stub of a call may copy a closure that is lent to the heap, and the copy holds what the closure holds.
    // The closures that an object makes on demand are cells of the heap wherever the object lies, and they hold the environment that the object keeps for them.
    bool mayHoldWithoutKeeping = opcode == Opcode::NewClosure ? !draft.isLent() : opcode == Opcode::NewObject && walker.program().at(draft.layout()).isEnvironment();
    // A closure over what a caller has lent lies in the frame for as long as something names it. It may come to a join, and the sweep follows the record of a cell of the frame and no other name of it.
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
        // A saved frame holds whatever lives across the suspension. That of the function that has made the cell goes on in that function, which hands the cell back no earlier than it has done with it.
        // A callee that is entered and awaited in one answers for what it is handed, like that of a CallDirect.
        if (maySuspend(opcode) && cellMadeByUnitInEnvironmentCells(walker, operand))
            continue;
        // What the unit has made and hands back among its results is the caller's from there on. Whatever else it lists there somebody further up may have, so the byte says which of the two has arrived.
        if (opcode == Opcode::Results && cellMadeByUnitInEnvironmentCells(walker, operand))
            continue;
        // The plan hands back no cells of a layout that some written position takes "or nothing", and only through such a position does an environment meet nothing.
        if (mayBeNothingInEnvironmentCells(walker, operand.id())) {
            walker.fail("what may be no environment is kept, and nobody can tell whether it has a byte to set"_s);
            return;
        }
        if (!buildMarkInEnvironmentCells(walker, operand, draft.origin()))
            return;
    }
}

// The sweep finds what lives across a suspension, behind the walk, and a saved frame is a cell of the heap. Whoever has made what the body is handed goes on while the body is suspended, and comes to his release.
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

// ---- Handing back

// The plan gives the byte to the scopes that a body makes outside loops, handlers and coroutines. Such a scope is done with where the function returns: to know that takes no look at what lives where.
// A PROPOSAL IS NO PROOF THAT NOBODY HOLDS THE CELL: the byte says that at run time, and the sweep gives its mark to no release behind which a name of the cell lives.
// BUT A PROPOSAL NAMES A RECORD, AND THE RECORD MUST STAND ABOVE IT. At a return the scope is mostly dead by the liveness of the body, and what is dead is bound to whatever the way that was walked last has left:
// an edge brings a record of its own only for what is live. So: THE ALLOCATION WAS DRAFTED ONCE IN THIS COPY OF THE BODY. Every way from the entry to a block below the allocation as written
// comes by that allocation, so by that record. The arms of a guess walk a stretch of the body once each: then there are two, and nothing is proposed.
void noteEnvironmentThatUnitMakes(Walker& walker, const Draft& allocation)
{
    Walker::EnvironmentCellsOfUnit& cells = walker.environmentCellsOfUnit();
    cells.hasMadeSome = true;
    cells.mayNameSome = true;
    // What stands for no value of the body is bound to no name, so nobody proposes it.
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
    // The plan has looked at the body as written. In a copy the body stands where its caller has it: in a loop of the caller each turn makes its own, and hands it back at the return of the copy.
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
    // What is built here notes no name.
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

// Every unit whose frame a suspension saves: the closed body of a coroutine that has two bodies is one as well.
void markEnvironmentsThatCoroutineIsHanded(Walker& walker)
{
    if (!walker.isUnreachable() && isBodyOfCoroutine(walker.unit().role, walker.program().at(walker.unit().function).isCoroutine()))
        markWhatCoroutineIsHandedInEnvironmentCells(walker);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
