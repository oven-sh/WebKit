/*
 * Copyright (C) 2018 mce sys Ltd. All rights reserved.
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
#include <wtf/text/ExternalStringImpl.h>

namespace WTF {

WTF_EXPORT_PRIVATE Ref<ExternalStringImpl> ExternalStringImpl::create(std::span<const Latin1Character> characters, void* context, ExternalStringImplFreeFunction&& free)
{
    return adoptRef(*new ExternalStringImpl(characters, context, WTF::move(free)));
}

WTF_EXPORT_PRIVATE Ref<ExternalStringImpl> ExternalStringImpl::create(std::span<const char16_t> characters, void* context, ExternalStringImplFreeFunction&& free)
{
    return adoptRef(*new ExternalStringImpl(characters, context, WTF::move(free)));
}


WTF_EXPORT_PRIVATE Ref<ExternalStringImpl> ExternalStringImpl::createStatic(std::span<const Latin1Character> characters)
{
    return adoptRef(*new ExternalStringImpl(characters, nullptr, [](auto, auto, auto) -> void {}));
}

WTF_EXPORT_PRIVATE Ref<ExternalStringImpl> ExternalStringImpl::createStatic(std::span<const char16_t> characters)
{
    return adoptRef(*new ExternalStringImpl(characters, nullptr, [](auto, auto, auto) -> void {}));
}



ExternalStringImpl::ExternalStringImpl(std::span<const Latin1Character> characters, void* ctx, ExternalStringImplFreeFunction&& free)
    : StringImpl(characters, ConstructWithoutCopying)
    , m_free(WTF::move(free))
{
    ASSERT(m_free);
    m_freeCtx = ctx;
    // Plain relaxed store: this object is still under construction, hence
    // provably unpublished (SPEC-vmstate §4.5).
    m_hashAndFlags.store((m_hashAndFlags.load(std::memory_order_relaxed) & ~s_hashMaskBufferOwnership) | BufferExternal, std::memory_order_relaxed);
}


ExternalStringImpl::ExternalStringImpl(std::span<const char16_t> characters, void* ctx, ExternalStringImplFreeFunction&& free)
    : StringImpl(characters, ConstructWithoutCopying)
    , m_free(WTF::move(free))
{
    ASSERT(m_free);
    m_freeCtx = ctx;
    // Plain relaxed store: this object is still under construction, hence
    // provably unpublished (SPEC-vmstate §4.5).
    m_hashAndFlags.store((m_hashAndFlags.load(std::memory_order_relaxed) & ~s_hashMaskBufferOwnership) | BufferExternal, std::memory_order_relaxed);
}

ExternalStringImpl::ExternalStringImpl(std::span<const Latin1Character> characters, ExternalStringImplFreeFunction&& free)
    : StringImpl(characters, ConstructWithoutCopying)
    , m_free(WTF::move(free))
{
    ASSERT(m_free);
    m_freeCtx = nullptr;
    // Plain relaxed store: this object is still under construction, hence
    // provably unpublished (SPEC-vmstate §4.5).
    m_hashAndFlags.store((m_hashAndFlags.load(std::memory_order_relaxed) & ~s_hashMaskBufferOwnership) | BufferExternal, std::memory_order_relaxed);
    m_refCount |= s_refCountFlagIsStaticString;
}

ExternalStringImpl::ExternalStringImpl(std::span<const char16_t> characters, ExternalStringImplFreeFunction&& free)
    : StringImpl(characters, ConstructWithoutCopying)
    , m_free(WTF::move(free))
{
    ASSERT(m_free);
    m_freeCtx = nullptr;
    // Plain relaxed store: this object is still under construction, hence
    // provably unpublished (SPEC-vmstate §4.5).
    m_hashAndFlags.store((m_hashAndFlags.load(std::memory_order_relaxed) & ~s_hashMaskBufferOwnership) | BufferExternal, std::memory_order_relaxed);
    m_refCount |= s_refCountFlagIsStaticString;
}

void ExternalStringImpl::releaseBufferEarly()
{
    void* free_ctx = std::exchange(m_freeCtx, reinterpret_cast<void*>(isAlreadyReleasedMarker));
    if (reinterpret_cast<uintptr_t>(free_ctx) != isAlreadyReleasedMarker)
        m_free(free_ctx, const_cast<Latin1Character*>(m_data8), m_length);
}

} // namespace WTF
