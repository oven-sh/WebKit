/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubFrontEnds.h"

namespace JSC { namespace AOT {

constexpr StubKey copyObjectFrontEnd() { return StubKey(StubFamily::CopyObjectFrontEnd); }
constexpr StubKey assignToNewObjectFrontEnd() { return StubKey(StubFamily::AssignToNewObjectFrontEnd); }

static_assert(exists(copyObjectFrontEnd()) && descriptorOf(StubFamily::CopyObjectFrontEnd).linkage == StubLinkage::CalledByCode);
static_assert(exists(assignToNewObjectFrontEnd()) && descriptorOf(StubFamily::AssignToNewObjectFrontEnd).linkage == StubLinkage::CalledByCode);

PreludeExit emitCopyDataPropertiesPrelude(StubAssembler&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
