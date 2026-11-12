/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"
#include "AOTMachineSlots.h"

namespace JSC { namespace AOT { namespace Machine {

bool validate(Procedure&);
bool validateBehindBlockPasses(Procedure&);
bool checkVectorsAcrossCalls(Procedure&);
JS_EXPORT_PRIVATE bool checkThatDefinitionsDominateUses(Procedure&);

bool checkAllocation(Procedure&);
bool checkSavesAtHeads(Procedure&);

bool checkFrame(Procedure&);

bool checkReferences(Procedure&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
