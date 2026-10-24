/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObjectBuilder.h"
#include "AOTObjectCopy.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "CustomGetterSetter.h"
#include "ErrorInstance.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "RegExp.h"
#include "TopExceptionScope.h"
#include <cstddef>
#include <span>
#include <wtf/StdLibExtras.h>
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

SymbolImpl::StaticSymbolImpl deferredReportPrivateName { "deferredReport", SymbolImpl::s_flagIsPrivate };
SymbolImpl::StaticSymbolImpl reportedValuePrivateName { "reportedValue", SymbolImpl::s_flagIsPrivate };

PropertyName hiddenName(SymbolImpl::StaticSymbolImpl& name)
{
    return PropertyName(&static_cast<SymbolImpl&>(name));
}

constexpr unsigned maxDepthOfCopiedData = 32;
constexpr unsigned maxObjectsOfCopiedData = 1024;

// A copy of a value made of plain objects, arrays and primitives. Making it runs nothing of the program. Empty for any other value.
JSValue tryCopyPlainData(JSGlobalObject* realm, JSValue value, unsigned remainingDepth, unsigned& remainingObjects)
{
    if (!value.isObject())
        return value;
    if (!remainingDepth || !remainingObjects--)
        return { };

    VM& vm = realm->vm();
    if (isPlainDataObject(value)) {
        JSObject* object = asObject(value);
        Structure* structure = object->structure();
        PropertyOffset lastOffset = structure->maxOffset();
        if (structure->realm() != realm || (lastOffset != invalidOffset && !isInlineOffset(lastOffset)))
            return { };
        JSFinalObject* copy = JSFinalObject::create(vm, structure);
        for (PropertyOffset offset = 0; offset <= lastOffset; offset++) {
            JSValue member = object->getDirect(offset);
            if (member && member.isObject())
                member = tryCopyPlainData(realm, member, remainingDepth - 1, remainingObjects);
            if (!member)
                return { };
            copy->putDirectOffset(vm, offset, member);
        }
        return copy;
    }

    if (!isJSArray(value) || asObject(value)->structure()->realm() != realm)
        return { };
    JSArray* copy = tryCloneArrayFromFast<ArrayFillMode::Undefined>(realm, value);
    if (!copy)
        return { };
    if (!hasContiguous(copy->indexingType()))
        return hasInt32(copy->indexingType()) || hasDouble(copy->indexingType()) || !copy->length() ? copy : JSValue();
    for (unsigned index = 0; index < copy->length(); index++) {
        JSValue element = copy->getIndexQuickly(index);
        if (!element.isObject())
            continue;
        JSValue elementCopy = tryCopyPlainData(realm, element, remainingDepth - 1, remainingObjects);
        if (!elementCopy)
            return { };
        copy->setIndexQuickly(vm, index, elementCopy);
    }
    return copy;
}

JSC_DECLARE_CUSTOM_GETTER(deferredReportGetter);
JSC_DECLARE_HOST_FUNCTION(deferReport);
JSC_DECLARE_HOST_FUNCTION(matchesPattern);

bool isDeferred(VM& vm, JSObject* error, PropertyName name)
{
    JSValue held = error->getDirect(vm, name);
    auto* placeholder = held ? dynamicDowncast<CustomGetterSetter>(held) : nullptr;
    return placeholder && placeholder->getter() == deferredReportGetter;
}

// The first read of either property asks the library for both.
JSC_DEFINE_CUSTOM_GETTER(deferredReportGetter, (JSGlobalObject* realm, EncodedJSValue thisValue, PropertyName name))
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* error = asObject(JSValue::decode(thisValue));
    JSValue report = error->getDirect(vm, hiddenName(deferredReportPrivateName));
    MarkedArgumentBuffer arguments;
    arguments.append(error->getDirect(vm, hiddenName(reportedValuePrivateName)));
    JSValue messageAndIssues = call(realm, report, arguments, "The report of a refused value is a function"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // Nothing was called for one who only inquires.
    if (!messageAndIssues.isObject())
        return JSValue::encode(jsUndefined());

    JSValue message = messageAndIssues.get(realm, 0u);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue issues = messageAndIssues.get(realm, 1u);
    RETURN_IF_EXCEPTION(scope, { });
    Identifier issuesName = Identifier::fromString(vm, "issues"_s);
    if (isDeferred(vm, error, vm.propertyNames->message))
        error->putDirect(vm, vm.propertyNames->message, message, static_cast<unsigned>(PropertyAttribute::DontEnum));
    if (isDeferred(vm, error, issuesName))
        error->putDirect(vm, issuesName, issues);
    error->putDirect(vm, hiddenName(deferredReportPrivateName), jsUndefined(), static_cast<unsigned>(PropertyAttribute::DontEnum));
    error->putDirect(vm, hiddenName(reportedValuePrivateName), jsUndefined(), static_cast<unsigned>(PropertyAttribute::DontEnum));
    return JSValue::encode(error->getDirect(vm, name));
}

// deferReport(error, value, report): whether `message` and `issues` of the error are now what report(a copy of the value) will answer when one of them is first read.
JSC_DEFINE_HOST_FUNCTION(deferReport, (JSGlobalObject* realm, CallFrame* callFrame))
{
    VM& vm = realm->vm();
    auto* error = dynamicDowncast<ErrorInstance>(callFrame->argument(0));
    if (!error)
        return JSValue::encode(jsBoolean(false));
    unsigned remainingObjects = maxObjectsOfCopiedData;
    JSValue copy = tryCopyPlainData(realm, callFrame->argument(1), maxDepthOfCopiedData, remainingObjects);
    if (!copy)
        return JSValue::encode(jsBoolean(false));

    CustomGetterSetter* placeholder = CustomGetterSetter::create(vm, deferredReportGetter, nullptr);
    error->putDirectCustomAccessor(vm, vm.propertyNames->message, placeholder, PropertyAttribute::DontEnum | PropertyAttribute::CustomValue);
    error->putDirectCustomAccessor(vm, Identifier::fromString(vm, "issues"_s), placeholder, static_cast<unsigned>(PropertyAttribute::CustomValue));
    error->putDirect(vm, hiddenName(deferredReportPrivateName), callFrame->argument(2), static_cast<unsigned>(PropertyAttribute::DontEnum));
    error->putDirect(vm, hiddenName(reportedValuePrivateName), copy, static_cast<unsigned>(PropertyAttribute::DontEnum));
    return JSValue::encode(jsBoolean(true));
}

// matchesPattern(source, value): what new RegExp(source).test(value) answers. RegExp.$1 and its like stay what the last match of the program made them.
JSC_DEFINE_HOST_FUNCTION(matchesPattern, (JSGlobalObject* realm, CallFrame* callFrame))
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String source = asString(callFrame->argument(0))->value(realm);
    RETURN_IF_EXCEPTION(scope, { });
    RegExp* regExp = RegExp::create(vm, source, { });
    if (!regExp->isValid()) [[unlikely]]
        return throwVMError(realm, scope, regExp->errorToThrow(realm));
    JSString* text = callFrame->argument(1).toString(realm);
    RETURN_IF_EXCEPTION(scope, { });
    auto characters = text->view(realm);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(!!regExp->match(realm, characters, 0))));
}

bool tryStoreInExpectedRow(ObjectBuilder& builder, unsigned index, const JSC__AOT__FieldValue& value)
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetPreparedPlans, EncodedJSValue, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetPreparedPlans);
    DeferGCForAWhile deferGC(vm);
    JSCell*& kept = instance->realm.preparedPlans;
    if (!kept) [[unlikely]] {
        JSMap* map = JSMap::create(vm, globalObject->mapStructure());
        // A plan and a rule are JSON, which neither name is.
        map->set(globalObject, jsNontrivialString(vm, "deferReport"_s), JSFunction::create(vm, globalObject, 3, "deferReport"_s, deferReport, ImplementationVisibility::Private));
        map->set(globalObject, jsNontrivialString(vm, "matchesPattern"_s), JSFunction::create(vm, globalObject, 2, "matchesPattern"_s, matchesPattern, ImplementationVisibility::Private));
        vm.mutatorFence();
        kept = map;
    }
    OPERATION_RETURN(scope, JSValue::encode(kept));
}

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
        if (!tryStoreInExpectedRow(builder, index, values[index]))
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
