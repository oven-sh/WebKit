/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrames.h"
#include "AOTStubKey.h"
#include <expected>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class BuildContext;
class StubCatalog;

static constexpr unsigned minCodeGranuleShift = 6;
static constexpr unsigned maxCodeGranuleShift = 12;

struct StubPlacement {
    StubKey key;
    uint32_t offset;
};

struct StubReturnSite {
    uint32_t offset;
    StubFrameKind kind;
    uint8_t padding[3] { };
};
static_assert(sizeof(StubReturnSite) == 8 && std::has_unique_object_representations_v<StubReturnSite>);

struct StubBlob {
    std::optional<uint32_t> offsetOf(StubKey) const;

    RelocatableCode code;
    Vector<StubPlacement> placements;
    Vector<StubReturnSite> returnSites;
    WTF::BitSet<numberOfEntries> entriesUsed;
};

std::expected<StubBlob, LinkFailure> buildStubBlob(BuildContext&, const StubCatalog&, std::span<const StubKey> calledKeys, const WTF::BitSet<numberOfEntries>& entriesUsedByFunctions);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
