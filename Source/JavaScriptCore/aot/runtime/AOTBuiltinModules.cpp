/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptions.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTMembersLeftOut.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTState.h"
#include "AOTStubsCompiledMethods.h"
#include "BuiltinExecutables.h"
#include "ConstructData.h"
#include "FunctionExecutable.h"
#include "JITCode.h"
#include "JSCInlines.h"
#include "LLIntData.h"
#include "NativeExecutable.h"
#include "ObjectConstructor.h"
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
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = builtinModules.evaluate(*module);
    RETURN_IF_EXCEPTION(scope, { });
    if (value.isObject() && instance->state->modules->table().recordOfRuntimeSourceIsHandedOutFrozen(id)) {
        objectConstructorFreeze(globalObject, asObject(value));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return value;
}

static bool buildKeptForCompiledCode(JSGlobalObject* globalObject, uint32_t id)
{
    Instance* instance = Instance::of(globalObject);
    return instance && instance->state && instance->state->modules->table().valueOfRuntimeSourceStaysWithCompiledCode(id) && !instance->state->modules->table().recordOfRuntimeSourceIsHandedOutFrozen(id);
}

void willExposeBuiltinModuleToNativeCode(JSGlobalObject* globalObject, uint32_t id)
{
    if (!buildKeptForCompiledCode(globalObject, id))
        return;
    RuntimeCheckFailure failure("escape check failed"_s);
    failure.detail("native code takes the exports of the built-in module ", BuiltinTable::singleton().tokenOf(RuntimeModuleID(id)), ", but the compiler assumed that only compiled code uses them");
    failure.report();
    CRASH();
}

void willExposeBuiltinExportToNativeCode(JSGlobalObject* globalObject, uint32_t id, ASCIILiteral exportName)
{
    if (!buildKeptForCompiledCode(globalObject, id))
        return;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.nameOf(byteCast<uint8_t>(exportName.span()));
    if (name && std::ranges::contains(table.at(RuntimeModuleID(id)).exportsReadByNativeCode, name) && !std::ranges::contains(table.at(RuntimeModuleID(id)).memberOnlyExports, name))
        return;
    RuntimeCheckFailure failure("escape check failed"_s);
    failure.detail("native code reads the export '", exportName, "' of the built-in module ", table.tokenOf(RuntimeModuleID(id)), ", which the table of built-ins does not list, so the compiler assumed that only compiled code uses it");
    failure.report();
    CRASH();
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
        String line = modules.table().fullPath(module);
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
    const BuiltinTable& builtins = BuiltinTable::singleton();
    const BuiltinID sort = builtins.canonicalID(Builtins::ArrayPrototypeSort);
    for (const CompiledRuntimeFunctionData& entry : tables.compiledRuntimeFunctions()) {
        if (entry.function < tables.closureFunctions.first || entry.function >= tables.closureFunctions.end)
            continue;
        uint64_t word = instance.functionTables.closureWords[tables.closureNumberOf(entry.function)];
        const RuntimeFunctionData& function = builtins.at(RuntimeFunctionID(entry.id));
        for (BuiltinID builtin : function.implements) {
            if (builtin == sort) {
                instance.stubs.wordOfCompiledSort = word;
                continue;
            }
            if (function.role != RuntimeFunctionRole::EngineBuiltin)
                continue;
            if (std::optional<CompiledMethodOfArray> method = compiledMethodOfArray(builtin))
                instance.stubs.wordsOfCompiledMethodsOfArray[static_cast<unsigned>(*method)] = word;
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
    RELEASE_ASSERT(kind.closureStructureOfKind() != classObjectClosureStructure);
    Closure* closure = Closure::create(vm, ClosureTable::realmClosureStructure(vm, globalObject, kind.closureStructureOfKind()), *function);
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

bool rejectsLibrarySource()
{
    const Image* image = Image::current();
    return image && !image->allowsLibrarySource();
}

bool rejectsLibrarySource(JSGlobalObject* realm)
{
    return Instance::of(realm) && rejectsLibrarySource();
}

static std::optional<uint32_t> builtinFunctionHolding(const FunctionExecutable& executable)
{
    return executable.source().provider()->builtinFunctionAt(executable.source().startOffset());
}

static bool isLibraryFunction(ScriptExecutable& executable, bool includesEngineBuiltins)
{
    auto* function = dynamicDowncast<FunctionExecutable>(&executable);
    if (!function || !function->isBuiltinFunction() || function->unlinkedExecutable()->isBuiltinDefaultClassConstructor())
        return false;
    std::optional<uint32_t> id = builtinFunctionHolding(*function);
    return includesEngineBuiltins || !id || *id >= numberOfBuiltinCodes;
}

static Lock s_functionsOfTestRunnerLock;
static Vector<uint32_t, 1>& functionsOfTestRunner() WTF_REQUIRES_LOCK(s_functionsOfTestRunnerLock)
{
    static NeverDestroyed<Vector<uint32_t, 1>> functions;
    return functions;
}

void allowTestRunnerFunction(VM& vm, FunctionExecutable& function)
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

bool isInterpretationDisallowed(JSGlobalObject* realm, ScriptExecutable& executable)
{
    return rejectsLibrarySource(realm) && isLibraryFunction(executable, Image::current()->isBuiltWithLibrary()) && !isOfTestRunner(executable);
}

bool isFunctionDisallowed(JSFunction* function)
{
    auto* executable = dynamicDowncast<FunctionExecutable>(function->executable());
    return executable && isInterpretationDisallowed(function->realm(), *executable);
}

bool isUnreachableLibraryFunction(JSFunction* function)
{
    auto* executable = dynamicDowncast<FunctionExecutable>(function->executable());
    if (!executable || !Instance::of(function->realm()) || !isLibraryFunction(*executable, false))
        return false;
    return !Image::current()->allowsLibrarySource();
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
            if (called.function != function || (called.condition && called.condition->isAboutWhatProgramReplaces()))
                continue;
            add(called.builtin);
            if (called.condition && called.condition->isAskedForOtherNames)
                add(called.condition->isAskedForOtherNames);
        }
    }
    return names;
}

bool isClosedWorld(const Image& image)
{
    return image.isBuiltWithLibrary() && !image.admitsUnknownJavaScript() && !image.loadsCodeOutsideImage() && !hasFlag(image.header().options.admitted(), AdmittedCode::Inspector);
}

static thread_local bool t_eventMayExposeGlobalObject;

void noteGlobalObjectStoodInForReceiverOfEvents(JSGlobalObject* realm)
{
    if (!Instance::of(realm))
        return;
    const Image& image = *Image::current();
    if (!image.letsGlobalObjectGo() && isClosedWorld(image))
        t_eventMayExposeGlobalObject = true;
}

[[noreturn]] static void crashOnReachabilityBug(const String& leftOut)
{
    RuntimeCheckFailure failure("reachability check failed"_s);
    failure.detail(leftOut, " was used, but it was not compiled into this executable because nothing seemed to use it");
    failure.report();
    CRASH();
}

void throwBuiltinNotCompiled(JSGlobalObject* realm, const String& builtin)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    bool isReachabilityBug = isClosedWorld(*Image::current()) && !isReifyingForPresenceCheck();
    if (isReachabilityBug && !t_eventMayExposeGlobalObject)
        crashOnReachabilityBug(builtin);
    JSObject* error = isReachabilityBug ? createBuiltinBehindTargetOfEventNotCompiledError(realm, builtin) : createBuiltinNotCompiledError(realm, builtin);
    noteOmittedCodeError(realm, error);
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
    bool isReachabilityBug = isClosedWorld(*Image::current()) && !isReifyingForPresenceCheck();
    String leftOut = !isReachabilityBug ? String() : names.isEmpty() ? makeString("The built-in function '"_s, function.name().string(), '\'') : makeStringByJoining(names.span(), " or "_s);
    if (isReachabilityBug && !t_eventMayExposeGlobalObject)
        crashOnReachabilityBug(leftOut);
    JSObject* error = isReachabilityBug ? createBuiltinBehindTargetOfEventNotCompiledError(realm, leftOut) : names.size() == 1 ? createBuiltinNotCompiledError(realm, names[0]) : names.isEmpty() ? createBuiltinFunctionNotCompiledError(realm, function.name().string()) : createBuiltinsNotCompiledError(realm, names.span());
    bool isGetterOfAccessor = id && table.contains(RuntimeFunctionID(*id)) && std::ranges::any_of(table.at(RuntimeFunctionID(*id)).implements, [&](BuiltinID builtin) {
        return table.at(builtin).kind == BuiltinKind::Accessor;
    });
    if (!isGetterOfAccessor)
        noteOmittedCodeError(realm, error);
    throwException(realm, scope, error);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
