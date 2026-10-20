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

using B3::operationMemoryCopy;
using B3::operationMemoryFill;

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeInImage);

constinit const uint8_t* CodeInImage::s_directory { nullptr };
constinit std::span<const uint8_t> CodeInImage::s_code;
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
    case B3::RuntimeFunction::MemoryCopy:
        return ImagePointerTable::slotOf(ImageCodeOperation::operationMemoryCopy);
    case B3::RuntimeFunction::MemoryFill:
        return ImagePointerTable::slotOf(ImageCodeOperation::operationMemoryFill);
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

void CodeInImage::setImage(const uint8_t* directory, std::span<const uint8_t> code)
{
    RELEASE_ASSERT(!s_directory);
    if (!directory)
        return;
    s_directory = directory;
    s_code = code;
}

bool CodeInImage::isInProcess()
{
    return s_directory && reinterpret_cast<const DirectoryInImage*>(s_directory)->numberOfModules;
}

// Only Options::crashIfInterpreterIsUsed() asks about a module without code.
bool CodeInImage::needsSourceDigests()
{
    return isInProcess() || (s_directory && Options::crashIfInterpreterIsUsed());
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

static const ModuleInImage* findRecord(const uint8_t* directory, const ModuleInformation& info)
{
    if (!directory || !info.hasSourceDigest)
        return nullptr;
    for (auto& module : modulesIn(directory)) {
        if (module.sourceSize != info.sourceSize || module.digest != info.sourceDigest)
            continue;
        if (module.numberOfFunctions != info.internalFunctionCount() || module.numberOfImportedFunctions != info.importFunctionCount())
            return nullptr;
        return &module;
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
    const ModuleInImage* module = findRecord(s_directory, info);
    return module && !usesDisabledVectors(*module) ? module : nullptr;
}

bool CodeInImage::isModuleOfProgram(const ModuleInformation& info)
{
    if (!s_directory || !info.hasSourceDigest)
        return false;
    auto hasSameSource = [&](auto& module) {
        return module.sourceSize == info.sourceSize && module.digest == info.sourceDigest;
    };
    return std::ranges::any_of(modulesIn(s_directory), hasSameSource) || std::ranges::any_of(declinedModulesIn(s_directory), hasSameSource);
}

std::span<const FunctionInImage> CodeInImage::functionsOf(const ModuleInImage& module)
{
    return { reinterpret_cast<const FunctionInImage*>(s_directory + module.functions), module.numberOfFunctions };
}

std::optional<InterpreterFallbackReason> CodeInImage::fallbackReason(const ModuleInformation& info, MemoryMode mode)
{
    const ModuleInImage* module = findRecord(s_directory, info);
    return module ? fallbackReasonFor(*module, mode) : std::nullopt;
}

RefPtr<CodeInImage> CodeInImage::tryCreate(const ModuleInformation& info, const IPIntCallees& ipintCallees, MemoryMode mode, CodePtr<WasmEntryPtrTag> interpreterEntrypoint)
{
    const ModuleInImage* module = findRecord(s_directory, info);
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

    std::span functions = functionsOf(module);
    auto* savedRegisters = reinterpret_cast<const SavedRegistersInImage*>(s_directory + module.savedRegisters);
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
        callee->setCodeInImage(s_code.subspan(function.entry, end - function.entry), savedRegistersFrom(savedRegisters[function.savedRegisters]));
        m_pointers[slot] = CalleeBits::boxNativeCallee(callee.ptr());
        m_callees[index] = callee.copyRef();
        calleesWithCode.append(WTF::move(callee));
    }
    NativeCalleeRegistry::singleton().registerCallees(calleesWithCode);

    for (uint32_t type = 0; type < info.typeCount(); ++type)
        m_pointers[table.rtt(TypeSignatureIndex(type))] = const_cast<RTT*>(&info.rtt(TypeSignatureIndex(type)));

    auto* jsEntrypoints = reinterpret_cast<const JSEntrypointInImage*>(s_directory + module.jsEntrypoints);
    for (unsigned index = 0; index < module.numberOfJSEntrypoints; ++index) {
        const RTT& rtt = info.rtt(TypeSignatureIndex(jsEntrypoints[index].type));
        Ref<JSToWasmICCallee> callee = JSToWasmICCallee::create(rtt.icEntrypointSavedRegisters());
        callee->setEntrypoint(MacroAssemblerCodeRef<JSEntryPtrTag>::createSelfManagedCodeRef(CodePtr<JSEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(s_code.data() + jsEntrypoints[index].code))));
        m_pointers[table.jsEntrypointCallee(index)] = CalleeBits::boxNativeCallee(callee.ptr());
        m_jsEntrypoints.append({ &rtt, WTF::move(callee) });
    }

    auto* importStubs = reinterpret_cast<const uint32_t*>(s_directory + module.importStubs);
    for (unsigned index = 0; index < module.numberOfImportedFunctions; ++index)
        m_importStubs[index] = CodePtr<WasmEntryPtrTag>::fromUntaggedPtr(const_cast<uint8_t*>(s_code.data() + importStubs[index]));
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
