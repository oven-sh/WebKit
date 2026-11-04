/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinCallEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTDiagnostics.h"
#include "AOTIRProgram.h"
#include "AOTNumbering.h"
#include "AOTRegExpFlags.h"
#include "AOTStubsArrays.h"
#include "AOTStubsRegExp.h"

namespace JSC { namespace AOT { namespace Typed {

// The module of the function of the operation's inline frame, and the position in it. Code::locationOf() serves reports: for a function without a text it answers with the call in the frame around it, where several literals would meet.
static std::optional<Wire::SourceLocation> placeWhereLiteralObjectIsWritten(const Program& program, const Code& code, const Operation& literal)
{
    FunctionID writtenIn = code.inlineFrames()[literal.inlineFrame].callee;
    if (!writtenIn)
        return std::nullopt;
    Wire::SourceLocation place = program.at(writtenIn).locationAt(literal.position);
    if (!hasModule(place))
        return std::nullopt;
    return place;
}

void AllocationEmitter::generateNewRegExp(const ValueContext& value)
{
    if (value->isNeverToldApart() || emitter<AccessEmitter>().isPatternLeftToCall(value.id())) {
        emitter<BuiltinCallEmitter>().generateSharedRegExpLiteral(value);
        return;
    }

    StringID pattern = value->pattern();
    OptionSet<Yarr::Flags> flags = toYarrFlags(value->regExpFlags());

    // The stub takes the RegExp out of whatever object fills the cell. Copies of one call of RegExp become different literals where they are inlined under other arguments,
    // so the place alone does not name the cell: the pattern and the flags are part of its key.
    Wire::SourceLocation written = placeWhereLiteralObjectIsWritten(m_program, m_code, value.value()).value_or(noSourceLocation);
    uint32_t sharedCell = m_mentions.regExpLiteralCell(written, pattern, flags);

    Word32 record = numberOf(m_mentions.regExp(pattern, flags));
    LValue sharedObject = m_out.load64(instanceWord(m_heaps.entire(AbstractHeap::AllocatorState), sharedCell));
    define(value, CellPointer(callStub(value, newRegExpLiteral(), { record.value(), word32(flags.toRaw()).value(), numberOf(sharedCell).value(), sharedObject }).first));
}

void AllocationEmitter::generateNewTemplateObject(const ValueContext& value)
{
    std::optional<Wire::SourceLocation> written = placeWhereLiteralObjectIsWritten(m_program, m_code, value.value());
    if (!written) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    Vector<uint32_t, 8> cookedAndRawStrings;
    for (TemplateString part : value->templateStrings()) {
        cookedAndRawStrings.append(part.cooked ? part.cooked.index() : Wire::none);
        cookedAndRawStrings.append(part.raw.index());
    }
    uint32_t request = m_mentions.request(RequestKind::TemplateObject, cookedAndRawStrings.span());

    uint32_t keyOfSiteCell[] { written->module, written->position };
    TypedPointer siteCell = instanceWord(m_heaps.entire(AbstractHeap::AllocatorState), m_mentions.templateObjectCell(*written));
    Integer existing(m_out.load64(siteCell));
    define(value, either<CellPointer>(isNotZero(existing), [&] {
        return CellPointer(existing.value());
    }, [&] {
        CellPointer created = callOperation<CellPointer>(value, RuntimeOperation::NewTemplateObject, { numberOf(request), word32(static_cast<int32_t>(written->module)), numberOf(m_mentions.templateObjectCell(*written)) }, { .isOnRarePath = true });
        mutatorFence();
        m_out.store64(created.value(), siteCell);
        emitter<BarrierEmitter>().markCard(m_mentions.cardOfWord(NumberSpace::TemplateObjectCell, keyOfSiteCell));
        return created;
    }));
}

void AllocationEmitter::generateNewRuntimeError(const ValueContext& value)
{
    Word32 errorKind = word32(static_cast<int32_t>(value->runtimeError()));
    if (StringID name = value->name()) {
        define(value, callOperation<CellPointer>(value, RuntimeOperation::NewRuntimeErrorWithName, { errorKind, numberOf(m_mentions.add(NumberSpace::ShownString, name)) }, { .capturesStackTrace = true }));
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewRuntimeError, { errorKind }, { .capturesStackTrace = true }));
}

void AllocationEmitter::generateNewFunctionThatThrows(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewFunctionThatThrows, { in<Boxed>(value->getterOperand()), numberOf(m_mentions.add(NumberSpace::ShownString, value->name())) }));
}

void AllocationEmitter::generateNewSuppressedError(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewSuppressedError, { in<Boxed>(value->errorOperand()), in<Boxed>(value->suppressedOperand()) }, { .capturesStackTrace = true }));
}

CellPointer AllocationEmitter::createArguments(const ValueContext& value, std::optional<CellPointer> callee)
{
    ValueID countOperand = value->countOperand();
    Word32 count = word32(0);
    switch (representationOf(countOperand)) {
    case Representation::Int32:
        count = in<Word32>(countOperand);
        break;
    case Representation::Int64:
        count = low32(in<Integer>(countOperand));
        break;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        fail(value, InternalErrorKind::WrongRepresentation);
        break;
    }

    Vector<Boxed, 8> parameters;
    for (ValueID parameter : value->parametersOperands())
        parameters.append(in<Boxed>(parameter));

    CellPointer rest = in<CellPointer>(value->restOperand());
    Pointer buffer = argumentBuffer(parameters.span());
    if (callee)
        return CellPointer(callStub(value, createMappedArguments(), { callee->value(), count.value(), rest.value(), buffer.value(), word32(static_cast<int32_t>(parameters.size())).value() }).first);
    return CellPointer(callStub(value, AOT::createArguments(), { count.value(), rest.value(), buffer.value(), word32(static_cast<int32_t>(parameters.size())).value() }).first);
}

void AllocationEmitter::generateCreateUnmappedArguments(const ValueContext& value)
{
    define(value, createArguments(value, std::nullopt));
}

void AllocationEmitter::generateCreateMappedArguments(const ValueContext& value)
{
    define(value, createArguments(value, in<CellPointer>(value->calleeOperand())));
}

void AllocationEmitter::generateGetMappedArgument(const ValueContext& value)
{
    CellPointer arguments = in<CellPointer>(value->argumentsOperand());
    uint32_t index = value->index();
    Word32 indexWord = word32(static_cast<int32_t>(index));
    Condition isWithinCapacity = below(indexWord, loadField<Word32>(arguments, m_heaps.DirectArguments_minCapacity));
    define(value, either<Boxed>(isWithinCapacity, [&] {
        return loadField<Boxed>(arguments, m_heaps.DirectArguments_storage[index]);
    }, [&] {
        return callOperation<Boxed>(value, RuntimeOperation::GetMappedArgument, { arguments, indexWord }, { .isOnRarePath = true });
    }));
}

void AllocationEmitter::generateSetMappedArgument(const ValueContext& value)
{
    CellPointer arguments = in<CellPointer>(value->argumentsOperand());
    ValueID storedValue = value->valueOperand();
    Boxed stored = in<Boxed>(storedValue);
    uint32_t index = value->index();
    Word32 indexWord = word32(static_cast<int32_t>(index));
    Condition isWithinCapacity = below(indexWord, loadField<Word32>(arguments, m_heaps.DirectArguments_minCapacity));
    ifElse(isWithinCapacity, [&] {
        storeField(arguments, m_heaps.DirectArguments_storage[index], stored);
        barrierAfterStore(value, arguments, stored, typeOf(storedValue));
    }, [&] {
        callVoidOperation(value, RuntimeOperation::SetMappedArgument, { arguments, indexWord, stored }, { .isOnRarePath = true });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
