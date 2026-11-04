/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWasmModules.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"

#if ENABLE(WEBASSEMBLY_OMGJIT)
#include "AOTB3Policy.h"
#include "AOTCodeDescription.h"
#include "AOTImageFormat.h"
#include "AOTRelocationLowering.h"
#include "AOTSectionLinker.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "B3CompilationReport.h"
#include "B3Policy.h"
#include "B3Procedure.h"
#include "CCallHelpers.h"
#include "JSWebAssemblyInstance.h"
#include "WasmBinding.h"
#include "WasmCalleeGroup.h"
#include "WasmCallingConvention.h"
#include "WasmCodeInImage.h"
#include "WasmCompilationContext.h"
#include "WasmLimits.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WasmOMGIRGenerator.h"
#include "WasmTypeDefinition.h"
#include <algorithm>
#include <limits>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/text/Base64.h>
#endif

namespace JSC { namespace AOT {

static void reportDecline(BuildContext& context, Typed::ModuleID module, UserDiagnosticKind kind, LimitID limit, uint32_t count)
{
    SourceLocation location { module.index(), 0 };
    if (limit != LimitID::None)
        context.gaveUp(limit, Subject { .location = location });
    context.diagnostics().add(UserDiagnostic { .kind = kind, .limit = limit, .count = count, .location = location });
}

#if ENABLE(WEBASSEMBLY)

UserDiagnosticKind diagnosticKindOf(Wasm::ImageDeclineReason reason)
{
    switch (reason) {
    case Wasm::ImageDeclineReason::None:
        return UserDiagnosticKind::Unknown;
#define DIAGNOSTIC_KIND_OF_REASON(name) \
    case Wasm::ImageDeclineReason::name: \
        return UserDiagnosticKind::Wasm##name;
    FOR_EACH_WASM_IMAGE_DECLINE_REASON(DIAGNOSTIC_KIND_OF_REASON)
#undef DIAGNOSTIC_KIND_OF_REASON
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#endif // ENABLE(WEBASSEMBLY)

#if ENABLE(WEBASSEMBLY_OMGJIT)

static constexpr size_t maxSavedRegisterSets = static_cast<size_t>(std::numeric_limits<decltype(Wasm::FunctionInImage::savedRegisters)>::max()) + 1;
static constexpr uint32_t codeUnitAlignment = 16;
static constexpr uint32_t jumpTableAlignment = sizeof(int32_t);
static constexpr uint32_t throwStubUnit = 0;
static constexpr size_t directoryAlignment = 8;
static constexpr uint64_t maxSectionOffset = std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), defaultLimitValue(LimitID::VectorBytes, hostArchitecture()));

static_assert(Wasm::maxModuleSize <= std::numeric_limits<uint32_t>::max());
static_assert(defaultLimitValue(LimitID::WasmImageCodeSize, hostArchitecture()) <= std::numeric_limits<uint32_t>::max());
static_assert(WTF::isValidCapacityForVector<uint8_t>(maxSectionOffset));
static_assert(!(sizeof(Wasm::DirectoryInImage) % directoryAlignment));
static_assert(!(sizeof(Wasm::ModuleInImage) % directoryAlignment));
static_assert(!(sizeof(Wasm::DeclinedModuleInImage) % directoryAlignment));
static_assert(alignof(Wasm::SavedRegistersInImage) == directoryAlignment);

static Wasm::ImageDeclineReason declineReasonFor(B3::FailureKind kind)
{
    switch (kind) {
    case B3::FailureKind::AbsoluteAddress:
        return Wasm::ImageDeclineReason::ProcessAddressInCode;
    case B3::FailureKind::RelocationNotLowered:
        return Wasm::ImageDeclineReason::CallsUnlistedOperation;
    case B3::FailureKind::VariableReadBeforeSet:
    case B3::FailureKind::SpillSlotLacksItsValue:
    case B3::FailureKind::RegisterLacksItsValue:
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
    policy.validation.level = lowestValidationLevel;
    policy.targetFeatures = features;
    return policy;
}

static CCallHelpers::Address addressOfSlot(GPRReg table, uint32_t slot)
{
    return CCallHelpers::Address(table, safeCast<int32_t>(slot * sizeof(void*)));
}

static void emitThrowStub(CCallHelpers& jit)
{
    jit.preserveReturnAddressAfterCall(GPRInfo::argumentGPR2);
    jit.move(GPRInfo::wasmContextInstancePointer, GPRInfo::argumentGPR0);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR0, JSWebAssemblyInstance::offsetOfVM()), GPRInfo::argumentGPR3);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR3, VM::topEntryFrameOffset()), GPRInfo::argumentGPR3);
    jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(GPRInfo::argumentGPR3);
    jit.prepareWasmCallOperation(GPRInfo::argumentGPR0);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR0, JSWebAssemblyInstance::offsetOfImageCodePointers()), GPRInfo::nonPreservedNonArgumentGPR0);
    jit.call(addressOfSlot(GPRInfo::nonPreservedNonArgumentGPR0, Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationThrowExceptionFromOMG)), OperationPtrTag);
    jit.farJump(GPRInfo::returnValueGPR, ExceptionHandlerPtrTag);
    jit.breakpoint();
}

static void emitInterpreterStub(CCallHelpers& jit, uint32_t calleeSlot)
{
    GPRReg callee = Wasm::wasmCallingConvention().prologueScratchGPRs[0];
    GPRReg table = Wasm::wasmCallingConvention().prologueScratchGPRs[1];
    jit.loadPtr(CCallHelpers::Address(GPRInfo::wasmContextInstancePointer, JSWebAssemblyInstance::offsetOfImageCodePointers()), table);
    jit.loadPtr(addressOfSlot(table, calleeSlot), callee);
    // The call has happened and the prologue has not, as in emitWasmToWasm().
    jit.storeWasmCalleeToCalleeCallFrame(callee, safeCast<int>(sizeof(CallerFrameAndPC)) - safeCast<int>(prologueStackPointerDelta()));
    jit.loadPtr(addressOfSlot(table, Wasm::ImagePointerTable::interpreterEntrypoint), callee);
    jit.farJump(callee, WasmEntryPtrTag);
}

namespace {

struct WasmModuleCompiler {
    Ref<Wasm::Module> module;
    Ref<Wasm::CalleeGroup> group;
    MemoryMode mode;
    const Wasm::ModuleInformation& info { module->moduleInformation() };
    Vector<uint32_t> jsEntrypointTypes { };
    Wasm::ImagePointerTable pointers { };
    UncheckedKeyHashMap<const Wasm::RTT*, uint32_t> typeIndices { };

    void numberTypes()
    {
        UncheckedKeyHashSet<const Wasm::RTT*> typesWithJSEntrypoint;
        for (auto& exported : info.exports) {
            if (exported.kind != Wasm::ExternalKind::Function || exported.kindIndex < info.importFunctionCount())
                continue;
            uint32_t type = info.typeSignatureIndexFromFunctionIndexSpace(Wasm::FunctionSpaceIndex(exported.kindIndex)).rawIndex();
            const Wasm::RTT& rtt = info.rtt(Wasm::TypeSignatureIndex(type));
            if (rtt.mayHaveICEntrypointInImage() && typesWithJSEntrypoint.add(&rtt).isNewEntry)
                jsEntrypointTypes.append(type);
        }
        pointers = Wasm::ImagePointerTable::forModule(info, jsEntrypointTypes.size());

        for (uint32_t type = 0; type < info.typeCount(); ++type)
            typeIndices.add(&info.rtt(Wasm::TypeSignatureIndex(type)), type);
    }

    void compileFunction(Wasm::FunctionCodeIndex functionIndex, B3::CompilationScratch& scratch, BuildContext& buildContext, WasmFunctionCode& result) const
    {
        result.usesVectors = info.usesSIMD(functionIndex);

        Wasm::FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(functionIndex);
        const Wasm::RTT& signature = info.rtt(info.internalFunctionTypeSignatureIndices[functionIndex]);
        Ref<Wasm::IPIntCallee> profiledCallee = group->ipintCalleeFromFunctionIndexSpace(functionIndexSpace);
        Ref<Wasm::OMGCallee> callee = Wasm::OMGCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { profiledCallee });
        Vector<Wasm::UnlinkedWasmToWasmCall> unlinkedCalls;
        Wasm::CompilationContext context;
        context.wasmEntrypointJIT = makeUnique<CCallHelpers>();
        AssemblerPolicy assemblerPolicy = buildContext.assemblerPolicy();
#if CPU(X86_64)
        if (result.usesVectors)
            assemblerPolicy.features = assemblerPolicy.features.with(CPUFeature::AVX);
#endif
        RelocationRecorder recorder(*context.wasmEntrypointJIT, assemblerPolicy);
        ImageRelocationLowering lowering({ RelocationSpace::WebAssemblyPointers, GPRInfo::wasmContextInstancePointer, safeCast<int32_t>(JSWebAssemblyInstance::offsetOfImageCodePointers()), true, Wasm::runtimeFunctionSlot }, recorder);
        B3::Policy policy = b3PolicyFor(assemblerPolicy.features, lowering);
        Wasm::CodeForImage codeForImage { pointers, typeIndices, recorder, policy, scratch, static_cast<uint32_t>(RelocationSpace::WebAssemblyPointers) };
        context.codeForImage = &codeForImage;
        auto compiled = Wasm::parseAndCompileOMG(context, profiledCallee.get(), callee.get(), info.functions[functionIndex], signature, unlinkedCalls, module.get(), group.get(), info, mode, Wasm::CompilationMode::OMGMode, functionIndex, UINT32_MAX);
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

    std::optional<LinkFailure> generateStubs(WasmModuleCode& code, BuildContext& context) const
    {
        std::optional<LinkFailure> failure;
        auto generate = [&](const auto& emit) -> Vector<uint8_t> {
            if (failure)
                return { };
            CCallHelpers jit;
            RelocationRecorder recorder(jit, context.assemblerPolicy());
            emit(jit);
            auto stub = recorder.finalize(LinkBuffer::Profile::WasmThunk, &context);
            if (!stub) {
                failure = stub.error();
                return { };
            }
            RELEASE_ASSERT(stub->relocations.isEmpty());
            return WTF::move(stub->bytes);
        };

        code.throwStub = generate(emitThrowStub);
        for (unsigned index = 0; index < info.importFunctionCount(); ++index) {
            code.importStubs.append(generate([&](CCallHelpers& jit) {
                Wasm::emitWasmToWasm(jit, info, index);
            }));
        }
        for (uint32_t entry = 0; entry < jsEntrypointTypes.size(); ++entry) {
            const Wasm::RTT& rtt = info.rtt(Wasm::TypeSignatureIndex(jsEntrypointTypes[entry]));
            code.jsEntrypoints.append({ jsEntrypointTypes[entry], generate([&](CCallHelpers& jit) {
                RegisterAtOffsetList registersToSpill = rtt.icEntrypointSavedRegisters();
                bool isEmitted = rtt.emitJSToWasmICEntrypoint(jit, registersToSpill, nullptr, Wasm::RTT::ICEntrypointImagePointers { pointers.jsEntrypointCallee(entry), Wasm::ImagePointerTable::genericJSEntrypoint });
                RELEASE_ASSERT(isEmitted);
            }) });
        }
        for (unsigned index = 0; index < code.functions.size(); ++index) {
            if (code.functions[index].hasCode())
                continue;
            code.functions[index].code.bytes = generate([&](CCallHelpers& jit) {
                emitInterpreterStub(jit, pointers.callee(Wasm::FunctionCodeIndex(index)));
            });
        }
        return failure;
    }
};

} // anonymous namespace

static void internSavedRegisters(WasmModuleCode& module)
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

WasmModuleOrDecline compileWasmModule(VM& vm, std::span<const uint8_t> source, const CodeTemplates& templates, BuildContext& context)
{
    if (!isX86_64() && !isARM64())
        return makeUnexpected(Wasm::ImageDeclineReason::UnsupportedCPU);
    if (source.size() > Wasm::maxModuleSize)
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);
    auto validated = Wasm::Module::validateSync(vm, Vector<uint8_t>(source));
    if (!validated)
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);
    const Wasm::ModuleInformation& info = (*validated)->moduleInformation();
    if (!info.internalFunctionCount())
        return makeUnexpected(Wasm::ImageDeclineReason::NoFunctions);
    if (info.hasGCObjectTypes())
        return makeUnexpected(Wasm::ImageDeclineReason::HasGCTypes);
    MemoryMode mode = info.memoryCount() && !info.memory(0).isMemory64() ? MemoryMode::Signaling : MemoryMode::BoundsChecking;
    Ref<Wasm::CalleeGroup> group = (*validated)->compileSync(vm, mode);
    if (!group->runnable())
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);

    WasmModuleCompiler compiler { WTF::move(*validated), WTF::move(group), mode };
    compiler.numberTypes();

    WasmModuleCode code;
    code.digest = Wasm::CodeInImage::digestOf(source);
    code.sourceSize = static_cast<uint32_t>(source.size());
    code.isForSignalingMemory = mode == MemoryMode::Signaling;
    code.functions = Vector<WasmFunctionCode>(info.internalFunctionCount());
    forEachIndexInParallel(context.parallelLoops(), code.functions.size(), [&](unsigned index, unsigned worker) {
        compiler.compileFunction(Wasm::FunctionCodeIndex(index), context.backEndScratch(worker), context, code.functions[index]);
    });
    internSavedRegisters(code);

    size_t numberOfFunctions = code.functions.size();
    WasmModuleOrDecline linked = [&]() -> WasmModuleOrDecline {
        if (std::optional<LinkFailure> failure = compiler.generateStubs(code, context))
            return makeUnexpected(declineReasonFor(*failure));
        return linkWasmModule(WTF::move(code), templates, context);
    }();
    size_t numberOfInterpretedFunctions = linked ? linked->interpretedFunctions.size() : numberOfFunctions;
    context.statistics().count(StatisticID::WasmFunctionsCompiled, numberOfFunctions - numberOfInterpretedFunctions);
    context.statistics().count(StatisticID::WasmFunctionsInterpreted, numberOfInterpretedFunctions);
    return linked;
}

namespace {

enum class UnitKind : uint8_t { Stub, Function, Constant, JumpTable };

struct UnitOrigin {
    UnitKind kind;
    uint32_t function;
    uint32_t index;
};

struct FunctionUnits {
    uint32_t code;
    uint32_t firstConstant;
    uint32_t firstJumpTable;
};

struct ModuleUnits {
    uint32_t append(const LinkUnit& unit, UnitOrigin origin)
    {
        units.append(unit);
        origins.append(origin);
        return static_cast<uint32_t>(units.size() - 1);
    }

    uint32_t appendStub(const Vector<uint8_t>& bytes) { return append({ bytes.span(), { }, codeUnitAlignment, UnitSharing::Unique }, { UnitKind::Stub, 0, 0 }); }
    uint32_t size() const { return static_cast<uint32_t>(units.size()); }

    Vector<LinkUnit> units;
    Vector<UnitOrigin> origins;
    Vector<FunctionUnits> functions;
    Vector<RelocatableCode> jumpTables;
    uint32_t firstImportStub { 0 };
    uint32_t firstJSEntrypoint { 0 };
    uint32_t firstDataUnit { 0 };
};

} // anonymous namespace

static RelocatableCode encodeJumpTable(const JumpTable& table)
{
    RelocatableCode encoded;
    encoded.bytes = Vector<uint8_t>(FillWith { }, table.targets.size() * sizeof(int32_t), 0);
    encoded.relocations.reserveInitialCapacity(table.targets.size());
    std::span<uint8_t> words = encoded.bytes.mutableSpan();
    for (uint32_t index = 0; index < table.targets.size(); ++index) {
        // Distance32 adds the distance from the word to its target, and a linked word holds the distance from the start of the table.
        int32_t offset = safeCast<int32_t>(index * sizeof(int32_t));
        memcpySpan(words.subspan(offset), asByteSpan(offset));
        encoded.relocations.append(Relocation { .offset = static_cast<uint32_t>(offset), .kind = RelocationKind::Distance32, .space = SymbolSpace::JumpTable, .entry = primaryEntry, .symbol = index });
    }
    return encoded;
}

static ModuleUnits collectUnits(const WasmModuleCode& module)
{
    ModuleUnits units;
    uint32_t throwStub = units.appendStub(module.throwStub);
    RELEASE_ASSERT(throwStub == throwStubUnit);

    units.firstImportStub = units.size();
    for (const Vector<uint8_t>& stub : module.importStubs)
        units.appendStub(stub);

    units.firstJSEntrypoint = units.size();
    for (const WasmJSEntrypointCode& entrypoint : module.jsEntrypoints)
        units.appendStub(entrypoint.bytes);

    size_t numberOfJumpTables = 0;
    units.functions.reserveInitialCapacity(module.functions.size());
    for (uint32_t functionIndex = 0; functionIndex < module.functions.size(); ++functionIndex) {
        const RelocatableCode& code = module.functions[functionIndex].code;
        uint32_t unit = units.append({ code.bytes.span(), code.relocations.span(), codeUnitAlignment, UnitSharing::Unique }, { UnitKind::Function, functionIndex, 0 });
        units.functions.append(FunctionUnits { unit, 0, 0 });
        numberOfJumpTables += module.functions[functionIndex].jumpTables.size();
    }

    // The units hold spans of the tables, which stay where they are made.
    units.firstDataUnit = units.size();
    units.jumpTables.reserveInitialCapacity(numberOfJumpTables);
    for (uint32_t functionIndex = 0; functionIndex < module.functions.size(); ++functionIndex) {
        const WasmFunctionCode& function = module.functions[functionIndex];
        units.functions[functionIndex].firstConstant = units.size();
        for (uint32_t index = 0; index < function.constants.size(); ++index) {
            const ConstantData& constant = function.constants[index];
            units.append({ constant.bytes.span(), { }, constant.alignment, UnitSharing::Unique }, { UnitKind::Constant, functionIndex, index });
        }

        units.functions[functionIndex].firstJumpTable = units.size();
        for (uint32_t index = 0; index < function.jumpTables.size(); ++index) {
            units.jumpTables.append(encodeJumpTable(function.jumpTables[index]));
            const RelocatableCode& table = units.jumpTables.last();
            units.append({ table.bytes.span(), table.relocations.span(), jumpTableAlignment, UnitSharing::Unique }, { UnitKind::JumpTable, functionIndex, index });
        }
    }
    return units;
}

static std::optional<UnitTarget> resolveInFunction(const WasmFunctionCode& function, const ModuleUnits& units, const FunctionUnits& functionUnits, const Relocation& relocation)
{
    switch (relocation.space) {
    case SymbolSpace::WebAssembly: {
        const Wasm::ImageCallTarget& target = function.callTargets[relocation.symbol];
        switch (target.kind) {
        case Wasm::ImageCallTarget::Kind::ThrowStub:
            return UnitTarget { throwStubUnit };
        case Wasm::ImageCallTarget::Kind::Function:
            return UnitTarget { units.functions[target.function].code };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case SymbolSpace::Constant:
        RELEASE_ASSERT(relocation.symbol < function.constants.size());
        return UnitTarget { functionUnits.firstConstant + relocation.symbol };
    case SymbolSpace::JumpTable:
        RELEASE_ASSERT(relocation.symbol < function.jumpTables.size());
        return UnitTarget { functionUnits.firstJumpTable + relocation.symbol };
    case SymbolSpace::Function:
    case SymbolSpace::Stub:
    case SymbolSpace::ArgumentThunk:
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::SiteCache:
    case SymbolSpace::RegExp:
    case SymbolSpace::FFI:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<UnitTarget> resolve(const WasmModuleCode& module, const ModuleUnits& units, uint32_t unit, const Relocation& relocation)
{
    const UnitOrigin& origin = units.origins[unit];
    switch (origin.kind) {
    case UnitKind::Function:
        return resolveInFunction(module.functions[origin.function], units, units.functions[origin.function], relocation);
    case UnitKind::JumpTable:
        return UnitTarget { units.functions[origin.function].code, module.functions[origin.function].jumpTables[origin.index].targets[relocation.symbol] };
    case UnitKind::Stub:
    case UnitKind::Constant:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wasm::ImageDeclineReason reportSectionLinkFailure(const SectionLinkFailure& failure, BuildContext& context)
{
    switch (failure.error) {
    case SectionLinkError::SectionTooLarge:
    case SectionLinkError::OutOfPCRelativeReach:
        return Wasm::ImageDeclineReason::CodeTooLarge;
    case SectionLinkError::SymbolWithoutUnit:
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = failure.relocation });
        return Wasm::ImageDeclineReason::CodeGeneratorFailed;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

WasmModuleOrDecline linkWasmModule(WasmModuleCode&& module, const CodeTemplates& templates, BuildContext& context)
{
    ModuleUnits units = collectUnits(module);
    auto resolveUnit = [&](uint32_t unit, const Relocation& relocation) {
        return resolve(module, units, unit, relocation);
    };
    UnitResolver resolver = resolveUnit;
    auto section = linkSection({ units.units.span(), resolver, templates, context.limit(LimitID::WasmImageCodeSize) });
    if (!section)
        return makeUnexpected(reportSectionLinkFailure(section.error(), context));

    RELEASE_ASSERT(section->bytes.size() <= maxSectionOffset);
    const Vector<uint32_t>& offsets = section->uniqueUnitOffsets;

    Wasm::ModuleForImage result;
    result.digest = module.digest;
    result.sourceSize = module.sourceSize;
    result.isForSignalingMemory = module.isForSignalingMemory;
    result.functionsEnd = units.firstDataUnit < units.size() ? offsets[units.firstDataUnit] : static_cast<uint32_t>(section->bytes.size());
    result.importStubs.append(offsets.span().subspan(units.firstImportStub, module.importStubs.size()));

    result.jsEntrypoints.reserveInitialCapacity(module.jsEntrypoints.size());
    for (uint32_t index = 0; index < module.jsEntrypoints.size(); ++index)
        result.jsEntrypoints.append(Wasm::JSEntrypointInImage { module.jsEntrypoints[index].type, offsets[units.firstJSEntrypoint + index] });

    result.functions.reserveInitialCapacity(module.functions.size());
    for (uint32_t index = 0; index < module.functions.size(); ++index) {
        const WasmFunctionCode& function = module.functions[index];
        result.functions.append(Wasm::FunctionInImage { offsets[units.functions[index].code], function.savedRegistersIndex, function.hasCode(), function.usesVectors });
        result.usesVectors |= function.hasCode() && function.usesVectors;
        if (!function.hasCode())
            result.interpretedFunctions.append({ index, function.declineReason });
    }

    result.code = static_cast<WTF::Vector<uint8_t>&&>(WTF::move(section->bytes));
    result.savedRegisters = WTF::move(module.savedRegisters);
    return result;
}

static uint32_t appendToDirectory(Vector<uint8_t>& directory, std::span<const uint8_t> bytes)
{
    uint32_t offset = static_cast<uint32_t>(directory.size());
    directory.append(bytes);
    directory.insertFill(directory.size(), 0, WTF::roundUpToMultipleOf<directoryAlignment>(directory.size()) - directory.size());
    return offset;
}

static Wasm::ModuleInImage appendModule(Vector<uint8_t>& code, Vector<uint8_t>& directory, Wasm::ModuleForImage& module, uint32_t base)
{
    code.insertFill(code.size(), isX86() ? 0xcc : 0, WTF::roundUpToMultipleOf<WasmSectionBuilder::codeAlignment>(code.size()) - code.size());
    RELEASE_ASSERT(code.size() == base);
    code.appendVector(module.code);
    module.code.clear();

    for (auto& function : module.functions)
        function.entry += base;
    for (uint32_t& stub : module.importStubs)
        stub += base;
    for (auto& entry : module.jsEntrypoints)
        entry.code += base;

    Wasm::ModuleInImage record { };
    record.digest = module.digest;
    record.sourceSize = module.sourceSize;
    record.isForSignalingMemory = module.isForSignalingMemory;
    record.usesVectors = module.usesVectors;
    record.numberOfFunctions = static_cast<uint32_t>(module.functions.size());
    record.numberOfImportedFunctions = static_cast<uint32_t>(module.importStubs.size());
    record.functions = appendToDirectory(directory, asBytes(module.functions.span()));
    record.functionsEnd = module.functionsEnd + base;
    record.importStubs = appendToDirectory(directory, asBytes(module.importStubs.span()));
    record.numberOfJSEntrypoints = static_cast<uint32_t>(module.jsEntrypoints.size());
    record.jsEntrypoints = appendToDirectory(directory, asBytes(module.jsEntrypoints.span()));
    record.savedRegisters = appendToDirectory(directory, asBytes(module.savedRegisters.span()));
    return record;
}

bool WasmSectionBuilder::append(Wasm::ModuleForImage&& module)
{
    auto arraySize = [](const auto& array) -> uint64_t {
        return WTF::roundUpToMultipleOf<directoryAlignment>(array.sizeInBytes());
    };
    uint64_t base = WTF::roundUpToMultipleOf<codeAlignment>(m_codeSize);
    uint64_t codeSize = base + module.code.size();
    uint64_t directorySize = m_directorySize + sizeof(Wasm::ModuleInImage) + arraySize(module.functions) + arraySize(module.importStubs) + arraySize(module.jsEntrypoints) + arraySize(module.savedRegisters);
    if (codeSize > maxSectionOffset || directorySize > maxSectionOffset)
        return false;
    m_modules.append(PlacedModule { WTF::move(module), static_cast<uint32_t>(base) });
    m_codeSize = codeSize;
    m_directorySize = directorySize;
    return true;
}

void WasmSectionBuilder::appendDeclined(const Wasm::DeclinedModuleInImage& module)
{
    uint64_t directorySize = m_directorySize + sizeof(module);
    if (directorySize > maxSectionOffset)
        return;
    m_declinedModules.append(module);
    m_directorySize = directorySize;
}

WebAssemblyImageInputs WasmSectionBuilder::finish()
{
    if (m_modules.isEmpty() && m_declinedModules.isEmpty())
        return { };

    WebAssemblyImageInputs section;
    if (!m_modules.isEmpty())
        section.codeAlignment = codeAlignment;
    section.numberOfModules = static_cast<uint32_t>(m_modules.size());
    section.code.reserveInitialCapacity(m_codeSize);
    section.directory.reserveInitialCapacity(m_directorySize);

    Wasm::DirectoryInImage header { section.numberOfModules, static_cast<uint32_t>(m_declinedModules.size()) };
    appendToDirectory(section.directory, asByteSpan(header));
    size_t recordsOffset = section.directory.size();
    section.directory.insertFill(recordsOffset, 0, m_modules.size() * sizeof(Wasm::ModuleInImage));
    appendToDirectory(section.directory, asBytes(m_declinedModules.span()));
    for (size_t index = 0; index < m_modules.size(); ++index) {
        Wasm::ModuleInImage record = appendModule(section.code, section.directory, m_modules[index].module, m_modules[index].base);
        memcpySpan(section.directory.mutableSpan().subspan(recordsOffset + index * sizeof(record)), asByteSpan(record));
    }

    RELEASE_ASSERT(section.code.size() == m_codeSize);
    RELEASE_ASSERT(section.directory.size() == m_directorySize);
    return section;
}

using DeclineCount = std::pair<Wasm::ImageDeclineReason, uint32_t>;

namespace {

struct KnownModule {
    std::array<uint8_t, 20> digest;
    size_t size;
    bool isInImage;
    bool isInText { false };
    Vector<DeclineCount, 2> declines;
};

} // anonymous namespace

static LimitID limitOf(Wasm::ImageDeclineReason reason)
{
    return reason == Wasm::ImageDeclineReason::CodeTooLarge ? LimitID::WasmImageCodeSize : LimitID::None;
}

static Vector<DeclineCount, 2> collectDeclines(const WasmModuleOrDecline& module)
{
    if (!module)
        return { { module.error(), 1 } };
    Vector<DeclineCount, 2> declines;
    for (auto [function, reason] : module->interpretedFunctions) {
        auto decline = std::ranges::find(declines, reason, &DeclineCount::first);
        if (decline != declines.end())
            ++decline->second;
        else
            declines.append({ reason, 1 });
    }
    AOT::sort(declines, { }, &DeclineCount::first);
    return declines;
}

// The run time finds code by the digest of the bytes a module is made from, so text that only looks like a module costs bytes and never an answer.
static std::span<const uint8_t> base64OfEmbeddedModule(std::span<const uint8_t> text)
{
    auto modulePrefix = "AGFzbQEAAA"_span;
    auto base64Marker = "base64,"_span;
    constexpr size_t minLength = 64;
    constexpr size_t maxPrefixLength = 64;
    static_assert(maxPrefixLength <= minLength);
    if (text.size() < minLength)
        return { };
    if (spanHasPrefix(text, modulePrefix))
        return text;
    if (text[0] != 'd')
        return { };
    size_t at = WTF::find(text.first(maxPrefixLength), base64Marker);
    if (at == notFound)
        return { };
    text = text.subspan(at + base64Marker.size());
    if (!spanHasPrefix(text, modulePrefix))
        return { };
    return text;
}

WebAssemblyImageInputs compileWebAssemblyModules(VM& vm, const Typed::Program& program, const CodeTemplates& templates, BuildContext& context, const BitVector* stringsInImage)
{
    WasmSectionBuilder builder;
    Vector<KnownModule> modules;
    auto add = [&](std::span<const uint8_t> bytes) -> KnownModule& {
        auto digest = Wasm::CodeInImage::digestOf(bytes);
        size_t index = modules.findIf([&](auto& known) {
            return known.digest == digest && known.size == bytes.size();
        });
        if (index != notFound)
            return modules[index];

        WasmModuleOrDecline module = compileWasmModule(vm, bytes, templates, context);
        Vector<DeclineCount, 2> declines = collectDeclines(module);
        bool isInImage = module && builder.append(WTF::move(*module));
        if (module && !isInImage)
            declines = { { Wasm::ImageDeclineReason::CodeTooLarge, 1 } };
        if (!isInImage)
            builder.appendDeclined({ digest, static_cast<uint32_t>(bytes.size()) });
        modules.append(KnownModule { .digest = digest, .size = bytes.size(), .isInImage = isInImage, .declines = WTF::move(declines) });
        return modules.last();
    };

    for (auto& entry : program.webAssemblyModules) {
        for (auto [reason, count] : add(entry.bytes).declines)
            reportDecline(context, entry.module, diagnosticKindOf(reason), limitOf(reason), count);
    }

    uint64_t numberFound = 0;
    uint64_t numberDeclined = 0;
    for (size_t index = 0; index < program.strings.size(); ++index) {
        if (stringsInImage && !stringsInImage->get(index))
            continue;
        std::span<const uint8_t> base64 = base64OfEmbeddedModule(program.strings[Typed::StringID(index)]);
        std::optional<Vector<uint8_t>> bytes = base64.empty() ? std::nullopt : base64Decode(base64);
        if (!bytes)
            continue;
        KnownModule& module = add(bytes->span());
        if (std::exchange(module.isInText, true))
            continue;
        ++numberFound;
        if (!module.isInImage)
            ++numberDeclined;
        for (auto [reason, count] : module.declines) {
            if (LimitID limit = limitOf(reason); limit != LimitID::None)
                context.gaveUp(limit);
        }
    }
    context.statistics().count(StatisticID::WebAssemblyModulesFoundInText, numberFound);
    context.statistics().count(StatisticID::WebAssemblyModulesInTextDeclined, numberDeclined);
    return builder.finish();
}

bool mayHaveWebAssemblyModules(const Typed::Program& program)
{
    if (!program.webAssemblyModules.isEmpty())
        return true;
    for (size_t index = 0; index < program.strings.size(); ++index) {
        if (!base64OfEmbeddedModule(program.strings[Typed::StringID(index)]).empty())
            return true;
    }
    return false;
}

#else

WebAssemblyImageInputs compileWebAssemblyModules(VM&, const Typed::Program& program, const CodeTemplates&, BuildContext& context, const BitVector*)
{
#if ENABLE(WEBASSEMBLY)
    constexpr UserDiagnosticKind reason = UserDiagnosticKind::WasmUnsupportedCPU;
#else
    constexpr UserDiagnosticKind reason = UserDiagnosticKind::WasmLeftOutOfThisBuild;
#endif
    for (auto& entry : program.webAssemblyModules)
        reportDecline(context, entry.module, reason, LimitID::None, 1);
    return { };
}

bool mayHaveWebAssemblyModules(const Typed::Program& program)
{
    return !program.webAssemblyModules.isEmpty();
}

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
