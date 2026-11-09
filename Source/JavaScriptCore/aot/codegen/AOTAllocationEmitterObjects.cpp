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
#include "AOTConversionEmitter.h"
#include "AOTDiagnostics.h"
#include "AOTIRProgram.h"
#include "AOTNumbering.h"
#include "AOTRegExpFlags.h"
#include "AOTStubsArrays.h"
#include "AOTStubsRegExp.h"

namespace JSC { namespace AOT { namespace Typed {

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
    if (emitter<AccessEmitter>().isPatternLeftToCall(value.id())) {
        emitter<BuiltinCallEmitter>().generateSharedRegExpLiteral(value);
        return;
    }

    StringID pattern = value->pattern();
    OptionSet<Yarr::Flags> flags = toYarrFlags(value->regExpFlags());

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

void AllocationEmitter::generateNewSuppressedError(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewSuppressedError, { in<Boxed>(value->errorOperand()), in<Boxed>(value->suppressedOperand()) }, { .capturesStackTrace = true }));
}

bool AllocationEmitter::isListedByArgumentsObjectAlone(ValueID list) const
{
    const Operation& made = m_code.at(list);
    if (made.opcode != Opcode::NewArray || !m_code.has(Mechanism::EscapeAnalysis, m_facts))
        return false;
    if (made.storage() != AllocationStorage::Heap || !made.holes().empty())
        return false;
    ValueID user = m_code.onlyUserOf(list);
    if (!user || m_code.at(user).opcode != Opcode::CreateUnmappedArguments || m_code.at(user).restOperand() != list)
        return false;
    for (const LiveAtSuspension& record : m_code.suspensions()) {
        if (std::ranges::contains(m_code.liveAt(record.suspension), list))
            return false;
    }
    return true;
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
    case Representation::RawWord:
        fail(value, InternalErrorKind::WrongRepresentation);
        break;
    }

    Vector<Boxed, 8> parameters;
    for (ValueID parameter : value->parametersOperands())
        parameters.append(in<Boxed>(parameter));
    if (isListedByArgumentsObjectAlone(value->restOperand())) {
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        for (ValueID element : operationAt(value->restOperand()).elementsOperands()) {
            const Type& type = typeOf(element);
            parameters.append(conversions.box(value, conversions.inFormOfType(get(element), type), type));
        }
    }

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
        barrierAfterStore(value, arguments, typeOf(storedValue));
    }, [&] {
        callVoidOperation(value, RuntimeOperation::SetMappedArgument, { arguments, indexWord, stored }, { .isOnRarePath = true });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
