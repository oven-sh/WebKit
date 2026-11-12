/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionTables.h"

#if ENABLE(AOT)

#include "AOTBuiltinModules.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTConvention.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTMembersLeftOut.h"
#include "AOTModuleHost.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTProgramStrings.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "FunctionRareData.h"
#include "JSCInlines.h"
#include "NativeExecutable.h"
#include "Options.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Lock.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(FunctionTables::numberOfExecutables == ClosureWords::numberOfExecutables);
static_assert(FunctionTables::numberOfExecutables - 1 <= ResumeClosureRequest::maxExecutable);

namespace {

struct FunctionKindFields {
    CallableKind callableKind { CallableKind::Plain };
    bool isAsync { false };
    bool isGenerator { false };
    bool takesNewTarget { false };
    bool isConstructor { false };
    bool isDerivedConstructor { false };
};

} // anonymous namespace

static consteval FunctionKindData functionKindWith(FunctionKindFields fields)
{
    FunctionKindData kind { };
    kind.callableKind = static_cast<unsigned>(fields.callableKind);
    kind.isAsync = fields.isAsync;
    kind.isGenerator = fields.isGenerator;
    kind.takesNewTarget = fields.takesNewTarget;
    kind.isConstructor = fields.isConstructor;
    kind.isDerivedConstructor = fields.isDerivedConstructor;
    return kind;
}

static consteval bool usesNotConstructibleExecutable(FunctionKindFields fields)
{
    FunctionKindData kind = functionKindWith(fields);
    return FunctionTables::executableKind(kind) == FunctionTables::ExecutableKind::NotConstructible && !FunctionTables::canConstruct(kind);
}

static_assert(FunctionTables::executableKind(functionKindWith({ })) == FunctionTables::ExecutableKind::Constructible);
static_assert(FunctionTables::canConstruct(functionKindWith({ })));
static_assert(closureWordFlags(functionKindWith({ })) == ClosureWord::Flags { });

static_assert(FunctionTables::executableKind(functionKindWith({ .isConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(FunctionTables::canConstruct(functionKindWith({ .isConstructor = true })));
static_assert(closureWordFlags(functionKindWith({ .isConstructor = true })) == ClosureWord::Flags { .isNotCallable = true });

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true }))
    == ClosureWord::Flags { .isNotCallable = true, .takesNewTarget = true });

static_assert(usesNotConstructibleExecutable({ .isAsync = true }));
static_assert(usesNotConstructibleExecutable({ .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .isAsync = true, .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Arrow }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Method }));

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true })) == FunctionTables::ExecutableKind::Constructible);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true })) == ClosureWord::Flags { .takesNewTarget = true });

uint32_t closureNumberOf(FunctionID function)
{
    return programTables().closureNumberOf(function.index());
}

FunctionID closureFunctionWithNumber(uint32_t number)
{
    const ProgramTables& tables = programTables();
    RELEASE_ASSERT(tables.hasClosureNumber(number));
    return FunctionID(tables.closureFunctionWithNumber(number));
}

unsigned functionLengthOf(FunctionID function)
{
    const ProgramTables& tables = programTables();
    ASSERT(function.index() >= tables.closureFunctions.first && function.index() < tables.closureFunctions.end);
    return tables.functionLengthOf(function.index());
}

FunctionID sourceFunctionOf(FunctionID function)
{
    return FunctionID(programTables().sourceFunctionOf(function.index()));
}

static const uint64_t* closureWordsByNumber(const Image& image)
{
    const ClosureFunctionRangeData& range = image.programTables().closureFunctions;
    return image.section<uint64_t>(ImageSection::ClosureWords).subspan(range.first - range.omittedWordCount).data();
}

FunctionTables::FunctionTables(VM& vm)
    : words(closureWordsByNumber(*Image::current()))
{
    RELEASE_ASSERT(!Options::useJIT());
    for (unsigned index = 0; index < numberOfExecutables; ++index) {
        auto kind = static_cast<ExecutableKind>(index);
        bool isClass = kind == ExecutableKind::Class;
        bool constructs = isClass || kind == ExecutableKind::Constructible;
        NativeExecutable* executable = vm.getHostFunction(isClass ? callClassConstructor : callCompiledClosure, ImplementationVisibility::Private, constructs ? constructCompiledClosure : callHostFunctionAsConstructor, 0, String());
        executables[index] = Strong<JSCell>(vm, executable);
    }
}

NativeExecutable* FunctionTables::executableOf(FunctionID function) const
{
    return uncheckedDowncast<NativeExecutable>(executables[static_cast<unsigned>(executableKind(functionKindData(function)))].get());
}

NEVER_INLINE FunctionTables& FunctionTables::createFor(VM& vm)
{
    auto& owner = vm.aotState().functionTables;
    owner = makeUnique<FunctionTables>(vm);
    return *owner;
}

FunctionTables& FunctionTables::of(VM& vm)
{
    RELEASE_ASSERT(vm.hasCompiledCode());
    if (auto& tables = vm.aotState().functionTables) [[likely]]
        return *tables;
    return createFor(vm);
}

static ClosureTable& closureTableOf(JSObject* callee)
{
    return (*Instance::owning(callee)->state).objects->closures;
}

static ASCIILiteral initialNamePrefix(FunctionKindData kind)
{
    bool isGetter = kind.accessor() == MethodKind::Getter;
    RELEASE_ASSERT(isGetter || kind.accessor() == MethodKind::Setter);
    return isGetter ? "get "_s : "set "_s;
}

String writtenFunctionName(FunctionID function)
{
    const Image& image = *Image::current();
    ASSERT(function.index() < image.programTables().numberOfFunctions());
    std::optional<uint32_t> name = image.programTables().nameOfFunction(function.index());
    return name && image.text().hasText(StringID(*name)) ? image.text().createString(StringID(*name)) : emptyString();
}

String initialNameProperty(FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    String written = writtenFunctionName(function);
    return kind.hasInitialNamePrefix() ? makeString(initialNamePrefix(kind), written) : written;
}

JSString* initialNamePropertyString(VM& vm, State& state, FunctionID function)
{
    if (functionKindData(function).hasInitialNamePrefix())
        return jsString(vm, initialNameProperty(function));
    return state.strings->writtenNameOfFunction(vm, function);
}

static String functionNameAsWritten(const FunctionDescription& description)
{
    if (description.codeType() == ModuleCode || functionKindData(description.function()).isCommonJSModule)
        return emptyString();
    return writtenFunctionName(description.function());
}

static String nameInFramesOfFunctionNamedByComputedKey(FunctionID function)
{
    FunctionID source = sourceFunctionOf(function);
    return functionKindData(source).isGenerator ? emptyString() : initialNameProperty(source);
}

static bool runsInBodyWithoutName(const FunctionDescription& description)
{
    FunctionKindData kind = functionKindData(sourceFunctionOf(description.function()));
    return kind.hasInferredName && (kind.isGenerator || (kind.isAsync && description.isNamedByOwner()));
}

String functionName(const FunctionDescription& description, NameForm form)
{
    FunctionID function = description.function();
    switch (form) {
    case NameForm::AsWritten:
        return functionNameAsWritten(description);
    case NameForm::InFrames:
        if (runsInBodyWithoutName(description))
            return emptyString();
        if (description.isNamedByComputedKey()) [[unlikely]]
            return nameInFramesOfFunctionNamedByComputedKey(function);
        return functionNameAsWritten(description);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSString* shareComputedName(VM& vm, FunctionID function, const String& name)
{
    VMState::ComputedFunctionName ownName { .hash = name.hash(), .length = name.length() };
    VMState::ComputedFunctionName& shared = vm.aotState().computedFunctionNames.add(sourceFunctionOf(function).index(), WTF::move(ownName)).iterator->value;
    if (shared.hash != name.hash() || shared.length != name.length())
        shared.isOneOfSeveral = true;
    if (JSString* string = shared.name.get(); string && !shared.isOneOfSeveral) {
        if (string->tryGetValue().data == name)
            return string;
        shared.isOneOfSeveral = true;
    }
    JSString* string = jsString(vm, name);
    shared.name = shared.isOneOfSeveral ? Weak<JSString>() : Weak<JSString>(string);
    return string;
}

String computedNameInFrames(VM& vm, const FunctionDescription& description)
{
    if (runsInBodyWithoutName(description))
        return emptyString();
    FunctionID source = sourceFunctionOf(description.function());
    if (functionKindData(source).isGenerator)
        return emptyString();
    const auto& names = vm.aotState().computedFunctionNames;
    auto found = names.find(source.index());
    JSString* name = found == names.end() ? nullptr : found->value.name.get();
    return name ? String(name->tryGetValueWithoutGC().data) : emptyString();
}

static String computedFunctionName(JSGlobalObject* globalObject, const Identifier& key, MethodKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = key.string();
    if (UniquedStringImpl* uid = key.impl(); uid->isSymbol()) {
        auto& symbol = static_cast<SymbolImpl&>(*uid);
        if (symbol.isPrivate())
            name = StringImpl::createSubstringSharingImpl(symbol, 0, symbol.length());
        else
            name = symbol.isNullSymbol() ? emptyString() : tryMakeString('[', StringView(uid), ']');
        if (!name) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope, "Function name is too long"_s);
            return { };
        }
    }
    if (kind == MethodKind::Method)
        return name;
    bool isGetter = kind == MethodKind::Getter;
    name = tryMakeString(isGetter ? "get "_s : "set "_s, name);
    if (!name) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope, isGetter ? "Getter name is too long"_s : "Setter name is too long"_s);
    return name;
}

void setComputedFunctionName(VM& vm, JSObject* closure, const Identifier& key, MethodKind kind)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = computedFunctionName(closure->realm(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    std::optional<FunctionID> function = functionOf(closure);
    if (function && !closure->getDirect(vm, vm.propertyNames->name) && initialNameProperty(sourceFunctionOf(*function)) == name)
        return;
    JSString* nameString = function ? shareComputedName(vm, *function, name) : jsString(vm, name);
    CompiledCallee::reifyProperties(vm, closure);
    closure->putDirect(vm, vm.propertyNames->name, nameString, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    if (isCompiledClosure(closure)) {
        FunctionRareData* rareData = uncheckedDowncast<Closure>(closure)->ensureRareData(vm);
        rareData->setHasReifiedName();
        rareData->setHasModifiedNameForBoundOrNonHostFunction();
    }
}

static std::span<const FunctionSourceEntry> findFunctionSourceEntries(const ProgramTables& tables, uint32_t source)
{
    auto sources = tables.functionSources();
    if (sources.empty())
        return { };
    std::span entries { tables.at<FunctionSourceEntry>(sources[0].entriesOffset), sources[0].numberOfEntries };
    auto found = std::ranges::equal_range(entries, source, { }, [&](const FunctionSourceEntry& entry) { return tables.sourceFunctionOf(entry.function); });
    return { found.begin(), found.end() };
}

static std::optional<std::span<const Latin1Character>> decompressSourceBlocks(Image::DecompressedSourceBlocks& decompressedSource, DecompressSourceFunction decompress, const uint8_t* compressed, std::span<const uint32_t> ends, uint32_t sourceSize, uint32_t start, uint32_t end)
{
    RELEASE_ASSERT(start <= end && end <= sourceSize);
    RELEASE_ASSERT(sourceSize <= static_cast<uint64_t>(ends.size()) * sourceBlockSize);
    if (!decompress)
        return std::nullopt;
    Locker locker { decompressedSource.lock };
    if (!decompressedSource.bytes) {
        size_t sizeReserved = roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(sourceSize, 1));
        void* text = OSAllocator::tryReserveUncommitted(sizeReserved);
        void* isDecompressed = nullptr;
        if (!text)
            return std::nullopt;
        if (!tryFastZeroedMalloc(ends.size()).getValue(isDecompressed)) {
            OSAllocator::releaseDecommitted(text, sizeReserved, 0);
            return std::nullopt;
        }
        decompressedSource.bytes = static_cast<uint8_t*>(text);
        decompressedSource.isDecompressed = static_cast<uint8_t*>(isDecompressed);
    }
    uint8_t* decompressed = decompressedSource.bytes;
    uint8_t* isDecompressed = decompressedSource.isDecompressed;
    for (uint32_t block = start / sourceBlockSize; static_cast<uint64_t>(block) * sourceBlockSize < end; ++block) {
        if (isDecompressed[block])
            continue;
        uint32_t first = block ? ends[block - 1] : 0;
        size_t offset = static_cast<size_t>(block) * sourceBlockSize;
        size_t sizeOfBlock = std::min<size_t>(sourceBlockSize, sourceSize - offset);
        OSAllocator::commit(decompressed + offset, roundUpToMultipleOf(WTF::pageSize(), sizeOfBlock), true, false);
        if (!decompress({ decompressed + offset, sizeOfBlock }, { compressed + first, ends[block] - first }))
            return std::nullopt;
        isDecompressed[block] = 1;
    }
    return std::span { byteCast<Latin1Character>(decompressed) + start, end - start };
}

static std::optional<std::span<const Latin1Character>> bytesOf(const Image& image, const FunctionSourceEntry& entry)
{
    const ProgramTables& tables = image.programTables();
    const FunctionSources& sources = tables.functionSources()[0];
    if (sources.numberOfBlocks)
        return decompressSourceBlocks(image.decompressedFunctionSources(), image.decompressSource(), tables.at<uint8_t>(sources.sourceOffset), { tables.at<uint32_t>(sources.blockEndsOffset), sources.numberOfBlocks }, sources.sourceSize, entry.start, entry.end);
    return std::span { tables.at<Latin1Character>(sources.sourceOffset) + entry.start, entry.end - entry.start };
}

std::optional<std::span<const Latin1Character>> quotedSourceBytes(uint32_t start, uint32_t length)
{
    const Image& image = *Image::current();
    std::span<const uint8_t> text = image.programTables().quotedSourceText();
    std::span<const uint32_t> blocks = image.programTables().quotedSourceBlocks();
    if (blocks.empty())
        return byteCast<Latin1Character>(text.subspan(start, length));
    return decompressSourceBlocks(image.decompressedQuotedSources(), image.decompressSource(), text.data(), blocks.subspan(1), blocks[0], start, start + length);
}

std::optional<std::span<const Latin1Character>> functionSourceBytes(FunctionID function)
{
    const Image& image = *Image::current();
    const ProgramTables& tables = image.programTables();
    std::span entries = findFunctionSourceEntries(tables, tables.sourceFunctionOf(function.index()));
    auto entry = std::ranges::find(entries, function.index(), [](const FunctionSourceEntry& entry) -> uint32_t { return entry.function; });
    if (entry == entries.end())
        return std::span<const Latin1Character> { };
    return bytesOf(image, *entry);
}

JSString* functionSourceText(JSGlobalObject* globalObject, FunctionID function)
{
    VM& vm = globalObject->vm();
    FunctionTables::SourceTextMap& answered = FunctionTables::of(vm).sourceTexts;
    if (auto found = answered.find(function); found != answered.end()) {
        if (JSString* text = found->value.get())
            return text;
    }
    std::optional decompressed = functionSourceBytes(function);
    if (!decompressed) {
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    std::span bytes = *decompressed;
    if (!bytes.data())
        return nullptr;
    JSString* text = charactersAreAllASCII(bytes) ? jsOwnedString(vm, StringImpl::createWithoutCopying(bytes)) : jsString(vm, String::fromUTF8ReplacingInvalidSequences(bytes));
    answered.set(function, Weak<JSString>(text));
    return text;
}

std::optional<uint32_t> closureNumberOf(const JSCell* cell)
{
    switch (cell->type()) {
    case SlimClosureType:
        return uncheckedDowncast<SlimClosure>(cell)->closureNumber();
    case SlimClosureWithPropertiesType:
        return uncheckedDowncast<ReifiedSlimClosure>(cell)->closureNumber();
    case CompiledClosureType:
        return uncheckedDowncast<Closure>(cell)->closureNumber();
    default:
        return std::nullopt;
    }
}

std::optional<FunctionID> functionOf(const JSCell* cell)
{
    std::optional<uint32_t> number = closureNumberOf(cell);
    if (!number)
        return std::nullopt;
    const ProgramTables& tables = programTables();
    return FunctionID(tables.closureFunctionWithNumber(*number));
}

static FunctionID checkedFunctionOf(const JSCell* callee)
{
    std::optional<FunctionID> function = functionOf(callee);
    RELEASE_ASSERT(function);
    return *function;
}

namespace CompiledCallee {

bool isCompiledCallee(const JSCell* cell)
{
    return isCompiledCalleeType(cell->type());
}

String name(JSObject* callee)
{
    return initialNameProperty(checkedFunctionOf(callee));
}

unsigned length(JSObject* callee)
{
    return functionLengthOf(checkedFunctionOf(callee));
}

bool canAssumeNameAndLengthAreOriginal(VM& vm, JSObject* callee)
{
    ASSERT(isSlimClosureType(callee->type()));
    if (callee->type() == SlimClosureType)
        return true;
    FunctionID function = checkedFunctionOf(callee);
    constexpr unsigned attributesAtCreation = PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum;
    unsigned attributes = 0;
    JSValue length = callee->getDirect(vm, vm.propertyNames->length, attributes);
    if (!length || attributes != attributesAtCreation || length != jsNumber(functionLengthOf(function)))
        return false;
    JSValue name = callee->getDirect(vm, vm.propertyNames->name, attributes);
    if (!name)
        return false;
    if (attributes == (attributesAtCreation | PropertyAttribute::CustomValue))
        return true;
    return attributes == attributesAtCreation && name.isString() && asString(name)->tryGetValueWithoutGC() == initialNameProperty(function);
}

JSValue sourceText(JSGlobalObject* globalObject, JSObject* callee)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    CallableKind callableKind = kindData.callable();
    ASCIILiteral async = kindData.isAsync ? "async "_s : ""_s;
    bool isProgramFunction = !kindData.isInRuntimeModule;
    JSString* text = isProgramFunction ? functionSourceText(globalObject, function) : nullptr;
    RETURN_IF_EXCEPTION(scope, { });
    if (text)
        return text;
    JSValue made;
    bool isWrittenWithoutName = callableKind == CallableKind::Arrow || (!isProgramFunction && kindData.hasInferredName);
    if (isProgramFunction && callableKind == CallableKind::Arrow)
        made = jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s);
    else if (String name = isWrittenWithoutName ? emptyString() : initialNameProperty(function); kindData.isConstructor)
        made = jsMakeNontrivialString(globalObject, "class "_s, name, name.isEmpty() ? ""_s : " "_s, "{ [native code] }"_s);
    else if (isProgramFunction && callableKind == CallableKind::Method)
        made = jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "*"_s : ""_s, name, "() { [native code] }"_s);
    else
        made = jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "function* "_s : "function "_s, name, "() { [native code] }"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (isProgramFunction)
        FunctionTables::of(globalObject->vm()).sourceTexts.set(function, Weak<JSString>(asString(made)));
    return made;
}

JSString* functionStringIfCreated(VM& vm, JSObject* callee)
{
    FunctionID function = checkedFunctionOf(callee);
    if (functionKindData(function).isInRuntimeModule)
        return nullptr;
    const FunctionTables::SourceTextMap& answered = FunctionTables::of(vm).sourceTexts;
    auto found = answered.find(function);
    return found == answered.end() ? nullptr : found->value.get();
}

JSString* sourceStringIfCreated(VM& vm, JSObject* callee)
{
    const ProgramTables& tables = Image::current()->programTables();
    uint32_t function = checkedFunctionOf(callee).index();
    bool holdsSource = std::ranges::contains(findFunctionSourceEntries(tables, tables.sourceFunctionOf(function)), function, [](const FunctionSourceEntry& entry) -> uint32_t { return entry.function; });
    return holdsSource ? functionStringIfCreated(vm, callee) : nullptr;
}

FunctionKind kind(JSObject* callee)
{
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    if (kindData.isInRuntimeModule)
        return FunctionKind::Builtin;
    if (kindData.isConstructor)
        return FunctionKind::Class;
    if (kindData.isGenerator)
        return kindData.isAsync ? FunctionKind::AsyncGenerator : FunctionKind::Generator;
    if (kindData.isAsync)
        return FunctionKind::Async;
    switch (kindData.callable()) {
    case CallableKind::Arrow:
        return FunctionKind::Arrow;
    case CallableKind::Method:
        return FunctionKind::Method;
    case CallableKind::Plain:
        return FunctionDescription(function).isStrict() ? FunctionKind::Strict : FunctionKind::Sloppy;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String displayName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    if (FunctionDescription(*function).isNamedByComputedKey())
        return emptyString();
    FunctionKindData kindData = functionKindData(*function);
    bool takesNameFromOwner = kindData.hasInferredName || kindData.accessor() != MethodKind::Method;
    if (kindData.isInRuntimeModule && takesNameFromOwner)
        return emptyString();
    return writtenFunctionName(*function);
}

bool reifyProperties(VM& vm, JSObject* object)
{
    if (object->type() != SlimClosureType)
        return false;
    uncheckedDowncast<SlimClosure>(object)->reify(vm);
    return true;
}

void setPrototypeProperty(VM& vm, JSObject* callee, JSObject* prototype, unsigned attributes)
{
    RELEASE_ASSERT(isCompiledCallee(callee));
    if (callee->type() == CompiledClosureType) {
        uncheckedDowncast<Closure>(callee)->setPrototypeProperty(vm, prototype, attributes);
        return;
    }
    reifyProperties(vm, callee);
    callee->putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void setNameProperty(JSGlobalObject* globalObject, JSObject* callee, JSString* name, unsigned attributes)
{
    VM& vm = globalObject->vm();
    RELEASE_ASSERT(isCompiledCallee(callee));
    if (callee->type() == CompiledClosureType)
        uncheckedDowncast<Closure>(callee)->setNameProperty(vm, name, attributes);
    else {
        reifyProperties(vm, callee);
        callee->putDirect(vm, vm.propertyNames->name, name, attributes);
    }
}

Structure* cachedBoundFunctionStructure(JSObject* callee)
{
    if (!isSlimClosureType(callee->type()))
        return nullptr;
    return closureTableOf(callee).asyncBoundFunctionStructure();
}

void setCachedBoundFunctionStructure(VM&, JSObject* callee, Structure* structure)
{
    if (isSlimClosureType(callee->type()))
        closureTableOf(callee).setAsyncBoundFunctionStructure(structure);
}

} // namespace CompiledCallee

DeclaredAs declaredAs(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    if (!function)
        function = compiledBuiltinFunctionID(cell);
    if (!function)
        return isOmittedFunctionPlaceholder(cell) ? DeclaredAs::Function : DeclaredAs::NotCompiled;
    FunctionKindData kindData = functionKindData(*function);
    if (kindData.isConstructor)
        return FunctionDescription(*function).isNakedConstructor() ? DeclaredAs::Function : DeclaredAs::Class;
    if (kindData.isGenerator)
        return kindData.isAsync ? DeclaredAs::AsyncGeneratorFunction : DeclaredAs::GeneratorFunction;
    return kindData.isAsync ? DeclaredAs::AsyncFunction : DeclaredAs::Function;
}

bool hasOwnAwait(JSCell* cell)
{
    if (declaredAs(cell) != DeclaredAs::AsyncFunction)
        return false;
    std::optional<FunctionID> function = functionOf(cell);
    return function && !std::ranges::binary_search(programTables().asyncFunctionsWithoutAwait(), sourceFunctionOf(*function).index());
}

bool mayImportOwnOriginal(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    return !function || std::ranges::binary_search(programTables().functionsImportingOwnOriginal(), sourceFunctionOf(*function).index());
}

Vector<String> requestedOriginals(JSCell* cell)
{
    Vector<String> specifiers;
    std::optional<FunctionID> function = functionOf(cell);
    if (!function)
        return specifiers;
    const Image& image = *Image::current();
    for (const RequestedOriginalData& asked : std::ranges::equal_range(programTables().requestedOriginals(), sourceFunctionOf(*function).index(), { }, &RequestedOriginalData::function))
        specifiers.append(image.text().createString(StringID(asked.specifier)));
    return specifiers;
}

String closureDeclaredName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    FunctionKindData kindData = functionKindData(*function);
    if (kindData.hasInferredName || kindData.accessor() != MethodKind::Method)
        return emptyString();
    return writtenFunctionName(*function);
}

bool belongsToBuiltinModule(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    return function && functionKindData(*function).isInRuntimeModule;
}

bool isCompiledClassObject(JSFunction* candidate)
{
    std::optional<FunctionID> function = functionOf(candidate);
    return function && functionKindData(*function).isConstructor;
}

bool isClosureOf(JSCell* cell, FunctionID function)
{
    BuiltinID calledBuiltin = builtinOfFunctionInFrames(function);
    if (std::optional<FunctionID> closureFunction = functionOf(cell))
        return !calledBuiltin && sourceFunctionOf(*closureFunction) == sourceFunctionOf(function);
    std::optional<uint32_t> implemented = calledBuiltin ? std::optional<uint32_t>(calledBuiltin.index()) : programTables().builtinImplementedBy(function.index());
    JSObject* object = cell->getObject();
    if (!implemented || !object)
        return false;
    JSGlobalObject* realm = object->realmMayBeNull();
    return realm && findExistingBuiltin(realm, BuiltinID(*implemented)) == object;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
