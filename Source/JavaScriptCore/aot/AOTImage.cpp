/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImage.h"

#if ENABLE(FTL_JIT)

#include "AOTCompiler.h"
#include "CachedTypes.h"
#include "CodeBlock.h"
#include "CodeCache.h"
#include "ExecutableAllocator.h"
#include "FunctionExecutable.h"
#include "JSCBytecodeCacheVersion.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleEnvironment.h"
#include "JSModuleRecord.h"
#include "Options.h"
#include "SourceProvider.h"
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBuilder);
WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

uint64_t imageStamp()
{
    // The code has the layout of the engine's objects in it, and the order of the runtime table. Not proof against every change
    // (an image belongs in the executable that has the engine in it), but it catches the ones that come from mixing builds.
    uint64_t stamp = computeJSCBytecodeCacheVersion();
    auto mix = [&](uint64_t value) {
        stamp = (stamp ^ value) * 0x100000001b3ULL;
        stamp ^= stamp >> 29;
    };
    mix(numberOfEntries);
    mix(lowestAccessibleAddress()); // How a frame says which function it is a frame of.
    mix(Instance::offsetOfData());
    mix(numberOfStubs);
    mix(numOpcodeIDs);
    mix(sizeof(VM));
    mix(sizeof(JSGlobalObject));
    mix(sizeof(CodeBlock));
    mix(sizeof(Data));
    mix(VM::offsetOfSoftStackLimit());
    mix(VM::offsetOfHeapBarrierThreshold());
    mix(CodeBlock::offsetOfJITData());
    mix(JSGlobalObject::offsetOfGlobalLexicalBindingEpoch());
#if CPU(ARM64)
    mix(1 | MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics() << 8);
#elif CPU(X86_64)
    mix(2);
#endif
    return stamp;
}

// ---- Building

void ImageBuilder::add(ImageKey key, uint64_t rank, CompiledCode&& code)
{
    // Where they pointed depends on where the code was when it was compiled, which is nothing to do with the code.
    for (auto& call : code.info.stubCalls)
        memset(code.bytes.mutableSpan().data() + call.offset, 0, sizeof(uint32_t));
    // Nor has the number it went by.
    memset(code.bytes.mutableSpan().data() + OBJECT_OFFSETOF(CodeHeader, index), 0, sizeof(uint32_t));
    Locker locker { m_lock };
    m_functions.append(Function { key, rank, WTF::move(code) });
}

Vector<uint8_t> ImageBuilder::finish()
{
    Locker locker { m_lock };
    std::ranges::sort(m_functions, [](const Function& a, const Function& b) {
        return a.rank < b.rank;
    });

    unsigned capacity = 16;
    while (capacity < m_functions.size() * 2)
        capacity *= 2;

    // Where everything goes. The stubs come first, and again whenever the last copy is about to be out of reach.
    const StubBlob& stubs = stubBlob();
    Vector<size_t> stubsAt;
    Vector<std::pair<size_t, size_t>> placement; // Of each function: where it is, and where the stubs it calls are.
    size_t recordsSize = 0;
    size_t codeSize = 0;
    for (auto& function : m_functions) {
        recordsSize += sizeof(ImageFunction) + function.code.info.calleeSaveRegisters.registerCount() * sizeof(ImageCalleeSave) + function.code.info.catchEntrypoints.size() * sizeof(ImageCatchEntrypoint) + function.code.info.sites.size() * sizeof(Site) + function.code.info.knownCallees.size() * sizeof(ImageKey);
        RELEASE_ASSERT(function.code.info.sites.size() == function.code.info.numSlots);
        codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize);
        if (usesStubs && (stubsAt.isEmpty() || codeSize + function.code.bytes.size() - stubsAt.last() > reachOfStubCall)) {
            stubsAt.append(codeSize);
            codeSize = WTF::roundUpToMultipleOf<imageFunctionAlignment>(codeSize + stubs.bytes.size());
        }
        placement.append({ codeSize, stubsAt.isEmpty() ? 0 : stubsAt.last() });
        codeSize += function.code.bytes.size();
    }
    RELEASE_ASSERT(recordsSize < std::numeric_limits<uint32_t>::max());

    ImageHeader header { };
    header.magic = imageMagic;
    header.stamp = imageStamp();
    header.tableOffset = sizeof(ImageHeader);
    header.tableCapacity = capacity;
    header.recordsOffset = header.tableOffset + capacity * sizeof(ImageKey);
    header.recordsSize = recordsSize;
    header.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(static_cast<size_t>(header.recordsOffset) + recordsSize);
    header.codeSize = codeSize;
    header.size = WTF::roundUpToMultipleOf<imagePageSize>(header.codeOffset + codeSize);
    header.numberOfFunctions = m_functions.size();
    for (unsigned i = 0; i < numberOfStubs; ++i)
        header.stubOffsets[i] = stubs.offsets[i];

    Vector<uint8_t> image;
    image.fill(0, header.size);
    uint8_t* base = image.mutableSpan().data();
    memcpy(base, &header, sizeof(header));
    auto* table = reinterpret_cast<ImageKey*>(base + header.tableOffset);
    uint8_t* records = base + header.recordsOffset;
    uint8_t* code = base + header.codeOffset;

    for (size_t at : stubsAt)
        memcpy(code + at, stubs.bytes.span().data(), stubs.bytes.size());

    Vector<uint32_t> functionInBucket;
    functionInBucket.fill(0, capacity);
    size_t recordAt = 0;
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& function = m_functions[index];
        auto& info = function.code.info;
        auto [codeAt, stubsForThis] = placement[index];

        ImageFunction record { };
        record.codeOffset = codeAt;
        record.codeSize = function.code.bytes.size();
        record.entryOffset = info.entryOffset;
        record.arityCheckOffset = info.arityCheckOffset;
        record.directEntryOffset = info.directEntryOffset;
        record.frameSizeInBytes = info.frameSizeInBytes;
        record.numSlots = info.numSlots;
        record.bytecodeHash = info.bytecodeHash;
        record.numberOfCalleeSaves = info.calleeSaveRegisters.registerCount();
        record.numberOfCatchEntrypoints = info.catchEntrypoints.size();
        record.numberOfKnownCallees = info.knownCallees.size();
        record.usesStaticImports = info.usesStaticImports;

        ImageKey key = function.key;
        key.record = recordAt + 1;
        unsigned mask = capacity - 1;
        unsigned bucket = key.hash() & mask;
        bool duplicate = false;
        while (table[bucket].record) {
            if (table[bucket].sameFunction(key)) {
                duplicate = true;
                break;
            }
            bucket = (bucket + 1) & mask;
        }
        if (duplicate) {
            // The same function again (the same text evaluated twice) is fine. Two functions that cannot be told apart are not:
            // neither is in the image then, since either could be asked for.
            auto& earlier = m_functions[functionInBucket[bucket]].code.bytes;
            if (earlier.size() != function.code.bytes.size() || memcmp(earlier.span().data(), function.code.bytes.span().data(), earlier.size()))
                table[bucket].kind = std::numeric_limits<uint32_t>::max();
            continue;
        }
        table[bucket] = key;
        functionInBucket[bucket] = index;

        memcpy(records + recordAt, &record, sizeof(record));
        recordAt += sizeof(record);
        for (unsigned i = 0; i < info.calleeSaveRegisters.registerCount(); ++i) {
            const RegisterAtOffset& entry = info.calleeSaveRegisters.at(i);
            ImageCalleeSave save { entry.reg().index(), static_cast<int32_t>(entry.offset()) };
            memcpy(records + recordAt, &save, sizeof(save));
            recordAt += sizeof(save);
        }
        for (auto& [bytecodeOffset, codeOffset] : info.catchEntrypoints) {
            ImageCatchEntrypoint entrypoint { bytecodeOffset, codeOffset };
            memcpy(records + recordAt, &entrypoint, sizeof(entrypoint));
            recordAt += sizeof(entrypoint);
        }
        memcpy(records + recordAt, info.sites.span().data(), info.sites.size() * sizeof(Site));
        recordAt += info.sites.size() * sizeof(Site);
        memcpy(records + recordAt, info.knownCallees.span().data(), info.knownCallees.size() * sizeof(ImageKey));
        recordAt += info.knownCallees.size() * sizeof(ImageKey);

        memcpy(code + codeAt, function.code.bytes.span().data(), function.code.bytes.size());
        uint32_t indexInProgram = index;
        memcpy(code + codeAt + OBJECT_OFFSETOF(CodeHeader, index), &indexInProgram, sizeof(indexInProgram));
        for (auto& call : info.stubCalls)
            retargetStubCall(code, codeAt + call.offset, stubsForThis + stubs.offsets[static_cast<unsigned>(call.stub)], call.isTailCall);
    }

    // With every function in its place: the calls from one to another.
    for (size_t index = 0; index < m_functions.size(); ++index) {
        auto& info = m_functions[index].code.info;
        size_t codeAt = placement[index].first;
        for (auto& call : info.stubCalls) {
            if (call.function == StubCall::noFunction)
                continue;
            const ImageKey& key = info.knownCallees[call.function];
            unsigned mask = capacity - 1;
            for (unsigned bucket = key.hash() & mask; table[bucket].record; bucket = (bucket + 1) & mask) {
                if (!table[bucket].sameFunction(key))
                    continue;
                size_t target = functionInBucket[bucket];
                auto& targetInfo = m_functions[target].code.info;
                if (call.isDirect && !targetInfo.directEntryOffset)
                    break;
                size_t targetAt = placement[target].first + (call.isDirect ? targetInfo.directEntryOffset : call.skipsArityCheck ? targetInfo.entryOffset : targetInfo.arityCheckOffset);
                size_t from = codeAt + call.offset;
                if ((from > targetAt ? from - targetAt : targetAt - from) <= reachOfStubCall)
                    retargetStubCall(code, from, targetAt, false);
                break;
            }
        }
    }
    m_functions.clear();
    return image;
}

// ---- Running

namespace {

struct Registry {
    Lock lock;
    Vector<Image*, 2> images;
    std::atomic<bool> hasAny { false };
};

Registry& registry()
{
    static NeverDestroyed<Registry> instance;
    return instance;
}

} // anonymous namespace

Image* Image::registerImage(std::span<const uint8_t> data, const void* code)
{
    if (data.size() < sizeof(ImageHeader))
        return nullptr;
    auto& header = *reinterpret_cast<const ImageHeader*>(data.data());
    if (header.magic != imageMagic || header.stamp != imageStamp() || header.size > data.size())
        return nullptr;
    if (!hasOneBitSet(header.tableCapacity)
        || static_cast<uint64_t>(header.tableOffset) + static_cast<uint64_t>(header.tableCapacity) * sizeof(ImageKey) > header.recordsOffset
        || static_cast<uint64_t>(header.recordsOffset) + header.recordsSize > header.codeOffset
        || header.codeOffset + header.codeSize > header.size)
        return nullptr;
    // Its functions know what their numbers are.
    if (!reserveFunctionIndicesForImage(header.numberOfFunctions))
        return nullptr;

    auto* image = new Image(data, code);
    auto& all = registry();
    Locker locker { all.lock };
    all.images.append(image);
    all.hasAny.store(true, std::memory_order_release);
    return image;
}

bool Image::containsCode(const void* pointer)
{
    auto& all = registry();
    if (!all.hasAny.load(std::memory_order_acquire))
        return false;
    // (Nothing is ever taken out, and this may be asked while the thread that has the lock is stopped.)
    for (unsigned i = 0; i < all.images.size(); ++i) {
        Image* image = all.images[i];
        if (static_cast<const uint8_t*>(pointer) - static_cast<const uint8_t*>(image->m_code) < static_cast<ptrdiff_t>(image->header().codeSize) && pointer >= image->m_code)
            return true;
    }
    return false;
}

Image* Image::registerImageFromFile(const char* path)
{
    auto contents = FileSystem::readEntireFile(String::fromUTF8(path));
    if (!contents || contents->size() < sizeof(ImageHeader))
        return nullptr;
    // Both for good.
    auto* data = new Vector<uint8_t>(WTF::move(*contents));
    auto& header = *reinterpret_cast<const ImageHeader*>(data->span().data());
    if (header.magic != imageMagic || header.codeOffset + header.codeSize > data->size())
        return nullptr;
    void* code = nullptr;
    if (header.codeSize) {
        RefPtr<ExecutableMemoryHandle> handle = ExecutableAllocator::singleton().allocate(header.codeSize, JITCompilationCanFail);
        if (!handle)
            return nullptr;
        code = handle->start().untaggedPtr();
        performJITMemcpy<jitMemcpyRepatch>(code, data->span().data() + header.codeOffset, header.codeSize);
        MacroAssembler::cacheFlush(code, header.codeSize);
        auto* forGood = handle.leakRef();
        UNUSED_VARIABLE(forGood);
    }
    return registerImage(data->span(), code);
}

bool Image::hasAny()
{
    return registry().hasAny.load(std::memory_order_acquire);
}

const void* Image::addressOfStub(Stub stub)
{
    if (!usesStubs || !hasAny())
        return nullptr;
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        if (image->header().codeSize)
            return static_cast<const uint8_t*>(image->m_code) + image->header().stubOffsets[static_cast<unsigned>(stub)];
    }
    return nullptr;
}

const ImageFunction* Image::lookup(const ImageKey& key) const
{
    auto& header = this->header();
    auto* table = reinterpret_cast<const ImageKey*>(m_data.data() + header.tableOffset);
    unsigned mask = header.tableCapacity - 1;
    for (unsigned bucket = key.hash() & mask; table[bucket].record; bucket = (bucket + 1) & mask) {
        if (table[bucket].sameFunction(key))
            return reinterpret_cast<const ImageFunction*>(m_data.data() + header.recordsOffset + table[bucket].record - 1);
    }
    return nullptr;
}

std::pair<Image*, const ImageFunction*> Image::find(const ImageKey& key)
{
    auto& all = registry();
    Locker locker { all.lock };
    for (Image* image : all.images) {
        if (auto* function = image->lookup(key))
            return { image, function };
    }
    return { nullptr, nullptr };
}

uint32_t moduleIDFor(SourceProvider& provider)
{
    if (uint32_t id = provider.aotModuleID())
        return id;
    // Without an embedder to number the modules, a module is known by its text.
    if (!Options::aotImagePath())
        return 0;
    uint32_t id = provider.hash() | 1;
    provider.setAOTModuleID(id);
    return id;
}

ImageKey imageKeyForTopLevelCode(uint32_t module)
{
    ImageKey key;
    key.module = module;
    key.start = std::numeric_limits<uint32_t>::max();
    key.kind = 0xff << 1;
    return key;
}

std::optional<ImageKey> imageKeyFor(ScriptExecutable* scriptExecutable, CodeSpecializationKind kind)
{
    SourceProvider* provider = scriptExecutable->source().provider();
    // The source of a constructor that nobody wrote is one of the engine's own.
    if (auto* function = dynamicDowncast<FunctionExecutable>(scriptExecutable); function && function->unlinkedExecutable()->isBuiltinDefaultClassConstructor() && function->topLevelExecutable())
        provider = function->topLevelExecutable()->source().provider();
    if (!provider)
        return std::nullopt;
    uint32_t module = moduleIDFor(*provider);
    if (!module)
        return std::nullopt;
    auto* executable = dynamicDowncast<FunctionExecutable>(scriptExecutable);
    if (!executable) {
        if (scriptExecutable->isEvalExecutable())
            return std::nullopt;
        return imageKeyForTopLevelCode(module);
    }
    auto functionKey = orderFunctionKey(*executable->unlinkedExecutable(), executable->source());
    if (!functionKey)
        return std::nullopt;
    ImageKey key;
    key.module = module;
    key.start = functionKey->start;
    key.kind = static_cast<uint32_t>(functionKey->kind) << 1 | (kind == CodeSpecializationKind::CodeForConstruct);
    return key;
}

unsigned hashOfCode(std::span<const uint8_t> code)
{
    unsigned hash = 2166136261u;
    for (uint8_t byte : code)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

static std::atomic<unsigned> s_installedFromImage;

static String nameForLogging(ScriptExecutable* executable)
{
    if (auto* function = dynamicDowncast<FunctionExecutable>(executable))
        return function->ecmaName().string();
    return "(top level)"_s;
}

ImageCode findInImage(ScriptExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSScope* scope)
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* path = Options::aotImagePath(); path && !Options::aotWriteImage()) {
            if (!Image::registerImageFromFile(path))
                dataLogLn("AOT: ", path, " is not an image for this engine");
        }
        if (Options::aotReportStats()) {
            atexit([] {
                dataLogLn("AOT: ", s_installedFromImage.load(), " functions ran from an image");
            });
        }
    });
    if (!Image::hasAny())
        return { };

    auto key = imageKeyFor(executable, kind);
    if (!key) {
        if (Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: no key for ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " bytecode ", unlinkedCodeBlock->instructionsSize());
        return { };
    }
    auto [image, function] = Image::find(*key);
    if (!function) {
        if (Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: not in the image: ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") bytecode ", unlinkedCodeBlock->instructionsSize());
        return { };
    }

    // The code finds what it has of the realm by its own number, so that is for one function of the realm. The same text evaluated
    // a second time is another function: it has constants of its own, for one thing.
    if (Instance* instance = scope->realm()->aotInstance()) {
        Data* data = instance->data[reinterpret_cast<const CodeHeader*>(image->codeFor(*function))->index];
        if (data && data->executable != executable)
            return { };
    }

    if (function->usesStaticImports) {
        while (scope && scope->type() != ModuleEnvironmentType)
            scope = scope->next();
        auto* record = scope ? dynamicDowncast<JSModuleRecord>(uncheckedDowncast<JSModuleEnvironment>(scope)->moduleRecord()) : nullptr;
        if (!record || !record->isLinkedAsInImage(scope->globalObject())) {
            if (Options::aotVerbose()) [[unlikely]]
                dataLogLn("AOT: the module of ", nameForLogging(executable), " of ", executable->source().provider()->sourceURL(), " is not linked the way it was compiled for");
            return { };
        }
    }

    if (Options::aotValidateImage()) [[unlikely]] {
        if (hashOfBytecode(unlinkedCodeBlock) != function->bytecodeHash) {
            dataLogLn("AOT: the image's code for ", nameForLogging(executable), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") was compiled from other bytecode");
            return { };
        }
    }
    if (Options::aotVerbose()) [[unlikely]]
        dataLogLn("AOT: ", nameForLogging(executable), " (module ", key->module, " start ", key->start, " kind ", key->kind, ") is at ", RawPointer(image->codeFor(*function)), " size ", function->codeSize, " hash ", hashOfCode({ image->codeFor(*function), function->codeSize }));
    return { image, function };
}

Ref<JITCode> codeFromImage(ImageCode code, UnlinkedCodeBlock* unlinkedCodeBlock)
{
    auto [image, function] = code;
    s_installedFromImage++;
    return adoptRef(*new JITCode(const_cast<uint8_t*>(image->codeFor(*function)), *function, JITCode::wayInto(unlinkedCodeBlock)));
}

// ---- Options::aotWriteImage()

void addToImageBeingWritten(ScriptExecutable* executable, CodeSpecializationKind kind, const JITCode& jitCode)
{
    static NeverDestroyed<ImageBuilder> builder;
    static std::atomic<uint64_t> rank;
    static std::once_flag once;
    std::call_once(once, [] {
        atexit([] {
            Vector<uint8_t> image = builder.get().finish();
            if (!FileSystem::overwriteEntireFile(String::fromUTF8(Options::aotImagePath()), image.span()))
                dataLogLn("AOT: cannot write ", Options::aotImagePath());
        });
    });

    auto key = imageKeyFor(executable, kind);
    if (!key)
        return;
    CompiledCode code;
    code.info = jitCode.info();
    code.bytes.append(std::span { static_cast<const uint8_t*>(const_cast<JITCode&>(jitCode).dataAddressAtOffset(0)), jitCode.info().codeSize });
    builder.get().add(*key, rank++, WTF::move(code));
}

} // namespace AOT

bool registerAOTImage(std::span<const uint8_t> image, const void* code)
{
    return !!AOT::Image::registerImage(image, code);
}

static void compileAllIn(VM&, UnlinkedCodeBlock&, AOTCompileAllResult&);

static void compileAllOf(VM& vm, UnlinkedFunctionExecutable& executable, AOTCompileAllResult& result)
{
    auto [forCall, forConstruct] = executable.codeBlocksDecodingCached(vm);
    for (UnlinkedFunctionCodeBlock* codeBlock : { forCall, forConstruct }) {
        if (!codeBlock)
            continue;
        result.functions++;
        result.bytecodeBytes += codeBlock->instructionsSize();
        AOT::CompiledCode code;
        if (AOT::compileForImage(vm, codeBlock, code)) {
            result.compiled++;
            result.codeBytes += code.bytes.size();
        }
        compileAllIn(vm, *codeBlock, result);
    }
}

static void compileAllIn(VM& vm, UnlinkedCodeBlock& codeBlock, AOTCompileAllResult& result)
{
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i)
        compileAllOf(vm, *codeBlock.functionDecl(i), result);
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i)
        compileAllOf(vm, *codeBlock.functionExpr(i), result);
}

std::optional<AOTCompileAllResult> aotCompileAllFunctions(VM& vm, const SourceCode& source, bool isModule)
{
    DeferGC deferGC(vm);
    vm.keepUnlinkedCode();
    ParserError error;
    UnlinkedCodeBlock* codeBlock = isModule
        ? static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes))
        : static_cast<UnlinkedCodeBlock*>(recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, std::numeric_limits<unsigned>::max(), OptimizeBytecode::Yes));
    std::optional<AOTCompileAllResult> result;
    if (!error.isValid() && codeBlock) {
        result = AOTCompileAllResult { };
        result->functions++;
        result->bytecodeBytes += codeBlock->instructionsSize();
        AOT::CompiledCode code;
        if (AOT::compileForImage(vm, codeBlock, code)) {
            result->compiled++;
            result->codeBytes += code.bytes.size();
        }
        compileAllIn(vm, *codeBlock, *result);
    }
    vm.stopKeepingUnlinkedCode();
    return result;
}

std::optional<size_t> aotImageSize(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(AOT::ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const AOT::ImageHeader*>(image.data());
    if (header.magic != AOT::imageMagic || header.size > image.size())
        return std::nullopt;
    return static_cast<size_t>(header.size);
}

std::optional<std::pair<size_t, size_t>> aotImageCodeRange(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(AOT::ImageHeader))
        return std::nullopt;
    auto& header = *reinterpret_cast<const AOT::ImageHeader*>(image.data());
    if (header.magic != AOT::imageMagic || header.size > image.size() || header.codeOffset > header.size)
        return std::nullopt;
    return std::pair { static_cast<size_t>(header.codeOffset), static_cast<size_t>(header.size - header.codeOffset) };
}

} // namespace JSC

#endif // ENABLE(FTL_JIT)
