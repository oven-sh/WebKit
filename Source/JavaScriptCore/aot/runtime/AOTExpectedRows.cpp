/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObjectBuilder.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include <cstddef>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/SymbolImpl.h>

struct JSC__AOT__Name {
    const uint8_t* utf8;
    uint32_t length;
};

struct JSC__AOT__FieldValue {
    enum class Kind : uint32_t { Boxed, Int32, Double, Boolean };

    Kind kind;
    union {
        JSC::EncodedJSValue boxed;
        int32_t int32;
        double number;
        bool boolean;
    };
};
static_assert(sizeof(JSC__AOT__FieldValue) == 16 && offsetof(JSC__AOT__FieldValue, boxed) == 8, "FieldValue in Bun's src/sql_jsc/shared/RowDecoder.rs is #[repr(C, u32)] with the same variants in the same order");

namespace JSC { namespace AOT {

namespace {

SymbolImpl::StaticSymbolImpl expectedResultTypePrivateName { "expectedResultType", SymbolImpl::s_flagIsPrivate };

PropertyName expectedResultTypeName()
{
    return PropertyName(&static_cast<SymbolImpl&>(expectedResultTypePrivateName));
}

bool tryStore(ObjectBuilder& builder, unsigned index, const JSC__AOT__FieldValue& value)
{
    switch (value.kind) {
    case JSC__AOT__FieldValue::Kind::Boxed:
        return builder.tryStore(index, JSValue::decode(value.boxed));
    case JSC__AOT__FieldValue::Kind::Int32:
        return builder.tryStoreInt32(index, value.int32);
    case JSC__AOT__FieldValue::Kind::Double:
        return builder.tryStoreDouble(index, value.number);
    case JSC__AOT__FieldValue::Kind::Boolean:
        return builder.tryStoreBoolean(index, value.boolean);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTExpectRows, void, (Instance* instance, EncodedJSValue encodedTarget, uint32_t shape))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTExpectRows);
    JSValue target = JSValue::decode(encodedTarget);
    auto* object = dynamicDowncast<JSObjectWithButterfly>(target);
    if (!object || !instance->state->objects->layouts.hasLayoutForShape(shape))
        return;
    if (object->structure()->hasImmutableProperties())
        return;
    object->putDirect(vm, expectedResultTypeName(), jsNumber(shape), static_cast<unsigned>(PropertyAttribute::DontEnum));
}

extern "C" JS_EXPORT_PRIVATE uint32_t JSC__AOT__expectedResultType(JSGlobalObject* globalObject, EncodedJSValue encodedTarget)
{
    Instance* instance = Instance::of(globalObject);
    JSValue target = JSValue::decode(encodedTarget);
    if (!instance || !instance->state || !target || !target.isObject())
        return 0;
    JSValue mark = asObject(target)->getDirect(globalObject->vm(), expectedResultTypeName());
    if (!mark || !mark.isInt32() || mark.asInt32() <= 0)
        return 0;
    return mark.asInt32();
}

extern "C" JS_EXPORT_PRIVATE void* JSC__AOT__ObjectBuilderPlan__create(JSGlobalObject* globalObject, uint32_t expectedResultType, const JSC__AOT__Name* names, uint32_t count)
{
    Instance* instance = Instance::of(globalObject);
    if (!instance || !instance->state)
        return nullptr;

    State& state = *instance->state;
    const RuntimeLayoutTable& layouts = state.objects->layouts;
    if (!layouts.hasLayoutForShape(expectedResultType))
        return nullptr;

    Vector<AtomString, 16> atoms;
    Vector<UniquedStringImpl*, 16> uids;
    atoms.reserveInitialCapacity(count);
    uids.reserveInitialCapacity(count);
    for (const JSC__AOT__Name& entry : std::span { names, count }) {
        AtomString atom = AtomString::fromUTF8(std::span { byteCast<char>(entry.utf8), entry.length });
        if (atom.isNull())
            return nullptr;
        uids.append(atom.impl());
        atoms.append(WTF::move(atom));
    }

    return ObjectBuilder::tryCreatePlan(state, layouts.layoutForShape(expectedResultType), uids.span()).release();
}

extern "C" JS_EXPORT_PRIVATE void JSC__AOT__ObjectBuilderPlan__destroy(void* plan)
{
    delete static_cast<ObjectBuilderPlan*>(plan);
}

extern "C" JS_EXPORT_PRIVATE EncodedJSValue JSC__AOT__ObjectBuilderPlan__createObject(JSGlobalObject* globalObject, const void* opaquePlan, const JSC__AOT__FieldValue* fieldValues)
{
    auto& plan = *static_cast<const ObjectBuilderPlan*>(opaquePlan);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    std::span values { fieldValues, plan.numberOfFields() };
    ObjectBuilder builder(vm, plan);
    for (unsigned index = 0; index < values.size(); ++index) {
        if (!tryStore(builder, index, values[index]))
            return { };
    }
    JSObject* object = builder.finish();
    if (scope.exception()) [[unlikely]] {
        scope.clearExceptionExceptTermination();
        return { };
    }
    return object ? JSValue::encode(object) : EncodedJSValue { };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
