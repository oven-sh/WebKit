/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTShell.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTHost.h"
#include "AOTImageLoading.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTVector.h"
#include "AbstractModuleRecord.h"
#include "ConsoleClient.h"
#include "CustomGetterSetter.h"
#include "Exception.h"
#include "GlobalObjectMethodTable.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "MarkedBlockCage.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "PropertyNameArray.h"
#include "ScriptArguments.h"
#include "Symbol.h"
#include <array>
#include <atomic>
#include <cmath>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <tuple>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/WTFProcess.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::Shell {

static JSC_DECLARE_HOST_FUNCTION(functionRunCompiledEntryPoint);
static JSC_DECLARE_HOST_FUNCTION(functionReportRejectedEntryPoint);
static JSC_DECLARE_HOST_FUNCTION(functionAOTOperationCount);
static JSC_DECLARE_HOST_FUNCTION(functionAOTRuntimeCounters);
static JSC_DECLARE_HOST_FUNCTION(functionAOTBuiltinEntries);
static JSC_DECLARE_HOST_FUNCTION(functionRequire);
static JSC_DECLARE_HOST_FUNCTION(functionSetRequireCacheEntry);
static JSC_DECLARE_HOST_FUNCTION(functionOverrideRequire);
static JSC_DECLARE_HOST_FUNCTION(functionStoreInSlotWithoutTest);
static JSC_DECLARE_HOST_FUNCTION(functionLockCell);
static JSC_DECLARE_HOST_FUNCTION(functionUnlockCell);
static JSC_DECLARE_HOST_FUNCTION(functionCallWhileAnotherThreadLocksCell);
static JSC_DECLARE_HOST_FUNCTION(functionRequestTerminationOnResolve);
static JSC_DECLARE_HOST_FUNCTION(functionCallAndCancelTermination);
static JSC_DECLARE_HOST_FUNCTION(functionRequestTerminationAndCall);
static JSC_DECLARE_HOST_FUNCTION(functionRequestTerminationAndConstruct);
static JSC_DECLARE_CUSTOM_GETTER(getExportsOfModuleObject);
static JSC_DECLARE_CUSTOM_SETTER(setExportsOfModuleObject);
static JSC_DECLARE_CUSTOM_GETTER(getParentOfModuleObject);
static JSC_DECLARE_CUSTOM_GETTER(getChildrenOfModuleObject);

static constexpr std::array<ASCIILiteral, 14> functionNames {
    "runCompiledEntryPoint"_s,
    "setRequireCacheEntry"_s,
    "overrideRequire"_s,
    "aotOperationCount"_s,
    "aotRuntimeCounters"_s,
    "aotBuiltinEntries"_s,
    "storeInSlotWithoutTest"_s,
    "lockCell"_s,
    "unlockCell"_s,
    "callWhileAnotherThreadLocksCell"_s,
    "requestTerminationOnResolve"_s,
    "callAndCancelTermination"_s,
    "requestTerminationAndCall"_s,
    "requestTerminationAndConstruct"_s,
};

static std::optional<ModuleID> s_terminateOnResolveModule;
static VM* s_terminateOnResolveVM;

struct MappedImage {
    std::span<const uint8_t> bytes;
    intptr_t descriptor;
};

#if OS(WINDOWS)
static std::optional<MappedImage> mapFile(const char* path)
{
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size;
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size))
        return std::nullopt;
    // As MAP_PRIVATE elsewhere: the image is brought to its address in the process's own copy of the pages.
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_WRITECOPY, 0, 0, nullptr);
    void* bytes = mapping ? MapViewOfFile(mapping, FILE_MAP_COPY, 0, 0, 0) : nullptr;
    if (!bytes)
        return std::nullopt;
    return MappedImage { { static_cast<const uint8_t*>(bytes), static_cast<size_t>(size.QuadPart) }, reinterpret_cast<intptr_t>(file) };
}
#else
static std::optional<MappedImage> mapFile(const char* path)
{
    int fileDescriptor = open(path, O_RDONLY);
    struct stat status;
    void* bytes = fileDescriptor >= 0 && !fstat(fileDescriptor, &status) ? mmap(nullptr, status.st_size, PROT_READ, MAP_PRIVATE, fileDescriptor, 0) : MAP_FAILED;
    if (bytes == MAP_FAILED)
        return std::nullopt;
    return MappedImage { { static_cast<const uint8_t*>(bytes), static_cast<size_t>(status.st_size) }, fileDescriptor };
}
#endif

// The table of built-ins describes a realm with the globals Bun turns on, and an image may name any of them.
void enableGlobalsOfBuiltinTable()
{
    if (!Options::aotImagePath())
        return;
    Options::useSharedArrayBuffer() = true;
    Options::useShadowRealm() = true;
    Options::useTemporal() = true;
    Options::useExplicitResourceManagement() = true;
}

void mapImageFromOptions()
{
    const char* path = byteCast<char>(Options::aotImagePath());
    if (!path)
        return;

    std::optional<MappedImage> image = mapFile(path);
    if (!image) {
        dataLogLn("Cannot use ", path, ": cannot read it");
        exitProcess(EXIT_FAILURE);
    }

    Wire::ImageVerdict verdict = useImage(image->bytes, ImageFile { image->descriptor, 0 }, std::array<uint8_t, 16> { });
    if (verdict != Wire::ImageVerdict::Accepted) {
        dataLogLn("Cannot use ", path, ": ", nameOf(verdict));
        exitProcess(EXIT_FAILURE);
    }

    if (!Image::current()->header().options.useWideReferences() && !MarkedBlockCage::require()) {
        dataLogLn("Cannot use ", path, ": its references are narrow, and cells were allocated outside the cell cage");
        exitProcess(EXIT_FAILURE);
    }
}

JSC_DEFINE_HOST_FUNCTION(functionReportRejectedEntryPoint, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    globalObject->globalObjectMethodTable()->reportUncaughtExceptionAtEventLoop(globalObject, Exception::create(vm, callFrame->argument(0)));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsUndefined());
}

static std::optional<ModuleID> moduleFromArgument(JSGlobalObject* globalObject, JSValue id)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!Image::isRegistered()) {
        throwVMError(globalObject, scope, "no image is mapped"_s);
        return std::nullopt;
    }

    size_t recordCount = Image::current()->programTables().modules().size();
    if (!id.isInt32() || id.asInt32() < 1 || static_cast<size_t>(id.asInt32()) >= recordCount) {
        String text = id.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        throwVMRangeError(globalObject, scope, makeString("there is no module "_s, text));
        return std::nullopt;
    }
    return ModuleID(static_cast<uint32_t>(id.asInt32() - 1));
}

JSC_DEFINE_HOST_FUNCTION(functionRunCompiledEntryPoint, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::optional<ModuleID> module = moduleFromArgument(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });

    JSPromise* promise = importModule(globalObject, *module, std::nullopt);
    RETURN_IF_EXCEPTION(scope, { });

    JSFunction* onRejected = JSFunction::create(vm, globalObject, 1, String(), functionReportRejectedEntryPoint, ImplementationVisibility::Public);
    promise->then(globalObject, jsUndefined(), onRejected);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(promise);
}

JSC_DEFINE_HOST_FUNCTION(functionSetRequireCacheEntry, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    std::optional<ModuleID> module = moduleFromArgument(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    willSetRequireCacheEntry(globalObject, *module, callFrame->argument(1));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(functionOverrideRequire, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    willChangeRequireOverride(globalObject, RequireOverride::RequireFunction, callFrame->argument(0).toBoolean(globalObject));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(functionAOTOperationCount, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!Options::aotCountsPath() || !Image::isRegistered())
        return JSValue::encode(jsNull());
    String name = callFrame->argument(0).toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if (name == nameOf(entry))
            return JSValue::encode(jsNumber(static_cast<double>(operationCallCount(vm, entry))));
    }
    return throwVMError(globalObject, scope, makeString("there is no operation "_s, name));
}

JSC_DEFINE_HOST_FUNCTION(functionAOTRuntimeCounters, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!Image::isRegistered())
        return JSValue::encode(jsEmptyString(vm));
    StringPrintStream out;
    dumpCounts(vm, out);
    RELEASE_AND_RETURN(scope, JSValue::encode(jsString(vm, out.toString())));
}

JSC_DEFINE_HOST_FUNCTION(functionAOTBuiltinEntries, (JSGlobalObject* globalObject, CallFrame*))
{
    return JSValue::encode(BuiltinTable::singleton().describeForTesting(globalObject));
}

JSC_DEFINE_HOST_FUNCTION(functionStoreInSlotWithoutTest, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Object* object = compiledObjectOrNull(callFrame->argument(0));
    Identifier name = callFrame->argument(1).toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    const FieldData* field = object ? object->fieldNamed(object->state(), name) : nullptr;
    if (!field || !object->store(vm, *field, AcceptedValue::provenAtBuildTime(callFrame->argument(2))))
        return throwVMError(globalObject, scope, "the object has no slot of that name that can hold the value"_s);
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(functionLockCell, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!callFrame->argument(0).isCell())
        return throwVMError(globalObject, scope, "Expected a cell"_s);
    callFrame->argument(0).asCell()->cellLock().lock();
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(functionUnlockCell, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!callFrame->argument(0).isCell() || !callFrame->argument(0).asCell()->cellLock().isLocked())
        return throwVMError(globalObject, scope, "Expected a locked cell"_s);
    callFrame->argument(0).asCell()->cellLock().unlock();
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(functionCallWhileAnotherThreadLocksCell, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!callFrame->argument(0).isCell())
        return throwVMError(globalObject, scope, "Expected a cell"_s);
    JSCell* cell = callFrame->argument(0).asCell();
    std::atomic<bool> isDone { false };
    Ref<Thread> thread = Thread::create("Cell locker"_s, [&] {
        constexpr unsigned turnsPerState = 64;
        while (!isDone.load(std::memory_order_relaxed)) {
            {
                Locker locker { cell->cellLock() };
                for (unsigned turn = 0; turn < turnsPerState && !isDone.load(std::memory_order_relaxed); ++turn) { }
            }
            for (unsigned turn = 0; turn < turnsPerState && !isDone.load(std::memory_order_relaxed); ++turn) { }
        }
    });
    JSValue result = call(globalObject, callFrame->argument(1), jsUndefined(), ArgList { }, "Expected a function"_s);
    isDone.store(true);
    thread->waitForCompletion();
    ensureStillAliveHere(cell);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

// Requests termination when the loader resolves that module, as a watchdog thread would at that moment.
JSC_DEFINE_HOST_FUNCTION(functionRequestTerminationOnResolve, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String key = callFrame->argument(0).toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    s_terminateOnResolveModule = findModule(key);
    if (!s_terminateOnResolveModule)
        return throwVMError(globalObject, scope, makeString("there is no module "_s, key));
    s_terminateOnResolveVM = &vm;
    return JSValue::encode(jsUndefined());
}

// What a host does for a call with a time limit. Returns whether a termination was pending.
JSC_DEFINE_HOST_FUNCTION(functionCallAndCancelTermination, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    vm.ensureTerminationException();
    call(globalObject, callFrame->argument(0), jsUndefined(), ArgList { }, "Expected a function"_s);
    std::ignore = scope.exception();
    bool wasPending = vm.cancelTermination();
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(wasPending));
}

// As a watchdog thread that fires behind the last check of a native caller. The callee gets the arguments behind itself.
template<bool isConstruct>
static EncodedJSValue requestTerminationAndEnter(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue callee = callFrame->argument(0);
    if (isConstruct ? !callee.isConstructor() : !callee.isCallable())
        return throwVMTypeError(globalObject, scope, "Expected a function"_s);
    MarkedArgumentBuffer arguments;
    for (unsigned index = 1; index < callFrame->argumentCount(); ++index)
        arguments.append(callFrame->uncheckedArgument(index));
    RELEASE_ASSERT(!arguments.hasOverflowed());
    vm.notifyNeedTermination();
    if constexpr (isConstruct)
        RELEASE_AND_RETURN(scope, JSValue::encode(construct(globalObject, callee, arguments, "Expected a function"_s)));
    else
        RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, callee, jsUndefined(), arguments, "Expected a function"_s)));
}

JSC_DEFINE_HOST_FUNCTION(functionRequestTerminationAndCall, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return requestTerminationAndEnter<false>(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(functionRequestTerminationAndConstruct, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return requestTerminationAndEnter<true>(globalObject, callFrame);
}

// An image locks `console`, so no script can give the shell one that prints.
class ConsoleOfShell final : public ConsoleClient {
    WTF_MAKE_TZONE_ALLOCATED(ConsoleOfShell);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ConsoleOfShell);
public:
    void messageWithTypeAndLevel(MessageType, MessageLevel level, JSGlobalObject* globalObject, Ref<Inspector::ScriptArguments>&& arguments) final
    {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        StringBuilder line;
        for (size_t index = 0; index < arguments->argumentCount(); ++index) {
            JSValue value = arguments->argumentAt(index);
            if (index)
                line.append(' ');
            if (value.isSymbol()) {
                if (auto description = asSymbol(value)->tryGetDescriptiveString())
                    line.append(description.value());
                continue;
            }
            if (value.isDouble() && !value.asDouble() && std::signbit(value.asDouble())) {
                line.append("-0"_s);
                continue;
            }
            String text = value.toWTFString(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            line.append(text);
        }
        line.append('\n');
        CString bytes = line.toString().utf8();
        FILE* stream = level == MessageLevel::Error || level == MessageLevel::Warning ? stderr : stdout;
        fwrite(bytes.data(), 1, bytes.length(), stream);
        fflush(stream);
    }

    void count(JSGlobalObject*, const String&) final { }
    void countReset(JSGlobalObject*, const String&) final { }
    void profile(JSGlobalObject*, const String&) final { }
    void profileEnd(JSGlobalObject*, const String&) final { }
    void takeHeapSnapshot(JSGlobalObject*, const String&) final { }
    void time(JSGlobalObject*, const String&) final { }
    void timeLog(JSGlobalObject*, const String&, Ref<Inspector::ScriptArguments>&&) final { }
    void timeEnd(JSGlobalObject*, const String&) final { }
    void timeStamp(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) final { }
    void record(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) final { }
    void recordEnd(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) final { }
    void screenshot(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) final { }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ConsoleOfShell);

void addShellFunctions(VM& vm, JSGlobalObject* globalObject)
{
    if (Options::aotImagePath())
        globalObject->setConsoleClient(*new ConsoleOfShell);

    const std::pair<NativeFunction, unsigned> functions[] = {
        { functionRunCompiledEntryPoint, 1 },
        { functionSetRequireCacheEntry, 2 },
        { functionOverrideRequire, 1 },
        { functionAOTOperationCount, 1 },
        { functionAOTRuntimeCounters, 0 },
        { functionAOTBuiltinEntries, 0 },
        { functionStoreInSlotWithoutTest, 3 },
        { functionLockCell, 1 },
        { functionUnlockCell, 1 },
        { functionCallWhileAnotherThreadLocksCell, 2 },
        { functionRequestTerminationOnResolve, 1 },
        { functionCallAndCancelTermination, 1 },
        { functionRequestTerminationAndCall, 1 },
        { functionRequestTerminationAndConstruct, 1 },
    };
    static_assert(std::size(functions) == functionNames.size());

    for (size_t index = 0; index < functionNames.size(); ++index) {
        auto [function, length] = functions[index];
        Identifier identifier = Identifier::fromString(vm, functionNames[index]);
        globalObject->putDirect(vm, identifier, JSFunction::create(vm, globalObject, length, identifier.string(), function, ImplementationVisibility::Public), static_cast<unsigned>(PropertyAttribute::DontEnum));
    }
}

std::span<const ASCIILiteral> shellFunctionNames()
{
    return functionNames;
}

bool isKeyOfCompiledModule(const String& key)
{
    std::optional<ModuleID> module = findModule(key);
    if (module && s_terminateOnResolveModule == module) [[unlikely]] {
        s_terminateOnResolveModule = std::nullopt;
        s_terminateOnResolveVM->notifyNeedTermination();
    }
    return !!module;
}

JSPromise* fetchCompiledModule(JSGlobalObject* globalObject, const String& key)
{
    std::optional<ModuleID> module = findModule(key);
    if (!module)
        return nullptr;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    AbstractModuleRecord* record = moduleRecord(globalObject, *module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSPromise* source = JSPromise::create(vm, globalObject->promiseStructure());
    source->fulfill(vm, record);
    return source;
}

static ModuleID moduleOfModuleObject(JSGlobalObject* globalObject, JSObject* module)
{
    return ModuleID(static_cast<uint32_t>(module->getDirect(globalObject->vm(), Identifier::fromString(globalObject->vm(), "numberInImage"_s)).asInt32()));
}

JSC_DEFINE_CUSTOM_GETTER(getExportsOfModuleObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    return JSValue::encode(moduleExports(globalObject, moduleOfModuleObject(globalObject, asObject(JSValue::decode(thisValue)))));
}

JSC_DEFINE_CUSTOM_SETTER(setExportsOfModuleObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, EncodedJSValue value, PropertyName))
{
    setModuleExports(globalObject, moduleOfModuleObject(globalObject, asObject(JSValue::decode(thisValue))), JSValue::decode(value));
    return true;
}

JSC_DEFINE_CUSTOM_GETTER(getParentOfModuleObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    JSObject* parent = parentModuleObject(globalObject, moduleOfModuleObject(globalObject, asObject(JSValue::decode(thisValue))));
    return JSValue::encode(parent ? JSValue(parent) : jsUndefined());
}

JSC_DEFINE_CUSTOM_GETTER(getChildrenOfModuleObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* module = asObject(JSValue::decode(thisValue));
    auto* children = uncheckedDowncast<JSArray>(module->getDirect(vm, Identifier::fromString(vm, "listedChildren"_s)));
    bool areShown = forEachNewChild(globalObject, moduleOfModuleObject(globalObject, module), [&](JSObject* child) {
        children->push(globalObject, child);
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(areShown ? JSValue(children) : jsUndefined());
}

// Compiled code calls a function that the table lists under an object of the host without testing that it is there.
static void addMissingHostFunctions(JSGlobalObject* globalObject, JSObject* object, BuiltinID holder)
{
    VM& vm = globalObject->vm();
    const BuiltinTable& table = BuiltinTable::singleton();
    for (BuiltinID member : table.membersOf(holder)) {
        Identifier name = table.identifierOf(vm, table.at(member).name);
        if (table.at(member).kind != BuiltinKind::Function || isValidOffset(object->getDirectOffset(vm, name)))
            continue;
        JSFunction* function = JSNativeStdFunction::create(vm, globalObject, 0, name.string(), [member](JSGlobalObject*, CallFrame*) -> EncodedJSValue {
            dataLogLn("AOT: the program needs the host's ", BuiltinTable::singleton().textOf(member), ", and this realm has no host");
            exitProcess(4);
        });
        addMissingHostFunctions(globalObject, function, table.canonicalID(member));
        object->putDirect(vm, name, function);
    }
}

void addMissingImportMetaFunctions(JSGlobalObject* globalObject, JSObject* importMeta)
{
    if (Image::current())
        addMissingHostFunctions(globalObject, importMeta, Builtins::ImportMeta);
}

static JSFunction* createRequireFunction(JSGlobalObject* globalObject)
{
    JSFunction* require = JSFunction::create(globalObject->vm(), globalObject, 1, "require"_s, functionRequire, ImplementationVisibility::Public);
    addMissingHostFunctions(globalObject, require, Builtins::ModuleRequire);
    return require;
}

static void createModuleObject(JSGlobalObject* globalObject, JSString* path, ModuleID number, OptionSet<ModuleObjectTrait> traits, ModuleObjects& objects)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSArray* children = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, void());
    JSArray* paths = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, void());

    unsigned hidden = PropertyAttribute::DontEnum | 0;
    JSObject* module = constructEmptyObject(globalObject);
    module->putDirect(vm, Identifier::fromString(vm, "numberInImage"_s), jsNumber(number.index()), hidden);
    module->putDirect(vm, Identifier::fromString(vm, "listedChildren"_s), children, hidden);
    if (traits.contains(ModuleObjectTrait::HasExportsVariable))
        module->putDirectCustomAccessor(vm, vm.propertyNames->exports, CustomGetterSetter::create(vm, getExportsOfModuleObject, setExportsOfModuleObject), PropertyAttribute::CustomValue | 0);
    module->putDirect(vm, vm.propertyNames->id, traits.contains(ModuleObjectTrait::IsMain) ? jsString(vm, String("."_s)) : path);
    module->putDirect(vm, Identifier::fromString(vm, "filename"_s), path);
    module->putDirect(vm, Identifier::fromString(vm, "loaded"_s), jsBoolean(true));
    module->putDirectCustomAccessor(vm, Identifier::fromString(vm, "children"_s), CustomGetterSetter::create(vm, getChildrenOfModuleObject, nullptr), PropertyAttribute::CustomValue | PropertyAttribute::ReadOnly);
    module->putDirect(vm, Identifier::fromString(vm, "paths"_s), paths);
    module->putDirectCustomAccessor(vm, Identifier::fromString(vm, "parent"_s), CustomGetterSetter::create(vm, getParentOfModuleObject, nullptr), PropertyAttribute::CustomValue | PropertyAttribute::ReadOnly);
    addMissingHostFunctions(globalObject, module, Builtins::ModuleObject);

    objects.module = module;
    objects.require = createRequireFunction(globalObject);
}

static void createRequirer(JSGlobalObject* globalObject, JSObject* importMeta, ModuleObjects& objects)
{
    objects.module = importMeta;
    objects.require = createRequireFunction(globalObject);
}

static JSValue requireSpecifier(JSGlobalObject* globalObject, JSObject*, JSValue specifier)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    String text = specifier.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (specifier.isString()) {
        if (std::optional<ModuleID> module = findModule(text))
            RELEASE_AND_RETURN(scope, requireModule(globalObject, *module));
    }
    throwException(globalObject, scope, createError(globalObject, makeString("Cannot find module '"_s, text, '\'')));
    return { };
}

JSC_DEFINE_HOST_FUNCTION(functionRequire, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    willLoadByName(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(requireSpecifier(globalObject, nullptr, callFrame->argument(0))));
}

static JSValue resolveSpecifier(JSGlobalObject* globalObject, JSObject*, JSValue specifier, ResolvedSpecifierForm form)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    String text = specifier.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    switch (form) {
    case ResolvedSpecifierForm::Path:
    case ResolvedSpecifierForm::PathForRequire:
        return jsString(vm, text);
    case ResolvedSpecifierForm::URL:
        return jsString(vm, makeString("file://"_s, text));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasESModuleMarker(JSGlobalObject* globalObject, JSObject* exports)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& markerName = vm.propertyNames->__esModule;

    PropertySlot slot(exports, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = exports->getPropertySlot(globalObject, markerName, slot);
    RETURN_IF_EXCEPTION(scope, false);
    return isFound && slot.isValue() && slot.getValue(globalObject, markerName).pureToBoolean() == TriState::True;
}

static void captureCommonJSExports(JSGlobalObject* globalObject, SyntheticExportRule rule, JSValue value, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& defaultName = vm.propertyNames->defaultKeyword;

    JSObject* exports = value.getObject();
    if (!exports) {
        names.append(defaultName);
        values.append(value);
        return;
    }

    bool hasMarker = rule == SyntheticExportRule::CommonJs && hasESModuleMarker(globalObject, exports);
    RETURN_IF_EXCEPTION(scope, void());

    PropertyNameArrayBuilder properties(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    exports->methodTable()->getOwnPropertyNames(exports, globalObject, properties, hasMarker ? DontEnumPropertiesMode::Exclude : DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());

    const Identifier& omittedName = hasMarker ? vm.propertyNames->__esModule : defaultName;
    bool hasDefault = false;
    for (auto& property : properties) {
        if (property == vm.propertyNames->constructor || property == omittedName)
            continue;

        PropertySlot slot(exports, PropertySlot::InternalMethodType::Get);
        bool isFound = exports->getPropertySlot(globalObject, property, slot);
        RETURN_IF_EXCEPTION(scope, void());
        if (!isFound)
            continue;
        if ((slot.attributes() & PropertyAttribute::DontEnum) && !slot.isValue() && !slot.isCustom())
            continue;

        JSValue member = slot.getValue(globalObject, property);
        if (scope.exception()) [[unlikely]] {
            TRY_CLEAR_EXCEPTION(scope, void());
            member = jsUndefined();
        }

        names.append(property);
        values.append(member);
        hasDefault |= property == defaultName;
    }

    if (!hasDefault) {
        names.append(defaultName);
        values.append(value);
    }
}

static JSObject* captureSyntheticExports(JSGlobalObject* globalObject, SyntheticExportRule rule, JSValue value, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (rule) {
    case SyntheticExportRule::CommonJs:
    case SyntheticExportRule::CommonJsIgnoringAnnotation:
        scope.release();
        captureCommonJSExports(globalObject, rule, value, names, values);
        return nullptr;
    case SyntheticExportRule::RuntimeModule:
        throwTypeError(globalObject, scope, "the shell has no runtime modules"_s);
        return nullptr;
    case SyntheticExportRule::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSObject* createAsyncModuleRequiredError(JSGlobalObject* globalObject, const String& path)
{
    return createTypeError(globalObject, makeString("require() async module \""_s, path, "\" is unsupported. use \"await import()\" instead."_s));
}

constinit const ModuleHost moduleHost {
    .createModuleObject = createModuleObject,
    .createRequirer = createRequirer,
    .requireSpecifier = requireSpecifier,
    .resolveSpecifier = resolveSpecifier,
    .captureSyntheticExports = captureSyntheticExports,
    .requireNativeModule = nullptr,
    .captureNativeModuleExports = nullptr,
    .requireCache = nullptr,
    .requireExtensions = nullptr,
    .createAsyncModuleRequiredError = createAsyncModuleRequiredError,
    .createEmptyJSONBodyError = nullptr,
    .createLoadError = nullptr,
};

} // namespace JSC::AOT::Shell

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
