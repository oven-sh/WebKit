/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"

namespace JSC { namespace AOT {

class StubAssembler;

enum class StackOverflowForm : uint8_t { InBody, AtPrologue };

constexpr StubKey handleException(bool preservesRegisters) { return StubKey(preservesRegisters ? StubFamily::HandleExceptionPreservingRegisters : StubFamily::HandleException); }
constexpr StubKey handleExceptionAtPrologue() { return StubKey(StubFamily::HandleExceptionAtPrologue); }
constexpr StubKey throwStackOverflow(bool isAtPrologue = false) { return StubKey(StubFamily::ThrowStackOverflow).withForm(static_cast<uint8_t>(isAtPrologue ? StackOverflowForm::AtPrologue : StackOverflowForm::InBody)); }
constexpr StubKey catchException() { return StubKey(StubFamily::Catch); }
constexpr StubKey rejectInFrame() { return StubKey(StubFamily::RejectInFrame); }

// Neither falls through. Both are entered with the stack as at the entry of a stub: the return address is the site that threw.
void emitUnwind(StubAssembler&);
void emitThrowStackOverflow(StubAssembler&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
