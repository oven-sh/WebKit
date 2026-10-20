/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsElements.h"
#include "AOTStubsPropertyKeys.h"
#include "ECMAMode.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ElementKeyForm : uint8_t { Index, Value, Unsupported };

} // anonymous namespace

static ElementKeyForm elementKeyFormOf(Representation representation)
{
    switch (representation) {
    case Representation::Int32:
    case Representation::Int64:
        return ElementKeyForm::Index;
    case Representation::JSValue:
        return ElementKeyForm::Value;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return ElementKeyForm::Unsupported;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSC::ECMAMode engineModeOf(ECMAMode mode)
{
    switch (mode) {
    case ECMAMode::Strict:
        return JSC::ECMAMode::strict();
    case ECMAMode::Sloppy:
        return JSC::ECMAMode::sloppy();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StubKey cachedStoreStubFor(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
    case PutMode::Sloppy:
        return putByValCached(mode);
    case PutMode::Define:
        return defineByValCached();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static RuntimeOperation deleteOperationFor(ECMAMode mode)
{
    switch (mode) {
    case ECMAMode::Strict:
        return RuntimeOperation::DeleteByVal;
    case ECMAMode::Sloppy:
        return RuntimeOperation::DeleteByValSloppy;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed AccessEmitter::getByVal(const OperationContext& context, Boxed receiver, Boxed key)
{
    return Boxed(callStub(context, getByValCached(), { receiver.value(), key.value() }).first);
}

void AccessEmitter::putByVal(const OperationContext& context, Boxed receiver, Boxed key, Boxed value, PutMode mode)
{
    callStub(context, cachedStoreStubFor(mode), { receiver.value(), key.value(), value.value() });
}

Condition AccessEmitter::hasByVal(const OperationContext& context, Boxed receiver, Boxed key)
{
    return Condition(m_out.castToInt32(callStub(context, inByValCached(PresenceQuestion::In), { receiver.value(), key.value() }).first));
}

void AccessEmitter::generateElemGet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    ValueID property = value->propertyOperand();
    switch (elementKeyFormOf(representationOf(property))) {
    case ElementKeyForm::Index: {
        Integer index = integerOf(property);
        define(value, Boxed(callStub(value, AOT::getByVal(true), { object.value(), index.value() }).first));
        return;
    }
    case ElementKeyForm::Value: {
        Boxed key = in<Boxed>(property);
        if (value->keyIsIntegral()) {
            define(value, Boxed(callStub(value, AOT::getByVal(false), { object.value(), key.value() }).first));
            return;
        }
        define(value, getByVal(value, object, key));
        return;
    }
    case ElementKeyForm::Unsupported:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessEmitter::generateElemSet(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    ValueID property = value->propertyOperand();
    Boxed stored = in<Boxed>(value->valueOperand());
    switch (elementKeyFormOf(representationOf(property))) {
    case ElementKeyForm::Index: {
        Integer index = integerOf(property);
        callStub(value, AOT::putByVal(true, engineModeOf(value->mode())), { object.value(), index.value(), stored.value() });
        return;
    }
    case ElementKeyForm::Value: {
        Boxed key = in<Boxed>(property);
        if (value->keyIsIntegral()) {
            callStub(value, AOT::putByVal(false, engineModeOf(value->mode())), { object.value(), key.value(), stored.value() });
            return;
        }
        putByVal(value, object, key, stored, putModeOf(value->mode()));
        return;
    }
    case ElementKeyForm::Unsupported:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessEmitter::generateElemHas(const ValueContext& value)
{
    defineTest(value, hasByVal(value, in<Boxed>(value->objectOperand()), in<Boxed>(value->propertyOperand())));
}

void AccessEmitter::generateElemDelete(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    Boxed key = in<Boxed>(value->propertyOperand());
    define(value, callOperation<Condition>(value, deleteOperationFor(value->mode()), { object, key }));
}

void AccessEmitter::generateElemDefine(const ValueContext& value)
{
    Boxed object = boxCell(in<CellPointer>(value->objectOperand()));
    Boxed key = in<Boxed>(value->propertyOperand());
    Boxed stored = in<Boxed>(value->valueOperand());
    putByVal(value, object, key, stored, PutMode::Define);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
