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

#pragma once

#include "VM.h"
#include <variant>
#include <wtf/FileSystem.h>
#include <wtf/MallocPtr.h>

namespace JSC {

class CachePayload {
public:
    using Destructor = WTF::Function<void(const void*)>;
    JS_EXPORT_PRIVATE static CachePayload makeMappedPayload(FileSystem::MappedFileData&&);
    JS_EXPORT_PRIVATE static CachePayload makeMallocPayload(MallocPtr<uint8_t, VMMalloc>&&, size_t);
    JS_EXPORT_PRIVATE static CachePayload makePayloadWithDestructor(std::span<uint8_t>, Destructor&&);
    JS_EXPORT_PRIVATE static CachePayload makeEmptyPayload();

    JS_EXPORT_PRIVATE CachePayload(CachePayload&&);
    JS_EXPORT_PRIVATE ~CachePayload();

    size_t size() const { return span().size(); }
    JS_EXPORT_PRIVATE std::span<const uint8_t> span() const;

private:
    CachePayload(std::variant<FileSystem::MappedFileData, std::pair<MallocPtr<uint8_t, VMMalloc>, size_t>, std::span<uint8_t>>&&, Destructor&& = {nullptr});

    std::variant<FileSystem::MappedFileData, std::pair<MallocPtr<uint8_t, VMMalloc>, size_t>, std::span<uint8_t>> m_data;
    Destructor m_destructor { nullptr };
};

} // namespace JSC
