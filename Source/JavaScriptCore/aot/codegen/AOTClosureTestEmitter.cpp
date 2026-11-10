/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosureTestEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTClosureWord.h"
#include "AOTEntryEmitter.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSlimClosure.h"
#include "AOTSort.h"
#include "AOTStubsClosures.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "JSCell.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(JSObject::butterflyOffset() == offsetOfSlimClosureWord);
static_assert(minCell32 > 1);
static_assert(ClosureWord::functionMask < closureWordMarker && closureWordMarker + ClosureWord::functionMask < compiledObjectShapeBit);
static_assert(ClosureWord::functionShift == 32, "The number of the function is the lowest field of the upper half, so the other fields are an addend to it.");
static_assert(!(ClosureWord::encodeUpperHalf(0, 0, { .isNotCallable = true, .takesNewTarget = true }) & ClosureWord::functionMask));

ClosureTestEmitter::ClosureTestEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

FunctionKindData ClosureTestEmitter::closureKindOf(FunctionID function) const
{
    return m_plan.functionKind(m_program, conventions().closureEntryOf(function));
}

bool ClosureTestEmitter::closureWordNames(FunctionID function) const
{
    return hasOneInstance() && closureKindOf(function).isSlim && conventions().exists(conventions().closureEntryOf(function));
}

void ClosureTestEmitter::branchOnIsChangedClosureWithNumber(CellPointer cell, bool isSlim, uint32_t mentionOfClosureNumber, BranchTargets targets)
{
    LValue operand = cell.value();
    StubKey stub = changedClosureHasNumber(isSlim ? ChangedClosure::Slim : ChangedClosure::Full);
    emitter<RuntimeCallEmitter>().callStubWithoutSiteAndBranch(stub, singleElementSpan(operand), { .numberedImmediate = NumberedImmediate { mentionOfClosureNumber } }, targets);
}

Word32 ClosureTestEmitter::numberOfChangedClosure(CellPointer cell)
{
    LValue operand = cell.value();
    return Word32(m_out.castToInt32(emitter<RuntimeCallEmitter>().callStubWithoutSite(AOT::numberOfChangedClosure(), singleElementSpan(operand)).first));
}

Word32 ClosureTestEmitter::upperHalfOfWordAtSlimClosureWord(CellPointer cell)
{
    return Word32(m_out.load32(m_out.address(cell.value(), m_heaps.AOTSlimClosure_word, sizeof(uint32_t))));
}

Condition ClosureTestEmitter::isClosureOf(CellPointer cell, FunctionID function)
{
    return asBoolean([&](BranchTargets targets) {
        branchOnClosureTest(cell, function, targets);
    });
}

void ClosureTestEmitter::branchOnIsClosureOf(CellPointer cell, FunctionID function, BranchTargets targets)
{
    branchOnClosureTest(cell, function, targets);
}

void ClosureTestEmitter::branchOnClosureTest(CellPointer cell, FunctionID function, BranchTargets targets)
{
    if (!conventions().exists(conventions().closureEntryOf(function))) {
        m_out.jump(targets.notTaken.block());
        return;
    }
    bool isSlim = closureKindOf(function).isSlim;

    LBasicBlock hasOtherStructure = newBlock();
    if (closureWordNames(function)) {
        Word32 upperHalf = upperHalfOfWordAtSlimClosureWord(cell);
        LBasicBlock hasOtherWord = newBlock();
        m_out.branch(equal(upperHalf, upperHalfOfClosureWordOf(function)).value(), targets.taken, normalEdge(hasOtherWord));

        m_out.appendToContinuation(hasOtherWord);
        m_out.branch(hasMutableCellType(cell, SlimClosureWithPropertiesType).value(), rareEdge(hasOtherStructure), targets.notTaken);

        m_out.appendToContinuation(hasOtherStructure);
    } else {
        LBasicBlock hasInitialStructure = newBlock();
        m_out.branch(hasClosureStructure(cell, function).value(), normalEdge(hasInitialStructure), rareEdge(hasOtherStructure));

        m_out.appendTo(hasInitialStructure);
        branch(hasClosureWordOf(cell, isSlim, function), targets);

        m_out.appendTo(hasOtherStructure);
    }
    branchOnIsChangedClosureWithNumber(cell, isSlim, m_mentions.add(NumberSpace::ClosureFunction, function), targets);
}

void ClosureTestEmitter::branchOnChangedClosure(CellPointer cell, std::span<const ClosureCase> cases, BlockEdge otherwise)
{
    LBasicBlock isChanged = newBlock();
    m_out.branch(hasMutableCellType(cell, SlimClosureWithPropertiesType).value(), rareEdge(isChanged), otherwise);

    m_out.appendTo(isChanged);
    Word32 number = numberOfChangedClosure(cell);
    for (const ClosureCase& closureCase : cases) {
        LBasicBlock next = newBlock();
        m_out.branch(equal(number, closureNumberOf(closureCase.function)).value(), closureCase.target, normalEdge(next));
        m_out.appendToContinuation(next);
    }
    m_out.jump(otherwise);
}

Condition ClosureTestEmitter::isClosureOfAny(CellPointer cell, std::span<const FunctionID> functions)
{
    return asBoolean([&](BranchTargets targets) {
        branchOnIsClosureOfAny(cell, functions, targets);
    });
}

void ClosureTestEmitter::branchOnClosureNumberInRuns(Word32 closureNumber, std::span<const ClosureNumberRun> ascendingRuns, int32_t addend, bool mayLieBelowFirstRun, BranchTargets targets)
{
    static constexpr size_t maxRunsTestedInOrder = 3;
    if (ascendingRuns.size() > maxRunsTestedInOrder) {
        size_t middle = ascendingRuns.size() / 2;
        LBasicBlock lower = newBlock();
        LBasicBlock upper = newBlock();
        m_out.branch(below(closureNumber, closureNumberOf(ascendingRuns[middle].first, addend)).value(), normalEdge(lower), normalEdge(upper));
        m_out.appendTo(lower);
        branchOnClosureNumberInRuns(closureNumber, ascendingRuns.first(middle), addend, mayLieBelowFirstRun, targets);
        m_out.appendTo(upper);
        branchOnClosureNumberInRuns(closureNumber, ascendingRuns.subspan(middle), addend, false, targets);
        return;
    }

    auto takeIf = [&](Condition isInRun, bool isLastTest) {
        if (isLastTest) {
            branch(isInRun, targets);
            return;
        }
        LBasicBlock nextRun = newBlock();
        m_out.branch(isInRun.value(), targets.taken, normalEdge(nextRun));
        m_out.appendToContinuation(nextRun);
    };
    for (size_t position = 0; position < ascendingRuns.size(); ++position) {
        const ClosureNumberRun& run = ascendingRuns[position];
        bool isLastRun = position + 1 == ascendingRuns.size();
        if (run.first == run.last) {
            takeIf(equal(closureNumber, closureNumberOf(run.first, addend)), isLastRun);
            continue;
        }
        if (position || mayLieBelowFirstRun) {
            LBasicBlock isNotBelowRun = newBlock();
            m_out.branch(below(closureNumber, closureNumberOf(run.first, addend)).value(), targets.notTaken, normalEdge(isNotBelowRun));
            m_out.appendToContinuation(isNotBelowRun);
        }
        takeIf(belowOrEqual(closureNumber, closureNumberOf(run.last, addend + AOT::hasFlag(build().options().injectedFaults, Wire::FaultInjection::OverstateClosureNumberRuns))), isLastRun);
    }
}

void ClosureTestEmitter::branchOnIsClosureOfAny(CellPointer cell, std::span<const FunctionID> functions, BranchTargets targets)
{
    struct InitialStructure {
        bool isSlim;
        ptrdiff_t offsetOfHeader;
    };
    Vector<FunctionID, 8> members;
    Vector<InitialStructure, 4> structures;
    bool hasSlimMember = false;
    bool hasFullMember = false;
    for (FunctionID function : functions) {
        if (!conventions().exists(conventions().closureEntryOf(function)))
            continue;
        members.append(function);
        FunctionKindData kind = closureKindOf(function);
        (kind.isSlim ? hasSlimMember : hasFullMember) = true;
        ptrdiff_t offsetOfHeader = offsetOfClosureHeader(kind);
        bool isListed = structures.containsIf([&](const InitialStructure& listed) {
            return listed.offsetOfHeader == offsetOfHeader;
        });
        if (!isListed)
            structures.append({ !!kind.isSlim, offsetOfHeader });
    }
    if (members.isEmpty()) {
        m_out.jump(targets.notTaken.block());
        return;
    }

    AOT::sort(members);
    Vector<ClosureNumberRun, 4> runs;
    for (FunctionID member : members) {
        if (!runs.isEmpty() && (runs.last().last == member || conventions().haveNeighboringClosureNumbers(runs.last().last, member)))
            runs.last().last = member;
        else
            runs.append({ member, member });
    }
    if (runs.size() == 1 && runs[0].first == runs[0].last) {
        branchOnClosureTest(cell, runs[0].first, targets);
        return;
    }

    bool markerTellsSlimClosures = hasSlimMember && hasOneInstance();
    if (markerTellsSlimClosures) {
        structures.removeAllMatching([](const InitialStructure& structure) {
            return structure.isSlim;
        });
    }
    size_t formsAsCreatedLeft = markerTellsSlimClosures + structures.size();

    Join<Word32> closureNumber(m_out);
    auto closureNumberIn = [&](const Machine::AbstractHeap& word) {
        Word32 upperHalf(m_out.load32(m_out.address(cell.value(), word, sizeof(uint32_t))));
        return bitAnd(upperHalf, word32(ClosureWord::functionMask));
    };
    auto addForm = [&](Condition hasForm, BlockEdge otherForms, const auto& loadClosureNumber) {
        LBasicBlock form = newBlock();
        ScopeFork fork = m_out.forkScope();
        m_out.branch(hasForm.value(), normalEdge(form), otherForms);
        m_out.appendToArm(form, fork);
        closureNumber.add(loadClosureNumber());
        m_out.appendToArm(otherForms.block(), fork);
    };
    auto edgeBehindFormAsCreated = [&] {
        return --formsAsCreatedLeft ? normalEdge(newBlock()) : unlikelyEdge(newBlock());
    };

    if (markerTellsSlimClosures) {
        Word32 upperHalf = upperHalfOfWordAtSlimClosureWord(cell);
        Condition isAsCreated = equal(bitAnd(upperHalf, word32(static_cast<int32_t>(closureWordMarker | compiledObjectShapeBit))), word32(closureWordMarker));
        addForm(isAsCreated, edgeBehindFormAsCreated(), [&] {
            return bitAnd(upperHalf, word32(ClosureWord::functionMask));
        });
    }
    if (!structures.isEmpty()) {
        Word32 structureID = loadStructureID(cell);
        for (const InitialStructure& structure : structures) {
            Condition hasStructure = equal(structureID, loadImmutable32(structure.offsetOfHeader));
            addForm(hasStructure, edgeBehindFormAsCreated(), [&] {
                return closureNumberIn(structure.isSlim ? m_heaps.AOTSlimClosure_word : m_heaps.AOTClosure_entry);
            });
        }
    }
    LBasicBlock isChangedClosure = newBlock();
    if (hasFullMember) {
        LBasicBlock isNoFunctionObject = newBlock();
        m_out.branch(hasCellType(cell, CompiledClosureType).value(), rareEdge(isChangedClosure), normalEdge(isNoFunctionObject));
        m_out.appendToContinuation(isNoFunctionObject);
    }
    if (hasSlimMember)
        m_out.branch(hasMutableCellType(cell, SlimClosureWithPropertiesType).value(), rareEdge(isChangedClosure), targets.notTaken);
    else
        m_out.jump(targets.notTaken);

    m_out.appendTo(isChangedClosure);
    closureNumber.add(numberOfChangedClosure(cell));
    build().statistics().count(StatisticID::ClosureChecksByNumberRanges);
    branchOnClosureNumberInRuns(closureNumber.finish(), runs.span(), 0, true, targets);
}

Word32 ClosureTestEmitter::closureNumberOf(FunctionID function, int32_t addend)
{
    return numberOf(m_mentions.add(NumberSpace::ClosureFunction, function, addend));
}

uint32_t ClosureTestEmitter::mentionOfUpperHalfOfClosureWordOf(FunctionID function)
{
    EntryDescription entry = describeEntry(inputs(), conventions().closureEntryOf(function));
    ClosureWord::Flags flags = closureWordFlags(closureKindOf(function));
    flags.borrowedArguments = entry.borrowedArguments;
    flags.endsItsTailCalls = entry.endsItsTailCalls;
    return m_mentions.add(NumberSpace::ClosureFunction, function, static_cast<int32_t>(ClosureWord::encodeUpperHalf(entry.numberOfParameters, 0, flags)));
}

Word32 ClosureTestEmitter::upperHalfOfClosureWordOf(FunctionID function)
{
    return numberOf(mentionOfUpperHalfOfClosureWordOf(function));
}

std::optional<uint32_t> ClosureTestEmitter::mentionOfUpperHalfThatNames(FunctionID function)
{
    if (!conventions().exists(conventions().closureEntryOf(function)))
        return std::nullopt;
    if (!closureKindOf(function).isSlim)
        return m_mentions.add(NumberSpace::ClosureFunction, function, static_cast<int32_t>(closureWordMarker));
    if (!hasOneInstance())
        return std::nullopt;
    return mentionOfUpperHalfOfClosureWordOf(function);
}

Condition ClosureTestEmitter::hasClosureWordOf(CellPointer closure, bool isSlim, FunctionID function)
{
    const Machine::AbstractHeap& word = isSlim ? m_heaps.AOTSlimClosure_word : m_heaps.AOTClosure_entry;
    Word32 upperHalf(m_out.load32(m_out.address(closure.value(), word, sizeof(uint32_t))));
    return equal(upperHalf, upperHalfOfClosureWordOf(function));
}

Condition ClosureTestEmitter::isSlimClosureAsCreated(CellPointer cell)
{
    Word32 topByte(m_out.load8SignExt32(m_out.address(cell.value(), m_heaps.AOTSlimClosure_word, ClosureWord::topByteShift / 8)));
    return greaterThanOrEqual(topByte, word32(static_cast<uint32_t>(ClosureWord::minTopByteOfCalleeTaking(ClosureWord::maxNumberOfParameters))));
}

Condition ClosureTestEmitter::hasFunctionCellType(CellPointer cell)
{
    Condition isFunction = hasCellTypeIn(cell, functionTypes);
    return logicalOr(isFunction, hasCellTypeIn(cell, slimClosureTypes));
}

CellPointer ClosureTestEmitter::closureIfCreated(FunctionID function)
{
    if (m_plan.usesWideReferences())
        return CellPointer(m_out.load64(closureCellOf(function)));
    Word32 entry = closureTableEntry(function);
    Condition isNotCreated = isZero(entry);
    CellPointer none(m_out.constInt64(0));
    return select(isNotCreated, none, closureInTableEntry(entry));
}

CellPointer ClosureTestEmitter::closureInTableEntry(Word32 entry)
{
    return decode(NarrowReference(add(entry, 1).value()));
}

Word32 ClosureTestEmitter::closureTableEntry(FunctionID function)
{
    RELEASE_ASSERT(!m_plan.usesWideReferences());
    return Word32(m_out.load32(closureCellOf(function)));
}

TypedPointer ClosureTestEmitter::closureCellOf(FunctionID function)
{
    RELEASE_ASSERT(m_plan.hasClosureCell(function));
    Pointer table = loadImmutablePointer(AOT_INSTANCE_OFFSET(closureCells));
    Word32 number = numberOf(m_mentions.add(NumberSpace::ClosureCell, function));
    return m_out.baseIndex(m_heaps.AOTInstance_closureCells, table.value(), m_out.zeroExtPtr(number.value()), m_plan.usesWideReferences() ? ScaleEight : ScaleFour);
}

ptrdiff_t ClosureTestEmitter::offsetOfClosureHeader(FunctionKindData kind) const
{
    return kind.isSlim
        ? AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind.slimClosureKind() * sizeof(uint64_t)
        : AOT_INSTANCE_OFFSET(closures.closureHeaders) + kind.closureStructureOfKind() * sizeof(uint64_t);
}

Condition ClosureTestEmitter::hasClosureStructure(CellPointer cell, FunctionID function)
{
    Word32 structureID = loadStructureID(cell);
    return equal(structureID, loadImmutable32(offsetOfClosureHeader(closureKindOf(function))));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
