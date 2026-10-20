/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConvention.h"
#include "AOTInlineBuiltinEmitter.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTRegExpFlags.h"
#include "AOTStaticsEmitter.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCallHost.h"
#include "CodeSpecializationKind.h"
#include "JSCJSValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfArgumentGPRs <= StubKey::maxCount);

static StubKey hostFunctionStubFor(const BuiltinTable& builtins, BuiltinID builtin, unsigned count)
{
    StubKey withoutBoxedCall = callHostFunctionWithoutBoxedCall(count);
    if (!exists(withoutBoxedCall))
        return callHostFunction(count);
    std::optional<BuiltinBoxedStub> boxedStub = builtins.boxedCallStubOf(builtin);
    if (boxedStub && acceptsArgumentCount(boxedCallFormOf(*boxedStub), count))
        return callHostFunction(count);
    return withoutBoxedCall;
}

static std::optional<StubKey> stubKeyFor(const BuiltinTable& builtins, const BuiltinOverload& overload, unsigned count)
{
    if (count > numberOfArgumentGPRs)
        return std::nullopt;
    bool constructs = overload.mode == BuiltinCallMode::Construct;
    switch (overload.code.implementation.callee) {
    case BuiltinCallee::None:
    case BuiltinCallee::Function:
        return std::nullopt;
    case BuiltinCallee::HostFunction:
        if (constructs)
            return std::nullopt;
        return hostFunctionStubFor(builtins, overload.builtin, count);
    case BuiltinCallee::InternalFunction:
        return callInternalFunction(constructs ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall, count);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void failAndDefineZero(CodeGenerator& generator, const ValueContext& value, InternalErrorKind kind)
{
    generator.fail(value, kind);
    generator.define(value, generator.emitter<ConstantEmitter>().zeroOf(value.representation()));
}

BuiltinCallEmitter::BuiltinCallEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

void BuiltinCallEmitter::generateCallBuiltin(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    BuiltinOverloadID id = value->overload();
    const BuiltinOverload& overload = m_builtins.at(id);
    ValueID receiver = value->receiverOperand();
    std::span<const ValueID> arguments = value->argumentsOperands();

    auto defineResult = [&](Boxed result) {
        defineAs(value, result, typeOf(value.id()));
    };
    auto callFunction = [&] {
        std::optional<Boxed> boxedReceiver;
        if (receiver)
            boxedReceiver = boxedOperand(value, receiver);
        Vector<Boxed, 8> boxedArguments;
        for (ValueID argument : arguments)
            boxedArguments.append(boxedOperand(value, argument));
        defineResult(callBuiltinFunction(value, id, boxedReceiver, boxedArguments.span()));
    };

    switch (value->builtinCallForm()) {
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix: {
        std::optional<InlineBuiltin> builtin = inlineImplementationOf(m_program, value.value());
        if (!builtin) {
            fail(value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        InlineCall call(value, *builtin, { });
        emitter<InlineBuiltinEmitter>().generate(call);
        return;
    }
    case BuiltinCallForm::List:
        if (receiver || arguments.size() != 1 || overload.mode != BuiltinCallMode::Call) {
            failAndDefineZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        defineResult(callOperation<Boxed>(value, RuntimeOperation::Apply, { boxCell(calleeOf(value, overload)), undefined(), boxedOperand(value, arguments[0]) }));
        return;
    case BuiltinCallForm::AnyReceiver:
        callFunction();
        return;
    case BuiltinCallForm::Whole:
        break;
    }

    switch (overload.code.implementation.kind) {
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::HostCall:
    case BuiltinImplementationKind::Compiled:
    case BuiltinImplementationKind::Inline:
        callFunction();
        return;
    case BuiltinImplementationKind::Number: {
        std::optional<double> number = m_builtins.numberOf(overload.builtin);
        if (!number) {
            failAndDefineZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        defineResult(emitter<ConstantEmitter>().constantOf(jsNumber(*number), Representation::JSValue).as<Boxed>());
        return;
    }
    case BuiltinImplementationKind::Cell:
        defineResult(boxCell(builtinCell(value, BuiltinCellKey::object(overload.builtin))));
        return;
    case BuiltinImplementationKind::CachedGetter: {
        if (!receiver) {
            failAndDefineZero(generator(), value, InternalErrorKind::OpcodeWithoutCode);
            return;
        }
        std::optional<uint32_t> offsetOfCachedValue = BuiltinBindings::cachedValueOffsetOf(id);
        if (!offsetOfCachedValue) {
            callFunction();
            return;
        }
        defineResult(readCachedGetter(value, overload, *offsetOfCachedValue));
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed BuiltinCallEmitter::readCachedGetter(const ValueContext& value, const BuiltinOverload& overload, uint32_t offsetOfCachedValue)
{
    RELEASE_ASSERT(overload.mode == BuiltinCallMode::Get);
    Boxed receiver = boxedOperand(value, value->receiverOperand());
    Boxed cached(m_out.load64(m_out.address(m_heaps.entire(AbstractHeap::ExternalState), unboxCell(receiver).value(), offsetOfCachedValue)));
    return either<Boxed>(isNotEmpty(cached), [&] {
        return cached;
    }, [&] {
        return callBuiltinFunction(value, value->overload(), receiver, { });
    });
}

CellPointer BuiltinCallEmitter::builtinCell(const OperationContext& context, BuiltinCellKey key)
{
    return emitter<ConstantEmitter>().builtinCell(context, key);
}

CellPointer BuiltinCallEmitter::calleeOf(const OperationContext& context, const BuiltinOverload& overload)
{
    switch (overload.mode) {
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Construct:
        return builtinCell(context, BuiltinCellKey::object(overload.builtin));
    case BuiltinCallMode::Get:
        return builtinCell(context, BuiltinCellKey::getter(overload.builtin));
    case BuiltinCallMode::Set:
        return builtinCell(context, BuiltinCellKey::setter(overload.builtin));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed BuiltinCallEmitter::callBuiltinFunction(const OperationContext& context, BuiltinOverloadID id, std::optional<Boxed> receiver, std::span<const Boxed> arguments)
{
    const BuiltinOverload& overload = m_builtins.at(id);
    bool constructs = overload.mode == BuiltinCallMode::Construct;
    Boxed callee = boxCell(calleeOf(context, overload));
    Boxed thisValue = undefined();
    if (constructs)
        thisValue = callee;
    else if (receiver)
        thisValue = *receiver;
    else if (BuiltinID holder = m_builtins.at(overload.builtin).holder; holder && BuiltinCellKey::object(holder).hasCell(m_builtins))
        thisValue = boxCell(builtinCell(context, BuiltinCellKey::object(holder)));

    CallEmitter& calls = emitter<CallEmitter>();
    CallOptions options { .constructs = constructs };
    const Value* call = context.valueID() ? &m_function.at(context.valueID()) : nullptr;
    CalledBuiltin calledBuiltin { id, call && call->opcode == Opcode::CallBuiltin && call->builtinCallForm() == BuiltinCallForm::AnyReceiver };
    unsigned count = static_cast<unsigned>(arguments.size());
    if (std::optional<StubKey> stub = stubKeyFor(m_builtins, overload, count)) {
        Vector<LValue, 8> values;
        for (Boxed argument : arguments)
            values.append(argument.value());
        return calls.callByConvention(context, { .stub = *stub, .callee = callee.value(), .thisValue = thisValue.value(), .arguments = values.span(), .builtin = calledBuiltin }, options);
    }
    if (constructs)
        return callOperation<Boxed>(context, RuntimeOperation::Construct, { callee, callee, word32(count), argumentBuffer(arguments) }, options);
    return calls.callValue(context, { callee, thisValue, arguments, std::nullopt, calledBuiltin }, options);
}

void BuiltinCallEmitter::generateNewSetFromData(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewSetFromData, { word32(request->value()) }));
}

void BuiltinCallEmitter::generateNewMapFromData(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewMapFromData, { word32(request->value()) }));
}

void BuiltinCallEmitter::generateNewArgumentList(const ValueContext& value)
{
    define(value, callOperation<CellPointer>(value, RuntimeOperation::NewArgumentList, { in<Boxed>(value->calleeOperand()), in<Boxed>(value->arrayLikeOperand()) }));
}

void BuiltinCallEmitter::generateSharedRegExpLiteral(const ValueContext& value)
{
    std::optional<StaticsOffset> staticsOffset = value->staticsOffset();
    RegExpRecordID record = value->regExpRecord();
    if (!staticsOffset || !record) {
        failAndDefineZero(generator(), value, InternalErrorKind::PlacedPayloadMissing);
        return;
    }
    Integer shared(m_out.load64(emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *staticsOffset)));
    define(value, either<CellPointer>(isNotZero(shared), [&] {
        return CellPointer(shared.value());
    }, [&] {
        return callOperation<CellPointer>(value, RuntimeOperation::SharedRegExpLiteral, { word32(staticsOffset->value()), word32(record.index()), word32(toYarrFlags(value->regExpFlags()).toRaw()) }, { .isOnRarePath = true });
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
