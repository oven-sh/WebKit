/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationData.h"
#include "AOTStubDescriptor.h"
#include "AOTStubHelpers.h"
#include <span>

namespace JSC { namespace AOT {

enum class CreatedConstant : uint8_t { String, Key };
enum class ConstantAtSite : uint8_t { String, Key, BuiltinCell };

constexpr StubKey createConstant(CreatedConstant constant) { return StubKey(StubFamily::CreateConstant).withForm(static_cast<uint8_t>(constant)); }
constexpr StubKey builtinCell() { return StubKey(StubFamily::BuiltinCell); }
constexpr bool isWordOfStoreStringConstant(uint64_t offset) { return !(offset % sizeof(uint64_t)) && offset / sizeof(uint64_t) <= StubKey::maxCount; }
constexpr StubKey storeStringConstant(uint64_t offset) { return StubKey(StubFamily::StoreStringConstant).withCount(static_cast<unsigned>(offset / sizeof(uint64_t))); }
constexpr StubKey createConstantAtSite(ConstantAtSite constant) { return StubKey(StubFamily::CreateConstantAtSite).withForm(static_cast<uint8_t>(constant)); }

inline constexpr int32_t offsetOfNumberBehindCall = static_cast<int32_t>(ThrowSiteWord::sizeOfWordBehindCall - sizeof(uint32_t));

struct LazyHeaderRegisters {
    GPRReg number;
    GPRReg record;
    GPRReg offset;
    GPRReg lazyHeaders;
    GPRReg word;
};

void loadStringConstantsAndJump(StubAssembler&, std::span<const GPRReg> numbers, StubKey target);
void loadStringConstantsAndContinue(StubAssembler&, std::span<const GPRReg> numbers, const ScopedLambda<void()>& body);

void createLazyHeader(StubAssembler&, const LazyHeaderRegisters&, unsigned longestTextWithoutHeader, JumpList& cannot);

static_assert(exists(createConstant(CreatedConstant::Key)) && !exists(createConstant(CreatedConstant::Key).withForm(3)));
static_assert(exists(builtinCell()) && exists(builtinCell().withImmediate(1)) && !exists(createConstant(CreatedConstant::Key).withImmediate(1)));
static_assert(exists(createConstantAtSite(ConstantAtSite::BuiltinCell)) && !exists(createConstantAtSite(ConstantAtSite::BuiltinCell).withForm(3)));
static_assert([] {
    for (GPRReg reg : allocatableGPRs) {
        if (!exists(createConstantAtSite(ConstantAtSite::String).withOperandRegister(reg)))
            return false;
    }
    return true;
}());

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
