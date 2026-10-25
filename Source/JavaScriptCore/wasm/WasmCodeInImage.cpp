/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "WasmCodeInImage.h"

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "B3Operations.h"
#include "B3RelocatableValue.h"
#include "LLIntData.h"
#include "NativeCalleeRegistry.h"
#include "Options.h"
#include "WasmModuleInformation.h"
#include "WasmOperations.h"
#include <algorithm>
#include <wtf/SHA1.h>
#include <wtf/TZoneMallocInlines.h>

#if ENABLE(AOT)
#include "AOTRuntimeReporter.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

using B3::operationMemoryMove;
using B3::operationMemoryFill;

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeInImage);

struct CodeInImage::PublishedImage {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PublishedImage);

    std::span<const uint8_t> directory;
    std::span<const uint8_t> code;
    const PublishedImage* publishedBefore { nullptr };
};

constinit std::atomic<const CodeInImage::PublishedImage*> CodeInImage::s_lastPublished { nullptr };
constinit std::array<std::atomic<unsigned>, numberOfInterpreterFallbackReasons> CodeInImage::s_interpreterFallbackCounts { };

static std::array<void*, ImagePointerTable::numberOfOperations> imageCodeOperations()
{
    return { {
#define ADDRESS_OF_OPERATION(operation) std::bit_cast<void*>(tagCFunction<OperationPtrTag>(operation)),
        FOR_EACH_WASM_IMAGE_CODE_OPERATION(ADDRESS_OF_OPERATION)
#undef ADDRESS_OF_OPERATION
    } };
}

std::optional<uint32_t> ImagePointerTable::slotOfOperation(const void* taggedOperation)
{
    auto operations = imageCodeOperations();
    for (uint32_t slot = 0; slot < numberOfOperations; ++slot) {
        if (operations[slot] == taggedOperation)
            return slot;
    }
    return std::nullopt;
}

ImagePointerTable ImagePointerTable::forModule(const ModuleInformation& info, uint32_t numberOfJSEntrypoints)
{
    return { info.internalFunctionCount(), info.typeCount(), numberOfJSEntrypoints };
}

std::optional<uint32_t> runtimeFunctionSlot(B3::RuntimeFunction function)
{
    switch (function) {
    case B3::RuntimeFunction::MemoryMove:
        return ImagePointerTable::slotOf(ImageCodeOperation::operationMemoryMove);
    case B3::RuntimeFunction::MemoryFill:
        return ImagePointerTable::slotOf(ImageCodeOperation::operationMemoryFill);
    case B3::RuntimeFunction::GCSafeMemoryMove:
    case B3::RuntimeFunction::FmodDouble:
    case B3::RuntimeFunction::CeilDouble:
    case B3::RuntimeFunction::CeilFloat:
    case B3::RuntimeFunction::FloorDouble:
    case B3::RuntimeFunction::FloorFloat:
    case B3::RuntimeFunction::TruncDouble:
    case B3::RuntimeFunction::TruncFloat:
        break;
    }
    return std::nullopt;
}

std::array<uint8_t, 20> CodeInImage::digestOf(std::span<const uint8_t> source)
{
    SHA1 sha1;
    sha1.addBytes(source);
    SHA1::Digest digest;
    sha1.computeHash(digest);
    return digest;
}

RegisterAtOffsetList CodeInImage::savedRegistersFrom(const SavedRegistersInImage& saved)
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

void CodeInImage::addImage(std::span<const uint8_t> directory, std::span<const uint8_t> code)
{
    if (directory.empty())
        return;
    auto* image = new PublishedImage { directory, code, s_lastPublished.load(std::memory_order_relaxed) };
    while (!s_lastPublished.compare_exchange_weak(image->publishedBefore, image, std::memory_order_release, std::memory_order_relaxed)) { }
}

bool CodeInImage::contains(const void* pc)
{
    for (auto* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->publishedBefore) {
        if (std::bit_cast<uintptr_t>(pc) - std::bit_cast<uintptr_t>(image->code.data()) < image->code.size())
            return true;
    }
    return false;
}

bool CodeInImage::isInProcess()
{
    for (auto* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->publishedBefore) {
        if (reinterpret_cast<const DirectoryInImage*>(image->directory.data())->numberOfModules)
            return true;
    }
    return false;
}

// Only Options::crashIfInterpreterIsUsed() asks about a module without code.
bool CodeInImage::needsSourceDigests()
{
    return isInProcess() || (s_lastPublished.load(std::memory_order_acquire) && Options::crashIfInterpreterIsUsed());
}

const CodeInImage::PublishedImage& CodeInImage::imageHolding(const ModuleInImage& module)
{
    for (auto* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->publishedBefore) {
        if (std::bit_cast<uintptr_t>(&module) - std::bit_cast<uintptr_t>(image->directory.data()) < image->directory.size())
            return *image;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::span<const ModuleInImage> modulesIn(const uint8_t* directory)
{
    auto& header = *reinterpret_cast<const DirectoryInImage*>(directory);
    return { reinterpret_cast<const ModuleInImage*>(directory + sizeof(DirectoryInImage)), header.numberOfModules };
}

static std::span<const DeclinedModuleInImage> declinedModulesIn(const uint8_t* directory)
{
    auto& header = *reinterpret_cast<const DirectoryInImage*>(directory);
    return { reinterpret_cast<const DeclinedModuleInImage*>(std::to_address(modulesIn(directory).end())), header.numberOfDeclinedModules };
}

const ModuleInImage* CodeInImage::findRecord(const ModuleInformation& info)
{
    if (!info.hasSourceDigest)
        return nullptr;
    for (auto* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->publishedBefore) {
        for (auto& module : modulesIn(image->directory.data())) {
            if (module.sourceSize != info.sourceSize || module.digest != info.sourceDigest)
                continue;
            if (module.numberOfFunctions != info.internalFunctionCount() || module.numberOfImportedFunctions != info.importFunctionCount())
                return nullptr;
            return &module;
        }
    }
    return nullptr;
}

static bool usesDisabledVectors(const ModuleInImage& module)
{
    return module.usesVectors && !Options::useWasmSIMD();
}

static std::optional<InterpreterFallbackReason> fallbackReasonFor(const ModuleInImage& module, MemoryMode mode)
{
    if (usesDisabledVectors(module))
        return InterpreterFallbackReason::VectorsAreDisabled;
    if (module.isForSignalingMemory && mode != MemoryMode::Signaling)
        return InterpreterFallbackReason::MemoryChecksBounds;
    return std::nullopt;
}

const ModuleInImage* CodeInImage::find(const ModuleInformation& info)
{
    const ModuleInImage* module = findRecord(info);
    return module && !usesDisabledVectors(*module) ? module : nullptr;
}

bool CodeInImage::isModuleOfProgram(const ModuleInformation& info)
{
    if (!info.hasSourceDigest)
        return false;
    auto hasSameSource = [&](auto& module) {
        return module.sourceSize == info.sourceSize && module.digest == info.sourceDigest;
    };
    for (auto* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->publishedBefore) {
        const uint8_t* directory = image->directory.data();
        if (std::ranges::any_of(modulesIn(directory), hasSameSource) || std::ranges::any_of(declinedModulesIn(directory), hasSameSource))
            return true;
    }
    return false;
}

std::span<const FunctionInImage> CodeInImage::functionsOf(const ModuleInImage& module)
{
    return { reinterpret_cast<const FunctionInImage*>(imageHolding(module).directory.data() + module.functions), module.numberOfFunctions };
}

std::optional<InterpreterFallbackReason> CodeInImage::fallbackReason(const ModuleInformation& info, MemoryMode mode)
{
    const ModuleInImage* module = findRecord(info);
    return module ? fallbackReasonFor(*module, mode) : std::nullopt;
}

RefPtr<CodeInImage> CodeInImage::tryCreate(const ModuleInformation& info, const IPIntCallees& ipintCallees, MemoryMode mode, CodePtr<WasmEntryPtrTag> interpreterEntrypoint)
{
    const ModuleInImage* module = findRecord(info);
    if (!module || fallbackReasonFor(*module, mode))
        return nullptr;
    return adoptRef(*new CodeInImage(info, ipintCallees, *module, interpreterEntrypoint));
}

CodeInImage::CodeInImage(const ModuleInformation& info, const IPIntCallees& ipintCallees, const ModuleInImage& module, CodePtr<WasmEntryPtrTag> interpreterEntrypoint)
    : m_callees(module.numberOfFunctions)
    , m_importStubs(module.numberOfImportedFunctions)
    , m_isForSignalingMemory(module.isForSignalingMemory)
{
    ImagePointerTable table = ImagePointerTable::forModule(info, module.numberOfJSEntrypoints);
    m_pointers = FixedVector<void*>(table.size());

    auto operations = imageCodeOperations();
    for (uint32_t slot = 0; slot < ImagePointerTable::numberOfOperations; ++slot)
        m_pointers[slot] = operations[slot];
    m_pointers[ImagePointerTable::genericJSEntrypoint] = LLInt::getCodePtr<JSEntryPtrTag>(js_to_wasm_wrapper_entry).taggedPtr();
    m_pointers[ImagePointerTable::interpreterEntrypoint] = interpreterEntrypoint.taggedPtr();

    const PublishedImage& image = imageHolding(module);
    const uint8_t* directory = image.directory.data();
    std::span functions = functionsOf(module);
    auto* savedRegisters = reinterpret_cast<const SavedRegistersInImage*>(directory + module.savedRegisters);
    Vector<Ref<OMGCallee>> calleesWithCode;
    calleesWithCode.reserveInitialCapacity(functions.size());
    for (size_t index = 0; index < functions.size(); ++index) {
        const FunctionInImage& function = functions[index];
        uint32_t slot = table.callee(FunctionCodeIndex(index));
        if (!function.hasCode) {
            m_pointers[slot] = CalleeBits::boxNativeCallee(ipintCallees.at(index).ptr());
            continue;
        }
        uint32_t end = index + 1 < functions.size() ? functions[index + 1].entry : module.functionsEnd;
        FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(FunctionCodeIndex(index));
        Ref<OMGCallee> callee = OMGCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { ipintCallees.at(index).get() });
        callee->setCodeInImage(image.code.subspan(function.entry, end - function.entry), savedRegistersFrom(savedRegisters[function.savedRegisters]));
        m_pointers[slot] = CalleeBits::boxNativeCallee(callee.ptr());
        m_callees[index] = callee.copyRef();
        calleesWithCode.append(WTF::move(callee));
    }
    NativeCalleeRegistry::singleton().registerCallees(calleesWithCode);

    for (uint32_t type = 0; type < info.typeCount(); ++type)
        m_pointers[table.rtt(TypeSignatureIndex(type))] = const_cast<RTT*>(&info.rtt(TypeSignatureIndex(type)));

    auto* jsEntrypoints = reinterpret_cast<const JSEntrypointInImage*>(directory + module.jsEntrypoints);
    for (unsigned index = 0; index < module.numberOfJSEntrypoints; ++index) {
        const RTT& rtt = info.rtt(TypeSignatureIndex(jsEntrypoints[index].type));
        Ref<JSToWasmICCallee> callee = JSToWasmICCallee::create(rtt.icEntrypointSavedRegisters());
        callee->setEntrypoint(MacroAssemblerCodeRef<JSEntryPtrTag>::createSelfManagedCodeRef(CodePtr<JSEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(image.code.data() + jsEntrypoints[index].code))));
        m_pointers[table.jsEntrypointCallee(index)] = CalleeBits::boxNativeCallee(callee.ptr());
        m_jsEntrypoints.append({ &rtt, WTF::move(callee) });
    }

    auto* importStubs = reinterpret_cast<const uint32_t*>(directory + module.importStubs);
    for (unsigned index = 0; index < module.numberOfImportedFunctions; ++index)
        m_importStubs[index] = CodePtr<WasmEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(image.code.data() + importStubs[index]));
}

void CodeInImage::didFallBackToInterpreter(InterpreterFallbackReason reason)
{
    if (s_interpreterFallbackCounts[static_cast<size_t>(reason)].fetch_add(1, std::memory_order_relaxed))
        return;
#if ENABLE(AOT)
    AOT::reportInterpreterFallback(reason);
#endif
}

unsigned CodeInImage::interpreterFallbackCount(InterpreterFallbackReason reason)
{
    return s_interpreterFallbackCounts[static_cast<size_t>(reason)].load(std::memory_order_relaxed);
}

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
