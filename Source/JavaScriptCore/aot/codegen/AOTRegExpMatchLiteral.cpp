/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolvedCallEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTRegExpFlags.h"
#include "AOTStubsRegExp.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "VM.h"

namespace JSC { namespace AOT { namespace Typed {

void ResolvedCallEmitter::generateRegExpMatchLiteral(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    ValueID regExp = value->regExpOperand();
    bool recordsMatch = m_facts.mayReadLegacyRegExpStatics();
    if (!regExp && (value->regExpFlags().containsAny(flagsOfMatchesUsingObject) || recordsMatch)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    CellPointer string = in<CellPointer>(value->stringOperand());
    NumberedImmediate record { m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())) };
    if (!regExp) {
        define(value, Boxed(callStub(value, regExpMatchLiteralWithoutObject(value->matchForm()), { string.value() }, { .numberedImmediate = record }).first));
        return;
    }

    StubKey stub = regExpMatchLiteral(value->matchForm(), value->regExpFlags().contains(RegExpFlags::Global), recordsMatch);
    define(value, Boxed(callStub(value, stub, { in<CellPointer>(regExp).value(), string.value() }, { .numberedImmediate = record }).first));
}

void ResolvedCallEmitter::generateBoundsOfRegExpMatch(const ValueContext& value)
{
    const Machine::AbstractHeap& state = m_heaps.entire(AbstractHeap::RegExpState);
    Pointer output(m_out.loadPtr(m_out.address(state, vm().value(), VM::offsetOfAOTState() + VMState::offsetOfRegExpMatchOutput())));
    Vector<Operand, 8> results;
    for (uint32_t bound = 0; bound < 2 * value->count(); ++bound)
        results.append(Word32(m_out.load32(m_out.address(state, output.value(), bound * sizeof(int)))));
    defineResults(value, results.span());
}

void ResolvedCallEmitter::generateRegExpMatchLiteralInParts(const ValueContext& value)
{
    ValueID regExp = value->regExpOperand();
    bool recordsMatch = m_facts.mayReadLegacyRegExpStatics();
    if (!regExp && (value->regExpFlags().containsAny(flagsOfMatchesUsingObject) || recordsMatch)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, boolean(false));
        return;
    }

    CellPointer string = in<CellPointer>(value->stringOperand());
    NumberedImmediate record { m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())) };
    if (!regExp) {
        define(value, unboxBoolean(Boxed(callStub(value, regExpMatchLiteralWithoutObject(RegExpMatchForm::Parts), { string.value() }, { .numberedImmediate = record }).first)));
        return;
    }

    StubKey stub = regExpMatchLiteral(RegExpMatchForm::Parts, value->regExpFlags().contains(RegExpFlags::Global), recordsMatch);
    define(value, unboxBoolean(Boxed(callStub(value, stub, { in<CellPointer>(regExp).value(), string.value() }, { .numberedImmediate = record }).first)));
}

void ResolvedCallEmitter::generateNewRegExpMatchArray(const ValueContext& value)
{
    CellPointer string = in<CellPointer>(value->stringOperand());
    Word32 record = numberOf(m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())));
    CellPointer noObject(pointerConstant(0).value());
    define(value, unboxCell(callOperation<Boxed>(value, RuntimeOperation::RegExpMatchLiteral, { noObject, string, record, word32(static_cast<int32_t>(RegExpMatchForm::Exec)) })));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
