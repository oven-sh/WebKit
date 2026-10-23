/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

// AOTIRPrinter.h says why. The other half is AOTIRPrinterTypesForTestaot.cpp: file-local names of the two collide.
#if ENABLE(AOT) && !ASSERT_ENABLED

#define AOT_IR_PRINTER_FOR_TESTAOT 1

#include "AOTAnalysisDump.cpp"
#include "AOTIRPrinter.cpp"
#include "AOTPlaceTypeTestsDump.cpp"
#include "AOTSchemaEntryPrinter.cpp"
#include "AOTSlotPlanDump.cpp"

#endif // ENABLE(AOT) && !ASSERT_ENABLED
