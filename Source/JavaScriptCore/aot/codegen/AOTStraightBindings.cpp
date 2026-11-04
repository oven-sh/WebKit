/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTPlan.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfBindings()
{
    return { Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::ScopeHasBinding };
}

// Link has heard of nothing that such a block alone names, so its code may mention nothing.
static bool standsInBlockNeverEnteredInStraightBinding(StraightEmitter& emitter)
{
    const Vector<BlockID>& blocks = emitter.facts().summaries().at(emitter.function()).blocksNeverEntered;
    return !blocks.isEmpty() && std::ranges::binary_search(blocks, emitter.currentBlock().id());
}

static JSValue markOfVariableFilledAtFirstRead(const DeferredVariable& deferred)
{
    if (deferred.mark == Primitive::Undefined)
        return jsUndefined();
    return deferred.mark == Primitive::Null ? jsNull() : JSValue();
}

// The operation as its body writes it, at the position of the read, with the slot of the read for what it gives.
static void makeObjectOfVariableFilledAtFirstRead(StraightEmitter& emitter, const BodyValue& read, const DeferredVariable& deferred)
{
    const BodyIndex* index = emitter.facts().bodies().find(deferred.body);
    if (!index || !deferred.made) {
        emitter.fail(read);
        return;
    }
    BodyValue written = index->value(deferred.made);
    Vector<uint32_t, 16> words;
    words.append(unsafeMakeSpan(written.words(), written.sizeInWords()));
    words[offsetof(Wire::ValueHead, position) / sizeof(uint32_t)] = read.position();
    BodyValue made = BodyValue::operation(words.span().data(), &written.unit(), read.id());
    switch (made.opcode()) {
    case Opcode::Data:
        emitter.generateConstant(made);
        return;
    case Opcode::NewRegExp:
        emitter.generateAllocation(made);
        return;
    default:
        emitter.fail(read);
        return;
    }
}

static void generateReadOfVariableFilledAtFirstRead(StraightEmitter& emitter, const BodyValue& read, const DeferredVariable& deferred)
{
    constexpr GPRReg held = StraightEmitter::resultGPR;
    PlannedVariable planned = emitter.plan().variable(deferred.variable);
    if (!planned.hasWordInStatics() || planned.encoding != VariableEncoding::Plain || planned.representation != SlotRepresentation::JSValue) {
        emitter.fail(read);
        return;
    }

    CCallHelpers& jit = emitter.jit();
    emitter.relocations().loadInstanceWord({ SymbolSpace::InstanceWord, emitter.mentions().add(NumberSpace::Variable, deferred.variable) }, InstanceWordWidth::Word64, instanceGPR, held);
    CCallHelpers::Jump isMade = jit.branch64(CCallHelpers::NotEqual, held, CCallHelpers::TrustedImm32(static_cast<int32_t>(JSValue::encode(markOfVariableFilledAtFirstRead(deferred)))));
    makeObjectOfVariableFilledAtFirstRead(emitter, read, deferred);
    emitter.load(read.id(), held);
    emitter.storeVariable(deferred.variable, held);
    isMade.link(&jit);
    emitter.define(read);
}

static void generateGetModuleVariable(StraightEmitter& emitter, const BodyValue& value)
{
    if (emitter.index().isUnused(value.id()))
        return;
    if (const DeferredVariable* deferred = emitter.plan().deferredDefinitions().findDeferred(value.variable())) {
        generateReadOfVariableFilledAtFirstRead(emitter, value, *deferred);
        return;
    }
    emitter.loadVariable(value.variable(), StraightEmitter::resultGPR);
    emitter.define(value);
}

static void generateGetEnvironmentField(StraightEmitter& emitter, const BodyValue& value)
{
    if (emitter.index().isUnused(value.id()))
        return;
    emitter.load(value.environmentOperand(), StraightEmitter::resultGPR);
    emitter.loadSlot(StraightEmitter::resultGPR, emitter.plan().slot(value.layout(), value.field()), StraightEmitter::resultGPR);
    emitter.define(value);
}

static void generatePutEnvironmentField(StraightEmitter& emitter, const BodyValue& value)
{
    const GPRReg environment = StraightEmitter::resultGPR;
    const GPRReg stored = StraightEmitter::temporary(1);
    const PlacedSlot& slot = emitter.plan().slot(value.layout(), value.field());
    emitter.load(value.environmentOperand(), environment);
    emitter.load(value.valueOperand(), stored);
    if (!emitter.isConstant(value.valueOperand()) && !emitter.plan().layout(value.layout()).liesOnlyInFrames) {
        emitter.storeSlot(environment, slot, stored);
        return;
    }
    if (slot.storage != SlotStorage::Inline || slot.representation != SlotRepresentation::JSValue) {
        emitter.fail(value);
        return;
    }
    emitter.jit().store64(stored, CCallHelpers::Address(environment, static_cast<int32_t>(slot.offset)));
}

void StraightEmitter::generateBinding(const BodyValue& value)
{
    if (standsInBlockNeverEnteredInStraightBinding(*this))
        return;
    switch (value.opcode()) {
    case Opcode::GetModuleVariable:
        generateGetModuleVariable(*this, value);
        return;
    case Opcode::PutModuleVariable:
        load(value.valueOperand(), resultGPR);
        storeVariable(value.variable(), resultGPR);
        return;
    case Opcode::GetEnvironmentField:
        generateGetEnvironmentField(*this, value);
        return;
    case Opcode::PutEnvironmentField:
        generatePutEnvironmentField(*this, value);
        return;
    case Opcode::ScopeHasBinding:
        callOperation(value, RuntimeOperation::ScopeHasBinding, { value.scopeOperand(), StraightArgument::number(mentionOfName(value.key())) });
        defineBoolean(value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
