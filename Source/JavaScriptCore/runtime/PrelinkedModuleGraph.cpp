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

#include "config.h"
#include "PrelinkedModuleGraph.h"

#if USE(BUN_JSC_ADDITIONS)

#include "BuiltinNames.h"
#include "CachedTypes.h"
#include "JSCInlines.h"
#include "VM.h"

namespace JSC {

template<typename T>
static std::optional<std::span<const T>> arrayAt(std::span<const uint8_t> blob, uint32_t offset, uint32_t count)
{
    static_assert(alignof(T) <= alignof(uint32_t));
    if (offset % alignof(T) || offset > blob.size() || count > (blob.size() - offset) / sizeof(T))
        return std::nullopt;
    return std::span { std::bit_cast<const T*>(blob.data() + offset), count };
}

RefPtr<PrelinkedModuleGraph> PrelinkedModuleGraph::tryCreate(VM& vm, DecoderStringTable& strings, std::span<const uint8_t> blob, std::span<const uint32_t> stringSlots)
{
    if (blob.size() < sizeof(Header) || std::bit_cast<uintptr_t>(blob.data()) % alignof(uint32_t) || std::bit_cast<uintptr_t>(stringSlots.data()) % alignof(uint32_t))
        return nullptr;
    const Header& header = *std::bit_cast<const Header*>(blob.data());
    if (header.magic != magic || header.version != currentVersion || header.stringCount != stringSlots.size())
        return nullptr;
    auto modules = arrayAt<Module>(blob, header.modulesOffset, header.moduleCount);
    auto requests = arrayAt<Request>(blob, header.requestsOffset, header.requestCount);
    auto imports = arrayAt<Import>(blob, header.importsOffset, header.importCount);
    auto exports = arrayAt<Export>(blob, header.exportsOffset, header.exportCount);
    auto starExports = arrayAt<uint32_t>(blob, header.starExportsOffset, header.starExportCount);
    auto importIndex = arrayAt<uint16_t>(blob, header.importIndexOffset, header.importIndexBytes / sizeof(uint16_t));
    if (!modules || !requests || !imports || !exports || !starExports || !importIndex)
        return nullptr;
    auto within = [](uint32_t first, uint32_t count, size_t size) { return first <= size && count <= size - first; };
    for (const Module& m : *modules) {
        if (m.keySid >= header.stringCount || m.requestCount > std::numeric_limits<uint16_t>::max() || m.importCount >= std::numeric_limits<uint16_t>::max()
            || !within(m.firstRequest, m.requestCount, requests->size()) || !within(m.firstImport, m.importCount, imports->size())
            || !within(m.firstExport, m.exportCount, exports->size()) || !within(m.firstStarExport, m.starExportCount, starExports->size())
            || m.importIndexOffset % sizeof(uint16_t) || !within(m.importIndexOffset / sizeof(uint16_t), importIndexSlotCount(m.importCount), importIndex->size()))
            return nullptr;
    }
    return adoptRef(*new PrelinkedModuleGraph(vm, strings, blob, header, stringSlots));
}

PrelinkedModuleGraph::PrelinkedModuleGraph(VM& vm, DecoderStringTable& strings, std::span<const uint8_t> blob, const Header& header, std::span<const uint32_t> stringSlots)
    : m_vm(vm)
    , m_strings(strings)
    , m_stringSlots(stringSlots)
    , m_modules(*arrayAt<Module>(blob, header.modulesOffset, header.moduleCount))
    , m_requests(*arrayAt<Request>(blob, header.requestsOffset, header.requestCount))
    , m_imports(*arrayAt<Import>(blob, header.importsOffset, header.importCount))
    , m_exports(*arrayAt<Export>(blob, header.exportsOffset, header.exportCount))
    , m_starExports(*arrayAt<uint32_t>(blob, header.starExportsOffset, header.starExportCount))
    , m_importIndex(*arrayAt<uint16_t>(blob, header.importIndexOffset, header.importIndexBytes / sizeof(uint16_t)))
{
    // The producer hashed names with its own copy of StringHasher; by-name lookups binary-search on those hashes, so make
    // sure the two agree (a sample is enough: a different hasher disagrees almost everywhere) and scan linearly if not.
    auto verify = [&](auto entries, auto sidOf) {
        for (const auto& entry : entries.first(std::min<size_t>(entries.size(), 16))) {
            uint32_t sid = sidOf(entry);
            if (sid >= m_stringSlots.size())
                continue;
            if (m_strings.hashForSlot(m_stringSlots[sid]) != std::optional<uint32_t> { entry.nameHash() }) {
                m_hashesVerified = false;
                return;
            }
        }
    };
    verify(m_imports, [](const Import& i) { return i.localSid; });
    verify(m_exports, [](const Export& e) { return e.exportSid; });
}

PrelinkedModuleGraph::~PrelinkedModuleGraph() = default;

ScriptFetchParameters::Type PrelinkedModuleGraph::Request::type() const
{
    switch (fetchKind()) {
    case FetchKind::None:
    case FetchKind::JavaScript:
        return ScriptFetchParameters::Type::JavaScript;
    case FetchKind::WebAssembly:
        return ScriptFetchParameters::Type::WebAssembly;
    case FetchKind::JSON:
        return ScriptFetchParameters::Type::JSON;
    case FetchKind::HostDefined:
        return ScriptFetchParameters::Type::HostDefined;
    }
    return ScriptFetchParameters::Type::JavaScript;
}

RefPtr<ScriptFetchParameters> PrelinkedModuleGraph::fetchParameters(const Request& request)
{
    ScriptFetchParameters::Type type;
    switch (request.fetchKind()) {
    case FetchKind::None:
        return nullptr;
    case FetchKind::JavaScript:
        type = ScriptFetchParameters::Type::JavaScript;
        break;
    case FetchKind::WebAssembly:
        type = ScriptFetchParameters::Type::WebAssembly;
        break;
    case FetchKind::JSON:
        type = ScriptFetchParameters::Type::JSON;
        break;
    case FetchKind::HostDefined:
        return ScriptFetchParameters::create(identifier(request.hostDefinedTypeSid).string());
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    RefPtr<ScriptFetchParameters>& shared = m_fetchParameters[static_cast<unsigned>(type)];
    if (!shared)
        shared = ScriptFetchParameters::create(type);
    return shared;
}

const Identifier& PrelinkedModuleGraph::identifier(uint32_t sid) const
{
    ASSERT(!isCompilationThread());
    if (sid == starDefaultSid)
        return m_vm.propertyNames->starDefaultPrivateName;
    if (sid == starNamespaceSid)
        return m_vm.propertyNames->starNamespacePrivateName;
    RELEASE_ASSERT(sid < m_stringSlots.size(), sid, m_stringSlots.size());
    // Each sid is resolved through the string table once (its slot cache, or the atom table for a 1-3 character name)
    // and then served from here: request specifiers and resolved names repeat across the graph's modules and edges.
    if (m_identifiers.isEmpty()) [[unlikely]]
        m_identifiers.grow(m_stringSlots.size());
    Identifier& cached = m_identifiers[sid];
    if (cached.isNull()) [[unlikely]] {
        RefPtr<AtomStringImpl> atom = m_strings.atomForSlot(m_vm, m_stringSlots[sid]);
        RELEASE_ASSERT(atom, sid, m_stringSlots[sid]);
        cached = Identifier::fromUid(m_vm, atom.get());
    }
    return cached;
}

bool PrelinkedModuleGraph::nameEquals(uint32_t sid, UniquedStringImpl& name) const
{
    ASSERT(!isCompilationThread() && !name.isSymbol());
    if (sid >= m_stringSlots.size())
        return false;
    if (sid < m_identifiers.size() && !m_identifiers[sid].isNull())
        return m_identifiers[sid].impl() == &name;
    // By contents, so that a lookup never creates an atom; on a match `name` is the atom identifier(sid) would make.
    if (!m_strings.slotEquals(m_stringSlots[sid], name))
        return false;
    if (m_identifiers.isEmpty()) [[unlikely]]
        m_identifiers.grow(m_stringSlots.size());
    m_identifiers[sid] = Identifier::fromUid(m_vm, &name);
    return true;
}

template<typename Entry, uint32_t Entry::*nameSid>
const Entry* PrelinkedModuleGraph::findByName(std::span<const Entry> entries, UniquedStringImpl* name) const
{
    if (!name || name->isSymbol() || entries.empty())
        return nullptr;
    if (!m_hashesVerified) [[unlikely]] {
        for (const Entry& entry : entries) {
            if (nameEquals(entry.*nameSid, *name))
                return &entry;
        }
        return nullptr;
    }
    uint32_t target = name->existingSymbolAwareHash();
    size_t low = 0;
    size_t high = entries.size();
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (entries[mid].nameHash() < target)
            low = mid + 1;
        else
            high = mid;
    }
    for (size_t i = low; i < entries.size() && entries[i].nameHash() == target; ++i) {
        if (nameEquals(entries[i].*nameSid, *name))
            return &entries[i];
    }
    return nullptr;
}

auto PrelinkedModuleGraph::findImport(const Module& module, UniquedStringImpl* localName) const -> const Import*
{
    if (!localName || localName->isSymbol() || !module.importCount)
        return nullptr;
    if (!m_hashesVerified) [[unlikely]]
        return findByName<Import, &Import::localSid>(imports(module), localName);
    auto entries = imports(module);
    uint32_t hash = localName->existingSymbolAwareHash();
    uint32_t mask = importIndexSlotCount(module.importCount) - 1;
    const uint16_t* slots = m_importIndex.data() + module.importIndexOffset / sizeof(uint16_t);
    for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
        uint16_t slot = slots[i];
        if (!slot)
            return nullptr;
        const Import& entry = entries[slot - 1];
        if (entry.nameHash() == hash && nameEquals(entry.localSid, *localName))
            return &entry;
    }
}

auto PrelinkedModuleGraph::findExport(const Module& module, UniquedStringImpl* exportName) const -> const Export*
{
    return findByName<Export, &Export::exportSid>(exports(module), exportName);
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
