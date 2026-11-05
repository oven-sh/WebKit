/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "JSArrayBufferViewInlines.h"
#include "JSArrayIterator.h"
#include "JSCInlines.h"
#include "JSMapIterator.h"
#include "JSSetIterator.h"

namespace JSC { namespace AOT {

static UGPRPair doneAndValueOfStraightStep(bool hasValue, JSValue value)
{
    return makeUGPRPair(JSValue::encode(jsBoolean(!hasValue)), JSValue::encode(hasValue ? value : jsUndefined()));
}

static bool stepArrayIteratorForStraightCode(JSGlobalObject* globalObject, JSArrayIterator* iterator, JSValue& value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t index = iterator->index();
    if (index == JSArrayIterator::doneIndex)
        return false;

    JSObject* iterated = iterator->iteratedObject();
    double length = 0;
    if (isTypedView(iterated->type())) {
        JSArrayBufferView* view = validateTypedArray(globalObject, uncheckedDowncast<JSArrayBufferView>(iterated));
        RETURN_IF_EXCEPTION(scope, false);
        length = static_cast<double>(view->length());
    } else {
        JSValue written = iterated->get(globalObject, vm.propertyNames->length);
        RETURN_IF_EXCEPTION(scope, false);
        length = written.toLength(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (static_cast<double>(index) >= length) {
        iterator->setIndex(JSArrayIterator::doneIndex);
        return false;
    }

    iterator->setIndex(index + 1);
    IterationKind kind = iterator->kind();
    if (kind == IterationKind::Keys) {
        value = jsNumber(index);
        return true;
    }
    JSValue element = iterated->get(globalObject, static_cast<uint64_t>(index));
    RETURN_IF_EXCEPTION(scope, false);
    if (kind != IterationKind::Entries) {
        value = element;
        return true;
    }
    value = constructArrayPair(globalObject, jsNumber(index), element);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTStraightStepArrayIterator, UGPRPair, (Instance* instance, JSCell* iterator))
{
    constexpr Entry entry = Entry::operationAOTStraightStepArrayIterator;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue value;
    bool hasValue = stepArrayIteratorForStraightCode(globalObject, cellArgument<JSArrayIterator>(iterator, entry), value);
    OPERATION_RETURN(scope, doneAndValueOfStraightStep(hasValue, value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStraightStepMapIterator, UGPRPair, (Instance* instance, JSCell* iterator))
{
    constexpr Entry entry = Entry::operationAOTStraightStepMapIterator;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue value;
    bool hasValue = cellArgument<JSMapIterator>(iterator, entry)->next(globalObject, value);
    OPERATION_RETURN(scope, doneAndValueOfStraightStep(hasValue, value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStraightStepSetIterator, UGPRPair, (Instance* instance, JSCell* iterator))
{
    constexpr Entry entry = Entry::operationAOTStraightStepSetIterator;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue value;
    bool hasValue = cellArgument<JSSetIterator>(iterator, entry)->next(globalObject, value);
    OPERATION_RETURN(scope, doneAndValueOfStraightStep(hasValue, value));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
