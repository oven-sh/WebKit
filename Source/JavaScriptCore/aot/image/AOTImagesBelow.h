/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTVector.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

struct DecodedString;

// CompileOptions::imagesBelow, read. The image that is built goes on where the last of them ends, and a name that one of them has keeps its number.
class ImagesBelow {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ImagesBelow);
    WTF_MAKE_NONCOPYABLE(ImagesBelow);
public:
    // Null: the bytes are no images of this compiler that share their realms.
    static std::unique_ptr<ImagesBelow> tryCreate(std::span<const std::span<const uint8_t>> images);

    unsigned count() const { return m_images.size(); }
    const ImageNumberRanges& numbersOfLast() const { return m_images.last()->header().numbers; }
    uint32_t capacityOfStringHeaders() const { return m_images.first()->header().numbers.capacityOfStringHeaders; }
    // What one of them leaves open, for its tests or for a write of its own, no image over it locks or takes for a constant.
    void forEachUnlockedBuiltinMember(NOESCAPE const Invocable<void(BuiltinID)> auto& functor) const
    {
        for (const std::unique_ptr<Image>& image : m_images) {
            for (uint32_t member : image->programTables().unlockedBuiltinMembers())
                functor(BuiltinID(member & ~ProgramTables::unlockedBuiltinMember));
        }
    }

    std::optional<uint32_t> numberOfName(const DecodedString&) const;
    // Whether a file with code at the path is a module of one of them, entry point or not.
    bool holdsFile(StringView path) const;
    // Of a key that two images make the same symbol of: a well-known or a registered symbol, a private name of the engine.
    std::optional<uint32_t> numberOfEngineSymbol(PropertyKeyKind, uint32_t wellKnownSymbol, const String& text) const;

private:
    ImagesBelow() = default;
    StringView viewOf(size_t position, StringID) const;

    Vector<std::unique_ptr<Image>, 2> m_images;
    UncheckedKeyHashMap<String, uint32_t> m_engineSymbols;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
