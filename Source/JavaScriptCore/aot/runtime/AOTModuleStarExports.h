/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModules.h"
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC { namespace AOT {

struct VariableBinding {
    VariableBinding() = default;
    explicit VariableBinding(SlotData variable)
        : variable(variable)
    {
    }

    // The offset alone does not identify a variable: it indexes the statics or, for SlotRepresentation::DeferredClosure, the closure table.
    SlotData variable { };

    friend bool operator==(const VariableBinding&, const VariableBinding&) = default;
};

struct CapturedBinding {
    ModuleID module;
    UniquedStringImpl* name { nullptr };

    friend bool operator==(const CapturedBinding&, const CapturedBinding&) = default;
};

struct NamespaceBinding {
    ModuleID module;
    AbstractModuleRecord::ModulePhase phase { AbstractModuleRecord::ModulePhase::Evaluation };

    friend bool operator==(const NamespaceBinding&, const NamespaceBinding&) = default;
};

struct UncompiledBinding {
    AbstractModuleRecord* record { nullptr };
    UniquedStringImpl* localName { nullptr };

    friend bool operator==(const UncompiledBinding&, const UncompiledBinding&) = default;
};

using StarBinding = Variant<VariableBinding, CapturedBinding, NamespaceBinding, UncompiledBinding>;

enum class OwnExportEntry : uint8_t { None, Local, FromStarExport };

struct StarCandidate {
    StarBinding binding;
    JSValue capturedValue;
    JSObject* liveSource { nullptr };
    bool isAmbiguous { false };
    OwnExportEntry ownEntry { OwnExportEntry::None };
};

using StarCandidates = UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, StarCandidate>;

enum class NameResolution : uint8_t { NotFound, Found, Ambiguous };

struct UnresolvedName {
    ModuleID exporter;
    uint32_t name;
    NameResolution resolution;
};

class StarExportResolver {
    WTF_MAKE_NONCOPYABLE(StarExportResolver);
public:
    explicit StarExportResolver(Modules&);

    // Null while an exception is pending. Valid until the next call.
    const StarCandidates* candidatesOf(ModuleID);
    const StarCandidate* find(ModuleID exporter, UniquedStringImpl* name);
    NameResolution resolve(ModuleID exporter, uint32_t nameIdentifier);
    std::optional<UnresolvedName> findUnresolvedName(std::span<const ModuleID> modules);
    JSValue capturedValueOf(const StarCandidate&);
    StarBinding bindingOf(const ModuleExportData&);

    static JSValue capturedValue(JSGlobalObject*, JSObject* captured, const Identifier& name);
    static void forEachCapturedExport(JSGlobalObject*, JSObject* captured, const ScopedLambda<void(const Identifier&, JSValue)>&);

private:
    using CandidatesByModule = UncheckedKeyHashMap<ModuleID, StarCandidates>;

    enum class ExportedNames : uint8_t { All, WithoutDefault };

    void solve(ModuleID root);
    bool collectUnsolvedModules(ModuleID root, Vector<ModuleID, 8>& postOrder);
    void addOwnCandidates(ModuleID);
    void addSourceCandidates(ModuleID source, StarCandidates&, ExportedNames);
    void addCapturedCandidates(ModuleID source, StarCandidates&, ExportedNames);
    void addUncompiledCandidates(ModuleID source, StarCandidates&, ExportedNames);
    bool addCandidatesOfStarTargets(ModuleID);
    bool join(StarCandidates&, UniquedStringImpl* name, const StarCandidate&);
    StarBinding bindingOf(const AbstractModuleRecord::Resolution&);
    StarBinding bindingOfCapturedName(ModuleID source, UniquedStringImpl* name);
    ModuleID representativeOf(ModuleID);
    bool isSolvedFromTable(ModuleID) const;

    Modules& m_modules;
    CandidatesByModule m_candidates;
    UncheckedKeyHashMap<JSObject*, ModuleID> m_hostValueModules;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
