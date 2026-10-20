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
#include "Exception.h"
#include "GlobalObjectMethodTable.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSPromise.h"
#include "MarkedBlockCage.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "PropertyNameArray.h"
#include <array>
#include <optional>
#include <stdlib.h>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/WTFProcess.h>
#include <wtf/text/MakeString.h>

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
static JSC_DECLARE_HOST_FUNCTION(functionEvictCompiledModule);
static JSC_DECLARE_HOST_FUNCTION(functionSetRequireCacheEntry);
static JSC_DECLARE_HOST_FUNCTION(functionOverrideRequire);

static constexpr std::array<ASCIILiteral, 7> functionNames {
    "runCompiledEntryPoint"_s,
    "evictCompiledModule"_s,
    "setRequireCacheEntry"_s,
    "overrideRequire"_s,
    "aotOperationCount"_s,
    "aotRuntimeCounters"_s,
    "aotBuiltinEntries"_s,
};

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
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* bytes = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
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

JSC_DEFINE_HOST_FUNCTION(functionEvictCompiledModule, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    std::optional<ModuleID> module = moduleFromArgument(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    evictModule(globalObject, *module);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsUndefined());
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

void addShellFunctions(VM& vm, JSGlobalObject* globalObject)
{
    const std::pair<NativeFunction, unsigned> functions[] = {
        { functionRunCompiledEntryPoint, 1 },
        { functionEvictCompiledModule, 1 },
        { functionSetRequireCacheEntry, 2 },
        { functionOverrideRequire, 1 },
        { functionAOTOperationCount, 1 },
        { functionAOTRuntimeCounters, 0 },
        { functionAOTBuiltinEntries, 0 },
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

static String directoryOf(StringView path)
{
    size_t lastSlash = path.reverseFind('/');
    if (lastSlash == notFound)
        return "."_s;
    if (!lastSlash)
        return "/"_s;
    return path.left(lastSlash).toString();
}

static void fillPathsAndRequire(JSGlobalObject* globalObject, JSString* path, CommonJSModuleObjects& objects)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto pathText = path->value(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    objects.filename = path;
    objects.dirname = jsString(vm, directoryOf(pathText.data));
    objects.require = JSFunction::create(vm, globalObject, 1, "require"_s, functionRequire, ImplementationVisibility::Public);
}

static void createCommonJSModule(JSGlobalObject* globalObject, JSString* path, JSObject* parent, CommonJSModuleObjects& objects)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSArray* children = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, void());
    JSArray* paths = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, void());
    JSObject* exports = constructEmptyObject(globalObject);

    JSObject* module = constructEmptyObject(globalObject);
    module->putDirect(vm, vm.propertyNames->exports, exports);
    module->putDirect(vm, vm.propertyNames->id, path);
    module->putDirect(vm, Identifier::fromString(vm, "filename"_s), path);
    module->putDirect(vm, Identifier::fromString(vm, "loaded"_s), jsBoolean(false));
    module->putDirect(vm, Identifier::fromString(vm, "children"_s), children);
    module->putDirect(vm, Identifier::fromString(vm, "paths"_s), paths);
    module->putDirect(vm, Identifier::fromString(vm, "parent"_s), parent ? JSValue(parent) : jsUndefined());

    objects.module = module;
    objects.exports = exports;
    RELEASE_AND_RETURN(scope, fillPathsAndRequire(globalObject, path, objects));
}

static ModuleAdoption adoptCommonJSModule(JSGlobalObject* globalObject, JSString* path, JSObject* module, CommonJSModuleObjects& objects)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    objects.module = module;
    objects.exports = module->get(globalObject, vm.propertyNames->exports);
    RETURN_IF_EXCEPTION(scope, ModuleAdoption::Rejected);
    fillPathsAndRequire(globalObject, path, objects);
    RETURN_IF_EXCEPTION(scope, ModuleAdoption::Rejected);
    return ModuleAdoption::Adopted;
}

static void createRequirer(JSGlobalObject* globalObject, JSObject* importMeta, CommonJSModuleObjects& objects)
{
    objects.module = importMeta;
    objects.require = JSFunction::create(globalObject->vm(), globalObject, 1, "require"_s, functionRequire, ImplementationVisibility::Public);
}

static ChildLink addChild(JSGlobalObject*, JSObject*, JSObject*)
{
    return ChildLink::Recorded;
}

static void pushObservableChild(JSGlobalObject* globalObject, JSObject* parent, JSObject* child)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue children = parent->get(globalObject, Identifier::fromString(vm, "children"_s));
    RETURN_IF_EXCEPTION(scope, void());
    if (auto* array = dynamicDowncast<JSArray>(children))
        RELEASE_AND_RETURN(scope, array->push(globalObject, child));
}

static void removeFromRequireCache(JSGlobalObject*, JSString*)
{
}

static JSValue requireSpecifier(JSGlobalObject* globalObject, JSObject*, JSValue specifier)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    String text = specifier.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (specifier.isString()) {
        if (std::optional<ModuleID> module = findModule(text))
            RELEASE_AND_RETURN(scope, requireModule(globalObject, *module, nullptr));
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

static void captureCommonJSExports(JSGlobalObject* globalObject, SyntheticExportRule rule, JSValue value, Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
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

static void captureSyntheticExports(JSGlobalObject* globalObject, SyntheticExportRule rule, JSValue value, Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (rule) {
    case SyntheticExportRule::CommonJs:
    case SyntheticExportRule::CommonJsIgnoringAnnotation:
        RELEASE_AND_RETURN(scope, captureCommonJSExports(globalObject, rule, value, names, values));
    case SyntheticExportRule::RuntimeModule:
        throwTypeError(globalObject, scope, "the shell has no runtime modules"_s);
        return;
    case SyntheticExportRule::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

constinit const ModuleHost moduleHost {
    .createCommonJSModule = createCommonJSModule,
    .adoptCommonJSModule = adoptCommonJSModule,
    .createRequirer = createRequirer,
    .addChild = addChild,
    .pushObservableChild = pushObservableChild,
    .removeFromRequireCache = removeFromRequireCache,
    .requireSpecifier = requireSpecifier,
    .resolveSpecifier = resolveSpecifier,
    .captureSyntheticExports = captureSyntheticExports,
    .requireNativeModule = nullptr,
    .captureNativeModuleExports = nullptr,
    .requireCache = nullptr,
};

} // namespace JSC::AOT::Shell

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
