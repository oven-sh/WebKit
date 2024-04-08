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

#pragma once

#include <wtf/Function.h>
#include <wtf/text/StringImpl.h>

namespace WTF {

class ExternalStringImpl;

#if !USE(BUN_JSC_ADDITIONS)
using ExternalStringImplFreeFunction = Function<void(ExternalStringImpl*, void*, unsigned)>;
#else
using ExternalStringImplFreeFunction = Function<void(void*, void*, unsigned)>;
#endif

class ExternalStringImpl final : public StringImpl {
public:
    WTF_EXPORT_PRIVATE static Ref<ExternalStringImpl> create(const LChar* characters, unsigned length, ExternalStringImplFreeFunction&&);
    WTF_EXPORT_PRIVATE static Ref<ExternalStringImpl> create(const UChar* characters, unsigned length, ExternalStringImplFreeFunction&&);
#if USE(BUN_JSC_ADDITIONS)
    WTF_EXPORT_PRIVATE static Ref<ExternalStringImpl> create(const LChar* characters, unsigned length, void* ctx, ExternalStringImplFreeFunction&&);
    WTF_EXPORT_PRIVATE static Ref<ExternalStringImpl> create(const UChar* characters, unsigned length, void* ctx, ExternalStringImplFreeFunction&&);
    WTF_EXPORT_PRIVATE static Ref<ExternalStringImpl> createStatic(const LChar* characters, unsigned length);
    WTF_EXPORT_PRIVATE static Ref<ExternalStringImpl> createStatic(const UChar* characters, unsigned length);
    WTF_EXPORT_PRIVATE static Ref<ExternalStringImpl> createStatic(const char* string);
#endif

private:
    friend class StringImpl;

    ExternalStringImpl(const LChar* characters, unsigned length, ExternalStringImplFreeFunction&&);
    ExternalStringImpl(const UChar* characters, unsigned length, ExternalStringImplFreeFunction&&);

#if !USE(BUN_JSC_ADDITIONS)
    ALWAYS_INLINE void freeExternalBuffer(void* buffer, unsigned bufferSize)
    {
        m_free(this, buffer, bufferSize);
    }
#else
    ExternalStringImpl(const LChar* characters, unsigned length, void* ctx, ExternalStringImplFreeFunction&&);
    ExternalStringImpl(const UChar* characters, unsigned length, void* ctx, ExternalStringImplFreeFunction&&);
    ExternalStringImpl(const LChar* characters, unsigned length);
    ExternalStringImpl(const UChar* characters, unsigned length);

    ALWAYS_INLINE void freeExternalBuffer(void* buffer, unsigned bufferSize)
    {
        m_free(m_freeCtx, buffer, bufferSize);
    }

    void* m_freeCtx;
#endif

    ExternalStringImplFreeFunction m_free;
};

} // namespace WTF

using WTF::ExternalStringImpl;
