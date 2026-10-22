/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "GPRInfo.h"
#include <optional>
#include <span>

namespace JSC {

class CCallHelpers;

namespace B3 { namespace Air {

class Arg;
class BasicBlock;
class Code;
struct GenerationContext;
struct Inst;

JS_EXPORT_PRIVATE void emitFrameSetup(CCallHelpers&, GenerationContext&, const Inst& enterFrame);
// A single word may be given back with a pop into deadRegister. poppedReloads are those operands of a LeaveFrame.
JS_EXPORT_PRIVATE void emitFrameTeardown(CCallHelpers&, const Code&, std::optional<GPRReg> deadRegister = std::nullopt, std::span<const Arg> poppedReloads = { });
// What a frame setup allocates below the frame record, or below the return address where there is no record.
JS_EXPORT_PRIVATE unsigned numberOfWordsAllocatedAtFrameSetup(const Code&);
JS_EXPORT_PRIVATE void emitCalleeSaveRestore(CCallHelpers&, const Code&);
// Of the teardown in front of a return.
JS_EXPORT_PRIVATE unsigned maxBytesOfFrameTeardown(const Code&);

// A register that holds nothing behind the LeaveFrame at that place.
JS_EXPORT_PRIVATE std::optional<GPRReg> registerDeadBehindFrameTeardown(const Code&, BasicBlock*, unsigned indexOfLeaveFrame);

inline constexpr GPRReg registerDeadAtReturns = GPRInfo::argumentGPR3;
static_assert(registerDeadAtReturns != GPRInfo::returnValueGPR && registerDeadAtReturns != GPRInfo::returnValueGPR2);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
