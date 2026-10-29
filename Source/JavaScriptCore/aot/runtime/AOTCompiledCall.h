/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTSavedFrame.h"
#include "AOTSlimClosure.h"
#include "ArgList.h"
#include "CallData.h"
#include "JSCJSValue.h"
#include "LLIntThunks.h"

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class VM;

namespace AOT {

JS_EXPORT_PRIVATE JSValue callCompiledCode(VM&, JSObject* callee, const CallData&, JSValue thisValue, JSCell* context, const ArgList&);
JS_EXPORT_PRIVATE JSValue callCompiledCode(JSGlobalObject*, JSCell* closure, JSValue thisValue, const ArgList&);
JS_EXPORT_PRIVATE JSValue constructCompiledCode(VM&, JSObject* constructor, const CallData&, const ArgList&, JSValue newTarget);

ALWAYS_INLINE CallData compiledCallDataOf(JSCell* cell)
{
    switch (cell->type()) {
    case CompiledClosureType:
        return Closure::getCallData(cell);
    case SlimClosureType:
        return SlimClosure::getCallData(cell);
    case SlimClosureWithPropertiesType:
        return ReifiedSlimClosure::getCallData(cell);
    case ObjectType:
        if (cell->classInfo() == SavedFrame::info())
            return SavedFrame::getCallData(cell);
        return { };
    default:
        return { };
    }
}

template<typename... Args>
ALWAYS_INLINE JSValue enterCompiledCode(VM& vm, const CallData& callData, JSObject* callee, JSValue thisValue, JSCell* context, Args... args)
{
    ASSERT(callData.type == CallData::Type::Compiled);
    void* entry = const_cast<void*>(callData.compiled.entrypoint);
    if constexpr (!sizeof...(args))
        return JSValue::decode(vmEntryToJavaScriptWith0Arguments(entry, &vm, nullptr, callee, thisValue, context));
    else if constexpr (sizeof...(args) == 1)
        return JSValue::decode(vmEntryToJavaScriptWith1Arguments(entry, &vm, nullptr, callee, thisValue, context, args...));
    else if constexpr (sizeof...(args) == 2)
        return JSValue::decode(vmEntryToJavaScriptWith2Arguments(entry, &vm, nullptr, callee, thisValue, context, args...));
    else if constexpr (sizeof...(args) == 3)
        return JSValue::decode(vmEntryToJavaScriptWith3Arguments(entry, &vm, nullptr, callee, thisValue, context, args...));
    else if constexpr (sizeof...(args) == 4)
        return JSValue::decode(vmEntryToJavaScriptWith4Arguments(entry, &vm, nullptr, callee, thisValue, context, args...));
    else if constexpr (sizeof...(args) == 5)
        return JSValue::decode(vmEntryToJavaScriptWith5Arguments(entry, &vm, nullptr, callee, thisValue, context, args...));
    else {
        static_assert(sizeof...(args) == 6);
        return JSValue::decode(vmEntryToJavaScriptWith6Arguments(entry, &vm, nullptr, callee, thisValue, context, args...));
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
