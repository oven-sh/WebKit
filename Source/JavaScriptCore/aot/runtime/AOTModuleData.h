/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTSlotRepresentation.h"
#include <optional>
#include <stdint.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

template<typename ID>
constexpr std::optional<ID> optionalIDInImage(uint32_t word)
{
    return word == UINT32_MAX ? std::nullopt : std::optional<ID>(ID(word));
}

struct ModuleData {
    enum class Flag : uint16_t {
        HasTopLevelAwait = 1 << 0,
        IsSynchronous = 1 << 1,
        IsSimple = 1 << 2,
        NeedsLink = 1 << 3,
        ReachesUncompiledCode = 1 << 4,
        HasDynamicStarExport = 1 << 5,
        IsEntryPoint = 1 << 6,
        IsEvaluatedAtMostOnce = 1 << 7,
        HasDeferredRequest = 1 << 8,
    };

    uint32_t publicPath;
    uint32_t displayPath;
    ModuleKind kind;
    ExternalKind externalKind;
    SyntheticExportRule exportRule;
    RequireResult requireResult;
    uint16_t flags;
    uint8_t unused[2];
    uint32_t body;
    uint32_t referrer;
    uint32_t runtimeModule;
    SlotData value;
    uint32_t firstRequest;
    uint32_t firstExport;
    uint32_t firstSyntheticExport;
    uint32_t firstRequiredName;

    bool has(Flag flag) const { return OptionSet<Flag>::fromRaw(flags).contains(flag); }
    void add(Flag flag) { flags |= static_cast<uint16_t>(flag); }
    bool hasTopLevelAwait() const { return has(Flag::HasTopLevelAwait); }
    bool isSynchronous() const { return has(Flag::IsSynchronous); }
    bool isSimple() const { return has(Flag::IsSimple); }
    bool needsLink() const { return has(Flag::NeedsLink); }
    bool reachesUncompiledCode() const { return has(Flag::ReachesUncompiledCode); }
    bool hasDynamicStarExport() const { return has(Flag::HasDynamicStarExport); }
    bool isEntryPoint() const { return has(Flag::IsEntryPoint); }
    bool isEvaluatedAtMostOnce() const { return has(Flag::IsEvaluatedAtMostOnce); }
    bool hasDeferredRequest() const { return has(Flag::HasDeferredRequest); }

    bool hasExportTable() const;
    bool isUncompiledCode() const { return kind == ModuleKind::External && externalKind == ExternalKind::UncompiledCode; }
    bool isHostValue() const { return kind == ModuleKind::External && externalKind == ExternalKind::HostValue; }
    bool capturesExports() const;

    std::optional<FunctionID> bodyFunction() const { return optionalIDInImage<FunctionID>(body); }
    std::optional<ModuleID> referrerModule() const { return optionalIDInImage<ModuleID>(referrer); }
    std::optional<uint32_t> registryID() const { return optionalIDInImage<uint32_t>(runtimeModule); }
};

inline bool ModuleData::hasExportTable() const
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return true;
    case ModuleKind::CommonJs:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline bool ModuleData::capturesExports() const
{
    switch (exportRule) {
    case SyntheticExportRule::CommonJs:
    case SyntheticExportRule::CommonJsIgnoringAnnotation:
    case SyntheticExportRule::RuntimeModule:
        return true;
    case SyntheticExportRule::None:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct ModuleRequestData {
    uint32_t target : 30;
    uint32_t isDeferred : 1;
    uint32_t isDynamicStarExport : 1;

    ModuleID targetModule() const { return ModuleID(static_cast<uint32_t>(target)); }
};

struct ModuleExportData {
    uint32_t name : 24;
    uint32_t kindBits : 6;
    uint32_t isDeferred : 1;
    uint32_t isFromStarExport : 1;
    union {
        SlotData variable;
        uint32_t module;
        uint32_t indirectExport;
    };

    ModuleExportKind kind() const { return static_cast<ModuleExportKind>(kindBits); }
    ModuleID targetModule() const { return ModuleID(module); }
};

struct IndirectExportData {
    uint32_t module;
    uint32_t importName;

    ModuleID targetModule() const { return ModuleID(module); }
};

struct SyntheticExportData {
    uint32_t name : 24;
    uint32_t absentIsUndefined : 1;
    uint32_t hasVariable : 1;
    uint32_t unused : 6;
    SlotData variable;
};

struct RequiredNameData {
    uint32_t exporter : 31;
    uint32_t mayBeAbsent : 1;
    uint32_t name;

    ModuleID exporterModule() const { return ModuleID(static_cast<uint32_t>(exporter)); }
};

struct RequireEdgeData {
    uint32_t referrer;
    uint32_t target;
    uint32_t specifier;

    ModuleID referrerModule() const { return ModuleID(referrer); }
    ModuleID targetModule() const { return ModuleID(target); }
    std::optional<StringID> specifierString() const { return optionalIDInImage<StringID>(specifier); }
};

struct ModuleResetSlotData {
    uint32_t module;
    SlotData slot;
};

struct ModuleFailureData {
    uint32_t importer;
    uint32_t specifier;

    ModuleID importerModule() const { return ModuleID(importer); }
    StringID specifierString() const { return StringID(specifier); }
};

struct CompiledRuntimeModuleData {
    uint32_t registryID;
    uint32_t module;
};

struct CompiledRuntimeFunctionData {
    uint32_t id;
    uint32_t function;
    uint32_t visibility;
};

static_assert(sizeof(ModuleData) == 48 && isImageRecord<ModuleData>);
static_assert(sizeof(ModuleRequestData) == 4 && isImageRecord<ModuleRequestData>);
static_assert(sizeof(ModuleExportData) == 8 && isImageRecord<ModuleExportData>);
static_assert(sizeof(IndirectExportData) == 8 && isImageRecord<IndirectExportData>);
static_assert(sizeof(SyntheticExportData) == 8 && isImageRecord<SyntheticExportData>);
static_assert(sizeof(RequiredNameData) == 8 && isImageRecord<RequiredNameData>);
static_assert(sizeof(RequireEdgeData) == 12 && isImageRecord<RequireEdgeData>);
static_assert(sizeof(ModuleResetSlotData) == 8 && isImageRecord<ModuleResetSlotData>);
static_assert(sizeof(ModuleFailureData) == 8 && isImageRecord<ModuleFailureData>);
static_assert(sizeof(CompiledRuntimeModuleData) == 8 && isImageRecord<CompiledRuntimeModuleData>);
static_assert(sizeof(CompiledRuntimeFunctionData) == 12 && isImageRecord<CompiledRuntimeFunctionData>);
static_assert(sizeof(ModuleData::Flag) == sizeof(ModuleData::flags));
static_assert(sizeof(ModuleKind) == 1 && sizeof(ExternalKind) == 1 && sizeof(SyntheticExportRule) == 1 && sizeof(RequireResult) == 1);

#define FOR_EACH_AOT_MODULE_PROGRAM_TABLE(v) \
    v(modules, ModuleData) \
    v(moduleRequests, ModuleRequestData) \
    v(moduleExports, ModuleExportData) \
    v(indirectExports, IndirectExportData) \
    v(syntheticExports, SyntheticExportData) \
    v(requiredNames, RequiredNameData) \
    v(requireEdges, RequireEdgeData) \
    v(moduleResetSlots, ModuleResetSlotData) \
    v(moduleFailures, ModuleFailureData) \
    v(modulesByPath, uint32_t) \
    v(compiledRuntimeModules, CompiledRuntimeModuleData) \
    v(compiledRuntimeFunctions, CompiledRuntimeFunctionData)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
