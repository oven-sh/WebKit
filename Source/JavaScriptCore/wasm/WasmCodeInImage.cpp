/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "WasmCodeInImage.h"

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "B3Operations.h"
#include "JSCConfig.h"
#include "LLIntData.h"
#include "LLIntEntrypoint.h"
#include "NativeCalleeRegistry.h"
#include "Options.h"
#include "WasmModuleInformation.h"
#include "WasmOperations.h"
#include "WasmSections.h"
#include "WasmStreamingParser.h"
#include <algorithm>
#include <wtf/LEBDecoder.h>
#include <wtf/SHA1.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Wasm {

using B3::operationMemoryMove;
using B3::operationGCSafeMemoryMove;
using B3::operationMemoryFill;

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeInImage);

struct CodeInImage::PublishedImage {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PublishedImage);

    std::span<const uint8_t> directory;
    std::span<const uint8_t> code;
    const PublishedImage* publishedBefore { nullptr };
};

constinit std::atomic<const CodeInImage::PublishedImage*> CodeInImage::s_lastPublished { nullptr };

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

std::array<uint8_t, 20> CodeInImage::digestOf(std::span<const uint8_t> source)
{
    SourceHasher sha1;
    constexpr size_t headerSize = 8;
    size_t hashedEnd = 0;
    size_t offset = std::min(headerSize, source.size());
    while (offset < source.size()) {
        Section section;
        uint32_t payloadSize;
        if (!decodeSection(source[offset++], section) || !WTF::LEBDecoder::decodeUInt32(source, offset, payloadSize) || payloadSize > source.size() - offset)
            break;
        if (section == Section::Data || section == Section::Custom) {
            sha1.addBytes(source.subspan(hashedEnd, offset - hashedEnd));
            hashedEnd = offset + payloadSize;
        }
        offset += payloadSize;
    }
    sha1.addBytes(source.subspan(hashedEnd));
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

CodePtr<ExceptionHandlerPtrTag> CodeInImage::catchRunwayFor(const void* pc)
{
    for (auto* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->publishedBefore) {
        if (std::bit_cast<uintptr_t>(pc) - std::bit_cast<uintptr_t>(image->code.data()) < image->code.size())
            return CodePtr<ExceptionHandlerPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(image->code.data() + reinterpret_cast<const DirectoryInImage*>(image->directory.data())->catchRunway));
    }
    return nullptr;
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

ImageCodeFlavor CodeInImage::flavorFor(const ModuleInformation& info, MemoryMode mode)
{
    // A signaling memory exists only where the fault handler does.
    if (mode == MemoryMode::Signaling)
        return ImageCodeFlavor::Trapping;
    // An instance without a memory has the mode of an empty one, and its code leaves nothing but null references to the fault handler.
    if (!info.memoryCount() && Options::useWasmFaultSignalHandler())
        return ImageCodeFlavor::Trapping;
    return ImageCodeFlavor::Checking;
}

const ModuleInImage* CodeInImage::findBeforeParsing(std::span<const uint8_t> source, ModuleInformation& info)
{
    if (!needsSourceDigests())
        return nullptr;
    info.sourceDigest = digestOf(source);
    info.sourceSize = source.size();
    info.hasSourceDigest = true;
    for (auto* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->publishedBefore) {
        for (auto& module : modulesIn(image->directory.data())) {
            if (module.sourceSize == info.sourceSize && module.digest == info.sourceDigest)
                return usesDisabledVectors(module) ? nullptr : &module;
        }
    }
    return nullptr;
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

std::span<const FunctionInImage> CodeInImage::functionsOf(const ModuleInImage& module, ImageCodeFlavor flavor)
{
    return { reinterpret_cast<const FunctionInImage*>(imageHolding(module).directory.data() + module.functions[static_cast<size_t>(flavor)]), module.numberOfFunctions };
}

RefPtr<CodeInImage> CodeInImage::tryCreate(const ModuleInformation& info, const IPIntCallees& ipintCallees, MemoryMode mode)
{
    const ModuleInImage* module = findRecord(info);
    if (!module || usesDisabledVectors(*module))
        return nullptr;
    return adoptRef(*new CodeInImage(info, ipintCallees, *module, flavorFor(info, mode)));
}

static FixedVector<HandlerInfo> handlersOf(const FunctionInImage& function, const HandlerInImage* handlersOfModule, std::span<const uint8_t> code)
{
    FixedVector<HandlerInfo> handlers(function.numberOfHandlers);
    for (uint32_t index = 0; index < function.numberOfHandlers; ++index) {
        const HandlerInImage& handler = handlersOfModule[function.firstHandler + index];
        auto type = static_cast<HandlerType>(handler.type);
        CodePtr<ExceptionHandlerPtrTag> place;
        if (type != HandlerType::Delegate)
            place = CodePtr<ExceptionHandlerPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(code.data() + handler.code));
        handlers[index].initialize(UnlinkedHandlerInfo(type, handler.start, handler.end, 0, handler.tryDepth, handler.exceptionIndexOrDelegateTarget), place);
    }
    return handlers;
}

CodeInImage::CodeInImage(const ModuleInformation& info, const IPIntCallees& ipintCallees, const ModuleInImage& module, ImageCodeFlavor flavor)
    : m_importStubs(module.numberOfImportedFunctions)
    , m_javaScriptImportStubs(module.numberOfImportedFunctions)
    , m_flavor(flavor)
    , m_servesEveryFlavor(module.functions[0] == module.functions[1])
{
    ImagePointerTable table = ImagePointerTable::forModule(info, module.numberOfJSEntrypoints);
    m_pointers = FixedVector<void*>(table.size());

    auto operations = imageCodeOperations();
    for (uint32_t slot = 0; slot < ImagePointerTable::numberOfOperations; ++slot)
        m_pointers[slot] = operations[slot];
    m_pointers[ImagePointerTable::genericJSEntrypoint] = LLInt::getCodePtr<JSEntryPtrTag>(js_to_wasm_wrapper_entry).taggedPtr();
    m_pointers[ImagePointerTable::defaultJSCall] = LLInt::defaultCall().code().taggedPtr();
    m_pointers[ImagePointerTable::wasmToJSCallee] = CalleeBits::boxNativeCallee(&WasmToJSCallee::singleton());
    m_pointers[ImagePointerTable::restoreFrameCallee] = CalleeBits::boxNativeCallee(&RestoreFrameCallee::singleton());
    m_pointers[ImagePointerTable::restoreFrameReturnAddress] = reinterpret_cast<void*>(untagCodePtr<CFunctionPtrTag>(wasm_restore_frame_return));
    m_pointers[ImagePointerTable::structureIDBase] = std::bit_cast<void*>(structureIDBase());
#if GIGACAGE_ENABLED
    m_pointers[ImagePointerTable::primitiveGigacageDisableRequest] = &Gigacage::disablePrimitiveGigacageRequested;
    m_pointers[ImagePointerTable::primitiveGigacageBase] = Gigacage::addressOfBasePtr(Gigacage::Primitive);
#endif

    const PublishedImage& image = imageHolding(module);
    const uint8_t* directory = image.directory.data();
    std::span functions = functionsOf(module, flavor);
    auto* savedRegisters = reinterpret_cast<const SavedRegistersInImage*>(directory + module.savedRegisters);
    auto* handlers = reinterpret_cast<const HandlerInImage*>(directory + module.handlers);
    m_callees.reserveInitialCapacity(functions.size());
    for (size_t index = 0; index < functions.size(); ++index) {
        const FunctionInImage& function = functions[index];
        FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(FunctionCodeIndex(index));
        Ref<OMGCallee> callee = OMGCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { ipintCallees.at(index).get() });
        callee->setCodeInImage(image.code.subspan(function.entry, function.size), savedRegistersFrom(savedRegisters[function.savedRegisters]));
        if (function.numberOfHandlers)
            callee->setExceptionHandlersInImage(handlersOf(function, handlers, image.code));
        m_pointers[table.callee(FunctionCodeIndex(index))] = CalleeBits::boxNativeCallee(callee.ptr());
        m_callees.append(WTF::move(callee));
    }
    NativeCalleeRegistry::singleton().registerCallees(m_callees);

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
    auto* javaScriptImportStubs = reinterpret_cast<const uint32_t*>(directory + module.javaScriptImportStubs);
    for (unsigned index = 0; index < module.numberOfImportedFunctions; ++index) {
        m_importStubs[index] = CodePtr<WasmEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(image.code.data() + importStubs[index]));
        m_javaScriptImportStubs[index] = CodePtr<WasmEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(image.code.data() + javaScriptImportStubs[index]));
    }
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
