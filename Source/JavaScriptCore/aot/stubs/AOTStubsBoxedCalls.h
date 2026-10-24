/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTStubDescriptor.h"
#include "AOTStubWords.h"
#include "AOTStubsCalls.h"

namespace JSC { namespace AOT {

using BoxedCallForm = BuiltinBoxedStub;

constexpr BoxedCallForm boxedCallFormOf(BuiltinBoxedStub stub) { return stub; }
bool acceptsArgumentCount(BoxedCallForm, unsigned count);

constexpr StubKey callBoxedBuiltin(BoxedCallForm form, unsigned count) { return StubKey(StubFamily::CallBoxedBuiltin).withForm(static_cast<uint8_t>(form)).withCount(count); }
constexpr StubKey weakMapSet() { return StubKey(StubFamily::WeakMapSet); }
constexpr StubKey stringStartsWith() { return StubKey(StubFamily::StringStartsWith); }
constexpr StubKey stringIndexOf() { return StubKey(StubFamily::StringIndexOf); }
constexpr StubKey findBoxedCall() { return StubKey(StubFamily::FindBoxedCall); }

// executable has the NativeExecutable of the callee.
void emitBoxedCallDispatch(StubAssembler&, const CallRegisters&, unsigned count, JumpList& hasNoBoxedCall);

static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms == numberOfBuiltinBoxedStubs);
static_assert(descriptorOf(StubFamily::CallBoxedBuiltin).minCount == BoxedCallEntry::minArgumentCount && descriptorOf(StubFamily::CallBoxedBuiltin).maxCount == BoxedCallEntry::maxArgumentCount);
static_assert(numberOfBuiltinBoxedStubs * 2 <= StubWords::numberOfBoxedCallEntries);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
