/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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
#include "CachedBytecode.h"

#include "CachedTypes.h"
#include "JSCInlines.h"
#include "SourceProvider.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "WeakInlines.h"
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(PersistentBytecodePayloads);

PersistentBytecodePayloads::PersistentBytecodePayloads(VM& vm)
    : m_vm(vm)
{
    vm.heap.registerWeakGCHashTable(this);
}

PersistentBytecodePayloads::~PersistentBytecodePayloads()
{
    m_vm.heap.unregisterWeakGCHashTable(this);
    for (auto& entry : m_entries) {
        if (entry.decoder)
            entry.decoder->clearPersistentPayloadIndex();
    }
}

uint16_t PersistentBytecodePayloads::indexFor(CachedBytecode& payload, SourceProvider& provider)
{
    ASSERT(payload.payloadIsPersistent());
    const uint8_t* bytes = payload.span().data();
    if (!bytes || payload.size() > std::numeric_limits<int32_t>::max())
        return 0;
    Identity identity { bytes, &provider };
    auto it = m_indices.find(identity);
    if (it != m_indices.end()) {
        if (m_entries[it->value - 1].payload->size() == payload.size())
            return it->value;
        return 0;
    }
    uint16_t index;
    if (!m_freeIndices.isEmpty())
        index = m_freeIndices.takeLast();
    else {
        if (m_entries.size() >= std::numeric_limits<uint16_t>::max())
            return 0;
        m_entries.append(Entry { });
        index = m_entries.size();
    }
    Entry& entry = m_entries[index - 1];
    ASSERT(!entry.payload && !entry.users && !entry.decoder);
    entry.payload = &payload;
    entry.provider = &provider;
    m_indices.add(identity, index);
    return index;
}

void PersistentBytecodePayloads::didCreateDecoder(uint16_t index, Decoder& decoder)
{
    Entry& entry = m_entries[index - 1];
    if (!entry.decoder)
        entry.decoder = &decoder;
    retain(index);
}

void PersistentBytecodePayloads::willDestroyDecoder(uint16_t index, Decoder& decoder)
{
    Entry& entry = m_entries[index - 1];
    if (entry.decoder == &decoder)
        entry.decoder = nullptr;
    release(index);
}

void PersistentBytecodePayloads::retain(uint16_t index)
{
    RELEASE_ASSERT(index && index <= m_entries.size() && m_entries[index - 1].payload);
    ++m_entries[index - 1].users;
}

void PersistentBytecodePayloads::release(uint16_t index)
{
    RELEASE_ASSERT(index && index <= m_entries.size() && m_entries[index - 1].users);
    if (!--m_entries[index - 1].users)
        removeEntry(index);
}

void PersistentBytecodePayloads::removeEntry(uint16_t index)
{
    Entry& entry = m_entries[index - 1];
    ASSERT(!entry.users && !entry.decoder);
    m_indices.remove(Identity { entry.payload->span().data(), entry.provider.get() });
    // Nothing can ask for these any more: asking takes a code block or a Decoder of this slot.
    m_childExecutables.removeIf([&](auto& remembered) {
        return remembered.key >> 32 == index;
    });
    // Last: dropping the provider can run the embedder's code.
    RefPtr payload = std::exchange(entry.payload, nullptr);
    RefPtr provider = std::exchange(entry.provider, nullptr);
    m_freeIndices.append(index);
}

RefPtr<Decoder> PersistentBytecodePayloads::decoderFor(VM& vm, uint16_t index)
{
    if (!index || index > m_entries.size())
        return nullptr;
    Entry& entry = m_entries[index - 1];
    if (!entry.payload)
        return nullptr;
    if (entry.decoder)
        return entry.decoder;
    return Decoder::create(vm, *entry.payload, entry.provider.get());
}

void PersistentBytecodePayloads::rememberChildExecutables(UnlinkedCodeBlock& codeBlock)
{
    unsigned declarations = codeBlock.numberOfFunctionDecls();
    unsigned count = declarations + codeBlock.numberOfFunctionExprs();
    if (!count)
        return;
    auto child = [&](unsigned position) {
        return position < declarations ? codeBlock.functionDeclIfDecoded(position) : codeBlock.functionExpr(position - declarations);
    };
    bool any = false;
    for (unsigned position = 0; position < count && !any; ++position)
        any = !!child(position);
    if (!any)
        return;
    FixedVector<Weak<UnlinkedFunctionExecutable>> children(count);
    for (unsigned position = 0; position < count; ++position) {
        if (UnlinkedFunctionExecutable* executable = child(position))
            children[position] = Weak<UnlinkedFunctionExecutable>(executable);
    }
    m_childExecutables.set(key(codeBlock.cachedPayloadIndex(), codeBlock.cachedRecordOffset()), WTF::move(children));
}

void PersistentBytecodePayloads::pruneStaleEntries()
{
    if (m_childExecutables.isEmpty())
        return;
    m_childExecutables.removeIf([](auto& entry) {
        bool anyAlive = false;
        for (auto& child : entry.value) {
            if (child)
                anyAlive = true;
            else
                child.clear();
        }
        return !anyAlive;
    });
}

auto PersistentBytecodePayloads::statistics() const -> Statistics
{
    Statistics result;
    for (auto& entry : m_entries) {
        if (!entry.payload)
            continue;
        result.payloads++;
        if (!entry.decoder)
            continue;
        result.liveDecoders++;
        entry.decoder->addRetainedTableSizes(result.decoderMappedPointers, result.decoderAtomsByOrdinal, result.decoderFinalizers);
    }
    result.parentsWithRememberedChildren = m_childExecutables.size();
    return result;
}

void CachedBytecode::addGlobalUpdate(Ref<CachedBytecode> bytecode)
{
    ASSERT(m_updates.isEmpty());
    m_leafExecutables.clear();
    copyLeafExecutables(bytecode.get());
    m_updates.append(CacheUpdate::GlobalUpdate { WTF::move(bytecode->m_payload) });
}

void CachedBytecode::addFunctionUpdate(const UnlinkedFunctionExecutable* executable, CodeSpecializationKind kind, Ref<CachedBytecode> bytecode)
{
    auto it = m_leafExecutables.find(executable);
    if (it == m_leafExecutables.end())
        return; // not recorded as a leaf of this payload (lean decoder, or its cached block was rejected): nothing to append to
    ptrdiff_t offset = it->value.base();
    ASSERT(offset);
    copyLeafExecutables(bytecode.get());
    m_updates.append(CacheUpdate::FunctionUpdate { offset, kind, { executable->features(), executable->lexicallyScopedFeatures(), executable->hasCapturedVariables() }, WTF::move(bytecode->m_payload), bytecode->rootOffset() });
}

void CachedBytecode::copyLeafExecutables(const CachedBytecode& bytecode)
{
    for (const auto& it : bytecode.m_leafExecutables) {
        auto addResult = m_leafExecutables.add(it.key, it.value + m_size);
        ASSERT_UNUSED(addResult, addResult.isNewEntry);
    }
    m_size += bytecode.size();
}

void CachedBytecode::commitUpdates(const ForEachUpdateCallback& callback) const
{
    off_t offset = m_payload.size();
    for (const auto& update : m_updates) {
        const CachePayload* payload = nullptr;
        if (update.isGlobal())
            payload = &update.asGlobal().m_payload;
        else {
            const CacheUpdate::FunctionUpdate& functionUpdate = update.asFunction();
            payload = &functionUpdate.m_payload;
            {
                ptrdiff_t kindOffset = functionUpdate.m_kind == CodeSpecializationKind::CodeForCall ? CachedFunctionExecutableOffsets::codeBlockForCallOffset() : CachedFunctionExecutableOffsets::codeBlockForConstructOffset();
                ptrdiff_t fieldOffset = kindOffset + CachedWriteBarrierOffsets::ptrOffset() + CachedPtrOffsets::offsetOffset();
                VariableLengthObjectBase::Offset offsetPayload = safeCast<VariableLengthObjectBase::Offset>(static_cast<ptrdiff_t>(offset + functionUpdate.m_rootOffset) - (functionUpdate.m_base + fieldOffset));
                static_assert(std::is_same<decltype(VariableLengthObjectBase::m_offset), VariableLengthObjectBase::Offset>::value);
                callback(functionUpdate.m_base + fieldOffset, { reinterpret_cast<const uint8_t*>(&offsetPayload), sizeof(offsetPayload) });
            }
            callback(functionUpdate.m_base + CachedFunctionExecutableOffsets::metadataOffset(), { reinterpret_cast<const uint8_t*>(&functionUpdate.m_metadata), sizeof(functionUpdate.m_metadata) });
        }

        ASSERT(payload);
        callback(offset, payload->span());
        offset += payload->size();
    }
    ASSERT(static_cast<size_t>(offset) == m_size);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
