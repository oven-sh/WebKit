/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWasmModuleCompiler.h"

#if ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTBuildContext.h"
#include "AOTCodeDescription.h"
#include "AOTImageFormat.h"
#include "AOTRelocation.h"
#include "AOTRelocationLowering.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWasmStubs.h"
#include "B3CompilationReport.h"
#include "B3Policy.h"
#include "B3Procedure.h"
#include "CCallHelpers.h"
#include "JSWebAssemblyInstance.h"
#include "WasmCalleeGroup.h"
#include "WasmCodeInImage.h"
#include "WasmCompilationContext.h"
#include "WasmLimits.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WasmOMGIRGenerator.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <wtf/HashSet.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WasmModuleCompiler);

static constexpr size_t maxSavedRegisterSets = static_cast<size_t>(std::numeric_limits<decltype(Wasm::FunctionInImage::savedRegisters)>::max()) + 1;

static Wasm::ImageDeclineReason declineReasonFor(B3::FailureKind kind)
{
    switch (kind) {
    case B3::FailureKind::AbsoluteAddress:
        return Wasm::ImageDeclineReason::ProcessAddressInCode;
    case B3::FailureKind::RelocationNotLowered:
        return Wasm::ImageDeclineReason::CallsUnlistedOperation;
    case B3::FailureKind::VariableReadBeforeSet:
        return Wasm::ImageDeclineReason::CodeGeneratorFailed;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wasm::ImageDeclineReason declineReasonFor(LinkFailure failure)
{
    switch (failure) {
    case LinkFailure::ProcessAddressInCode:
        return Wasm::ImageDeclineReason::ProcessAddressInCode;
    case LinkFailure::OutOfPCRelativeReach:
    case LinkFailure::OutOfMemory:
        return Wasm::ImageDeclineReason::CodeGeneratorFailed;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::expected<Wasm::SavedRegistersInImage, Wasm::ImageDeclineReason> encodeSavedRegisters(const RegisterAtOffsetList& savedRegisters)
{
    Wasm::SavedRegistersInImage result { 0, 0, 0 };
    for (auto& entry : savedRegisters) {
        if (entry.width() != conservativeWidthWithoutVectors(entry.reg()))
            return makeUnexpected(Wasm::ImageDeclineReason::SavesVectorRegister);
        result.registers |= 1ull << entry.reg().index();
    }
    if (savedRegisters.registerCount())
        result.offsetOfFirst = safeCast<int32_t>(savedRegisters.at(0).offset());
    RegisterAtOffsetList decoded = Wasm::CodeInImage::savedRegistersFrom(result);
    bool isSame = decoded.registerCount() == savedRegisters.registerCount();
    for (size_t index = 0; isSame && index < decoded.registerCount(); ++index)
        isSame = decoded.at(index) == savedRegisters.at(index);
    if (!isSame)
        return makeUnexpected(Wasm::ImageDeclineReason::SavedRegisterOrder);
    return result;
}

static B3::Policy b3PolicyFor(CPUFeatureSet features, B3::RelocationLowering& lowering)
{
    B3::Policy policy;
    policy.upstream.allowsSIMD = true;
    policy.upstream.usesWasmFaultSignalHandler = true;
    policy.upstream.eliminatesWasmGCAllocations = true;
    policy.addressing.isPositionIndependent = true;
    policy.addressing.dataSectionSpace = static_cast<uint32_t>(RelocationSpace::Constants);
    policy.addressing.jumpTableSpace = static_cast<uint32_t>(RelocationSpace::JumpTables);
    policy.addressing.relocationLowering = &lowering;
    policy.targetFeatures = features;
    return policy;
}

static void countFunctions(BuildContext& context, size_t numberOfFunctions, size_t numberOfInterpretedFunctions)
{
    context.statistics().count(StatisticID::WasmFunctionsCompiled, numberOfFunctions - numberOfInterpretedFunctions);
    context.statistics().count(StatisticID::WasmFunctionsInterpreted, numberOfInterpretedFunctions);
}

std::expected<std::unique_ptr<WasmModuleCompiler>, Wasm::ImageDeclineReason> WasmModuleCompiler::create(VM& vm, std::span<const uint8_t> source)
{
    if (!isX86_64() && !isARM64())
        return makeUnexpected(Wasm::ImageDeclineReason::UnsupportedCPU);
    if (source.size() > Wasm::maxModuleSize)
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);
    auto validated = Wasm::Module::validateSync(vm, Vector<uint8_t>(source));
    if (!validated)
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);
    Ref<Wasm::Module> module = WTF::move(*validated);
    const Wasm::ModuleInformation& info = module->moduleInformation();
    if (!info.internalFunctionCount())
        return makeUnexpected(Wasm::ImageDeclineReason::NoFunctions);
    if (info.hasGCObjectTypes())
        return makeUnexpected(Wasm::ImageDeclineReason::HasGCTypes);
    MemoryMode mode = info.memoryCount() && !info.memory(0).isMemory64() ? MemoryMode::Signaling : MemoryMode::BoundsChecking;
    Ref<Wasm::CalleeGroup> group = module->compileSync(vm, mode);
    if (!group->runnable())
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);
    return std::unique_ptr<WasmModuleCompiler>(new WasmModuleCompiler(WTF::move(module), WTF::move(group), mode, source));
}

WasmModuleCompiler::WasmModuleCompiler(Ref<Wasm::Module>&& module, Ref<Wasm::CalleeGroup>&& group, MemoryMode mode, std::span<const uint8_t> source)
    : m_module(WTF::move(module))
    , m_group(WTF::move(group))
    , m_mode(mode)
    , m_digest(Wasm::CodeInImage::digestOf(source))
    , m_sourceSize(static_cast<uint32_t>(source.size()))
{
    static_assert(Wasm::maxModuleSize <= std::numeric_limits<uint32_t>::max());

    const Wasm::ModuleInformation& info = m_module->moduleInformation();

    UncheckedKeyHashSet<const Wasm::RTT*> typesWithJSEntrypoint;
    for (auto& exported : info.exports) {
        if (exported.kind != Wasm::ExternalKind::Function || exported.kindIndex < info.importFunctionCount())
            continue;
        uint32_t type = info.typeSignatureIndexFromFunctionIndexSpace(Wasm::FunctionSpaceIndex(exported.kindIndex)).rawIndex();
        const Wasm::RTT& rtt = info.rtt(Wasm::TypeSignatureIndex(type));
        if (!rtt.mayHaveICEntrypointInImage() || !typesWithJSEntrypoint.add(&rtt).isNewEntry)
            continue;
        m_jsEntrypointTypes.append(type);
    }
    m_pointers = Wasm::ImagePointerTable::forModule(info, m_jsEntrypointTypes.size());

    for (uint32_t type = 0; type < info.typeCount(); ++type)
        m_typeIndices.add(&info.rtt(Wasm::TypeSignatureIndex(type)), type);
}

WasmModuleCompiler::~WasmModuleCompiler() = default;

AssemblerPolicy WasmModuleCompiler::assemblerPolicyFor(Wasm::FunctionCodeIndex function, const BuildContext& context) const
{
    AssemblerPolicy policy = context.assemblerPolicy();
#if CPU(X86_64)
    if (m_module->moduleInformation().usesSIMD(function))
        policy.features = policy.features.with(CPUFeature::AVX);
#else
    UNUSED_PARAM(function);
#endif
    return policy;
}

void WasmModuleCompiler::compileFunction(Wasm::FunctionCodeIndex functionIndex, B3::CompilationScratch& scratch, BuildContext& buildContext, WasmFunctionCode& result) const
{
    const Wasm::ModuleInformation& info = m_module->moduleInformation();
    result.usesVectors = info.usesSIMD(functionIndex);

    Wasm::FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(functionIndex);
    const Wasm::RTT& signature = info.rtt(info.internalFunctionTypeSignatureIndices[functionIndex]);
    Ref<Wasm::IPIntCallee> profiledCallee = m_group->ipintCalleeFromFunctionIndexSpace(functionIndexSpace);
    Ref<Wasm::OMGCallee> callee = Wasm::OMGCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { profiledCallee });
    Vector<Wasm::UnlinkedWasmToWasmCall> unlinkedCalls;
    Wasm::CompilationContext context;
    context.wasmEntrypointJIT = makeUnique<CCallHelpers>();
    AssemblerPolicy assemblerPolicy = assemblerPolicyFor(functionIndex, buildContext);
    RelocationRecorder recorder(*context.wasmEntrypointJIT, assemblerPolicy);
    ImageRelocationLowering lowering({ RelocationSpace::WebAssemblyPointers, GPRInfo::wasmContextInstancePointer, safeCast<int32_t>(JSWebAssemblyInstance::offsetOfImageCodePointers()), true, Wasm::runtimeFunctionSlot }, recorder);
    B3::Policy policy = b3PolicyFor(assemblerPolicy.features, lowering);
    Wasm::CodeForImage codeForImage { m_pointers, m_typeIndices, recorder, policy, scratch, static_cast<uint32_t>(RelocationSpace::WebAssemblyPointers) };
    context.codeForImage = &codeForImage;
    auto compiled = Wasm::parseAndCompileOMG(context, profiledCallee.get(), callee.get(), info.functions[functionIndex], signature, unlinkedCalls, m_module.get(), m_group.get(), info, m_mode, Wasm::CompilationMode::OMGMode, functionIndex, UINT32_MAX);
    RELEASE_ASSERT(unlinkedCalls.isEmpty());

    const std::optional<B3::Failure>& failure = context.procedure->report().failure;
    if (codeForImage.declineReason != Wasm::ImageDeclineReason::None)
        return result.decline(codeForImage.declineReason);
    if (failure)
        return result.decline(declineReasonFor(failure->kind));
    if (!compiled)
        return result.decline(Wasm::ImageDeclineReason::CodeGeneratorFailed);
    if (!(*compiled)->exceptionHandlers.isEmpty())
        return result.decline(Wasm::ImageDeclineReason::UsesExceptions);

    // Link tasks run here, and one can fail the procedure or decline the function.
    auto code = recorder.finalize(LinkBuffer::Profile::WasmOMG, &buildContext);
    if (!code)
        return result.decline(declineReasonFor(code.error()));
    if (failure)
        return result.decline(declineReasonFor(failure->kind));
    if (codeForImage.declineReason != Wasm::ImageDeclineReason::None)
        return result.decline(codeForImage.declineReason);

    auto savedRegisters = encodeSavedRegisters((*compiled)->entrypoint.calleeSaveRegisters);
    if (!savedRegisters)
        return result.decline(savedRegisters.error());

    result.code = WTF::move(*code);
    result.callTargets = WTF::move(codeForImage.callTargets);
    result.savedRegisters = *savedRegisters;
    result.constants = Typed::copyConstants(*context.procedure);
    result.jumpTables = Typed::takeJumpTables(*context.procedure);
}

void WasmModuleCompiler::internSavedRegisters(WasmModuleCode& module) const
{
    for (WasmFunctionCode& function : module.functions) {
        if (!function.hasCode())
            continue;
        size_t index = module.savedRegisters.find(function.savedRegisters);
        if (index == notFound) {
            if (module.savedRegisters.size() == maxSavedRegisterSets) {
                function.decline(Wasm::ImageDeclineReason::SavedRegisterOrder);
                continue;
            }
            index = module.savedRegisters.size();
            module.savedRegisters.append(function.savedRegisters);
        }
        function.savedRegistersIndex = static_cast<uint16_t>(index);
    }
}

std::expected<void, LinkFailure> WasmModuleCompiler::generateStubs(WasmModuleCode& module, BuildContext& context) const
{
    const Wasm::ModuleInformation& info = m_module->moduleInformation();

    WasmStubCode throwStub = generateWasmThrowStub(context);
    if (!throwStub)
        return makeUnexpected(throwStub.error());
    module.throwStub = WTF::move(*throwStub);

    for (unsigned index = 0; index < info.importFunctionCount(); ++index) {
        WasmStubCode stub = generateWasmImportStub(info, index, context);
        if (!stub)
            return makeUnexpected(stub.error());
        module.importStubs.append(WTF::move(*stub));
    }

    for (uint32_t entry = 0; entry < m_jsEntrypointTypes.size(); ++entry) {
        uint32_t type = m_jsEntrypointTypes[entry];
        WasmStubCode stub = generateWasmJSEntrypoint(info.rtt(Wasm::TypeSignatureIndex(type)), m_pointers.jsEntrypointCallee(entry), context);
        if (!stub)
            return makeUnexpected(stub.error());
        module.jsEntrypoints.append({ type, WTF::move(*stub) });
    }

    for (unsigned index = 0; index < module.functions.size(); ++index) {
        WasmFunctionCode& function = module.functions[index];
        if (function.hasCode())
            continue;
        WasmStubCode stub = generateWasmInterpreterStub(m_pointers.callee(Wasm::FunctionCodeIndex(index)), context);
        if (!stub)
            return makeUnexpected(stub.error());
        function.code.bytes = WTF::move(*stub);
    }
    return { };
}

std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason> WasmModuleCompiler::compile(const CodeTemplates& templates, BuildContext& context)
{
    const Wasm::ModuleInformation& info = m_module->moduleInformation();
    WasmModuleCode module;
    module.digest = m_digest;
    module.sourceSize = m_sourceSize;
    module.isForSignalingMemory = m_mode == MemoryMode::Signaling;
    module.functions = Vector<WasmFunctionCode>(info.internalFunctionCount());

    forEachIndexInParallel(context.parallelLoops(), module.functions.size(), [&](unsigned index, unsigned worker) {
        compileFunction(Wasm::FunctionCodeIndex(index), context.backEndScratch(worker), context, module.functions[index]);
    });
    internSavedRegisters(module);

    size_t numberOfFunctions = module.functions.size();
    if (auto stubs = generateStubs(module, context); !stubs) {
        countFunctions(context, numberOfFunctions, numberOfFunctions);
        return makeUnexpected(declineReasonFor(stubs.error()));
    }

    auto linked = linkWasmModule(WTF::move(module), templates, context);
    countFunctions(context, numberOfFunctions, linked ? linked->interpretedFunctions.size() : numberOfFunctions);
    return linked;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)
