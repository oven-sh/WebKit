/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <cstring>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <zstd.h>

extern "C" const unsigned char bun_icu_zstd_dict[];
extern "C" const unsigned bun_icu_zstd_dict_size;

// The hook of jsc.cpp (BUN_ICU_ZSTD), for a program that is not the shell.
extern "C" const void* bun_icu_maybe_decompress(const void* item, int32_t* length)
{
    uint32_t magic;
    if (!item)
        return item;
    memcpy(&magic, item, sizeof(magic));
    if (magic != ZSTD_MAGICNUMBER) [[likely]]
        return item;

    static Lock lock;
    static NeverDestroyed<HashMap<const void*, void*>> cache;
    static ZSTD_DCtx* context = ZSTD_createDCtx();
    static ZSTD_DDict* dictionary = bun_icu_zstd_dict_size ? ZSTD_createDDict_byReference(bun_icu_zstd_dict, bun_icu_zstd_dict_size) : nullptr;

    Locker locker { lock };
    size_t bound = *length > 0 ? static_cast<size_t>(*length) : 1u << 20;
    size_t compressedSize = ZSTD_findFrameCompressedSize(item, bound);
    if (ZSTD_isError(compressedSize))
        return item;
    unsigned long long size = ZSTD_getFrameContentSize(item, compressedSize);
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR)
        return item;

    if (void* cached = cache->get(item)) {
        *length = static_cast<int32_t>(size);
        return cached;
    }

    void* buffer = fastMalloc(roundUpToMultipleOf<16>(static_cast<size_t>(size)));
    size_t result = dictionary
        ? ZSTD_decompress_usingDDict(context, buffer, size, item, compressedSize, dictionary)
        : ZSTD_decompressDCtx(context, buffer, size, item, compressedSize);
    if (ZSTD_isError(result)) {
        fastFree(buffer);
        return item;
    }
    cache->add(item, buffer);
    *length = static_cast<int32_t>(size);
    return buffer;
}
