/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWasmSection.h"

#if ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTBuildContext.h"
#include "AOTSectionLinker.h"
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static constexpr uint32_t codeUnitAlignment = 16;
static constexpr uint32_t jumpTableAlignment = sizeof(int32_t);
static constexpr uint32_t throwStubUnit = 0;
static constexpr size_t directoryAlignment = 8;
static constexpr uint64_t maxSectionOffset = std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), defaultLimitValue(LimitID::VectorBytes, hostArchitecture()));

static_assert(defaultLimitValue(LimitID::WasmImageCodeSize, hostArchitecture()) <= std::numeric_limits<uint32_t>::max());
static_assert(WTF::isValidCapacityForVector<uint8_t>(maxSectionOffset));
static_assert(!(sizeof(Wasm::DirectoryInImage) % directoryAlignment));
static_assert(!(sizeof(Wasm::ModuleInImage) % directoryAlignment));
static_assert(!(sizeof(Wasm::DeclinedModuleInImage) % directoryAlignment));
static_assert(alignof(Wasm::SavedRegistersInImage) == directoryAlignment);

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

static Vector<RelocatableCode> encodeJumpTables(const WasmModuleCode& module)
{
    size_t numberOfJumpTables = 0;
    for (const WasmFunctionCode& function : module.functions)
        numberOfJumpTables += function.jumpTables.size();

    Vector<RelocatableCode> encoded;
    encoded.reserveInitialCapacity(numberOfJumpTables);
    for (const WasmFunctionCode& function : module.functions) {
        for (const JumpTable& table : function.jumpTables)
            encoded.append(encodeJumpTable(table));
    }
    return encoded;
}

static uint32_t appendUnit(ModuleUnits& units, const LinkUnit& unit, UnitOrigin origin)
{
    uint32_t index = static_cast<uint32_t>(units.units.size());
    units.units.append(unit);
    units.origins.append(origin);
    return index;
}

static uint32_t appendStub(ModuleUnits& units, const Vector<uint8_t>& bytes)
{
    return appendUnit(units, { bytes.span(), { }, codeUnitAlignment, UnitSharing::Unique }, { UnitKind::Stub, 0, 0 });
}

static void appendDataUnits(ModuleUnits& units, const WasmModuleCode& module)
{
    units.firstDataUnit = static_cast<uint32_t>(units.units.size());
    units.jumpTables = encodeJumpTables(module);
    size_t nextJumpTable = 0;
    for (uint32_t functionIndex = 0; functionIndex < module.functions.size(); ++functionIndex) {
        const WasmFunctionCode& function = module.functions[functionIndex];
        FunctionUnits& functionUnits = units.functions[functionIndex];

        functionUnits.firstConstant = static_cast<uint32_t>(units.units.size());
        for (uint32_t index = 0; index < function.constants.size(); ++index) {
            const ConstantData& constant = function.constants[index];
            appendUnit(units, { constant.bytes.span(), { }, constant.alignment, UnitSharing::Unique }, { UnitKind::Constant, functionIndex, index });
        }

        functionUnits.firstJumpTable = static_cast<uint32_t>(units.units.size());
        for (uint32_t index = 0; index < function.jumpTables.size(); ++index) {
            const RelocatableCode& table = units.jumpTables[nextJumpTable++];
            appendUnit(units, { table.bytes.span(), table.relocations.span(), jumpTableAlignment, UnitSharing::Unique }, { UnitKind::JumpTable, functionIndex, index });
        }
    }
}

static ModuleUnits collectUnits(const WasmModuleCode& module)
{
    ModuleUnits units;
    uint32_t throwStub = appendStub(units, module.throwStub);
    RELEASE_ASSERT(throwStub == throwStubUnit);

    units.firstImportStub = static_cast<uint32_t>(units.units.size());
    for (const Vector<uint8_t>& stub : module.importStubs)
        appendStub(units, stub);

    units.firstJSEntrypoint = static_cast<uint32_t>(units.units.size());
    for (const WasmJSEntrypointCode& entrypoint : module.jsEntrypoints)
        appendStub(units, entrypoint.bytes);

    units.functions.reserveInitialCapacity(module.functions.size());
    for (uint32_t functionIndex = 0; functionIndex < module.functions.size(); ++functionIndex) {
        const RelocatableCode& code = module.functions[functionIndex].code;
        uint32_t unit = appendUnit(units, { code.bytes.span(), code.relocations.span(), codeUnitAlignment, UnitSharing::Unique }, { UnitKind::Function, functionIndex, 0 });
        units.functions.append(FunctionUnits { unit, 0, 0 });
    }

    appendDataUnits(units, module);
    return units;
}

static uint32_t unitOfCallTarget(const ModuleUnits& units, const Wasm::ImageCallTarget& target)
{
    switch (target.kind) {
    case Wasm::ImageCallTarget::Kind::ThrowStub:
        return throwStubUnit;
    case Wasm::ImageCallTarget::Kind::Function:
        return units.functions[target.function].code;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<UnitTarget> resolveInFunction(const WasmFunctionCode& function, const ModuleUnits& units, const FunctionUnits& functionUnits, const Relocation& relocation)
{
    switch (relocation.space) {
    case SymbolSpace::WebAssembly:
        return UnitTarget { unitOfCallTarget(units, function.callTargets[relocation.symbol]) };
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

static Wasm::ModuleForImage createModuleForImage(WasmModuleCode&& module, const ModuleUnits& units, LinkedSection&& section)
{
    RELEASE_ASSERT(section.bytes.size() <= maxSectionOffset);
    const Vector<uint32_t>& offsets = section.uniqueUnitOffsets;

    Wasm::ModuleForImage result;
    result.digest = module.digest;
    result.sourceSize = module.sourceSize;
    result.isForSignalingMemory = module.isForSignalingMemory;
    result.functionsEnd = units.firstDataUnit < units.units.size() ? offsets[units.firstDataUnit] : static_cast<uint32_t>(section.bytes.size());

    result.importStubs.reserveInitialCapacity(module.importStubs.size());
    for (uint32_t index = 0; index < module.importStubs.size(); ++index)
        result.importStubs.append(offsets[units.firstImportStub + index]);

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

    result.code = static_cast<WTF::Vector<uint8_t>&&>(WTF::move(section.bytes));
    result.savedRegisters = WTF::move(module.savedRegisters);
    return result;
}

std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason> linkWasmModule(WasmModuleCode&& module, const CodeTemplates& templates, BuildContext& context)
{
    ModuleUnits units = collectUnits(module);
    auto resolveUnit = [&](uint32_t unit, const Relocation& relocation) {
        return resolve(module, units, unit, relocation);
    };
    UnitResolver resolver = resolveUnit;
    auto section = linkSection({ units.units.span(), resolver, templates, context.limit(LimitID::WasmImageCodeSize) });
    if (!section)
        return makeUnexpected(reportSectionLinkFailure(section.error(), context));
    return createModuleForImage(WTF::move(module), units, WTF::move(*section));
}

static uint64_t directorySizeOf(const Wasm::ModuleForImage& module)
{
    auto arraySize = [](const auto& array) -> uint64_t {
        return WTF::roundUpToMultipleOf<directoryAlignment>(array.sizeInBytes());
    };
    return sizeof(Wasm::ModuleInImage) + arraySize(module.functions) + arraySize(module.importStubs) + arraySize(module.jsEntrypoints) + arraySize(module.savedRegisters);
}

static void padToCodeAlignment(Vector<uint8_t>& code)
{
    code.insertFill(code.size(), isX86() ? 0xcc : 0, WTF::roundUpToMultipleOf<WasmSectionBuilder::codeAlignment>(code.size()) - code.size());
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
    padToCodeAlignment(code);
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
    uint64_t base = WTF::roundUpToMultipleOf<codeAlignment>(m_codeSize);
    uint64_t codeSize = base + module.code.size();
    uint64_t directorySize = m_directorySize + directorySizeOf(module);
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

    m_modules.clear();
    m_declinedModules.clear();
    m_codeSize = 0;
    m_directorySize = sizeof(Wasm::DirectoryInImage);
    return section;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)
