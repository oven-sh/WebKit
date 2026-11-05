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
#include "AOTInstance.h"
#include "AOTMembersLeftOut.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "AOTStubsCompiledMethods.h"
#include "BuiltinExecutables.h"
#include "ConstructData.h"
#include "FunctionExecutable.h"
#include "JITCode.h"
#include "JSCInlines.h"
#include "LLIntData.h"
#include "NativeExecutable.h"
#include "TopExceptionScope.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

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

JSValue evaluateEveryBuiltinModuleForTesting(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Instance* instance = Instance::of(globalObject);
    JSArray* asked = constructEmptyArray(globalObject, nullptr);
    if (scope.exception() || !asked) [[unlikely]] {
        scope.clearException();
        return jsUndefined();
    }
    if (!instance || !instance->state)
        return asked;
    Modules& modules = *instance->state->modules;
    for (unsigned index = 0; index < modules.table().size(); ++index) {
        ModuleID module(index);
        if (modules.table().data(module).kind != ModuleKind::RuntimeSource)
            continue;
        modules.require(module);
        String line = modules.table().pathText(module);
        if (Exception* exception = scope.exception()) {
            JSValue thrown = exception->value();
            scope.clearException();
            String text = thrown.toWTFString(globalObject);
            scope.clearException();
            line = makeString(line, ' ', text);
        }
        asked->push(globalObject, jsString(vm, line));
        scope.clearException();
    }
    return asked;
}

void fillWordsOfCompiledMethodsOfArray(Instance& instance)
{
    const ProgramTables& tables = instance.tables();
    ModuleTable modules(tables);
    for (const RuntimeFunctionData& function : BuiltinTable::singleton().runtimeFunctions()) {
        if (function.role != RuntimeFunctionRole::EngineBuiltin)
            continue;
        for (BuiltinID builtin : function.implements) {
            std::optional<CompiledMethodOfArray> method = compiledMethodOfArray(builtin);
            const CompiledRuntimeFunctionData* entry = method ? modules.findRuntimeFunction(function.id.index()) : nullptr;
            if (entry && tables.closureFunctions.first <= entry->function && entry->function < tables.closureFunctions.end)
                instance.stubs.wordsOfCompiledMethodsOfArray[static_cast<unsigned>(*method)] = instance.functionTables.closureWords[tables.closureNumberOf(entry->function)];
        }
    }
}

bool hasBuiltinFunction(VM&, uint32_t id)
{
    const Image* image = Image::current();
    return image && ModuleTable(image->programTables()).findRuntimeFunction(id);
}

static std::optional<FunctionID> findCompiledFunction(JSGlobalObject* globalObject, uint32_t id)
{
    Instance* instance = Instance::of(globalObject);
    const CompiledRuntimeFunctionData* entry = instance ? ModuleTable(instance->tables()).findRuntimeFunction(id) : nullptr;
    return entry ? std::optional(FunctionID(entry->function)) : std::nullopt;
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
        , function(function)
    {
    }

    const FunctionID function;
};

} // anonymous namespace

static FunctionID compiledBuiltinFunctionID(const NativeExecutable& executable)
{
    return static_cast<const CompiledBuiltinJITCode&>(executable.generatedJITCodeForCall().get()).function;
}

static JSObject* compiledClosureOfCallee(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    auto* executable = uncheckedDowncast<NativeExecutable>(uncheckedDowncast<JSFunction>(callFrame->jsCallee())->executable());
    return asObject(State::of(globalObject).objects->closures.closure(globalObject->vm(), compiledBuiltinFunctionID(*executable)));
}

static JSC_DECLARE_HOST_FUNCTION(callCompiledBuiltin);
static JSC_DECLARE_HOST_FUNCTION(constructCompiledBuiltin);

std::optional<FunctionID> compiledBuiltinFunctionID(const JSCell* cell)
{
    if (cell->type() != JSFunctionType)
        return std::nullopt;
    auto* executable = dynamicDowncast<NativeExecutable>(uncheckedDowncast<JSFunction>(cell)->executable());
    if (!executable || executable->function() != toTagged(callCompiledBuiltin))
        return std::nullopt;
    return compiledBuiltinFunctionID(*executable);
}

JSC_DEFINE_HOST_FUNCTION(callCompiledBuiltin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* closure = compiledClosureOfCallee(globalObject, callFrame);
    return JSValue::encode(JSC::call(globalObject, closure, JSC::getCallData(closure), callFrame->thisValue(), ArgList(callFrame)));
}

JSC_DEFINE_HOST_FUNCTION(constructCompiledBuiltin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* closure = compiledClosureOfCallee(globalObject, callFrame);
    if (RealmWords& words = Instance::of(globalObject)->realm; !words.functionOfEngineOverCompiledFunction) [[unlikely]] {
        words.compiledFunctionUnderFunctionOfEngine = closure;
        words.functionOfEngineOverCompiledFunction = callFrame->jsCallee();
    }
    return JSValue::encode(JSC::constructAllowingPrimitiveResult(globalObject, closure, JSC::getConstructData(closure), ArgList(callFrame), callFrame->newTarget()));
}

NativeExecutable* builtinExecutable(JSGlobalObject* globalObject, uint32_t id)
{
    std::optional<FunctionID> function = findCompiledFunction(globalObject, id);
    if (!function)
        return nullptr;
    return NativeExecutable::create(globalObject->vm(),
        adoptRef(*new CompiledBuiltinJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_native_call_trampoline), *function)), toTagged(callCompiledBuiltin),
        adoptRef(*new NativeJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_native_construct_trampoline), JITType::HostCallThunk, NoIntrinsic)), toTagged(FunctionTables::canConstruct(functionKindData(*function)) ? constructCompiledBuiltin : callHostFunctionAsConstructor),
        ImplementationVisibility::Private, functionLengthOf(*function), initialNameProperty(*function));
}

bool refusesLibraryText()
{
    const Image* image = Image::current();
    return image && !image->admitsLibraryText();
}

bool refusesLibraryText(JSGlobalObject* realm)
{
    return Instance::of(realm) && refusesLibraryText();
}

static std::optional<uint32_t> builtinFunctionHolding(const FunctionExecutable& executable)
{
    return executable.source().provider()->builtinFunctionAt(executable.source().startOffset());
}

static bool isTextOfHostLibrary(ScriptExecutable& executable)
{
    auto* function = dynamicDowncast<FunctionExecutable>(&executable);
    if (!function || !function->isBuiltinFunction() || function->unlinkedExecutable()->isBuiltinDefaultClassConstructor())
        return false;
    std::optional<uint32_t> id = builtinFunctionHolding(*function);
    return !id || *id >= numberOfBuiltinCodes;
}

static Lock s_functionsOfTestRunnerLock;
static Vector<uint32_t, 1>& functionsOfTestRunner() WTF_REQUIRES_LOCK(s_functionsOfTestRunnerLock)
{
    static NeverDestroyed<Vector<uint32_t, 1>> functions;
    return functions;
}

void admitFunctionOfTestRunner(VM& vm, FunctionExecutable& function)
{
    if (!vm.hasCompiledCode())
        return;
    std::optional<uint32_t> id = builtinFunctionHolding(function);
    Locker locker { s_functionsOfTestRunnerLock };
    if (id && !functionsOfTestRunner().contains(*id))
        functionsOfTestRunner().append(*id);
}

static bool isOfTestRunner(ScriptExecutable& executable)
{
    std::optional<uint32_t> id = builtinFunctionHolding(*uncheckedDowncast<FunctionExecutable>(&executable));
    Locker locker { s_functionsOfTestRunnerLock };
    return id && functionsOfTestRunner().contains(*id);
}

bool refusesToInterpret(JSGlobalObject* realm, ScriptExecutable& executable)
{
    return refusesLibraryText(realm) && isTextOfHostLibrary(executable) && !isOfTestRunner(executable);
}

bool refusesToRun(JSFunction* function)
{
    auto* executable = dynamicDowncast<FunctionExecutable>(function->executable());
    return executable && refusesToInterpret(function->realm(), *executable);
}

bool isNeededByNothingThatRuns(JSFunction* function)
{
    auto* executable = dynamicDowncast<FunctionExecutable>(function->executable());
    if (!executable || !Instance::of(function->realm()) || !isTextOfHostLibrary(*executable))
        return false;
    return !Image::current()->admitsLibraryText();
}

static Vector<String, 4> globalBuiltinsThatRun(RuntimeFunctionID function)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    Vector<String, 4> names;
    if (!table.contains(function))
        return names;

    auto add = [&](BuiltinID builtin) {
        if (table.isUnder(builtin, Builtins::GlobalThis) && !names.contains(table.textOf(builtin)))
            names.append(table.textOf(builtin));
    };
    for (BuiltinID builtin : table.at(function).implements)
        add(builtin);
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        for (const BuiltinCalledFunction& called : table.calledFunctionsOf(BuiltinID(index))) {
            if (called.function == function && !(called.condition && called.condition->isAboutWhatProgramReplaces()))
                add(called.builtin);
        }
    }
    return names;
}

void throwBuiltinNotCompiled(JSGlobalObject* realm, const String& builtin)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    JSObject* error = createBuiltinNotCompiledError(realm, builtin);
    noteErrorOfCodeLeftOut(realm, error);
    throwException(realm, scope, error);
}

void throwLibraryFunctionNotCompiled(JSGlobalObject* realm, ScriptExecutable& executable)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    auto& function = *uncheckedDowncast<FunctionExecutable>(&executable);
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<uint32_t> id = builtinFunctionHolding(function);
    Vector<String, 4> names = id ? globalBuiltinsThatRun(RuntimeFunctionID(*id)) : Vector<String, 4>();
    if (id && std::ranges::contains(table.moduleLoaderFunctions(), RuntimeFunctionID(*id)))
        names = { "The module loader"_str };
    JSObject* error = names.size() == 1 ? createBuiltinNotCompiledError(realm, names[0]) : names.isEmpty() ? createBuiltinFunctionNotCompiledError(realm, function.name().string()) : createBuiltinsNotCompiledError(realm, names.span());
    bool isGetterOfAccessor = id && table.contains(RuntimeFunctionID(*id)) && std::ranges::any_of(table.at(RuntimeFunctionID(*id)).implements, [&](BuiltinID builtin) {
        return table.at(builtin).kind == BuiltinKind::Accessor;
    });
    if (!isGetterOfAccessor)
        noteErrorOfCodeLeftOut(realm, error);
    throwException(realm, scope, error);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
