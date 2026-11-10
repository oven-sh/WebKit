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
#include "AOTImageFormat.h"
#include "AOTLimits.h"
#include "AOTSectionLinker.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "CCallHelpers.h"
#include "JSLock.h"
#include "JSWebAssemblyInstance.h"
#include "VM.h"
#include "B3ArgumentRegValue.h"
#include "B3CompilationReport.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3Operations.h"
#include "B3PatchpointValue.h"
#include "B3Policy.h"
#include "B3Procedure.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "WasmBBQJIT.h"
#include "WasmOMGIRGenerator.h"
#include "WasmBinding.h"
#include "WasmCallee.h"
#include "WasmCalleeGroup.h"
#include "WasmCodeInImage.h"
#include "WasmCompilationContext.h"
#include "WasmIPIntPlan.h"
#include "WasmLimits.h"
#include "WasmModule.h"
#include "WasmModuleInformation.h"
#include "WasmThunks.h"
#include "WasmToJS.h"
#include "WasmTypeDefinition.h"
#include "WasmTypeDefinitionInlines.h"
#include "WasmWorklist.h"
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/Base64.h>
#include <wtf/text/MakeString.h>
#endif

namespace JSC { namespace AOT {

static void reportDecline(BuildContext& context, SourceLocation location, UserDiagnosticKind kind, LimitID limit, uint32_t count)
{
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
static constexpr size_t directoryAlignment = 8;
static constexpr uint64_t maxSectionOffset = std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), defaultLimitValue(LimitID::VectorBytes, hostArchitecture()));

static_assert(Wasm::maxModuleSize <= std::numeric_limits<uint32_t>::max());
static_assert(defaultLimitValue(LimitID::WasmImageCodeSize, hostArchitecture()) <= std::numeric_limits<uint32_t>::max());
static_assert(!(sizeof(Wasm::DirectoryInImage) % directoryAlignment));
static_assert(!(sizeof(Wasm::ModuleInImage) % directoryAlignment));
static_assert(!(sizeof(Wasm::DeclinedModuleInImage) % directoryAlignment));
static_assert(alignof(Wasm::SavedRegistersInImage) == directoryAlignment);
static_assert(std::has_unique_object_representations_v<Wasm::DirectoryInImage> && std::has_unique_object_representations_v<Wasm::ModuleInImage>
    && std::has_unique_object_representations_v<Wasm::DeclinedModuleInImage> && std::has_unique_object_representations_v<Wasm::FunctionInImage>
    && std::has_unique_object_representations_v<Wasm::HandlerInImage> && std::has_unique_object_representations_v<Wasm::JSEntrypointInImage>
    && std::has_unique_object_representations_v<Wasm::SavedRegistersInImage>, "padding would make the bytes of the image differ from build to build");

static Wasm::ImageCodeFailure codeFailureFor(LinkFailure failure)
{
    switch (failure) {
    case LinkFailure::ProcessAddressInCode:
        return Wasm::ImageCodeFailure::ProcessAddressInCode;
    case LinkFailure::OutOfPCRelativeReach:
        return Wasm::ImageCodeFailure::OutOfPCRelativeReach;
    case LinkFailure::OutOfMemory:
        return Wasm::ImageCodeFailure::OutOfMemory;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral descriptionOf(Wasm::ImageCodeFailure failure)
{
    switch (failure) {
    case Wasm::ImageCodeFailure::None:
        break;
    case Wasm::ImageCodeFailure::NamesTypeOfOtherModule:
        return "it names a type that the module does not list"_s;
    case Wasm::ImageCodeFailure::SavesVectorRegister:
        return "it saves a vector register"_s;
    case Wasm::ImageCodeFailure::SavedRegisterOrder:
        return "its saved registers are not in the order of the run time"_s;
    case Wasm::ImageCodeFailure::TooManySavedRegisterSets:
        return "the module has too many sets of saved registers"_s;
    case Wasm::ImageCodeFailure::ProcessAddressInCode:
        return "it holds an address of the compiling process"_s;
    case Wasm::ImageCodeFailure::OutOfPCRelativeReach:
        return "a place in it is out of reach"_s;
    case Wasm::ImageCodeFailure::OutOfMemory:
        return "there is no memory for it"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportCodeFailure(BuildContext& context, SourceLocation location, ASCIILiteral what, uint32_t index, Wasm::ImageCodeFailure failure)
{
    context.diagnostics().add(InternalError {
        .kind = InternalErrorKind::WebAssemblyCodeNotForImage,
        .count = index,
        .location = location,
        .detail = makeString(what, ' ', index, ": "_s, descriptionOf(failure)),
    });
}

static std::expected<Wasm::SavedRegistersInImage, Wasm::ImageCodeFailure> encodeSavedRegisters(const RegisterAtOffsetList& savedRegisters)
{
    Wasm::SavedRegistersInImage result { 0, 0, 0 };
    for (auto& entry : savedRegisters) {
        if (entry.width() != conservativeWidthWithoutVectors(entry.reg()))
            return makeUnexpected(Wasm::ImageCodeFailure::SavesVectorRegister);
        result.registers |= 1ull << entry.reg().index();
    }
    if (savedRegisters.registerCount())
        result.offsetOfFirst = safeCast<int32_t>(savedRegisters.at(0).offset());
    RegisterAtOffsetList decoded = Wasm::CodeInImage::savedRegistersFrom(result);
    bool isSame = decoded.registerCount() == savedRegisters.registerCount();
    for (size_t index = 0; isSame && index < decoded.registerCount(); ++index)
        isSame = decoded.at(index) == savedRegisters.at(index);
    if (!isSame)
        return makeUnexpected(Wasm::ImageCodeFailure::SavedRegisterOrder);
    return result;
}

namespace {

struct WasmFunctionFailure {
    bool isSet() const { return wasRefusedByGenerator || failure != Wasm::ImageCodeFailure::None; }

    bool wasRefusedByGenerator { false };
    Wasm::ImageCodeFailure failure { Wasm::ImageCodeFailure::None };
};

enum class WasmRelocationSpace : uint32_t {
    Pointers = 1,
    Constants,
    JumpTables,
};

class WasmRelocationLowering final : public B3::RelocationLowering {
public:
    B3::Value* lower(B3::Procedure& procedure, B3::InsertionSet& insertionSet, size_t index, const B3::RelocatableValue& value) final
    {
        B3::RelocationSymbol symbol = value.symbol();
        B3::Origin origin = value.origin();
        B3::Value* address = nullptr;
        if (symbol.space == static_cast<uint32_t>(WasmRelocationSpace::Pointers)) {
            B3::Value* instance = insertionSet.insert<B3::ArgumentRegValue>(index, origin, GPRInfo::wasmContextInstancePointer);
            B3::Value* table = insertImmutableLoad(insertionSet, index, origin, instance, safeCast<int32_t>(JSWebAssemblyInstance::offsetOfImageCodePointers()));
            address = insertImmutableLoad(insertionSet, index, origin, table, safeCast<int32_t>(symbol.index * sizeof(void*)));
        } else if (symbol.space == static_cast<uint32_t>(WasmRelocationSpace::JumpTables))
            address = insertJumpTableAddress(procedure, insertionSet, index, origin, symbol.index);
        else if (symbol.space == static_cast<uint32_t>(WasmRelocationSpace::Constants))
            address = insertConstantsAddress(procedure, insertionSet, index, origin, symbol.index);
        else if (symbol.space == B3::runtimeFunctionSpace) {
            std::optional<uint32_t> slot = slotOfRuntimeFunction(static_cast<B3::RuntimeFunction>(symbol.index));
            if (!slot)
                return nullptr;
            B3::Value* instance = insertionSet.insert<B3::ArgumentRegValue>(index, origin, GPRInfo::wasmContextInstancePointer);
            B3::Value* table = insertImmutableLoad(insertionSet, index, origin, instance, safeCast<int32_t>(JSWebAssemblyInstance::offsetOfImageCodePointers()));
            address = insertImmutableLoad(insertionSet, index, origin, table, safeCast<int32_t>(*slot * sizeof(void*)));
        } else
            return nullptr;
        if (!value.addend())
            return address;
        B3::Value* addend = insertionSet.insertIntConstant(index, origin, B3::pointerType(), value.addend());
        return insertionSet.insert<B3::Value>(index, B3::Add, origin, address, addend);
    }

    bool isOutOfReach() const { return m_isOutOfReach; }

    void fillJumpTables(B3::Procedure& procedure, ImageLinkBuffer& buffer)
    {
        std::span<uint8_t> bytes = buffer.bytes();
        for (auto& [table, start] : m_tables) {
            uint32_t place = buffer.linkBuffer().offsetOf(*start);
            const Vector<uint32_t>& targets = procedure.jumpTable(table).targetOffsets;
            RELEASE_ASSERT(place + targets.size() * sizeof(int32_t) <= bytes.size());
            for (size_t entry = 0; entry < targets.size(); ++entry) {
                int32_t distance = static_cast<int32_t>(targets[entry]) - static_cast<int32_t>(place);
                memcpySpan(bytes.subspan(place + entry * sizeof(int32_t), sizeof(int32_t)), asByteSpan(distance));
            }
        }
    }

private:
    static std::optional<uint32_t> slotOfRuntimeFunction(B3::RuntimeFunction function)
    {
        switch (function) {
        case B3::RuntimeFunction::MemoryMove:
            return Wasm::ImagePointerTable::slotOfOperation(std::bit_cast<void*>(tagCFunction<OperationPtrTag>(B3::operationMemoryMove)));
        case B3::RuntimeFunction::GCSafeMemoryMove:
            return Wasm::ImagePointerTable::slotOfOperation(std::bit_cast<void*>(tagCFunction<OperationPtrTag>(B3::operationGCSafeMemoryMove)));
        case B3::RuntimeFunction::MemoryFill:
            return Wasm::ImagePointerTable::slotOfOperation(std::bit_cast<void*>(tagCFunction<OperationPtrTag>(B3::operationMemoryFill)));
        default:
            return std::nullopt;
        }
    }

    static B3::Value* insertImmutableLoad(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, B3::Value* base, int32_t offset)
    {
        B3::MemoryValue* load = insertionSet.insert<B3::MemoryValue>(index, B3::Load, B3::pointerType(), origin, base, offset);
        load->setControlDependent(false);
        load->setReadsMutability(B3::Mutability::Immutable);
        return load;
    }

    B3::Value* insertJumpTableAddress(B3::Procedure& procedure, B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, unsigned table)
    {
        size_t numberOfEntries = procedure.jumpTable(table).targetOffsets.size();
        B3::PatchpointValue* patchpoint = insertionSet.insert<B3::PatchpointValue>(index, B3::pointerType(), origin);
        patchpoint->effects = B3::Effects::none();
        patchpoint->resultConstraints = { B3::ValueRep::SomeRegister };
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([this, table, numberOfEntries](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            Box<CCallHelpers::Label> start;
            for (auto& [known, label] : m_tables) {
                if (known == table)
                    start = label;
            }
            if (!start) {
                start = Box<CCallHelpers::Label>::create();
                m_tables.append({ table, start });
                params.addLatePath([start, numberOfEntries](CCallHelpers& jit) {
                    *start = jit.label();
                    for (size_t entry = 0; entry < numberOfEntries; ++entry)
                        jit.m_assembler.buffer().putInt(0);
                });
            }
            linkAddress(jit, params[0].gpr(), start);
        });
        return patchpoint;
    }

    B3::Value* insertConstantsAddress(B3::Procedure& procedure, B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, unsigned section)
    {
        B3::Procedure::DataSectionRecord record = procedure.dataSections()[section];
        B3::PatchpointValue* patchpoint = insertionSet.insert<B3::PatchpointValue>(index, B3::pointerType(), origin);
        patchpoint->effects = B3::Effects::none();
        patchpoint->resultConstraints = { B3::ValueRep::SomeRegister };
        patchpoint->setRunsWithoutFrame(true);
        patchpoint->setGenerator([this, section, record](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            Box<CCallHelpers::Label> start;
            for (auto& [known, label] : m_constants) {
                if (known == section)
                    start = label;
            }
            if (!start) {
                start = Box<CCallHelpers::Label>::create();
                m_constants.append({ section, start });
                params.addLatePath([start, record](CCallHelpers& jit) {
                    while (jit.m_assembler.buffer().codeSize() % sizeof(v128_t))
                        jit.nop();
                    *start = jit.label();
                    RELEASE_ASSERT(!(record.size % sizeof(int32_t)));
                    for (int32_t word : unsafeMakeSpan(static_cast<const int32_t*>(record.data), record.size / sizeof(int32_t)))
                        jit.m_assembler.buffer().putInt(word);
                });
            }
            linkAddress(jit, params[0].gpr(), start);
        });
        return patchpoint;
    }

    void linkAddress(CCallHelpers& jit, GPRReg destination, Box<CCallHelpers::Label> target)
    {
        CCallHelpers::Label site = jit.movePCRelativeAddress(destination, PCRelativeReach::Far);
        jit.addLinkTask([this, site, target](LinkBuffer& linkBuffer) {
            if (!linkBuffer.link(site, RelocationKind::AddressFar, *target))
                m_isOutOfReach = true;
        });
    }

    Vector<std::pair<unsigned, Box<CCallHelpers::Label>>> m_tables;
    Vector<std::pair<unsigned, Box<CCallHelpers::Label>>> m_constants;
    bool m_isOutOfReach { false };
};

struct WasmModuleCompiler {
    Ref<Wasm::Module> module;
    Ref<Wasm::CalleeGroup> group;
    const Wasm::ModuleInformation& info { module->moduleInformation() };
    Vector<uint32_t> jsEntrypointTypes { };
    Wasm::ImagePointerTable pointers { };
    UncheckedKeyHashMap<const Wasm::RTT*, uint32_t> typeIndices { };
    Vector<Wasm::StructLayoutInImage> structLayouts { };
    bool usesGCTypes { false };

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

        structLayouts = Vector<Wasm::StructLayoutInImage>(info.typeCount());
        for (uint32_t type = 0; type < info.typeCount(); ++type) {
            const Wasm::RTT& rtt = info.rtt(Wasm::TypeSignatureIndex(type));
            if (rtt.kind() != Wasm::RTTKind::Function)
                usesGCTypes = true;
            if (typeIndices.add(&rtt, type).isNewEntry && rtt.kind() == Wasm::RTTKind::Struct)
                structLayouts[type] = layOutStruct(rtt);
        }
    }

    static Wasm::StructLayoutInImage layOutStruct(const Wasm::RTT& rtt)
    {
        unsigned end = Wasm::structPayloadOffsetPastShapeWord;
        unsigned gapPosition = 0;
        unsigned gapSize = 0;
        Wasm::StructLayoutInImage layout;
        layout.offsetsOfFieldsInPayload = Vector<uint32_t>(rtt.fieldCount(), [&](size_t field) {
            return Wasm::placeStructField(Wasm::typeSizeInBytes(rtt.field(field).type), end, gapPosition, gapSize);
        });
        layout.payloadSize = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(end);
        return layout;
    }

    WasmFunctionFailure compileWithOptimizingTier(Wasm::FunctionCodeIndex functionIndex, Wasm::ImageCodeFlavor flavor, BuildContext& buildContext, WasmFunctionCode& result) const
    {
        Wasm::FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(functionIndex);
        const Wasm::RTT& signature = info.rtt(info.internalFunctionTypeSignatureIndices[functionIndex]);
        Ref<Wasm::IPIntCallee> profiledCallee = group->ipintCalleeFromFunctionIndexSpace(functionIndexSpace);
        Ref<Wasm::OMGCallee> callee = Wasm::OMGCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { profiledCallee });
        Vector<Wasm::UnlinkedWasmToWasmCall> unlinkedCalls;
        Wasm::CompilationContext context;
        context.wasmEntrypointJIT = makeUnique<CCallHelpers>();
        AssemblerPolicy assemblerPolicy = buildContext.assemblerPolicy();
        RelocationRecorder recorder(*context.wasmEntrypointJIT, assemblerPolicy);
        WasmRelocationLowering lowering;
        B3::Policy policy;
        policy.upstream.allowsSIMD = true;
        bool usesTraps = flavor == Wasm::ImageCodeFlavor::Trapping && info.memoryCount();
        policy.upstream.usesWasmFaultSignalHandler = usesTraps;
        policy.optimization.narrowsIntegers = true;
        policy.selection.extendsInstructionSelection = true;
        policy.air.layout.mergesIdenticalBlocks = true;
        policy.addressing.isPositionIndependent = true;
        policy.addressing.dataSectionSpace = static_cast<uint32_t>(WasmRelocationSpace::Constants);
        policy.addressing.jumpTableSpace = static_cast<uint32_t>(WasmRelocationSpace::JumpTables);
        policy.addressing.relocationLowering = &lowering;
        policy.air.peepholes.emitsShufflesInPairOrder = true;
        policy.targetFeatures = assemblerPolicy.features;
        Wasm::CodeForImage codeForImage { pointers, typeIndices, structLayouts, recorder, flavor };
        codeForImage.dependsOnFlavor = true;
        codeForImage.policy = &policy;
        codeForImage.pointerSpace = static_cast<uint32_t>(WasmRelocationSpace::Pointers);
        context.codeForImage = &codeForImage;
        MemoryMode mode = usesTraps ? MemoryMode::Signaling : MemoryMode::BoundsChecking;
        auto compiled = Wasm::parseAndCompileOMG(context, profiledCallee.get(), callee.get(), info.functions[functionIndex], signature, unlinkedCalls, module.get(), group.get(), info, mode, Wasm::CompilationMode::OMGMode, functionIndex, UINT32_MAX);
        if (!compiled || !unlinkedCalls.isEmpty() || context.procedure->report().failure || !(*compiled)->exceptionHandlers.isEmpty()) {
            return { .wasRefusedByGenerator = true };
        }
        if (codeForImage.failure != Wasm::ImageCodeFailure::None)
            return { .failure = codeForImage.failure };

        ImageLinkBuffer buffer(*context.wasmEntrypointJIT, LinkBuffer::Profile::WasmOMG);
        if (buffer.didFailToAllocate())
            return { .failure = Wasm::ImageCodeFailure::OutOfMemory };
        buffer.finalize(false, "WebAssembly function"_s);
        if (context.procedure->report().failure)
            return { .wasRefusedByGenerator = true };
        if (lowering.isOutOfReach())
            return { .failure = Wasm::ImageCodeFailure::OutOfPCRelativeReach };
        lowering.fillJumpTables(*context.procedure, buffer);
        auto relocations = recorder.link(buffer, &buildContext);
        if (!relocations)
            return { .failure = codeFailureFor(relocations.error()) };
        auto savedRegisters = encodeSavedRegisters((*compiled)->entrypoint.calleeSaveRegisters);
        if (!savedRegisters)
            return { .failure = savedRegisters.error() };

        result.usesVectors = false;
        result.code = RelocatableCode { Vector<uint8_t>(buffer.bytes()), WTF::move(*relocations) };
        result.savedRegisters = *savedRegisters;
        result.dependsOnFlavor = true;
        result.threadedJumps = codeForImage.threadedJumps;
        result.firstStateTests = codeForImage.firstStateTests;
        for (size_t directCallee : (*compiled)->outgoingJITDirectCallees)
            result.directCallees.set(directCallee);
        return { };
    }

    WasmFunctionFailure compileFunction(Wasm::FunctionCodeIndex functionIndex, Wasm::ImageCodeFlavor flavor, BuildContext& buildContext, WasmFunctionCode& result) const
    {
        bool usesOptimizingTier = flavor == Wasm::ImageCodeFlavor::Trapping || !info.memoryCount();
        if (usesOptimizingTier && !usesGCTypes && !info.usesSIMD(functionIndex)) {
            WasmFunctionCode optimized;
            if (!compileWithOptimizingTier(functionIndex, flavor, buildContext, optimized).isSet()) {
                result = WTF::move(optimized);
                result.isOptimized = true;
                return { };
            }
        }
        result.usesVectors = info.usesSIMD(functionIndex);

        Wasm::FunctionSpaceIndex functionIndexSpace = info.toSpaceIndex(functionIndex);
        const Wasm::RTT& signature = info.rtt(info.internalFunctionTypeSignatureIndices[functionIndex]);
        Ref<Wasm::IPIntCallee> profiledCallee = group->ipintCalleeFromFunctionIndexSpace(functionIndexSpace);
        Ref<Wasm::BBQCallee> callee = Wasm::BBQCallee::create(functionIndexSpace, info.nameSection().get(functionIndexSpace), Ref { profiledCallee });
        Vector<Wasm::UnlinkedWasmToWasmCall> unlinkedCalls;
        Wasm::CompilationContext context;
        context.wasmEntrypointJIT = makeUnique<CCallHelpers>();
        AssemblerPolicy assemblerPolicy = buildContext.assemblerPolicy();
#if CPU(X86_64)
        if (result.usesVectors)
            assemblerPolicy.features = assemblerPolicy.features.with(CPUFeature::AVX);
#endif
        RelocationRecorder recorder(*context.wasmEntrypointJIT, assemblerPolicy);
        Wasm::CodeForImage codeForImage { pointers, typeIndices, structLayouts, recorder, flavor };
        context.codeForImage = &codeForImage;
        MemoryMode mode = flavor == Wasm::ImageCodeFlavor::Trapping ? MemoryMode::Signaling : MemoryMode::BoundsChecking;
        auto compiled = Wasm::parseAndCompileBBQ(context, profiledCallee.get(), callee.get(), info.functions[functionIndex], signature, unlinkedCalls, module.get(), group.get(), info, mode, functionIndex);
        RELEASE_ASSERT(unlinkedCalls.isEmpty());
        if (!compiled)
            return { .wasRefusedByGenerator = true };
        if (codeForImage.failure != Wasm::ImageCodeFailure::None)
            return { .failure = codeForImage.failure };

        ImageLinkBuffer buffer(*context.wasmEntrypointJIT, LinkBuffer::Profile::WasmBBQ);
        if (buffer.didFailToAllocate())
            return { .failure = Wasm::ImageCodeFailure::OutOfMemory };
        buffer.finalize(false, "WebAssembly function"_s);
        auto relocations = recorder.link(buffer, &buildContext);
        if (!relocations)
            return { .failure = codeFailureFor(relocations.error()) };

        auto savedRegisters = encodeSavedRegisters((*compiled)->entrypoint.calleeSaveRegisters);
        if (!savedRegisters)
            return { .failure = savedRegisters.error() };

        size_t numberOfEntrypointsTaken = 0;
        result.handlers.reserveInitialCapacity((*compiled)->exceptionHandlers.size());
        for (const Wasm::UnlinkedHandlerInfo& handler : (*compiled)->exceptionHandlers) {
            uint32_t place = 0;
            if (handler.m_type != Wasm::HandlerType::Delegate)
                place = buffer.linkBuffer().offsetOf(context.catchEntrypoints[numberOfEntrypointsTaken++]);
            result.handlers.append(Wasm::HandlerInImage { static_cast<uint32_t>(handler.m_type), handler.m_start, handler.m_end, handler.m_tryDepth, handler.m_exceptionIndexOrDelegateTarget, place });
        }
        RELEASE_ASSERT(numberOfEntrypointsTaken == context.catchEntrypoints.size());

        result.code = RelocatableCode { Vector<uint8_t>(buffer.bytes()), WTF::move(*relocations) };
        result.savedRegisters = *savedRegisters;
        result.dependsOnFlavor = codeForImage.dependsOnFlavor;
        for (size_t directCallee : (*compiled)->outgoingJITDirectCallees)
            result.directCallees.set(directCallee);
        return { };
    }

    bool reportFailures(const Vector<WasmFunctionFailure>& failures, BuildContext& context, SourceLocation location) const
    {
        bool isComplete = true;
        for (uint32_t index = 0; index < failures.size(); ++index) {
            if (!failures[index].isSet())
                continue;
            isComplete = false;
            if (failures[index].wasRefusedByGenerator)
                reportDecline(context, location, diagnosticKindOf(Wasm::ImageDeclineReason::FunctionNotCompiled), LimitID::None, index);
            else
                reportCodeFailure(context, location, "function"_s, index, failures[index].failure);
        }
        return isComplete;
    }

    bool generateStubs(WasmModuleCode& code, BuildContext& context, SourceLocation location) const
    {
        bool isComplete = true;
        auto generate = [&](ASCIILiteral what, uint32_t index, const auto& emit) -> RelocatableCode {
            CCallHelpers jit;
            RelocationRecorder recorder(jit, context.assemblerPolicy());
            emit(jit, recorder);
            auto stub = recorder.finalize(LinkBuffer::Profile::WasmThunk, &context);
            if (stub)
                return WTF::move(*stub);
            isComplete = false;
            reportCodeFailure(context, location, what, index, codeFailureFor(stub.error()));
            return { };
        };
        auto generateWithoutRelocations = [&](ASCIILiteral what, uint32_t index, const auto& emit) -> Vector<uint8_t> {
            RelocatableCode stub = generate(what, index, [&](CCallHelpers& jit, RelocationRecorder&) {
                emit(jit);
            });
            RELEASE_ASSERT(stub.relocations.isEmpty());
            return WTF::move(stub.bytes);
        };
        auto stubOf = [&](Wasm::ImageStub stub) -> RelocatableCode& {
            return code.stubs[static_cast<size_t>(stub)];
        };

        stubOf(Wasm::ImageStub::ThrowException) = generate("stub"_s, 0, [](CCallHelpers& jit, RelocationRecorder&) {
            Wasm::emitThrowExceptionFromWasm(jit);
        });
        stubOf(Wasm::ImageStub::ThrowStackOverflow) = generate("stub"_s, 1, [](CCallHelpers& jit, RelocationRecorder& recorder) {
            Wasm::emitPrepareToThrowStackOverflowFromWasm(jit);
            recorder.jump({ SymbolSpace::WebAssembly, Wasm::imageSymbolOf(Wasm::ImageStub::ThrowException) });
        });
        stubOf(Wasm::ImageStub::CatchRunway) = generate("stub"_s, 2, [](CCallHelpers& jit, RelocationRecorder&) {
            Wasm::emitCatchInWasm(jit);
        });

        for (unsigned index = 0; index < info.importFunctionCount(); ++index) {
            code.importStubs.append(generateWithoutRelocations("stub of import"_s, index, [&](CCallHelpers& jit) {
                Wasm::emitWasmToWasm(jit, info, index);
            }));
            code.javaScriptImportStubs.append(generateWithoutRelocations("stub of import from JavaScript"_s, index, [&](CCallHelpers& jit) {
                Wasm::emitWasmToJS(jit, info, index, &pointers);
            }));
        }
        for (uint32_t entry = 0; entry < jsEntrypointTypes.size(); ++entry) {
            const Wasm::RTT& rtt = info.rtt(Wasm::TypeSignatureIndex(jsEntrypointTypes[entry]));
            code.jsEntrypoints.append({ jsEntrypointTypes[entry], generateWithoutRelocations("entry from JavaScript"_s, entry, [&](CCallHelpers& jit) {
                RegisterAtOffsetList registersToSpill = rtt.icEntrypointSavedRegisters();
                bool isEmitted = rtt.emitJSToWasmICEntrypoint(jit, registersToSpill, nullptr, Wasm::RTT::ICEntrypointImagePointers { pointers.jsEntrypointCallee(entry), Wasm::ImagePointerTable::genericJSEntrypoint });
                RELEASE_ASSERT(isEmitted);
            }) });
        }
        return isComplete;
    }
};

} // anonymous namespace

static bool internSavedRegisters(WasmModuleCode& module, BuildContext& context, SourceLocation location)
{
    auto intern = [&](WasmFunctionCode& function, uint32_t index) {
        size_t known = module.savedRegisters.find(function.savedRegisters);
        if (known == notFound) {
            if (module.savedRegisters.size() == maxSavedRegisterSets) {
                reportCodeFailure(context, location, "function"_s, index, Wasm::ImageCodeFailure::TooManySavedRegisterSets);
                return false;
            }
            known = module.savedRegisters.size();
            module.savedRegisters.append(function.savedRegisters);
        }
        function.savedRegistersIndex = static_cast<uint16_t>(known);
        return true;
    };
    for (uint32_t index = 0; index < module.functions.size(); ++index) {
        if (!intern(module.functions[index], index))
            return false;
        if (module.functions[index].dependsOnFlavor && !intern(module.functionsThatCheck[index], index))
            return false;
    }
    return true;
}

namespace {

struct PendingWasmModule {
    WasmModuleCompiler compiler;
    WasmModuleCode code { };
    SourceLocation location { };
    Vector<WasmFunctionFailure> failures { };
    Vector<WasmFunctionFailure> checkingFailures { };
};

struct WasmFunctionTask {
    PendingWasmModule* module;
    uint32_t function;
    uint32_t bodySize;
    Wasm::ImageCodeFlavor flavor;
};

}

static std::expected<std::unique_ptr<PendingWasmModule>, Wasm::ImageDeclineReason> prepareWasmModule(VM& vm, std::span<const uint8_t> source, SourceLocation location)
{
    if (source.size() > Wasm::maxModuleSize)
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);
    Ref<Wasm::IPIntPlan> plan = adoptRef(*new Wasm::IPIntPlan(vm, Vector<uint8_t>(source), Wasm::CompilerMode::Validation, Wasm::Plan::dontFinalize(), Wasm::IPIntPlan::UsesCodeInImage::No));
    Wasm::ensureWorklist().enqueue(plan.get());
    plan->waitForCompletion();
    if (plan->failed())
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);
    Ref<Wasm::Module> validated = Wasm::Module::create(plan.get());
    size_t numberOfFunctions = validated->moduleInformation().internalFunctionCount();
    if (!numberOfFunctions)
        return makeUnexpected(Wasm::ImageDeclineReason::None);
    Ref<Wasm::CalleeGroup> group = validated->compileSync(vm, MemoryMode::BoundsChecking);
    if (!group->runnable())
        return makeUnexpected(Wasm::ImageDeclineReason::InvalidModule);

    std::unique_ptr<PendingWasmModule> module { new PendingWasmModule { { WTF::move(validated), WTF::move(group) } } };
    module->compiler.numberTypes();
    module->location = location;
    module->code.digest = Wasm::CodeInImage::digestOf(source);
    module->code.sourceSize = static_cast<uint32_t>(source.size());
    module->code.functions = Vector<WasmFunctionCode>(numberOfFunctions);
    module->code.functionsThatCheck = Vector<WasmFunctionCode>(numberOfFunctions);
    module->failures = Vector<WasmFunctionFailure>(numberOfFunctions);
    module->checkingFailures = Vector<WasmFunctionFailure>(numberOfFunctions);
    return module;
}

static bool hasSameCode(const WasmFunctionCode& a, const WasmFunctionCode& b)
{
    return a.isOptimized == b.isOptimized && a.code.bytes == b.code.bytes && a.code.relocations == b.code.relocations && a.handlers.isEmpty() && b.handlers.isEmpty()
        && a.directCallees == b.directCallees && a.savedRegisters == b.savedRegisters && a.usesVectors == b.usesVectors;
}

static void compileFunctions(std::span<const std::unique_ptr<PendingWasmModule>> modules, BuildContext& context)
{
    Vector<WasmFunctionTask> tasks;
    for (auto& module : modules) {
        if (!module)
            continue;
        for (uint32_t function = 0; function < module->code.functions.size(); ++function) {
            auto& body = module->compiler.info.functions[Wasm::FunctionCodeIndex(function)];
            uint32_t bodySize = safeCast<uint32_t>(body.end - body.start);
            tasks.append(WasmFunctionTask { module.get(), function, bodySize, Wasm::ImageCodeFlavor::Trapping });
            tasks.append(WasmFunctionTask { module.get(), function, bodySize, Wasm::ImageCodeFlavor::Checking });
        }
    }
    std::ranges::stable_sort(tasks, std::ranges::greater { }, &WasmFunctionTask::bodySize);
    forEachIndexInParallel(context.parallelLoops(), safeCast<unsigned>(tasks.size()), [&](unsigned index) {
        const WasmFunctionTask& task = tasks[index];
        bool isChecking = task.flavor == Wasm::ImageCodeFlavor::Checking;
        WasmFunctionCode& result = (isChecking ? task.module->code.functionsThatCheck : task.module->code.functions)[task.function];
        (isChecking ? task.module->checkingFailures : task.module->failures)[task.function] = task.module->compiler.compileFunction(Wasm::FunctionCodeIndex(task.function), task.flavor, context, result);
    });
    for (auto& module : modules) {
        if (!module)
            continue;
        for (uint32_t function = 0; function < module->code.functions.size(); ++function) {
            WasmFunctionCode& trapping = module->code.functions[function];
            if (trapping.isOptimized && hasSameCode(trapping, module->code.functionsThatCheck[function]))
                trapping.dependsOnFlavor = false;
            if (trapping.dependsOnFlavor)
                continue;
            module->code.functionsThatCheck[function] = { };
            module->checkingFailures[function] = { };
        }
    }
}

static WasmModuleOrDecline finishWasmModule(PendingWasmModule& module, const CodeTemplates& templates, BuildContext& context)
{
    if (!module.compiler.reportFailures(module.failures, context, module.location) || !module.compiler.reportFailures(module.checkingFailures, context, module.location))
        return makeUnexpected(Wasm::ImageDeclineReason::FunctionNotCompiled);
    if (!internSavedRegisters(module.code, context, module.location) || !module.compiler.generateStubs(module.code, context, module.location))
        return makeUnexpected(Wasm::ImageDeclineReason::FunctionNotCompiled);
    unsigned optimizedCopies = 0;
    unsigned baselineCopies = 0;
    unsigned optimizedCheckingCopies = 0;
    unsigned baselineCheckingCopies = 0;
    unsigned threadedJumps = 0;
    unsigned firstStateTests = 0;
    auto countCopy = [&](const WasmFunctionCode& copy) {
        ++(copy.isOptimized ? optimizedCopies : baselineCopies);
        threadedJumps += copy.threadedJumps;
        firstStateTests += copy.firstStateTests;
    };
    for (size_t function = 0; function < module.code.functions.size(); ++function) {
        countCopy(module.code.functions[function]);
        if (module.code.functions[function].dependsOnFlavor) {
            const WasmFunctionCode& checking = module.code.functionsThatCheck[function];
            countCopy(checking);
            ++(checking.isOptimized ? optimizedCheckingCopies : baselineCheckingCopies);
        }
    }
    context.statistics().count(StatisticID::WasmOptimizedCopies, optimizedCopies);
    context.statistics().count(StatisticID::WasmBaselineCopies, baselineCopies);
    context.statistics().count(StatisticID::WasmOptimizedCheckingCopies, optimizedCheckingCopies);
    context.statistics().count(StatisticID::WasmBaselineCheckingCopies, baselineCheckingCopies);
    context.statistics().count(StatisticID::WasmJumpsThreaded, threadedJumps);
    context.statistics().count(StatisticID::WasmFirstStateTests, firstStateTests);
    return linkWasmModule(WTF::move(module.code), templates, context);
}

WasmModuleOrDecline compileWasmModule(VM& vm, std::span<const uint8_t> source, const CodeTemplates& templates, BuildContext& context, SourceLocation location)
{
    auto module = prepareWasmModule(vm, source, location);
    if (!module)
        return makeUnexpected(module.error());
    compileFunctions(std::span<const std::unique_ptr<PendingWasmModule>>(&*module, 1), context);
    return finishWasmModule(**module, templates, context);
}

static BitVector findFunctionsWithTwoCopies(const WasmModuleCode& module)
{
    size_t numberOfFunctions = module.functions.size();
    Vector<Vector<uint32_t, 0>> callers(numberOfFunctions);
    for (uint32_t caller = 0; caller < numberOfFunctions; ++caller) {
        for (size_t callee : module.functions[caller].directCallees)
            callers[callee].append(caller);
    }

    BitVector result;
    Vector<uint32_t> worklist;
    auto add = [&](uint32_t function) {
        if (!result.set(function))
            worklist.append(function);
    };
    for (uint32_t function = 0; function < numberOfFunctions; ++function) {
        if (module.functions[function].dependsOnFlavor)
            add(function);
    }
    while (!worklist.isEmpty()) {
        for (uint32_t caller : callers[worklist.takeLast()])
            add(caller);
    }
    return result;
}

namespace {

struct ModuleUnits {
    uint32_t append(const RelocatableCode& code, std::optional<Wasm::ImageCodeFlavor> flavor = std::nullopt)
    {
        units.append(LinkUnit { code.bytes.span(), code.relocations.span(), codeUnitAlignment });
        flavors.append(flavor);
        return size() - 1;
    }

    uint32_t append(const Vector<uint8_t>& bytes)
    {
        units.append(LinkUnit { bytes.span(), { }, codeUnitAlignment });
        flavors.append(std::nullopt);
        return size() - 1;
    }

    uint32_t size() const { return static_cast<uint32_t>(units.size()); }

    Vector<LinkUnit> units;
    Vector<std::optional<Wasm::ImageCodeFlavor>> flavors;
    uint32_t firstStub { 0 };
    uint32_t firstImportStub { 0 };
    uint32_t firstJavaScriptImportStub { 0 };
    uint32_t firstJSEntrypoint { 0 };
    std::array<Vector<uint32_t>, Wasm::numberOfImageCodeFlavors> functions;
};

} // anonymous namespace

static const WasmFunctionCode& codeOf(const WasmModuleCode& module, uint32_t function, Wasm::ImageCodeFlavor flavor)
{
    if (flavor == Wasm::ImageCodeFlavor::Checking && module.functions[function].dependsOnFlavor)
        return module.functionsThatCheck[function];
    return module.functions[function];
}

static ModuleUnits collectUnits(const WasmModuleCode& module, const BitVector& functionsWithTwoCopies)
{
    ModuleUnits units;
    units.firstStub = units.size();
    for (const RelocatableCode& stub : module.stubs)
        units.append(stub);

    units.firstImportStub = units.size();
    for (const Vector<uint8_t>& stub : module.importStubs)
        units.append(stub);

    units.firstJavaScriptImportStub = units.size();
    for (const Vector<uint8_t>& stub : module.javaScriptImportStubs)
        units.append(stub);

    units.firstJSEntrypoint = units.size();
    for (const WasmJSEntrypointCode& entrypoint : module.jsEntrypoints)
        units.append(entrypoint.bytes);

    auto& trapping = units.functions[static_cast<size_t>(Wasm::ImageCodeFlavor::Trapping)];
    auto& checking = units.functions[static_cast<size_t>(Wasm::ImageCodeFlavor::Checking)];
    for (uint32_t function = 0; function < module.functions.size(); ++function)
        trapping.append(units.append(module.functions[function].code, Wasm::ImageCodeFlavor::Trapping));
    for (uint32_t function = 0; function < module.functions.size(); ++function) {
        if (functionsWithTwoCopies.get(function))
            checking.append(units.append(codeOf(module, function, Wasm::ImageCodeFlavor::Checking).code, Wasm::ImageCodeFlavor::Checking));
        else
            checking.append(trapping[function]);
    }
    return units;
}

static std::optional<UnitTarget> resolve(const ModuleUnits& units, uint32_t unit, const Relocation& relocation)
{
    if (relocation.space != SymbolSpace::WebAssembly)
        return std::nullopt;
    if (relocation.symbol < Wasm::numberOfImageStubs)
        return UnitTarget { units.firstStub + relocation.symbol };

    uint32_t function = relocation.symbol - Wasm::numberOfImageStubs;
    std::optional<Wasm::ImageCodeFlavor> flavor = units.flavors[unit];
    if (!flavor || function >= units.functions[0].size())
        return std::nullopt;
    return UnitTarget { units.functions[static_cast<size_t>(*flavor)][function] };
}

static Wasm::ImageDeclineReason reportSectionLinkFailure(const SectionLinkFailure& failure, BuildContext& context)
{
    switch (failure.error) {
    case SectionLinkError::SectionTooLarge:
    case SectionLinkError::OutOfPCRelativeReach:
        return Wasm::ImageDeclineReason::CodeTooLarge;
    case SectionLinkError::SymbolWithoutUnit:
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = failure.relocation });
        return Wasm::ImageDeclineReason::FunctionNotCompiled;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

WasmModuleOrDecline linkWasmModule(WasmModuleCode&& module, const CodeTemplates& templates, BuildContext& context)
{
    BitVector functionsWithTwoCopies = findFunctionsWithTwoCopies(module);
    ModuleUnits units = collectUnits(module, functionsWithTwoCopies);
    auto resolveUnit = [&](uint32_t unit, const Relocation& relocation) {
        return resolve(units, unit, relocation);
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
    result.catchRunway = offsets[units.firstStub + static_cast<uint32_t>(Wasm::ImageStub::CatchRunway)];
    result.importStubs.append(offsets.span().subspan(units.firstImportStub, module.importStubs.size()));
    result.javaScriptImportStubs.append(offsets.span().subspan(units.firstJavaScriptImportStub, module.javaScriptImportStubs.size()));

    result.jsEntrypoints.reserveInitialCapacity(module.jsEntrypoints.size());
    for (uint32_t index = 0; index < module.jsEntrypoints.size(); ++index)
        result.jsEntrypoints.append(Wasm::JSEntrypointInImage { module.jsEntrypoints[index].type, offsets[units.firstJSEntrypoint + index] });

    auto describe = [&](uint32_t index, Wasm::ImageCodeFlavor flavor) {
        const WasmFunctionCode& function = codeOf(module, index, flavor);
        uint32_t entry = offsets[units.functions[static_cast<size_t>(flavor)][index]];
        uint32_t firstHandler = static_cast<uint32_t>(result.handlers.size());
        for (Wasm::HandlerInImage handler : function.handlers) {
            if (static_cast<Wasm::HandlerType>(handler.type) != Wasm::HandlerType::Delegate)
                handler.code += entry;
            result.handlers.append(handler);
        }
        return Wasm::FunctionInImage { entry, static_cast<uint32_t>(function.code.bytes.size()), firstHandler, static_cast<uint32_t>(function.handlers.size()), function.savedRegistersIndex, function.usesVectors, 0 };
    };
    auto& trapping = result.functions[static_cast<size_t>(Wasm::ImageCodeFlavor::Trapping)];
    auto& checking = result.functions[static_cast<size_t>(Wasm::ImageCodeFlavor::Checking)];
    trapping.reserveInitialCapacity(module.functions.size());
    for (uint32_t index = 0; index < module.functions.size(); ++index) {
        trapping.append(describe(index, Wasm::ImageCodeFlavor::Trapping));
        result.usesVectors |= module.functions[index].usesVectors;
    }
    if (!functionsWithTwoCopies.isEmpty()) {
        checking.reserveInitialCapacity(module.functions.size());
        for (uint32_t index = 0; index < module.functions.size(); ++index)
            checking.append(functionsWithTwoCopies.get(index) ? describe(index, Wasm::ImageCodeFlavor::Checking) : trapping[index]);
    }

    result.code = WTF::move(section->bytes);
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

    for (auto& functions : module.functions) {
        for (auto& function : functions)
            function.entry += base;
    }
    for (auto& handler : module.handlers) {
        if (static_cast<Wasm::HandlerType>(handler.type) != Wasm::HandlerType::Delegate)
            handler.code += base;
    }
    for (uint32_t& stub : module.importStubs)
        stub += base;
    for (uint32_t& stub : module.javaScriptImportStubs)
        stub += base;
    for (auto& entry : module.jsEntrypoints)
        entry.code += base;

    constexpr size_t trapping = static_cast<size_t>(Wasm::ImageCodeFlavor::Trapping);
    constexpr size_t checking = static_cast<size_t>(Wasm::ImageCodeFlavor::Checking);
    Wasm::ModuleInImage record { };
    record.digest = module.digest;
    record.sourceSize = module.sourceSize;
    record.usesVectors = module.usesVectors;
    record.numberOfFunctions = static_cast<uint32_t>(module.functions[trapping].size());
    record.numberOfImportedFunctions = static_cast<uint32_t>(module.importStubs.size());
    record.functions[trapping] = appendToDirectory(directory, asBytes(module.functions[trapping].span()));
    record.functions[checking] = module.functions[checking].isEmpty() ? record.functions[trapping] : appendToDirectory(directory, asBytes(module.functions[checking].span()));
    record.handlers = appendToDirectory(directory, asBytes(module.handlers.span()));
    record.importStubs = appendToDirectory(directory, asBytes(module.importStubs.span()));
    record.javaScriptImportStubs = appendToDirectory(directory, asBytes(module.javaScriptImportStubs.span()));
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
    uint64_t directorySize = m_directorySize + sizeof(Wasm::ModuleInImage) + arraySize(module.functions[0]) + arraySize(module.functions[1]) + arraySize(module.handlers)
        + arraySize(module.importStubs) + arraySize(module.javaScriptImportStubs) + arraySize(module.jsEntrypoints) + arraySize(module.savedRegisters);
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

static void countFunctions(const Wasm::ModuleForImage& module, Statistics& statistics)
{
    auto& trapping = module.functions[static_cast<size_t>(Wasm::ImageCodeFlavor::Trapping)];
    auto& checking = module.functions[static_cast<size_t>(Wasm::ImageCodeFlavor::Checking)];
    uint64_t functionsWithTwoCopies = 0;
    uint64_t secondCopyBytes = 0;
    for (size_t index = 0; index < checking.size(); ++index) {
        if (checking[index].entry == trapping[index].entry)
            continue;
        ++functionsWithTwoCopies;
        secondCopyBytes += checking[index].size;
    }
    statistics.count(StatisticID::WasmFunctionsCompiled, trapping.size());
    statistics.count(StatisticID::WasmFunctionsWithTwoCopies, functionsWithTwoCopies);
    statistics.count(StatisticID::WasmSecondCopyBytesEmitted, secondCopyBytes);
}

WebAssemblyImageInputs WasmSectionBuilder::finish(BuildContext& context)
{
    if (m_modules.isEmpty() && m_declinedModules.isEmpty())
        return { };

    WebAssemblyImageInputs section;
    if (!m_modules.isEmpty())
        section.codeAlignment = codeAlignment;
    section.numberOfModules = static_cast<uint32_t>(m_modules.size());
    section.code.reserveInitialCapacity(m_codeSize);
    section.directory.reserveInitialCapacity(m_directorySize);

    Wasm::DirectoryInImage header { section.numberOfModules, static_cast<uint32_t>(m_declinedModules.size()), 0, 0 };
    if (!m_modules.isEmpty())
        header.catchRunway = m_modules[0].base + m_modules[0].module.catchRunway;
    appendToDirectory(section.directory, asByteSpan(header));
    size_t recordsOffset = section.directory.size();
    section.directory.insertFill(recordsOffset, 0, m_modules.size() * sizeof(Wasm::ModuleInImage));
    appendToDirectory(section.directory, asBytes(m_declinedModules.span()));
    for (size_t index = 0; index < m_modules.size(); ++index) {
        countFunctions(m_modules[index].module, context.statistics());
        Wasm::ModuleInImage record = appendModule(section.code, section.directory, m_modules[index].module, m_modules[index].base);
        memcpySpan(section.directory.mutableSpan().subspan(recordsOffset + index * sizeof(record)), asByteSpan(record));
    }

    RELEASE_ASSERT(section.code.size() == m_codeSize);
    RELEASE_ASSERT(section.directory.size() == m_directorySize);
    context.statistics().count(StatisticID::WasmCodeBytesEmitted, section.code.size());
    return section;
}

namespace {

struct KnownModule {
    std::array<uint8_t, 20> digest;
    size_t size;
    Wasm::ImageDeclineReason declineReason;
};

} // anonymous namespace

static LimitID limitOf(Wasm::ImageDeclineReason reason)
{
    return reason == Wasm::ImageDeclineReason::CodeTooLarge ? LimitID::WasmImageCodeSize : LimitID::None;
}

static std::span<const uint8_t> base64OfEmbeddedModule(std::span<const uint8_t> text)
{
    auto modulePrefix = "AGFzbQEAAA"_span;
    auto base64Marker = "base64,"_span;
    constexpr size_t maxPrefixLength = 64;
    if (spanHasPrefix(text, modulePrefix))
        return text;
    if (text.empty() || text[0] != 'd')
        return { };
    size_t at = WTF::find(text.first(std::min(text.size(), maxPrefixLength)), base64Marker);
    if (at == notFound)
        return { };
    text = text.subspan(at + base64Marker.size());
    if (!spanHasPrefix(text, modulePrefix))
        return { };
    return text;
}

static bool isHexOfEmbeddedModule(std::span<const uint8_t> text)
{
    return !(text.size() % 2) && (spanHasPrefix(text, "0061736d01000000"_span) || spanHasPrefix(text, "0061736D01000000"_span));
}

static std::optional<Vector<uint8_t>> decodeEmbeddedModule(std::span<const uint8_t> text)
{
    if (std::span<const uint8_t> base64 = base64OfEmbeddedModule(text); !base64.empty()) {
        std::optional<Vector<uint8_t>> bytes = base64Decode(base64, { Base64DecodeOption::IgnoreWhitespace });
        return bytes ? bytes : base64Decode(base64, { Base64DecodeOption::URL, Base64DecodeOption::IgnoreWhitespace });
    }
    if (!isHexOfEmbeddedModule(text))
        return std::nullopt;
    Vector<uint8_t> bytes(text.size() / 2);
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (!isASCIIHexDigit(text[2 * index]) || !isASCIIHexDigit(text[2 * index + 1]))
            return std::nullopt;
        bytes[index] = toASCIIHexValue(text[2 * index], text[2 * index + 1]);
    }
    return bytes;
}

namespace {

struct WasmModuleSet final : EarlyWasmModules {
    struct Mention {
        size_t module;
        SourceLocation location;
        bool isBase64;
    };

    ~WasmModuleSet() final
    {
        JSLockHolder locker(*vm);
        pendingModules.clear();
        vm = nullptr;
    }

    void add(std::span<const uint8_t> bytes, SourceLocation location, bool isBase64)
    {
        auto digest = Wasm::CodeInImage::digestOf(bytes);
        size_t index = modules.findIf([&](auto& known) {
            return known.digest == digest && known.size == bytes.size();
        });
        if (index == notFound) {
            index = modules.size();
            auto module = prepareWasmModule(*vm, bytes, location);
            modules.append(KnownModule { digest, bytes.size(), module ? Wasm::ImageDeclineReason::None : module.error() });
            pendingModules.append(module ? WTF::move(*module) : nullptr);
            finishedModules.append(std::nullopt);
        }
        mentions.append(Mention { index, location, isBase64 });
    }

    void compileAddedModules(const CodeTemplates& templates, BuildContext& context)
    {
        compileFunctions(pendingModules.span().subspan(numberCompiled), context);
        for (size_t index = numberCompiled; index < pendingModules.size(); ++index) {
            std::unique_ptr<PendingWasmModule> pending = WTF::move(pendingModules[index]);
            if (!pending)
                continue;
            WasmModuleOrDecline module = finishWasmModule(*pending, templates, context);
            if (module)
                finishedModules[index] = WTF::move(*module);
            else
                modules[index].declineReason = module.error();
        }
        numberCompiled = pendingModules.size();
    }

    RefPtr<VM> vm { VM::create(HeapType::Small) };
    Vector<KnownModule> modules;
    Vector<std::unique_ptr<PendingWasmModule>> pendingModules;
    Vector<std::optional<Wasm::ModuleForImage>> finishedModules;
    Vector<Mention> mentions;
    size_t numberCompiled { 0 };
};

}

static std::unique_ptr<WasmModuleSet> compileFileModules(const Typed::Program& program, const CodeTemplates& templates, BuildContext& context)
{
    auto set = makeUniqueWithoutFastMallocCheck<WasmModuleSet>();
    JSLockHolder locker(*set->vm);
    for (auto& entry : program.webAssemblyModules)
        set->add(entry.bytes, SourceLocation { entry.module.index(), 0 }, false);
    set->compileAddedModules(templates, context);
    return set;
}

std::unique_ptr<EarlyWasmModules> compileWebAssemblyFiles(const Typed::Program& program, BuildContext& context)
{
    if (program.webAssemblyModules.isEmpty())
        return nullptr;
    auto templates = CodeTemplates::create(context.assemblerPolicy(), &context);
    if (!templates)
        return nullptr;
    return compileFileModules(program, *templates, context);
}

WebAssemblyImageInputs compileWebAssemblyModules(const Typed::Program& program, const CodeTemplates& templates, BuildContext& context, const BitVector* stringsInImage, std::unique_ptr<EarlyWasmModules> early, const LinkedFunctions* functions)
{
    if (!mayHaveWebAssemblyModules(program))
        return { };
    std::unique_ptr<WasmModuleSet> set { static_cast<WasmModuleSet*>(early.release()) };
    if (!set)
        set = compileFileModules(program, templates, context);
    JSLockHolder locker(*set->vm);

    for (size_t index = 0; index < program.strings.size(); ++index) {
        if (stringsInImage && !stringsInImage->get(index))
            continue;
        std::optional<Vector<uint8_t>> bytes = decodeEmbeddedModule(program.strings[Typed::StringID(index)]);
        if (bytes)
            set->add(bytes->span(), noSourceLocation, true);
    }
    constexpr std::array<uint8_t, 8> moduleHeader { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
    for (size_t position = 0; functions && position < functions->size(); ++position) {
        for (const ConstantData& constant : (*functions)[position].body.rareLists->constants) {
            if (constant.kind == ConstantKind::ByteLiteral && spanHasPrefix(constant.bytes.span(), std::span<const uint8_t>(moduleHeader)))
                set->add(constant.bytes.span(), noSourceLocation, true);
        }
    }
    set->compileAddedModules(templates, context);

    WasmSectionBuilder builder;
    for (size_t index = 0; index < set->modules.size(); ++index) {
        KnownModule& known = set->modules[index];
        if (std::optional<Wasm::ModuleForImage>& module = set->finishedModules[index]) {
            if (builder.append(WTF::move(*module)))
                continue;
            known.declineReason = Wasm::ImageDeclineReason::CodeTooLarge;
        }
        builder.appendDeclined({ known.digest, static_cast<uint32_t>(known.size) });
    }
    for (const WasmModuleSet::Mention& mention : set->mentions) {
        Wasm::ImageDeclineReason reason = set->modules[mention.module].declineReason;
        if (reason == Wasm::ImageDeclineReason::None || reason == Wasm::ImageDeclineReason::FunctionNotCompiled)
            continue;
        if (!mention.isBase64 || reason != Wasm::ImageDeclineReason::InvalidModule)
            reportDecline(context, mention.location, diagnosticKindOf(reason), limitOf(reason), 1);
    }
    return builder.finish(context);
}

bool mayHaveWebAssemblyModules(const Typed::Program& program)
{
    if (!program.webAssemblyModules.isEmpty())
        return true;
    for (size_t index = 0; index < program.strings.size(); ++index) {
        if (!base64OfEmbeddedModule(program.strings[Typed::StringID(index)]).empty() || isHexOfEmbeddedModule(program.strings[Typed::StringID(index)]))
            return true;
    }
    return false;
}

#else

std::unique_ptr<EarlyWasmModules> compileWebAssemblyFiles(const Typed::Program&, BuildContext&)
{
    return nullptr;
}

WebAssemblyImageInputs compileWebAssemblyModules(const Typed::Program& program, const CodeTemplates&, BuildContext& context, const BitVector*, std::unique_ptr<EarlyWasmModules>, const LinkedFunctions*)
{
#if ENABLE(WEBASSEMBLY)
    constexpr UserDiagnosticKind reason = UserDiagnosticKind::WasmUnsupportedCPU;
#else
    constexpr UserDiagnosticKind reason = UserDiagnosticKind::WasmDisabledInBuild;
#endif
    for (auto& entry : program.webAssemblyModules)
        reportDecline(context, SourceLocation { entry.module.index(), 0 }, reason, LimitID::None, 1);
    return { };
}

bool mayHaveWebAssemblyModules(const Typed::Program& program)
{
    return !program.webAssemblyModules.isEmpty();
}

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
