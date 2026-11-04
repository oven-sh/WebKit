/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubFrontEnds.h"

namespace JSC { namespace AOT {

// Makes what RuntimeOperation::CopyObject makes. The operands are the arguments of the operation behind the instance, in the registers in which the operation takes them.
// Compiled code calls it in place of the operation: a copy that it makes itself goes through no call head.
constexpr StubKey copyObjectFrontEnd() { return StubKey(StubFamily::CopyObjectFrontEnd); }
// The same in front of RuntimeOperation::AssignToNewObject.
constexpr StubKey assignToNewObjectFrontEnd() { return StubKey(StubFamily::AssignToNewObjectFrontEnd); }

static_assert(exists(copyObjectFrontEnd()) && descriptorOf(StubFamily::CopyObjectFrontEnd).linkage == StubLinkage::CalledByCode);
static_assert(exists(assignToNewObjectFrontEnd()) && descriptorOf(StubFamily::AssignToNewObjectFrontEnd).linkage == StubLinkage::CalledByCode);

PreludeExit emitCopyDataPropertiesPrelude(StubAssembler&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
