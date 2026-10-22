/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTMembersLeftOut.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "BuiltinExecutables.h"
#include "ConstructData.h"
#include "Error.h"
#include "FunctionExecutable.h"
#include "JITCode.h"
#include "JSCInlines.h"
#include "LLIntData.h"
#include "NativeExecutable.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BuiltinModuleTable);

BuiltinModuleTable::BuiltinModuleTable(State& state)
    : m_state(state)
{
}

std::optional<ModuleID> BuiltinModuleTable::find(uint32_t registryID) const
{
    return m_state.modules->table().findRuntimeSource(registryID);
}

JSValue BuiltinModuleTable::evaluate(ModuleID module)
{
    return m_state.modules->require(module);
}

bool hasBuiltinModule(JSGlobalObject* globalObject, uint32_t id)
{
    Instance* instance = Instance::of(globalObject);
    return instance && instance->state && instance->state->builtinModules->find(id);
}

JSValue evaluateBuiltinModule(JSGlobalObject* globalObject, uint32_t id)
{
    Instance* instance = Instance::of(globalObject);
    RELEASE_ASSERT(instance && instance->state);
    BuiltinModuleTable& builtinModules = *instance->state->builtinModules;
    auto module = builtinModules.find(id);
    RELEASE_ASSERT(module);
    return builtinModules.evaluate(*module);
}

bool hasBuiltinFunction(VM&, uint32_t id)
{
    const Image* image = Image::current();
    return image && ModuleTable(image->programTables()).findRuntimeFunction(id);
}

static std::optional<FunctionID> findCompiledFunction(JSGlobalObject* globalObject, uint32_t id)
{
    Instance* instance = Instance::of(globalObject);
    if (!instance)
        return std::nullopt;
    const CompiledRuntimeFunctionData* entry = ModuleTable(instance->tables()).findRuntimeFunction(id);
    if (!entry)
        return std::nullopt;
    return FunctionID(entry->function);
}

JSFunction* builtinFunction(JSGlobalObject* globalObject, uint32_t id, JSObject* prototype)
{
    std::optional<FunctionID> function = findCompiledFunction(globalObject, id);
    if (!function)
        return nullptr;
    VM& vm = globalObject->vm();
    FunctionKindData kind = functionKindData(*function);
    RELEASE_ASSERT(kind.closureStructure != classObjectClosureStructure);
    Closure* closure = Closure::create(vm, ClosureTable::realmClosureStructure(vm, globalObject, kind.closureStructure), *function);
    if (prototype)
        closure->setPrototypeDirect(vm, prototype);
    return closure;
}

namespace {

class CompiledBuiltinJITCode final : public NativeJITCode {
public:
    CompiledBuiltinJITCode(CodeRef<JSEntryPtrTag> trampoline, FunctionID function)
        : NativeJITCode(trampoline, JITType::HostCallThunk, NoIntrinsic)
        , m_function(function)
    {
    }

    FunctionID function() const { return m_function; }

private:
    FunctionID m_function;
};

} // anonymous namespace

static NativeExecutable* executableOfCallee(CallFrame* callFrame)
{
    return uncheckedDowncast<NativeExecutable>(uncheckedDowncast<JSFunction>(callFrame->jsCallee())->executable());
}

static JSObject* compiledClosureOfCallee(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    FunctionID function = static_cast<const CompiledBuiltinJITCode&>(executableOfCallee(callFrame)->generatedJITCodeForCall().get()).function();
    return asObject(State::of(globalObject).objects->closures.closure(globalObject->vm(), function));
}

static JSC_DECLARE_HOST_FUNCTION(callCompiledBuiltin);
static JSC_DECLARE_HOST_FUNCTION(constructCompiledBuiltin);

JSC_DEFINE_HOST_FUNCTION(callCompiledBuiltin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* closure = compiledClosureOfCallee(globalObject, callFrame);
    return JSValue::encode(JSC::call(globalObject, closure, JSC::getCallData(closure), callFrame->thisValue(), ArgList(callFrame)));
}

JSC_DEFINE_HOST_FUNCTION(constructCompiledBuiltin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* closure = compiledClosureOfCallee(globalObject, callFrame);
    return JSValue::encode(JSC::construct(globalObject, closure, JSC::getConstructData(closure), ArgList(callFrame), callFrame->newTarget()));
}

NativeExecutable* builtinExecutable(JSGlobalObject* globalObject, uint32_t id)
{
    std::optional<FunctionID> function = findCompiledFunction(globalObject, id);
    if (!function)
        return nullptr;
    FunctionKindData kind = functionKindData(*function);
    // Private hides only the host function's frame. A stack trace shows the frame of the compiled function it calls.
    return NativeExecutable::create(globalObject->vm(),
        adoptRef(*new CompiledBuiltinJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_native_call_trampoline), *function)), toTagged(callCompiledBuiltin),
        adoptRef(*new NativeJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_native_construct_trampoline), JITType::HostCallThunk, NoIntrinsic)), toTagged(FunctionTables::canConstruct(kind) ? constructCompiledBuiltin : callHostFunctionAsConstructor),
        ImplementationVisibility::Private, functionLengthOf(*function), initialNameProperty(*function));
}

bool refusesLibraryText()
{
    const Image* image = Image::current();
    return image && !image->admitsLibraryText();
}

static std::optional<uint32_t> builtinFunctionHolding(const FunctionExecutable& executable)
{
    const SourceCode& source = executable.source();
    return source.provider()->builtinFunctionAt(source.startOffset());
}

static bool isTextOfHostLibrary(ScriptExecutable& executable)
{
    auto* function = dynamicDowncast<FunctionExecutable>(&executable);
    if (!function || !function->isBuiltinFunction() || function->unlinkedExecutable()->isBuiltinDefaultClassConstructor())
        return false;
    std::optional<uint32_t> id = builtinFunctionHolding(*function);
    return !id || *id >= numberOfBuiltinCodes;
}

bool refusesToInterpret(ScriptExecutable& executable)
{
    return refusesLibraryText() && isTextOfHostLibrary(executable);
}

bool refusesToRun(JSFunction* function)
{
    auto* executable = dynamicDowncast<FunctionExecutable>(function->executable());
    return executable && refusesToInterpret(*executable);
}

static Vector<String, 4> globalBuiltinsThatRun(RuntimeFunctionID function)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    Vector<String, 4> names;
    if (!table.contains(function))
        return names;

    // The built-in that a function implements also lists it among the functions it calls.
    auto add = [&](BuiltinID builtin) {
        if (!table.isUnder(builtin, Builtins::GlobalThis))
            return;
        String name = table.textOf(builtin);
        if (!names.contains(name))
            names.append(WTF::move(name));
    };
    for (BuiltinID builtin : table.at(function).implements)
        add(builtin);
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        for (const BuiltinCalledFunction& called : table.calledFunctionsOf(BuiltinID(index))) {
            if (called.function == function)
                add(called.builtin);
        }
    }
    return names;
}

static bool isGetterOfAccessor(RuntimeFunctionID function)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    return std::ranges::any_of(table.at(function).implements, [&](BuiltinID builtin) {
        return table.at(builtin).kind == BuiltinKind::Accessor;
    });
}

static void throwErrorOfCodeLeftOut(JSGlobalObject* realm, ThrowScope& scope, JSObject* error)
{
    noteErrorOfCodeLeftOut(realm, error);
    throwException(realm, scope, error);
}

void throwBuiltinNotCompiled(JSGlobalObject* realm, const String& builtin)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwErrorOfCodeLeftOut(realm, scope, createBuiltinNotCompiledError(realm, builtin));
}

void throwLibraryFunctionNotCompiled(JSGlobalObject* realm, ScriptExecutable& executable)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    auto& function = *uncheckedDowncast<FunctionExecutable>(&executable);
    Vector<String, 4> names;
    std::optional<uint32_t> id = builtinFunctionHolding(function);
    if (id)
        names = globalBuiltinsThatRun(RuntimeFunctionID(*id));
    // Reading an accessor is calling its getter. What it answers may be a number, and a function in the place of a number is a wrong answer: the read is refused.
    if (id && isGetterOfAccessor(RuntimeFunctionID(*id))) {
        throwException(realm, scope, names.size() == 1 ? createBuiltinNotCompiledError(realm, names[0]) : names.isEmpty() ? createBuiltinFunctionNotCompiledError(realm, function.name().string()) : createBuiltinsNotCompiledError(realm, names.span()));
        return;
    }
    if (names.isEmpty())
        throwErrorOfCodeLeftOut(realm, scope, createBuiltinFunctionNotCompiledError(realm, function.name().string()));
    else if (names.size() == 1)
        throwErrorOfCodeLeftOut(realm, scope, createBuiltinNotCompiledError(realm, names[0]));
    else
        throwErrorOfCodeLeftOut(realm, scope, createBuiltinsNotCompiledError(realm, names.span()));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
