/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowApply.h"
#include "AOTResultRulesInternal.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CollectionStep {
    ValueID hasEntry;
    ValueID entry;
    ValueID key;
    ValueID value;
};

} // anonymous namespace

static CollectionStep appendMapStep(IterationExpansion& expansion, const ArmState& state)
{
    ValueID step = expansion.builder().append(Opcode::MapNext, { state.subject, state.table->anyDefinition, state.position->anyDefinition });
    CollectionStep results;
    results.hasEntry = expansion.appendResultAt(step, static_cast<uint32_t>(MapNextResult::HasEntry));
    results.entry = expansion.appendResultAt(step, static_cast<uint32_t>(MapNextResult::Entry));
    results.key = expansion.appendResultAt(step, static_cast<uint32_t>(MapNextResult::Key));
    results.value = expansion.appendResultAt(step, static_cast<uint32_t>(MapNextResult::Value));
    return results;
}

static CollectionStep appendSetStep(IterationExpansion& expansion, const ArmState& state)
{
    ValueID step = expansion.builder().append(Opcode::SetNext, { state.subject, state.table->anyDefinition, state.position->anyDefinition });
    CollectionStep results;
    results.hasEntry = expansion.appendResultAt(step, static_cast<uint32_t>(SetNextResult::HasEntry));
    results.entry = expansion.appendResultAt(step, static_cast<uint32_t>(SetNextResult::Entry));
    results.key = expansion.appendResultAt(step, static_cast<uint32_t>(SetNextResult::Element));
    results.value = results.key;
    return results;
}

static CollectionStep appendCollectionStep(IterationExpansion& expansion, const ArmState& state)
{
    switch (state.arm.form) {
    case IterationForm::Map:
        return appendMapStep(expansion, state);
    case IterationForm::Set:
        return appendSetStep(expansion, state);
    case IterationForm::Protocol:
    case IterationForm::Array:
    case IterationForm::TypedArray:
    case IterationForm::String:
    case IterationForm::SplitString:
    case IterationForm::OwnEntries:
    case IterationForm::Generator:
    case IterationForm::AsyncGenerator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitCollectionOpen(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    state.table = expansion.addVariable(builder.append(Opcode::CollectionTable, { state.subject }));
    state.position = expansion.addVariable(builder.appendInteger(0));
}

// On the edge that has an entry, a result of the step is what the collection holds.
static ValueID appendContentsOfEntry(IterationExpansion& expansion, const ArmState& state, ValueID result, ContentsSlot slot)
{
    TypingContext typing { expansion.program(), expansion.function(), expansion.types(), TypeLevel::Proven };
    std::optional<Type> contents = tryTypeOfContentsRead(typing, typing.typeOf(state.subject), slot);
    if (!contents || typing.typeOf(result).isSubsetOf(*contents, expansion.program()))
        return result;

    IRBuilder& builder = expansion.builder();
    TypeID castType = expansion.types().intern(*contents);
    if (expansion.pass().build.validates())
        builder.append(makeValidateType(result, castType));
    return builder.append(makeAssumeType(result, castType));
}

// A collection whose contents have the type never is always empty. Nothing may follow the value that says so.
static bool tryNarrowToContentsOfEntry(IterationExpansion& expansion, const ArmState& state, CollectionStep& step)
{
    auto mayExist = [&](ValueID value) {
        return expansion.function().at(value).type != TypeTable::never;
    };
    if (state.arm.form == IterationForm::Set) {
        step.key = appendContentsOfEntry(expansion, state, step.key, ContentsSlot::Element);
        step.value = step.key;
        return mayExist(step.key);
    }
    step.key = appendContentsOfEntry(expansion, state, step.key, ContentsSlot::Key);
    if (!mayExist(step.key))
        return false;
    step.value = appendContentsOfEntry(expansion, state, step.value, ContentsSlot::Value);
    return mayExist(step.value);
}

void emitCollectionStep(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    CollectionStep step = appendCollectionStep(expansion, state);
    IRBuilder::BranchBlocks blocks = builder.emitBranch(step.hasEntry, BlockFrequency::Normal, BlockFrequency::Normal);
    if (tryNarrowToContentsOfEntry(expansion, state, step)) {
        // The entry number counts in the table the collection has now: the step followed a replaced table to its successor.
        expansion.define(*state.table, builder.append(Opcode::CollectionTable, { state.subject }));
        expansion.define(*state.position, expansion.appendIncrement(step.entry, 1));
        ValueID done = builder.appendBoolean(false);
        ValueID value = expansion.appendValueOfKind(state.arm.kind, step.key, step.value);
        expansion.finishStep(done, value);
    } else
        builder.emitUnreachable();

    builder.moveToEndOf(blocks.notTaken);
    expansion.finishStepAsDone();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
