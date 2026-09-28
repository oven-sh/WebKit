/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "TaggedArithmetic.h"

#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC {

static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticAdd);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticSub);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticMul);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticDiv);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticToFloat);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticIsInt);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticNewArray);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticIsWholeFloat);
static JSC_DECLARE_HOST_FUNCTION(taggedArithmeticEncodeAsDouble);

JSC_DEFINE_HOST_FUNCTION(taggedArithmeticAdd, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(taggedAdd(callFrame->argument(0), callFrame->argument(1)));
}

JSC_DEFINE_HOST_FUNCTION(taggedArithmeticSub, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(taggedSub(callFrame->argument(0), callFrame->argument(1)));
}

JSC_DEFINE_HOST_FUNCTION(taggedArithmeticMul, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(taggedMul(callFrame->argument(0), callFrame->argument(1)));
}

JSC_DEFINE_HOST_FUNCTION(taggedArithmeticDiv, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(taggedDiv(callFrame->argument(0), callFrame->argument(1)));
}

JSC_DEFINE_HOST_FUNCTION(taggedArithmeticToFloat, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(toTaggedFloat(callFrame->argument(0)));
}

JSC_DEFINE_HOST_FUNCTION(taggedArithmeticIsInt, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(jsBoolean(!!taggedInteger(callFrame->argument(0))));
}

// For tests.
JSC_DEFINE_HOST_FUNCTION(taggedArithmeticIsWholeFloat, (JSGlobalObject*, CallFrame* callFrame))
{
    return JSValue::encode(jsBoolean(callFrame->argument(0).isWholeFloat()));
}

// For tests: what JavaScript may make of a number at any time.
JSC_DEFINE_HOST_FUNCTION(taggedArithmeticEncodeAsDouble, (JSGlobalObject*, CallFrame* callFrame))
{
    JSValue value = callFrame->argument(0);
    if (!value.isNumber() || value.isWholeFloat())
        return JSValue::encode(value);
    return JSValue::encode(jsDoubleNumber(value.asNumber()));
}

JSC_DEFINE_HOST_FUNCTION(taggedArithmeticNewArray, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    RELEASE_AND_RETURN(scope, JSValue::encode(constructArray(globalObject, structure, ArgList(callFrame))));
}

JSObject* createTaggedArithmeticObject(VM& vm, JSGlobalObject* globalObject)
{
    JSObject* object = constructEmptyObject(globalObject);
    auto add = [&] (ASCIILiteral name, unsigned length, NativeFunction function, Intrinsic intrinsic) {
        object->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, name), length, function, ImplementationVisibility::Public, intrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    };
    add("add"_s, 2, taggedArithmeticAdd, TaggedAddIntrinsic);
    add("sub"_s, 2, taggedArithmeticSub, TaggedSubIntrinsic);
    add("mul"_s, 2, taggedArithmeticMul, TaggedMulIntrinsic);
    add("div"_s, 2, taggedArithmeticDiv, TaggedDivIntrinsic);
    add("toFloat"_s, 1, taggedArithmeticToFloat, NoIntrinsic);
    add("isInt"_s, 1, taggedArithmeticIsInt, IsTaggedIntIntrinsic);
    add("isWholeFloat"_s, 1, taggedArithmeticIsWholeFloat, NoIntrinsic);
    add("encodeAsDouble"_s, 1, taggedArithmeticEncodeAsDouble, NoIntrinsic);
    add("newArray"_s, 0, taggedArithmeticNewArray, NewContiguousArrayIntrinsic);
    return object;
}

} // namespace JSC
