/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationData.h"
#include "AOTStubDescriptor.h"
#include "AOTStubWords.h"
#include "CodeSpecializationKind.h"

namespace JSC { namespace AOT {

enum class InternalFunctionEntry : uint8_t { Call, Construct };

constexpr StubKey callHostFunction(unsigned count) { return StubKey(StubFamily::CallHostFunction).withCount(count); }
constexpr StubKey callHostFunctionWithoutBoxedCall(unsigned count) { return StubKey(StubFamily::CallHostFunctionWithoutBoxedCall).withCount(count); }
constexpr StubKey callHostFunctionThatMayBeUnfetched(unsigned count) { return StubKey(StubFamily::CallHostFunctionThatMayBeUnfetched).withCount(count); }
constexpr StubKey callHostFunctionCheckingResult(HostResult result, unsigned count) { return StubKey(StubFamily::CallHostFunctionCheckingResult).withForm(static_cast<uint8_t>(result)).withCount(count); }
constexpr StubKey callHostFunctionThatMayBeUnfetchedCheckingResult(HostResult result, unsigned count) { return StubKey(StubFamily::CallHostFunctionThatMayBeUnfetchedCheckingResult).withForm(static_cast<uint8_t>(result)).withCount(count); }
constexpr StubKey callHostFunctionWithCount() { return StubKey(StubFamily::CallHostFunctionWithCount); }
constexpr StubKey callHostGetter() { return StubKey(StubFamily::CallHostGetter); }

enum class DirectFunctionCall : uint8_t { WithFrame, Leaf, WithHostFrame, LeafReturningNumber };

inline constexpr unsigned directThisArgumentIndex = 3;
inline constexpr unsigned directCalleeArgumentIndex = 4;

constexpr StubKey callDirectFunction(DirectFunctionCall call) { return StubKey(StubFamily::CallDirectFunction).withForm(static_cast<uint8_t>(call)); }

constexpr StubKey callInternalFunction(CodeSpecializationKind kind, unsigned count)
{
    switch (kind) {
    case CodeSpecializationKind::CodeForCall:
        return StubKey(StubFamily::CallInternalFunction).withForm(static_cast<uint8_t>(InternalFunctionEntry::Call)).withCount(count);
    case CodeSpecializationKind::CodeForConstruct:
        return StubKey(StubFamily::CallInternalFunction).withForm(static_cast<uint8_t>(InternalFunctionEntry::Construct)).withCount(count);
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static_assert(exists(callHostFunction(0)) && exists(callHostFunction(numberOfArgumentGPRs)) && !exists(callHostFunction(numberOfArgumentGPRs + 1)));
static_assert(descriptorOf(StubFamily::CallHostFunctionWithoutBoxedCall).minCount == BoxedCallEntry::minArgumentCount && descriptorOf(StubFamily::CallHostFunctionWithoutBoxedCall).maxCount == BoxedCallEntry::maxArgumentCount);
static_assert(descriptorOf(StubFamily::CallHostFunctionCheckingResult).numberOfForms == static_cast<unsigned>(HostResult::Array) + 1 && descriptorOf(StubFamily::CallHostFunctionCheckingResult).maxCount == BoxedCallEntry::maxArgumentCount);
static_assert(bodyOf(callHostFunctionCheckingResult(HostResult::Array, 1)) == callHostFunctionThatMayBeUnfetchedCheckingResult(HostResult::Array, 1));
static_assert(descriptorOf(StubFamily::CallInternalFunction).numberOfForms == static_cast<unsigned>(InternalFunctionEntry::Construct) + 1);
static_assert(exists(callInternalFunction(CodeSpecializationKind::CodeForCall, 0)) && exists(callInternalFunction(CodeSpecializationKind::CodeForConstruct, numberOfArgumentGPRs)));
static_assert(descriptorOf(StubFamily::CallDirectFunction).numberOfForms == static_cast<unsigned>(DirectFunctionCall::LeafReturningNumber) + 1);
static_assert(descriptorOf(StubFamily::CallDirectFunction).binding != ImmediateBinding::OperationOffset);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
