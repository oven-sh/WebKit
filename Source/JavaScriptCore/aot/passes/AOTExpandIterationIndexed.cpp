/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTSort.h"
#include "AOTSplitStringWalk.h"
#include "AOTVector.h"
#include "JSArrayIterator.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static_assert(static_cast<uint32_t>(StepArrayIteratorResult::Done) == static_cast<uint32_t>(IteratorResumeResult::Done));
static_assert(static_cast<uint32_t>(StepArrayIteratorResult::Value) == static_cast<uint32_t>(IteratorResumeResult::Value));

static Range positionRangeOf(IterationForm form)
{
    switch (form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
        return { 0, maxExactInteger - 1 };
    case IterationForm::OwnEntries:
        // A step adds two to an index whose type only says that it is below the length.
        return { 0, maxExactInteger };
    case IterationForm::String:
        // A surrogate pair adds two to an index whose type only says that it is below the length.
        return { 0, Range::length().maximum + 1 };
    case IterationForm::SplitString:
        return positionRangeOfSplitString();
    case IterationForm::Protocol:
    case IterationForm::Map:
    case IterationForm::Set:
    case IterationForm::Generator:
    case IterationForm::AsyncGenerator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID positionTypeOf(IterationExpansion& expansion, const ArmState& state)
{
    return expansion.types().intern(Type::integer(positionRangeOf(state.arm.form)));
}

TypeID positionTypeOfStringWalk(TypeArena& types)
{
    return types.intern(Type::integer(positionRangeOf(IterationForm::String)));
}

static BlockID addSiblingBlock(IterationExpansion& expansion)
{
    IRBuilder& builder = expansion.builder();
    return builder.addBlock(expansion.function().at(builder.currentBlock()).frequency);
}

static IRBuilder::ProvenBranch branchOnInRange(IterationExpansion& expansion, ValueID position, ValueID length)
{
    IRBuilder& builder = expansion.builder();
    ValueID test = builder.append(Opcode::Less, { position, length });
    return builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Normal);
}

static void finishElementStep(IterationExpansion& expansion, ArmState& state, const IRBuilder::ProvenBranch& branch, ValueID element)
{
    IRBuilder& builder = expansion.builder();
    ValueID index = branch.provenOnTaken;
    expansion.define(*state.position, expansion.appendIncrement(index, 1));
    ValueID done = builder.appendBoolean(false);
    ValueID value = expansion.appendValueOfKind(state.arm.kind, index, element);
    expansion.finishStep(done, value);

    builder.moveToEndOf(branch.notTaken);
    expansion.finishStepAsDone();
}

void emitIndexedOpen(IterationExpansion& expansion, ArmState& state)
{
    if (state.patternRead == PatternRead::ByIndex)
        return;

    IRBuilder& builder = expansion.builder();
    // The call that makes an iterator validates the view. The @@iterator method of that iterator validates nothing.
    if (state.arm.form == IterationForm::TypedArray && !state.arm.bypassesIterator)
        builder.append(Opcode::RequireAttachedTypedArray, { state.subject });

    // A step is typed from the definition it names, before SSARepair picks the one that reaches it: every definition lies in the type of this one.
    ValueID zero = builder.appendInteger(0);
    BlockID start = addSiblingBlock(expansion);
    ValueID position = builder.addParameter(start, positionTypeOf(expansion, state));
    builder.emitJump(start, { zero });
    builder.moveToEndOf(start);
    state.position = expansion.addVariable(position);
}

void emitArrayStep(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    ValueID length = builder.append(Opcode::ToLength, { expansion.appendPropGet(state.subject, expansion.keyOf(BuiltinNames::length)) });
    IRBuilder::ProvenBranch branch = branchOnInRange(expansion, state.position->anyDefinition, length);

    ValueID element;
    if (state.arm.kind != JSC::IterationKind::Keys) {
        Value read = IRBuilder::make(Opcode::ElemGet, { state.subject, branch.provenOnTaken });
        read.setKeyIsIntegral(true);
        element = builder.append(WTF::move(read));
    }
    finishElementStep(expansion, state, branch, element);
}

void emitArrayStepByIndex(IterationExpansion& expansion, ArmState& state, uint32_t stepIndex)
{
    IRBuilder& builder = expansion.builder();
    ValueID index = builder.appendInteger(stepIndex);
    ValueID length = builder.append(Opcode::ToLength, { expansion.appendPropGet(state.subject, expansion.keyOf(BuiltinNames::length)) });
    ValueID done = builder.append(Opcode::LogicalNot, { builder.append(Opcode::Less, { index, length }) });

    Value read = IRBuilder::make(Opcode::ElemGet, { state.subject, index });
    read.setKeyIsIntegral(true);
    expansion.finishStep(done, builder.append(WTF::move(read)));
}

void emitTypedArrayStep(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    builder.append(Opcode::RequireAttachedTypedArray, { state.subject });
    Value lengthRead = IRBuilder::make(Opcode::GetTypedArrayLength, { state.subject });
    lengthRead.setTypedArrayType(state.arm.typedArrayType);
    ValueID length = builder.append(WTF::move(lengthRead));
    IRBuilder::ProvenBranch branch = branchOnInRange(expansion, state.position->anyDefinition, length);

    ValueID element;
    if (state.arm.kind != JSC::IterationKind::Keys) {
        Value read = IRBuilder::make(Opcode::GetTypedArrayElement, { state.subject, branch.provenOnTaken });
        read.setTypedArrayType(state.arm.typedArrayType);
        read.setKeyIsIntegral(true);
        element = builder.append(WTF::move(read));
    }
    finishElementStep(expansion, state, branch, element);
}

void emitStringStep(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    ValueID length = builder.append(Opcode::GetStringLength, { state.subject });
    IRBuilder::ProvenBranch branch = branchOnInRange(expansion, state.position->anyDefinition, length);
    ValueID index = branch.provenOnTaken;

    ValueID codePointAt = expansion.appendBuiltin(Builtins::StringPrototypeCodePointAt);
    ValueID codePoint = expansion.appendCall(codePointAt, state.subject, singleElementSpan(index));
    ValueID isAstral = builder.append(Opcode::Greater, { codePoint, builder.appendInteger(0xFFFF) });

    BlockID merge = addSiblingBlock(expansion);
    ValueID end = builder.addParameter(merge, positionTypeOf(expansion, state));
    IRBuilder::BranchBlocks blocks = builder.emitBranch(isAstral, BlockFrequency::Unlikely, BlockFrequency::Normal);
    builder.emitJump(merge, { expansion.appendIncrement(index, 2) });
    builder.moveToEndOf(blocks.notTaken);
    builder.emitJump(merge, { expansion.appendIncrement(index, 1) });

    builder.moveToEndOf(merge);
    ValueID character = builder.append(Opcode::Substring, { state.subject, index, end });
    expansion.define(*state.position, end);
    expansion.finishStep(builder.appendBoolean(false), character);

    builder.moveToEndOf(branch.notTaken);
    expansion.finishStepAsDone();
}

void emitSplitStringStep(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    SplitStringStep step = appendSplitStringStep(builder, { state.subject, state.arm.separator, state.arm.separatorSearch }, state.position->anyDefinition);
    ValueID substring = builder.append(Opcode::Substring, { state.subject, step.start, step.end });
    expansion.define(*state.position, step.next);
    expansion.finishStep(builder.appendBoolean(false), substring);

    builder.moveToEndOf(step.done);
    expansion.finishStepAsDone();
}

// The run time lists the keys and values as Object.entries does, at the moment the call stood for, one behind the other in one list.
void emitOwnEntriesOpen(IterationExpansion& expansion, ArmState& state)
{
    BuiltinOverloadID overload = *expansion.program().builtins().overloadIDsOf(Builtins::CompilerHelpersOwnEntriesFlat).begin();
    state.subject = expansion.builder().append(makeBuiltinCall(overload, BuiltinCallForm::Whole, { { }, { state.subject } }, { }));
    emitIndexedOpen(expansion, state);
}

// The pair is made here, where whoever takes it apart sees what it is made of.
void emitOwnEntriesStep(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    ValueID length = builder.append(Opcode::ToLength, { expansion.appendPropGet(state.subject, expansion.keyOf(BuiltinNames::length)) });
    IRBuilder::ProvenBranch branch = branchOnInRange(expansion, state.position->anyDefinition, length);
    ValueID index = branch.provenOnTaken;

    auto appendRead = [&](ValueID at) {
        Value read = IRBuilder::make(Opcode::ElemGet, { state.subject, at });
        read.setKeyIsIntegral(true);
        return builder.append(WTF::move(read));
    };
    ValueID key = appendRead(index);
    ValueID value = appendRead(expansion.appendIncrement(index, 1));
    expansion.define(*state.position, expansion.appendIncrement(index, 2));
    expansion.finishStep(builder.appendBoolean(false), expansion.appendPair(key, value));

    builder.moveToEndOf(branch.notTaken);
    expansion.finishStepAsDone();
}

static ValueID appendFieldRead(IterationExpansion& expansion, ValueID iterator, JSArrayIterator::Field field)
{
    Value read = IRBuilder::make(Opcode::GetInternalField, { iterator });
    read.setIndex(static_cast<uint32_t>(field));
    return expansion.builder().append(WTF::move(read));
}

static void appendPositionStore(IterationExpansion& expansion, ValueID iterator, ValueID position)
{
    Value store = IRBuilder::make(Opcode::PutInternalField, { iterator, position });
    store.setIndex(static_cast<uint32_t>(JSArrayIterator::Field::Index));
    expansion.builder().append(WTF::move(store));
}

// An array iterator is made for an object and keeps it.
static ValueID appendIteratedObject(IterationExpansion& expansion, ValueID iterator)
{
    ValueID iterated = appendFieldRead(expansion, iterator, JSArrayIterator::Field::IteratedObject);
    return convertTo(expansion.builder(), iterated, DeclaredType { .type = TypeTable::object });
}

static ValueID appendIsTypedArrayView(IterationExpansion& expansion, ValueID object)
{
    Value test = IRBuilder::make(Opcode::HasJSType, { object });
    test.setFirst(CheckedJSType::Int8ArrayType);
    test.setLast(CheckedJSType::BigUint64ArrayType);
    return expansion.builder().append(WTF::move(test));
}

static ValueID appendLengthOfIterated(IterationExpansion& expansion, ValueID iterated)
{
    IRBuilder& builder = expansion.builder();
    BlockID merge = addSiblingBlock(expansion);
    ValueID length = builder.addParameter(merge, TypeTable::number);

    IRBuilder::BranchBlocks blocks = builder.emitBranch(appendIsTypedArrayView(expansion, iterated), BlockFrequency::Normal, BlockFrequency::Normal);
    builder.append(Opcode::RequireAttachedTypedArray, { iterated });
    builder.emitJump(merge, { builder.append(Opcode::GetTypedArrayLength, { iterated }) });

    builder.moveToEndOf(blocks.notTaken);
    builder.emitJump(merge, { builder.append(Opcode::ToLength, { expansion.appendPropGet(iterated, expansion.keyOf(BuiltinNames::length)) }) });

    builder.moveToEndOf(merge);
    return length;
}

static void finishStepWithKind(IterationExpansion& expansion, ValueID kind, ValueID iterated, ValueID position)
{
    IRBuilder& builder = expansion.builder();
    auto finishWith = [&](ValueID value) {
        expansion.finishStep(builder.appendBoolean(false), value);
    };

    IRBuilder::BranchBlocks keys = builder.emitBranch(expansion.appendIsEqualToInteger(kind, static_cast<int64_t>(JSC::IterationKind::Keys)), BlockFrequency::Normal, BlockFrequency::Normal);
    finishWith(position);

    builder.moveToEndOf(keys.notTaken);
    ValueID element = builder.append(Opcode::ElemGet, { iterated, position });
    IRBuilder::BranchBlocks entries = builder.emitBranch(expansion.appendIsEqualToInteger(kind, static_cast<int64_t>(JSC::IterationKind::Entries)), BlockFrequency::Normal, BlockFrequency::Normal);
    finishWith(expansion.appendPair(position, element));

    builder.moveToEndOf(entries.notTaken);
    finishWith(element);
}

// %ArrayIteratorPrototype%.next on the state that JSArrayIterator keeps. Nobody knows what the iterator is over, so every read is the generic one.
static void emitArrayIteratorStep(IterationExpansion& expansion, ValueID iterator)
{
    IRBuilder& builder = expansion.builder();
    ValueID position = appendFieldRead(expansion, iterator, JSArrayIterator::Field::Index);
    IRBuilder::BranchBlocks finished = builder.emitBranch(expansion.appendIsEqualToInteger(position, JSArrayIterator::doneIndex), BlockFrequency::Normal, BlockFrequency::Normal);
    expansion.finishStepAsDone();

    builder.moveToEndOf(finished.notTaken);
    ValueID iterated = appendIteratedObject(expansion, iterator);
    ValueID length = appendLengthOfIterated(expansion, iterated);
    IRBuilder::BranchBlocks inRange = builder.emitBranch(builder.append(Opcode::Less, { position, length }), BlockFrequency::Normal, BlockFrequency::Normal);
    appendPositionStore(expansion, iterator, expansion.appendIncrement(position, 1));
    finishStepWithKind(expansion, appendFieldRead(expansion, iterator, JSArrayIterator::Field::Kind), iterated, position);

    builder.moveToEndOf(inRange.notTaken);
    appendPositionStore(expansion, iterator, builder.appendInteger(JSArrayIterator::doneIndex));
    expansion.finishStepAsDone();
}

PassOutcome expandArrayIteratorSteps(FunctionPassContext& pass, IRBuilder& builder)
{
    Vector<ProgramPoint, 1> steps;
    Vector<ValueID, 1> producers;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = pass.function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (pass.function.at(values[index]).opcode != Opcode::StepArrayIterator)
                continue;
            steps.append(ProgramPoint { block, index });
            producers.append(values[index]);
        }
    }
    if (steps.isEmpty())
        return PassOutcome::Unchanged;

    AOT::sort(steps, std::ranges::greater { }, [](const ProgramPoint& point) {
        return std::pair { point.block, point.index };
    });
    SSARepair repair(builder, pass.analyses);
    IterationExpansion expansion(pass, builder, repair, IterationOrigin::Source);
    IRBuilder::DeferredResultReads resultReads(builder, producers.span());
    for (const ProgramPoint& point : steps) {
        Value original = pass.function.at(pass.function.at(point.block).values[point.index]);
        builder.setOrigin(original.origin());
        IRBuilder::Expansion cut = builder.beginExpansion(point, resultReads);
        expansion.beginOperation(cut);
        emitArrayIteratorStep(expansion, original.iteratorOperand());
    }
    resultReads.finish();
    builder.setHandler(std::nullopt);
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
