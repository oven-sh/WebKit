/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BufferAccessorRegistry.h"

#if USE(BUN_JSC_ADDITIONS)

#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>

namespace JSC {

struct BufferAccessorRegistry {
    Lock lock;
    struct Entry {
        uint64_t data { 0 };
        bool isWrite { false };
        bool byteLengthFromArgument { false };
    };
    UncheckedKeyHashMap<void*, Entry> entries WTF_GUARDED_BY_LOCK(lock);
};

static BufferAccessorRegistry& bufferAccessorRegistry()
{
    static LazyNeverDestroyed<BufferAccessorRegistry> registry;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        registry.construct();
    });
    return registry.get();
}

void registerBufferAccessor(TaggedNativeFunction function, BufferAccessorDescriptor descriptor)
{
    ASSERT(descriptor.byteLengthFromArgument ? !descriptor.data.byteSize : (descriptor.data.byteSize == 1 || descriptor.data.byteSize == 2 || descriptor.data.byteSize == 4 || descriptor.data.byteSize == 8));
    ASSERT(descriptor.data.byteSize == 1 || descriptor.data.isLittleEndian != TriState::Indeterminate);
    auto& registry = bufferAccessorRegistry();
    Locker locker { registry.lock };
    registry.entries.set(function.untaggedPtr(), BufferAccessorRegistry::Entry { descriptor.data.asQuadWord, descriptor.isWrite, descriptor.byteLengthFromArgument });
}

std::optional<BufferAccessorDescriptor> bufferAccessorDescriptor(TaggedNativeFunction function)
{
    auto& registry = bufferAccessorRegistry();
    Locker locker { registry.lock };
    auto iterator = registry.entries.find(function.untaggedPtr());
    if (iterator == registry.entries.end())
        return std::nullopt;
    BufferAccessorDescriptor descriptor;
    descriptor.data.asQuadWord = iterator->value.data;
    descriptor.isWrite = iterator->value.isWrite;
    descriptor.byteLengthFromArgument = iterator->value.byteLengthFromArgument;
    return descriptor;
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
