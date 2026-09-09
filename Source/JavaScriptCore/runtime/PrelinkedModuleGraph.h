/*
 * Copyright (C) 2026 Anthropic PBC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "Identifier.h"
#include "ScriptFetchParameters.h"
#include <bit>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>

namespace JSC {

class DecoderStringTable;
class VM;

// A module graph the embedder resolved ahead of time (a bundle in an executable): every module's requests, import and
// export entries as flat index-keyed arrays borrowed from the embedder's payload, with each import already resolved to
// (module index, binding). Names are indices ("sids") into a string table shared with the bytecode cache, atomized only
// when a name has to become an Identifier. One instance per VM; module records made from it
// (AbstractModuleRecord::isPrelinked(), behind Options::usePrelinkedModuleInfo()) answer resolveImport / resolveExport /
// initializeEnvironment / getModuleNamespace from these arrays and only build their by-name entry maps when something
// else asks for them. The graph object itself is plain data and exists whether or not the option is on.
//
// Blob layout (version 3; written by Bun's src/bundler/prelinked_module_graph.rs `serialize`, bump both together),
// little-endian u32 throughout, 4-byte aligned:
//   Header, then the arrays it points at. Module-relative request indices (< 2^16); graph-wide module indices
//   (noModule = none); sids < stringCount, or starDefaultSid / starNamespaceSid. Imports are sorted by
//   (localHash, localSid), exports by (exportHash, exportSid); hash = StringImpl::hash() of the name (24 bits), and an
//   entry whose name is a sentinel carries hash 0xffffffff so it sorts last. Each module with imports also has an
//   open-addressed index over them: importIndexSlotCount(importCount) little-endian u16 slots at importIndexOffset (a
//   byte offset into the import-index region), slot = localHash & (slotCount - 1) then linear probing, value = the
//   import's position within the module's imports + 1, 0 = empty.
class PrelinkedModuleGraph final : public RefCounted<PrelinkedModuleGraph> {
    WTF_MAKE_NONCOPYABLE(PrelinkedModuleGraph);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PrelinkedModuleGraph);
public:
    static constexpr uint32_t magic = 0x474d4c50; // "PLMG"
    static constexpr uint32_t currentVersion = 3;
    static constexpr uint32_t noModule = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t starDefaultSid = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t starNamespaceSid = std::numeric_limits<uint32_t>::max() - 1;
    static constexpr uint32_t sentinelHash = std::numeric_limits<uint32_t>::max();

    struct Header {
        uint32_t magic;
        uint32_t version;
        uint32_t stringCount;
        uint32_t moduleCount;
        uint32_t requestCount;
        uint32_t importCount;
        uint32_t exportCount;
        uint32_t starExportCount;
        uint32_t modulesOffset;
        uint32_t requestsOffset;
        uint32_t importsOffset;
        uint32_t exportsOffset;
        uint32_t starExportsOffset;
        uint32_t importIndexOffset;
        uint32_t importIndexBytes;
        uint32_t reserved;
    };
    // Slots of a module's import index: none without imports, else the power of two >= 2 * importCount (load <= 1/2).
    static uint32_t importIndexSlotCount(uint32_t importCount) { return importCount ? std::bit_ceil(2 * importCount) : 0; }

    struct Module {
        enum Flags : uint32_t {
            HasImportMeta = 1 << 0,
            IsTypeScript = 1 << 1,
            HasTLA = 1 << 2,
            HasStarExports = 1 << 3,
            AllRequestsInGraph = 1 << 4, // every request resolves to another module of this graph
        };
        uint32_t keySid;
        uint32_t flags;
        uint32_t firstRequest;
        uint32_t requestCount;
        uint32_t firstImport;
        uint32_t importCount;
        uint32_t firstExport;
        uint32_t exportCount;
        uint32_t firstStarExport;
        uint32_t starExportCount;
        uint32_t importIndexOffset; // bytes into the import-index region
    };

    enum class FetchKind : uint8_t { None, JavaScript, WebAssembly, JSON, HostDefined };
    struct Request {
        static constexpr uint32_t phaseDeferBit = 1 << 3;
        uint32_t specifierSid;
        uint32_t moduleIndex; // noModule: not part of the graph (a builtin, an external file, a non-JavaScript import)
        uint32_t attributes; // FetchKind | phaseDeferBit
        uint32_t hostDefinedTypeSid;
        FetchKind fetchKind() const { return static_cast<FetchKind>(attributes & 7); }
        bool isDeferred() const { return attributes & phaseDeferBit; }
        ScriptFetchParameters::Type type() const;
    };

    enum class ImportKind : uint8_t { Single, SingleTypeScript, Namespace, NamespaceDefer };
    // What the bundler proved ResolveExport returns. Unresolved: it could not (the target is outside the graph, behind a
    // star export it could not flatten, or on a re-export cycle) and the runtime resolves by name on the target.
    enum class ResolutionKind : uint8_t { Unresolved, Binding, Namespace, NotFound, Ambiguous, Error };
    // ImportKind or ExportKind | ResolutionKind << 8 | module-relative request index << 16.
    struct KindBits {
        uint32_t bits;
        uint8_t kind() const { return bits & 0xff; }
        ResolutionKind resolution() const { return static_cast<ResolutionKind>((bits >> 8) & 0xff); }
        uint32_t request() const { return bits >> 16; }
    };
    struct Import {
        uint32_t localSid;
        uint32_t importNameSid; // starNamespaceSid for namespace imports
        KindBits bits;
        uint32_t localHash;
        uint32_t resolvedModule;
        uint32_t resolvedLocalSid;
        ImportKind kind() const { return static_cast<ImportKind>(bits.kind()); }
        ResolutionKind resolution() const { return bits.resolution(); }
        uint32_t request() const { return bits.request(); }
        uint32_t nameHash() const { return localHash; }
        bool isNamespace() const { return kind() == ImportKind::Namespace || kind() == ImportKind::NamespaceDefer; }
    };

    enum class ExportKind : uint8_t { Local, Indirect, Namespace };
    struct Export {
        uint32_t exportSid;
        uint32_t localOrImportSid; // Local: local name; Indirect: import name (starNamespaceSid: `import * as x` re-exported)
        KindBits bits;
        uint32_t exportHash;
        uint32_t resolvedModule;
        uint32_t resolvedLocalSid;
        ExportKind kind() const { return static_cast<ExportKind>(bits.kind()); }
        ResolutionKind resolution() const { return bits.resolution(); }
        uint32_t request() const { return bits.request(); }
        uint32_t nameHash() const { return exportHash; }
        // GetModuleNamespace of the requested module: cannot fail to resolve.
        bool isNamespaceReexport() const { return kind() == ExportKind::Namespace || (kind() == ExportKind::Indirect && localOrImportSid == starNamespaceSid); }
    };

    static_assert(sizeof(Header) == 16 * sizeof(uint32_t));
    static_assert(sizeof(Module) == 11 * sizeof(uint32_t));
    static_assert(sizeof(Request) == 4 * sizeof(uint32_t));
    static_assert(sizeof(Import) == 6 * sizeof(uint32_t));
    static_assert(sizeof(Export) == 6 * sizeof(uint32_t));

    // `blob` and `stringSlots` (sid -> EncoderStringTable::slotFor slot, resolved through `strings`) must outlive the VM.
    // Null if the blob does not check out; every range is validated here, so the accessors below never read outside it.
    JS_EXPORT_PRIVATE static RefPtr<PrelinkedModuleGraph> tryCreate(VM&, DecoderStringTable&, std::span<const uint8_t> blob, std::span<const uint32_t> stringSlots);
    JS_EXPORT_PRIVATE ~PrelinkedModuleGraph();

    VM& vm() const { return m_vm; }
    uint32_t moduleCount() const { return m_modules.size(); }
    uint32_t stringCount() const { return m_stringSlots.size(); }

    const Module& module(uint32_t moduleIndex) const
    {
        RELEASE_ASSERT(moduleIndex < m_modules.size(), moduleIndex, m_modules.size());
        return m_modules[moduleIndex];
    }
    std::span<const Request> requests(const Module& m) const { return m_requests.subspan(m.firstRequest, m.requestCount); }
    std::span<const Import> imports(const Module& m) const { return m_imports.subspan(m.firstImport, m.importCount); }
    std::span<const Export> exports(const Module& m) const { return m_exports.subspan(m.firstExport, m.exportCount); }
    std::span<const uint32_t> starExports(const Module& m) const { return m_starExports.subspan(m.firstStarExport, m.starExportCount); }
    const Request& request(const Module& m, uint32_t moduleRelativeIndex) const
    {
        RELEASE_ASSERT(moduleRelativeIndex < m.requestCount, moduleRelativeIndex, m.requestCount);
        return m_requests[m.firstRequest + moduleRelativeIndex];
    }
    uint32_t checkedModuleIndex(uint32_t moduleIndex) const
    {
        RELEASE_ASSERT(moduleIndex < m_modules.size(), moduleIndex, m_modules.size());
        return moduleIndex;
    }

    // The one atom this VM uses for the name (through the shared string table, so the bytecode's own identifiers are the
    // same atoms), made once per sid and kept for the VM's lifetime. The two sentinels map to the VM's *default* /
    // *namespace* private names. Mutator only.
    JS_EXPORT_PRIVATE const Identifier& identifier(uint32_t sid) const;
    // The specifier / fetch parameters a ModuleRequest for `request` carries. Parameters are shared per FetchKind.
    RefPtr<ScriptFetchParameters> fetchParameters(const Request&);

    // By-name lookups: an import through the module's hash index (one slot probe in the common case), an export by
    // binary search over the hash-sorted array; either way a hash match is confirmed by a pointer compare against the
    // sid's cached atom or, until it has one, a character compare; nothing is atomized. Null when the module has no
    // such entry. Symbols never match. Mutator only.
    const Import* findImport(const Module&, UniquedStringImpl* localName) const;
    const Export* findExport(const Module&, UniquedStringImpl* exportName) const;

private:
    PrelinkedModuleGraph(VM&, DecoderStringTable&, std::span<const uint8_t> blob, const Header&, std::span<const uint32_t> stringSlots);

    template<typename Entry, uint32_t Entry::*nameSid>
    const Entry* findByName(std::span<const Entry>, UniquedStringImpl*) const;
    bool nameEquals(uint32_t sid, UniquedStringImpl&) const;

    VM& m_vm;
    DecoderStringTable& m_strings;
    std::span<const uint32_t> m_stringSlots;
    std::span<const Module> m_modules;
    std::span<const Request> m_requests;
    std::span<const Import> m_imports;
    std::span<const Export> m_exports;
    std::span<const uint32_t> m_starExports;
    std::span<const uint16_t> m_importIndex;
    RefPtr<ScriptFetchParameters> m_fetchParameters[4]; // by FetchKind, JavaScript..JSON (None is null)
    mutable Vector<Identifier> m_identifiers; // by sid, sized once: the atom once identifier() made it or a by-name lookup met it; mutator only
    bool m_hashesVerified { true }; // the producer's name hashes agree with this build's StringHasher; else lookups scan
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
