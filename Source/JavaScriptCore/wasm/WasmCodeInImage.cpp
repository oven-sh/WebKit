/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "WasmCodeInImage.h"

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTImage.h"
#include "AOTTypedThreads.h"
#include "B3Operations.h"
#include "CCallHelpers.h"
#include "JSWebAssemblyInstance.h"
#include "LLIntData.h"
#include "LinkBuffer.h"
#include "NativeCalleeRegistry.h"
#include "WasmBinding.h"
#include "WasmCalleeGroup.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WasmOMGIRGenerator.h"
#include "WasmOperations.h"
#include <wtf/SHA1.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

using B3::operationMemoryCopy;
using B3::operationMemoryFill;

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeInImage);

const uint8_t* CodeInImage::s_start;
size_t CodeInImage::s_size;

static std::array<void*, CodeInImage::numberOfOperations> imageCodeOperations()
{
    return { {
#define ADDRESS_OF_OPERATION(operation) std::bit_cast<void*>(tagCFunction<OperationPtrTag>(operation)),
        FOR_EACH_WASM_IMAGE_CODE_OPERATION(ADDRESS_OF_OPERATION)
#undef ADDRESS_OF_OPERATION
    } };
}

std::optional<unsigned> CodeInImage::indexOfOperation(void* operation)
{
    auto operations = imageCodeOperations();
    for (unsigned index = 0; index < numberOfOperations; ++index) {
        if (operations[index] == operation)
            return index;
    }
    return std::nullopt;
}

unsigned CodeInImage::indexOfRTT(const ModuleInformation& info, TypeSignatureIndex type)
{
    return numberOfSharedPointers + info.internalFunctionCount() + type.rawIndex();
}

unsigned CodeInImage::indexOfJSEntrypointCallee(const ModuleInformation& info, unsigned entry)
{
    return numberOfSharedPointers + info.internalFunctionCount() + info.typeCount() + entry;
}

#if ENABLE(AOT)

static constexpr size_t codeAlignment = 16;

template<typename DidLink>
static Vector<uint8_t> link(CCallHelpers& jit, LinkBuffer::Profile profile, const DidLink& didLink)
{
    Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), profile);
    auto* start = static_cast<const uint8_t*>(linkBuffer.entrypoint<JITCompilationPtrTag>().untaggedPtr());
    size_t size = linkBuffer.size();
    MacroAssemblerCodeRef<JITCompilationPtrTag> code = FINALIZE_CODE_IF(false, linkBuffer, JITCompilationPtrTag, nullptr, "WebAssembly code for an image");
    UNUSED_VARIABLE(code);
    didLink(start);
    return Vector<uint8_t>(std::span { start, size });
}

static Vector<uint8_t> generateThrowStub()
{
    CCallHelpers jit;
    jit.preserveReturnAddressAfterCall(GPRInfo::argumentGPR2);
    jit.move(GPRInfo::wasmContextInstancePointer, GPRInfo::argumentGPR0);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR0, JSWebAssemblyInstance::offsetOfVM()), GPRInfo::argumentGPR3);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR3, VM::topEntryFrameOffset()), GPRInfo::argumentGPR3);
    jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(GPRInfo::argumentGPR3);
    jit.prepareWasmCallOperation(GPRInfo::argumentGPR0);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR0, JSWebAssemblyInstance::offsetOfImageCodePointers()), GPRInfo::nonPreservedNonArgumentGPR0);
    jit.call(CCallHelpers::Address(GPRInfo::nonPreservedNonArgumentGPR0, CodeInImage::indexOfThrowOperation * sizeof(void*)), OperationPtrTag);
    jit.farJump(GPRInfo::returnValueGPR, ExceptionHandlerPtrTag);
    jit.breakpoint();
    return link(jit, LinkBuffer::Profile::WasmThunk, [](const uint8_t*) { });
}

static Vector<uint8_t> generateImportStub(const ModuleInformation& info, unsigned importIndex)
{
    CCallHelpers jit;
    emitWasmToWasm(jit, info, importIndex);
    return link(jit, LinkBuffer::Profile::WasmThunk, [](const uint8_t*) { });
}

static RegisterAtOffsetList savedRegistersFrom(const SavedRegistersInImage& saved)
{
    RegisterSet registers;
    for (unsigned reg = 0; reg < 64; ++reg) {
        if (saved.registers >> reg & 1)
            registers.add(Reg::fromIndex(reg), IgnoreVectors);
    }
    RegisterAtOffsetList list(registers);
    if (list.registerCount())
        list.adjustOffsets(saved.offsetOfFirst - list.at(0).offset());
    return list;
}

static Vector<uint8_t> generateJSEntrypoint(const ModuleInformation& info, const RTT& rtt, unsigned entry)
{
    CCallHelpers jit;
    RegisterAtOffsetList registersToSpill = rtt.icEntrypointSavedRegisters();
    bool isEmitted = rtt.emitJSToWasmICEntrypoint(jit, registersToSpill, nullptr, RTT::ICEntrypointImagePointers { CodeInImage::indexOfJSEntrypointCallee(info, entry), CodeInImage::indexOfGenericJSEntrypoint });
    RELEASE_ASSERT(isEmitted);
    return link(jit, LinkBuffer::Profile::WasmThunk, [](const uint8_t*) { });
}

struct FunctionForImage {
    Vector<uint8_t> bytes;
    Vector<std::pair<uint32_t, uint32_t>> calls;
    Vector<uint32_t> callsToThrowStub;
    SavedRegistersInImage savedRegisters { 0, 0, 0 };
    String whyDeclined;
};

static void compileFunction(Module& module, CalleeGroup& group, const ModuleInformation& info, MemoryMode mode, FunctionCodeIndex functionIndex, FunctionForImage& result)
{
    if (info.usesExceptions(functionIndex)) {
        result.whyDeclined = "it throws or catches exceptions"_s;
        return;
    }
    FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(functionIndex);
    const RTT& signature = info.rtt(info.internalFunctionTypeSignatureIndices[functionIndex]);
    Ref<IPIntCallee> profiledCallee = group.ipintCalleeFromFunctionIndexSpace(functionIndexSpace);
    Ref<OMGCallee> callee = OMGCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { profiledCallee });
    Vector<UnlinkedWasmToWasmCall> unlinkedCalls;
    CodeForImage codeForImage;
    CompilationContext context;
    context.codeForImage = &codeForImage;
    auto compiled = parseAndCompileOMG(context, profiledCallee.get(), callee.get(), info.functions[functionIndex], signature, unlinkedCalls, module, group, info, mode, CompilationMode::OMGMode, functionIndex, UINT32_MAX);
    if (!compiled) {
        result.whyDeclined = compiled.error();
        return;
    }
    if (!codeForImage.whyDeclined.isNull()) {
        result.whyDeclined = codeForImage.whyDeclined;
        return;
    }
    if (!(*compiled)->exceptionHandlers.isEmpty()) {
        result.whyDeclined = "it catches exceptions"_s;
        return;
    }
    result.bytes = link(*context.wasmEntrypointJIT, LinkBuffer::Profile::WasmOMG, [&](const uint8_t* start) {
        for (auto& call : unlinkedCalls)
            result.calls.append({ safeCast<uint32_t>(static_cast<const uint8_t*>(call.callLocation.dataLocation()) - start), safeCast<uint32_t>(info.toCodeIndex(call.functionIndexSpace).rawIndex()) });
    });
    result.callsToThrowStub = WTF::move(codeForImage.callsToThrowStub);
    auto& savedRegisters = (*compiled)->entrypoint.calleeSaveRegisters;
    for (auto& entry : savedRegisters) {
        if (entry.width() != conservativeWidthWithoutVectors(entry.reg())) {
            result.whyDeclined = "it saves a whole vector register"_s;
            return;
        }
        result.savedRegisters.registers |= 1ull << entry.reg().index();
    }
    if (savedRegisters.registerCount())
        result.savedRegisters.offsetOfFirst = safeCast<int32_t>(savedRegisters.at(0).offset());
    RegisterAtOffsetList read = savedRegistersFrom(result.savedRegisters);
    bool isSame = read.registerCount() == savedRegisters.registerCount();
    for (size_t index = 0; isSame && index < read.registerCount(); ++index)
        isSame = read.at(index) == savedRegisters.at(index);
    if (!isSame)
        result.whyDeclined = "its saved registers are not in the usual order"_s;
}

static void linkCall(Vector<uint8_t>& code, size_t endOfCall, size_t target)
{
#if CPU(X86_64)
    RELEASE_ASSERT(endOfCall >= 5 && code[endOfCall - 5] == 0xe8);
    int32_t distance = safeCast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(endOfCall));
    memcpy(code.mutableSpan().data() + endOfCall - sizeof(distance), &distance, sizeof(distance));
#elif CPU(ARM64)
    size_t at = endOfCall - sizeof(uint32_t);
    int64_t distance = static_cast<int64_t>(target) - static_cast<int64_t>(at);
    RELEASE_ASSERT(!(distance & 3) && distance >= -(1 << 27) && distance < (1 << 27));
    uint32_t instruction = 0x94000000u | (static_cast<uint32_t>(distance >> 2) & 0x3ffffffu);
    memcpy(code.mutableSpan().data() + at, &instruction, sizeof(instruction));
#else
    UNUSED_PARAM(code);
    UNUSED_PARAM(endOfCall);
    UNUSED_PARAM(target);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

std::array<uint8_t, 20> CodeInImage::digestOf(std::span<const uint8_t> source)
{
    SHA1 sha1;
    sha1.addBytes(source);
    SHA1::Digest digest;
    sha1.computeHash(digest);
    return digest;
}

std::optional<ModuleForImage> CodeInImage::compile(VM& vm, std::span<const uint8_t> source, String& whyDeclined)
{
    if (!isX86_64() && !isARM64()) {
        whyDeclined = "this CPU is not supported"_s;
        return std::nullopt;
    }
    auto validated = Module::validateSync(vm, Vector<uint8_t>(source));
    if (!validated) {
        whyDeclined = validated.error();
        return std::nullopt;
    }
    Ref<Module> module = WTF::move(*validated);
    const ModuleInformation& info = module->moduleInformation();
    unsigned numberOfFunctions = info.internalFunctionCount();
    if (!numberOfFunctions) {
        whyDeclined = "it has no functions"_s;
        return std::nullopt;
    }
    if (info.hasGCObjectTypes()) {
        whyDeclined = "it has types of objects"_s;
        return std::nullopt;
    }
    if (info.exceptionIndexSpaceSize()) {
        whyDeclined = "it has exceptions"_s;
        return std::nullopt;
    }
    MemoryMode mode = info.memoryCount() && !info.memory(0).isMemory64() ? MemoryMode::Signaling : MemoryMode::BoundsChecking;
    Ref<CalleeGroup> group = module->compileSync(vm, mode);
    if (!group->runnable()) {
        whyDeclined = group->errorMessage();
        return std::nullopt;
    }

#if CPU(X86_64)
    bool usesVectors = false;
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        usesVectors |= info.usesSIMD(FunctionCodeIndex(index));
    bool supportedAVX = MacroAssembler::setSupportsAVX(usesVectors || MacroAssembler::supportsAVX());
    auto restoreAVX = makeScopeExit([&] {
        MacroAssembler::setSupportsAVX(supportedAVX);
    });
#endif

    Vector<FunctionForImage> functions(numberOfFunctions);
    AOT::Typed::forEachIndexInParallel(numberOfFunctions, [&](unsigned index) {
        compileFunction(module.get(), group.get(), info, mode, FunctionCodeIndex(index), functions[index]);
    });
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        if (!functions[index].whyDeclined.isNull()) {
            whyDeclined = makeString("function "_s, index, ": "_s, functions[index].whyDeclined);
            return std::nullopt;
        }
    }

    ModuleForImage result;
    result.digest = digestOf(source);
    result.sourceSize = safeCast<uint32_t>(source.size());
    result.isForSignalingMemory = mode == MemoryMode::Signaling;
    auto place = [&](const Vector<uint8_t>& bytes) {
        result.code.insertFill(result.code.size(), isX86() ? 0xcc : 0, WTF::roundUpToMultipleOf<codeAlignment>(result.code.size()) - result.code.size());
        uint32_t at = safeCast<uint32_t>(result.code.size());
        result.code.appendVector(bytes);
        return at;
    };
    uint32_t throwStub = place(generateThrowStub());
    for (unsigned index = 0; index < info.importFunctionCount(); ++index)
        result.importStubs.append(place(generateImportStub(info, index)));
    for (auto& exported : info.exports) {
        if (exported.kind != ExternalKind::Function || exported.kindIndex < info.importFunctionCount())
            continue;
        uint32_t type = info.typeSignatureIndexFromFunctionIndexSpace(FunctionSpaceIndex(exported.kindIndex)).rawIndex();
        const RTT& rtt = info.rtt(TypeSignatureIndex(type));
        if (!rtt.mayHaveICEntrypointInImage() || result.jsEntrypoints.containsIf([&](auto& entry) { return &info.rtt(TypeSignatureIndex(entry.type)) == &rtt; }))
            continue;
        uint32_t code = place(generateJSEntrypoint(info, rtt, result.jsEntrypoints.size()));
        result.jsEntrypoints.append({ type, code });
    }
    for (auto& function : functions) {
        bool functionHasVectors = info.usesSIMD(FunctionCodeIndex(result.entries.size()));
        result.usesVectors |= functionHasVectors;
        result.entries.append(place(function.bytes));
        size_t savedRegisters = result.savedRegisters.find(function.savedRegisters);
        if (savedRegisters == notFound) {
            savedRegisters = result.savedRegisters.size();
            result.savedRegisters.append(function.savedRegisters);
        }
        RELEASE_ASSERT(savedRegisters < functionUsesVectors);
        result.functionSavedRegisters.append(static_cast<uint16_t>(savedRegisters | (functionHasVectors ? functionUsesVectors : 0)));
    }
    result.entries.append(safeCast<uint32_t>(result.code.size()));
    if (result.code.size() > GB) {
        whyDeclined = "its code is too large"_s;
        return std::nullopt;
    }
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        for (auto [endOfCall, target] : functions[index].calls)
            linkCall(result.code, result.entries[index] + endOfCall, result.entries[target]);
        for (uint32_t endOfCall : functions[index].callsToThrowStub)
            linkCall(result.code, result.entries[index] + endOfCall, throwStub);
    }
    return result;
}

std::pair<Vector<uint8_t>, Vector<uint8_t>> CodeInImage::layOut(Vector<ModuleForImage>&& modules)
{
    Vector<uint8_t> directory;
    Vector<uint8_t> code;
    auto appendToDirectory = [&](std::span<const uint8_t> bytes) {
        directory.insertFill(directory.size(), 0, WTF::roundUpToMultipleOf<8>(directory.size()) - directory.size());
        uint32_t at = safeCast<uint32_t>(directory.size());
        directory.append(bytes);
        return at;
    };
    DirectoryInImage header { safeCast<uint32_t>(modules.size()), 0 };
    appendToDirectory(asByteSpan(header));
    size_t recordsAt = directory.size();
    directory.insertFill(directory.size(), 0, modules.size() * sizeof(ModuleInImage));
    for (size_t index = 0; index < modules.size(); ++index) {
        auto& module = modules[index];
        code.insertFill(code.size(), isX86() ? 0xcc : 0, WTF::roundUpToMultipleOf<codeAlignment>(code.size()) - code.size());
        uint32_t base = safeCast<uint32_t>(code.size());
        code.appendVector(module.code);
        for (uint32_t& entry : module.entries)
            entry += base;
        for (uint32_t& stub : module.importStubs)
            stub += base;
        for (auto& entry : module.jsEntrypoints)
            entry.code += base;
        ModuleInImage record { };
        record.digest = module.digest;
        record.sourceSize = module.sourceSize;
        record.isForSignalingMemory = module.isForSignalingMemory;
        record.usesVectors = module.usesVectors;
        record.numberOfFunctions = safeCast<uint32_t>(module.entries.size() - 1);
        record.numberOfImportedFunctions = safeCast<uint32_t>(module.importStubs.size());
        record.entries = appendToDirectory(asBytes(module.entries.span()));
        record.importStubs = appendToDirectory(asBytes(module.importStubs.span()));
        record.numberOfJSEntrypoints = safeCast<uint32_t>(module.jsEntrypoints.size());
        record.jsEntrypoints = appendToDirectory(asBytes(module.jsEntrypoints.span()));
        record.functionSavedRegisters = appendToDirectory(asBytes(module.functionSavedRegisters.span()));
        record.savedRegisters = appendToDirectory(asBytes(module.savedRegisters.span()));
        memcpy(directory.mutableSpan().data() + recordsAt + index * sizeof(ModuleInImage), &record, sizeof(record));
    }
    return { WTF::move(directory), WTF::move(code) };
}

bool CodeInImage::isInProcess()
{
    return !!AOT::Image::webAssembly().first;
}

static const ModuleInImage* find(const ModuleInformation& info)
{
    if (!info.hasSourceDigest)
        return nullptr;
    const uint8_t* directory = AOT::Image::webAssembly().first;
    if (!directory)
        return nullptr;
    auto& header = *reinterpret_cast<const DirectoryInImage*>(directory);
    std::span modules { reinterpret_cast<const ModuleInImage*>(directory + sizeof(DirectoryInImage)), header.numberOfModules };
    for (auto& module : modules) {
        if (module.sourceSize != info.sourceSize || module.digest != info.sourceDigest)
            continue;
        RELEASE_ASSERT(module.numberOfFunctions == info.internalFunctionCount() && module.numberOfImportedFunctions == info.importFunctionCount());
        return module.usesVectors && !Options::useWasmSIMD() ? nullptr : &module;
    }
    return nullptr;
}

const uint16_t* CodeInImage::functionSavedRegistersIfFound(const ModuleInformation& info)
{
    const ModuleInImage* module = find(info);
    return module ? reinterpret_cast<const uint16_t*>(AOT::Image::webAssembly().first + module->functionSavedRegisters) : nullptr;
}

RefPtr<CodeInImage> CodeInImage::tryCreate(const ModuleInformation& info, const IPIntCallees& ipintCallees, MemoryMode mode)
{
    const ModuleInImage* module = find(info);
    if (!module || (module->isForSignalingMemory && mode != MemoryMode::Signaling))
        return nullptr;
    auto [directory, code] = AOT::Image::webAssembly();
    return adoptRef(*new CodeInImage(info, ipintCallees, *module, directory, code));
}

CodeInImage::CodeInImage(const ModuleInformation& info, const IPIntCallees& ipintCallees, const ModuleInImage& module, const uint8_t* directory, const uint8_t* code)
    : m_pointers(numberOfSharedPointers + info.internalFunctionCount() + info.typeCount() + module.numberOfJSEntrypoints)
    , m_importStubs(module.numberOfImportedFunctions)
    , m_isForSignalingMemory(module.isForSignalingMemory)
{
    auto operations = imageCodeOperations();
    for (unsigned index = 0; index < numberOfOperations; ++index)
        m_pointers[index] = operations[index];
    m_pointers[indexOfGenericJSEntrypoint] = LLInt::getCodePtr<JSEntryPtrTag>(js_to_wasm_wrapper_entry).taggedPtr();

    auto* entries = reinterpret_cast<const uint32_t*>(directory + module.entries);
    auto* functionSavedRegisters = reinterpret_cast<const uint16_t*>(directory + module.functionSavedRegisters);
    auto* savedRegisters = reinterpret_cast<const SavedRegistersInImage*>(directory + module.savedRegisters);
    m_callees = FixedVector<Ref<OMGCallee>>::createWithSizeFromGenerator(module.numberOfFunctions, [&](size_t index) {
        FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(FunctionCodeIndex(index));
        Ref<OMGCallee> callee = OMGCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { ipintCallees.at(index).get() });
        callee->setCodeInImage(std::span { code + entries[index], entries[index + 1] - entries[index] }, savedRegistersFrom(savedRegisters[functionSavedRegisters[index] & ~functionUsesVectors]));
        m_pointers[indexOfCallee(index)] = CalleeBits::boxNativeCallee(callee.ptr());
        return callee;
    });
    NativeCalleeRegistry::singleton().registerCallees(m_callees);

    for (uint32_t type = 0; type < info.typeCount(); ++type)
        m_pointers[indexOfRTT(info, TypeSignatureIndex(type))] = const_cast<RTT*>(&info.rtt(TypeSignatureIndex(type)));

    auto* jsEntrypoints = reinterpret_cast<const JSEntrypointInImage*>(directory + module.jsEntrypoints);
    for (unsigned index = 0; index < module.numberOfJSEntrypoints; ++index) {
        const RTT& rtt = info.rtt(TypeSignatureIndex(jsEntrypoints[index].type));
        Ref<JSToWasmICCallee> callee = JSToWasmICCallee::create(rtt.icEntrypointSavedRegisters());
        callee->setEntrypoint(MacroAssemblerCodeRef<JSEntryPtrTag>::createSelfManagedCodeRef(CodePtr<JSEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(code + jsEntrypoints[index].code))));
        m_pointers[indexOfJSEntrypointCallee(info, index)] = CalleeBits::boxNativeCallee(callee.ptr());
        m_jsEntrypoints.append({ &rtt, WTF::move(callee) });
    }

    auto* importStubs = reinterpret_cast<const uint32_t*>(directory + module.importStubs);
    for (unsigned index = 0; index < module.numberOfImportedFunctions; ++index)
        m_importStubs[index] = CodePtr<WasmEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(code + importStubs[index]));
}

#else

bool CodeInImage::isInProcess() { return false; }
const uint16_t* CodeInImage::functionSavedRegistersIfFound(const ModuleInformation&) { return nullptr; }
RefPtr<CodeInImage> CodeInImage::tryCreate(const ModuleInformation&, const IPIntCallees&, MemoryMode) { return nullptr; }

#endif // ENABLE(AOT)

CodeInImage::~CodeInImage() = default;

CodePtr<JSEntryPtrTag> CodeInImage::jsEntrypoint(const RTT& rtt) const
{
    for (auto& [type, callee] : m_jsEntrypoints) {
        if (type == &rtt)
            return callee->jsToWasm();
    }
    return nullptr;
}

} } // namespace JSC::Wasm

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(WEBASSEMBLY_OMGJIT)
