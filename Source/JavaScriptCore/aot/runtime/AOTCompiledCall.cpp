/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiledCall.h"

#if ENABLE(AOT)

#include "Interpreter.h"
#include "JSCInlines.h"
#include "LLIntThunks.h"
#include "ProtoCallFrameInlines.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"

namespace JSC { namespace AOT {

static ALWAYS_INLINE JSValue enterWithArgumentList(VM& vm, const CallData& callData, JSObject* callee, JSValue thisSlot, JSCell* context, const ArgList& args)
{
    switch (args.size()) {
    case 0:
        return enterCompiledCode(vm, callData, callee, thisSlot, context);
    case 1:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0));
    case 2:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1));
    case 3:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2));
    case 4:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2), args.at(3));
    case 5:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2), args.at(3), args.at(4));
    case 6:
        return enterCompiledCode(vm, callData, callee, thisSlot, context, args.at(0), args.at(1), args.at(2), args.at(3), args.at(4), args.at(5));
    default:
        break;
    }
    ProtoCallFrame protoCallFrame;
    protoCallFrame.init(nullptr, callData.compiled.realm, callee, thisSlot, context, 1 + args.size(), args.data());
    return JSValue::decode(vmEntryToJavaScript(const_cast<void*>(callData.compiled.entrypoint), &vm, &protoCallFrame));
}

JSValue callCompiledCode(VM& vm, JSObject* callee, const CallData& callData, JSValue thisValue, JSCell* context, const ArgList& args)
{
    ASSERT(callData.type == CallData::Type::Compiled);

    auto clobberizeValidator = makeScopeExit([&] {
        vm.didEnterVM = true;
    });

    auto scope = DECLARE_THROW_SCOPE(vm);
    scope.assertNoException();
    ASSERT(!vm.isCollectorBusyOnCurrentThread());

    JSGlobalObject* realm = callData.compiled.realm;

    VMEntryScope entryScope(vm, realm);
    if (!vm.isSafeToRecurseSoft() || args.size() > maxArguments) [[unlikely]]
        return throwStackOverflowError(realm, scope);

    if (vm.disallowVMEntryCount) [[unlikely]]
        return VM::checkVMEntryPermission();

    scope.release();
    return enterWithArgumentList(vm, callData, callee, thisValue, context, args);
}

JSValue callCompiledCode(JSGlobalObject* realm, JSCell* closure, JSValue thisValue, const ArgList& arguments)
{
    CallData callData = JSC::getCallData(closure);
    RELEASE_ASSERT(callData.type == CallData::Type::Compiled);
    return callCompiledCode(realm->vm(), asObject(closure), callData, thisValue, nullptr, arguments);
}

JSObject* constructCompiledCode(VM& vm, JSObject* constructor, const CallData& constructData, const ArgList& args, JSValue newTarget)
{
    ASSERT(constructData.type == CallData::Type::Compiled);

    auto clobberizeValidator = makeScopeExit([&] {
        vm.didEnterVM = true;
    });

    auto throwScope = DECLARE_THROW_SCOPE(vm);
    throwScope.assertNoException();
    ASSERT(!vm.isCollectorBusyOnCurrentThread());

    JSGlobalObject* realm = constructData.compiled.realm;

    VMEntryScope entryScope(vm, realm);
    if (!vm.isSafeToRecurseSoft() || args.size() > maxArguments) [[unlikely]] {
        throwStackOverflowError(realm, throwScope);
        return nullptr;
    }

    if (vm.disallowVMEntryCount) [[unlikely]] {
        VM::checkVMEntryPermission();
        return realm->globalThis();
    }

    JSValue result = enterWithArgumentList(vm, constructData, constructor, newTarget, nullptr, args);
    RETURN_IF_EXCEPTION(throwScope, nullptr);
    return asObject(result);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
