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

static JSValue markOfVariableFilledAtFirstRead(const DeferredVariable& deferred)
{
    if (deferred.mark == Primitive::Undefined)
        return jsUndefined();
    return deferred.mark == Primitive::Null ? jsNull() : JSValue();
}

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
    PlannedVariable planned = emitter.plan().variable(deferred.variable);
    if (!planned.hasWordInStatics() || planned.encoding != VariableEncoding::Plain || planned.representation != SlotRepresentation::JSValue) {
        emitter.fail(read);
        return;
    }

    CCallHelpers& jit = emitter.jit();
    GPRReg held = emitter.takeResultRegister();
    emitter.relocations().loadInstanceWord({ SymbolSpace::InstanceWord, emitter.mentions().add(NumberSpace::Variable, deferred.variable) }, InstanceWordWidth::Word64, instanceGPR, held);
    CCallHelpers::Jump isMade = jit.branch64(CCallHelpers::NotEqual, held, CCallHelpers::TrustedImm32(static_cast<int32_t>(JSValue::encode(markOfVariableFilledAtFirstRead(deferred)))));
    makeObjectOfVariableFilledAtFirstRead(emitter, read, deferred);
    held = emitter.loadIntoResultRegister(read.id());
    emitter.storeVariable(deferred.variable, held);
    isMade.link(&jit);
    emitter.define(read, held);
}

static void generateGetModuleVariable(StraightEmitter& emitter, const BodyValue& value)
{
    if (emitter.index().isUnused(value.id()))
        return;
    if (const DeferredVariable* deferred = emitter.plan().deferredDefinitions().findDeferred(value.variable())) {
        generateReadOfVariableFilledAtFirstRead(emitter, value, *deferred);
        return;
    }
    emitter.loadVariable(value.variable(), emitter.takeResultRegister());
    emitter.define(value);
}

static void generateGetEnvironmentField(StraightEmitter& emitter, const BodyValue& value)
{
    if (emitter.index().isUnused(value.id()))
        return;
    GPRReg result = emitter.loadIntoResultRegister(value.environmentOperand());
    emitter.loadSlot(result, emitter.plan().slot(value.layout(), value.field()), result);
    emitter.define(value, result);
}

static void generatePutEnvironmentField(StraightEmitter& emitter, const BodyValue& value)
{
    const GPRReg stored = StraightEmitter::temporary(1);
    const PlacedSlot& slot = emitter.plan().slot(value.layout(), value.field());
    if (slot.representation == SlotRepresentation::OptionalInt64 || slot.representation == SlotRepresentation::OptionalDouble) {
        emitter.fail(value);
        return;
    }
    const GPRReg environment = emitter.loadIntoResultRegister(value.environmentOperand());
    emitter.load(value.valueOperand(), stored);
    if (!emitter.isConstant(value.valueOperand())) {
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
    switch (value.opcode()) {
    case Opcode::GetModuleVariable:
        generateGetModuleVariable(*this, value);
        return;
    case Opcode::PutModuleVariable:
        storeVariable(value.variable(), value.valueOperand());
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
