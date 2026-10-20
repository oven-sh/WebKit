/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTStubDescriptor.h"
#include "AOTStubWords.h"

namespace JSC { namespace AOT {

enum class BoxedCallForm : uint8_t { ArrayIncludes, ArrayIndexOf, ArrayIsArray, ArrayPush, ArraySlice, FunctionBind, IsNaN, MapDelete, MapGet, MapHas, MapSet, ObjectIs, ObjectSetPrototypeOf, SetAdd, SetDelete, SetHas, StringIndexOf, StringStartsWith, WeakMapGet, WeakMapHas, WeakMapSet, WeakSetHas };

constexpr BoxedCallForm boxedCallFormOf(BuiltinBoxedStub stub)
{
    switch (stub) {
    case BuiltinBoxedStub::ArrayIncludes:
        return BoxedCallForm::ArrayIncludes;
    case BuiltinBoxedStub::ArrayIndexOf:
        return BoxedCallForm::ArrayIndexOf;
    case BuiltinBoxedStub::ArrayIsArray:
        return BoxedCallForm::ArrayIsArray;
    case BuiltinBoxedStub::ArrayPush:
        return BoxedCallForm::ArrayPush;
    case BuiltinBoxedStub::ArraySlice:
        return BoxedCallForm::ArraySlice;
    case BuiltinBoxedStub::FunctionBind:
        return BoxedCallForm::FunctionBind;
    case BuiltinBoxedStub::IsNaN:
        return BoxedCallForm::IsNaN;
    case BuiltinBoxedStub::MapDelete:
        return BoxedCallForm::MapDelete;
    case BuiltinBoxedStub::MapGet:
        return BoxedCallForm::MapGet;
    case BuiltinBoxedStub::MapHas:
        return BoxedCallForm::MapHas;
    case BuiltinBoxedStub::MapSet:
        return BoxedCallForm::MapSet;
    case BuiltinBoxedStub::ObjectIs:
        return BoxedCallForm::ObjectIs;
    case BuiltinBoxedStub::ObjectSetPrototypeOf:
        return BoxedCallForm::ObjectSetPrototypeOf;
    case BuiltinBoxedStub::SetAdd:
        return BoxedCallForm::SetAdd;
    case BuiltinBoxedStub::SetDelete:
        return BoxedCallForm::SetDelete;
    case BuiltinBoxedStub::SetHas:
        return BoxedCallForm::SetHas;
    case BuiltinBoxedStub::StringIndexOf:
        return BoxedCallForm::StringIndexOf;
    case BuiltinBoxedStub::StringStartsWith:
        return BoxedCallForm::StringStartsWith;
    case BuiltinBoxedStub::WeakMapGet:
        return BoxedCallForm::WeakMapGet;
    case BuiltinBoxedStub::WeakMapHas:
        return BoxedCallForm::WeakMapHas;
    case BuiltinBoxedStub::WeakMapSet:
        return BoxedCallForm::WeakMapSet;
    case BuiltinBoxedStub::WeakSetHas:
        return BoxedCallForm::WeakSetHas;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool acceptsArgumentCount(BoxedCallForm form, unsigned count)
{
    switch (form) {
    case BoxedCallForm::ArrayIncludes:
    case BoxedCallForm::ArrayIndexOf:
    case BoxedCallForm::ArrayIsArray:
    case BoxedCallForm::ArrayPush:
    case BoxedCallForm::IsNaN:
    case BoxedCallForm::MapDelete:
    case BoxedCallForm::MapGet:
    case BoxedCallForm::MapHas:
    case BoxedCallForm::SetAdd:
    case BoxedCallForm::SetDelete:
    case BoxedCallForm::SetHas:
    case BoxedCallForm::StringIndexOf:
    case BoxedCallForm::StringStartsWith:
    case BoxedCallForm::WeakMapGet:
    case BoxedCallForm::WeakMapHas:
    case BoxedCallForm::WeakSetHas:
        return count == 1;
    case BoxedCallForm::MapSet:
    case BoxedCallForm::ObjectIs:
    case BoxedCallForm::ObjectSetPrototypeOf:
    case BoxedCallForm::WeakMapSet:
        return count == 2;
    case BoxedCallForm::ArraySlice:
        return count <= 2;
    case BoxedCallForm::FunctionBind:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr StubKey callBoxedBuiltin(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallBoxedBuiltin).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey weakMapSet() { return StubKey(StubFamily::WeakMapSet); }
constexpr StubKey stringStartsWith() { return StubKey(StubFamily::StringStartsWith); }
constexpr StubKey stringIndexOf() { return StubKey(StubFamily::StringIndexOf); }

// nativeFunction has the word at NativeExecutable::offsetOfNativeFunctionFor(CodeForCall) of the callee. The registers of the call are in place and stay so.
// Falls through when the function has no boxed call for this count. Overwrites entry and scratch.
void emitBoxedCallDispatch(StubAssembler&, GPRReg nativeFunction, unsigned count, GPRReg entry, GPRReg scratch);

static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms == static_cast<unsigned>(BoxedCallForm::WeakSetHas) + 1);
static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).minCount == BoxedCallEntry::minArgumentCount && descriptorOf(StubFamily::CallBoxedBuiltin).maxCount == BoxedCallEntry::maxArgumentCount);
static_assert(numberOfBuiltinBoxedStubs * 2 <= StubWords::numberOfBoxedCallEntries);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
