/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModules.h"
#include "AOTVector.h"
#include "Identifier.h"
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Variant.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC { namespace AOT {

struct VariableBinding {
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

// Ordered by the hashes of the names, not by their addresses: whoever walks the candidates and reads deferred exports runs their getters in the same order in every process.
using StarCandidates = UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, StarCandidate, IdentifierRepHash>;

class StarExportResolver {
    WTF_MAKE_NONCOPYABLE(StarExportResolver);
public:
    explicit StarExportResolver(Modules& modules)
        : m_modules(modules)
    {
    }

    // Null while an exception is pending. Valid until the next call.
    const StarCandidates* candidatesOf(ModuleID);
    const StarCandidate* find(ModuleID exporter, UniquedStringImpl* name);
    // Throws for the first name that an unlinked module of the list requires and that its exporter lacks or has from two bindings.
    void checkRequiredNames(std::span<const ModuleID> modules);
    // Reads a deferred export now. To find a candidate reads none.
    JSValue capturedValueOf(const StarCandidate&);
    StarBinding bindingOf(const ModuleExportData&);

    // What a table of exports holds under the name. In the captured exports of a module the table itself stands for a deferred export: Modules::capturedValue() reads that one.
    static JSValue storedValue(JSGlobalObject*, JSObject* table, const Identifier& name);
    // Hands over no value for a deferred export.
    static void forEachCapturedExport(Modules&, ModuleID, const ScopedLambda<void(const Identifier&, JSValue)>&);

private:
    using CandidatesByModule = UncheckedKeyHashMap<ModuleID, StarCandidates>;

    enum class ExportedNames : uint8_t { All, WithoutDefault };

    AbstractModuleRecord::Resolution::Type resolve(ModuleID exporter, const Identifier& name);
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
