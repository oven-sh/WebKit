/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTRegExpFlags.h"
#include "AOTStaticsEmitter.h"
#include "AOTStubsArrays.h"
#include "AOTStubsRegExp.h"

namespace JSC { namespace AOT { namespace Typed {

void AllocationEmitter::generateNewRegExp(const ValueContext& value)
{
    RegExpRecordID record = value->regExpRecord();
    if (!record) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    std::optional<StaticsOffset> sharedCellOffset = value->staticsOffset();
    Word32 recordNumber = word32(static_cast<int32_t>(record.index()));
    Word32 flags = word32(static_cast<int32_t>(toYarrFlags(value->regExpFlags()).toRaw()));
    Word32 offset = word32(static_cast<int32_t>(sharedCellOffset ? sharedCellOffset->value() : noSharedRegExpOffset));
    LValue sharedCell = m_out.constInt64(0);
    if (sharedCellOffset)
        sharedCell = m_out.load64(emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *sharedCellOffset));
    define(value, CellPointer(callStub(value, newRegExpLiteral(), { recordNumber.value(), flags.value(), offset.value(), sharedCell }).first));
}

void AllocationEmitter::generateNewTemplateObject(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    std::optional<StaticsOffset> siteCellOffset = value->staticsOffset();
    if (!request || !siteCellOffset) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    TypedPointer siteCell = emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *siteCellOffset);
    Integer existing(m_out.load64(siteCell));
    define(value, either<CellPointer>(isNotZero(existing), [&] {
        return CellPointer(existing.value());
    }, [&] {
        CellPointer created = callOperation<CellPointer>(value, RuntimeOperation::NewTemplateObject, { word32(static_cast<int32_t>(request->value())) }, { .isOnRarePath = true });
        mutatorFence();
        m_out.store64(created.value(), siteCell);
        emitter<BarrierEmitter>().markCardOfStatics(siteCellOffset->value());
        return created;
    }));
}

void AllocationEmitter::generateNewRuntimeError(const ValueContext& value)
{
    Word32 errorKind = word32(static_cast<int32_t>(value->runtimeError()));
    if (StringID name = value->name()) {
        define(value, callOperation<CellPointer>(value, RuntimeOperation::NewRuntimeErrorWithName, { errorKind, word32(static_cast<int32_t>(name.index())) }, { .capturesStackTrace = true }));
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewRuntimeError, { errorKind }, { .capturesStackTrace = true }));
}

void AllocationEmitter::generateNewFunctionThatThrows(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewFunctionThatThrows, { in<Boxed>(value->getterOperand()), word32(static_cast<int32_t>(value->name().index())) }));
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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
    return CellPointer(callStub(value, AOT::createArguments(), { count.value(), rest.value(), buffer.value() }).first);
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
