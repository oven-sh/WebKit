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
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

#include "CachedTypes.h"
#include "UnlinkedFunctionExecutable.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

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
    // The executable records being patched, as they will read after every patch so far, so each can be re-sealed.
    HashMap<ptrdiff_t, Vector<uint8_t>> patchedRecords;
    auto recordBytes = [&](ptrdiff_t base) -> Vector<uint8_t>& {
        return patchedRecords.ensure(base, [&] {
            // The record lives either in the original payload or in an update appended before this one.
            auto findIn = [&](std::span<const uint8_t> bytes, ptrdiff_t start) -> std::span<const uint8_t> {
                if (base < start || base >= start + static_cast<ptrdiff_t>(bytes.size()))
                    return { };
                auto record = bytes.subspan(base - start);
                RELEASE_ASSERT(record.size() >= CachedFunctionExecutableOffsets::fixedSize());
                RELEASE_ASSERT(CachedFunctionExecutableOffsets::isUpdatable(record)); // only the jsc shell's disk cache writes patchable records
                auto extentBytes = record.subspan(CachedFunctionExecutableOffsets::extentOffset(), 4);
                uint32_t extent = extentBytes[0] | extentBytes[1] << 8 | extentBytes[2] << 16 | static_cast<uint32_t>(extentBytes[3]) << 24;
                RELEASE_ASSERT(extent >= CachedFunctionExecutableOffsets::fixedSize() && extent <= record.size()); // our own encoder wrote this record moments ago
                return record.first(extent);
            };
            auto found = findIn(m_payload.span(), 0);
            ptrdiff_t start = m_payload.size();
            for (const auto& earlier : m_updates) {
                if (!found.empty())
                    break;
                const CachePayload& earlierPayload = earlier.isGlobal() ? earlier.asGlobal().m_payload : earlier.asFunction().m_payload;
                found = findIn(earlierPayload.span(), start);
                start += earlierPayload.size();
            }
            RELEASE_ASSERT(!found.empty());
            return Vector<uint8_t>(found);
        }).iterator->value;
    };
    auto patch = [&](ptrdiff_t base, ptrdiff_t fieldOffset, std::span<const uint8_t> bytes) {
        callback(base + fieldOffset, bytes);
        memcpySpan(recordBytes(base).mutableSpan().subspan(fieldOffset, bytes.size()), bytes);
    };
    auto littleEndian = [](uint32_t value) {
        std::array<uint8_t, 4> bytes;
        for (unsigned i = 0; i < 4; ++i)
            bytes[i] = static_cast<uint8_t>(value >> (8 * i));
        return bytes;
    };

    off_t offset = m_payload.size();
    for (const auto& update : m_updates) {
        const CachePayload* payload = nullptr;
        if (update.isGlobal())
            payload = &update.asGlobal().m_payload;
        else {
            const CacheUpdate::FunctionUpdate& functionUpdate = update.asFunction();
            payload = &functionUpdate.m_payload;
            {
                // The record's code block field for `kind` becomes the offset the update's root record will have once appended.
                ptrdiff_t fieldOffset = functionUpdate.m_kind == CodeSpecializationKind::CodeForCall ? CachedFunctionExecutableOffsets::codeBlockForCallOffset() : CachedFunctionExecutableOffsets::codeBlockForConstructOffset();
                auto bytes = littleEndian(safeCast<uint32_t>(offset + functionUpdate.m_rootOffset));
                patch(functionUpdate.m_base, fieldOffset, bytes);
            }
            {
                // u32 features, u8 lexicallyScopedFeatures, u8 hasCapturedVariables (see CachedFunctionExecutable).
                std::array<uint8_t, 6> bytes;
                auto features = littleEndian(functionUpdate.m_metadata.m_features);
                std::copy(features.begin(), features.end(), bytes.begin());
                bytes[4] = static_cast<uint8_t>(functionUpdate.m_metadata.m_lexicallyScopedFeatures);
                bytes[5] = functionUpdate.m_metadata.m_hasCapturedVariables;
                patch(functionUpdate.m_base, CachedFunctionExecutableOffsets::metadataOffset(), bytes);
            }
            {
                auto checksum = littleEndian(bytecodeCacheRecordChecksum(recordBytes(functionUpdate.m_base).span(), CachedFunctionExecutableOffsets::checksumOffset()));
                callback(functionUpdate.m_base + CachedFunctionExecutableOffsets::checksumOffset(), checksum);
            }
        }

        ASSERT(payload);
        callback(offset, payload->span());
        offset += payload->size();
    }
    ASSERT(static_cast<size_t>(offset) == m_size);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
