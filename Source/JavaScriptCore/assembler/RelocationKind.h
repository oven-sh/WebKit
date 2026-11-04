/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <wtf/Assertions.h>

namespace JSC {

enum class PCRelativeReach : uint8_t { Near, Far };

enum class RelocationKind : uint8_t {
    Call,
    Jump,
    ShortJump,
    ConditionalJump,
    ShortConditionalJump,
    ConditionalJumpThenNop,
    TestBitJump,
    TestBitJumpThenNop,
    InvertedJumpThenJump,
    AddressNear,
    AddressFar,
    Load64,
    Distance32,
    InstanceOffset,
};
static constexpr unsigned numberOfRelocationKinds = 14;
static_assert(static_cast<unsigned>(RelocationKind::InstanceOffset) + 1 == numberOfRelocationKinds);

constexpr bool transfersControl(RelocationKind kind)
{
    switch (kind) {
    case RelocationKind::Call:
    case RelocationKind::Jump:
    case RelocationKind::ShortJump:
    case RelocationKind::ConditionalJump:
    case RelocationKind::ShortConditionalJump:
    case RelocationKind::ConditionalJumpThenNop:
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
    case RelocationKind::InvertedJumpThenJump:
        return true;
    case RelocationKind::AddressNear:
    case RelocationKind::AddressFar:
    case RelocationKind::Load64:
    case RelocationKind::Distance32:
    case RelocationKind::InstanceOffset:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr RelocationKind addressKindFor(PCRelativeReach reach)
{
    switch (reach) {
    case PCRelativeReach::Near:
        return RelocationKind::AddressNear;
    case PCRelativeReach::Far:
        return RelocationKind::AddressFar;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct LinkedSite {
    uint32_t offset;
    uint32_t target;
    RelocationKind kind;

    friend bool operator==(const LinkedSite&, const LinkedSite&) = default;
};

} // namespace JSC
