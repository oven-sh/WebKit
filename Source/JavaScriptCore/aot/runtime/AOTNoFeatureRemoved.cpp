/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemovableFeature.h"

#include "WasmStreamingParser.h"
#include <wtf/Assertions.h>

namespace JSC {

extern "C" const char* Bun__removableFeatureName(uint16_t feature);

extern "C" const RemovableFeatureState Bun__removableFeatureState { };

extern "C" void Bun__abortForRemovedFeature(uint16_t)
{
    CRASH();
}

extern "C" EncodedJSValue Bun__throwForRemovedFeature(JSGlobalObject*, uint16_t)
{
    CRASH();
}

extern "C" EncodedJSValue Bun__throwForRemovedBuiltin(JSGlobalObject*, uint16_t, uint32_t)
{
    CRASH();
}

extern "C" EncodedJSValue Bun__throwForRemovedFunction(JSGlobalObject*, uint16_t, StringImpl*)
{
    CRASH();
}

extern "C" const char* Bun__removableFeatureName(uint16_t)
{
    return nullptr;
}

#if ENABLE(AOT) && ENABLE(WEBASSEMBLY)
static_assert(sizeof(SHA1) <= Wasm::SourceHasher::contextSize && alignof(SHA1) <= 8);

extern "C" void Bun__initializeSHA1(void* context)
{
    new (NotNull, context) SHA1;
}

extern "C" void Bun__updateSHA1(void* context, std::span<const uint8_t> bytes)
{
    static_cast<SHA1*>(context)->addBytes(bytes);
}

extern "C" void Bun__finishSHA1(void* context, std::array<uint8_t, 20>& digest)
{
    static_cast<SHA1*>(context)->computeHash(digest);
}
#endif

extern "C" void Bun__didFinishFirstCollectionOfCompiledProgram()
{
}

} // namespace JSC
