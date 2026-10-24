/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

// AOTIRPrinter.h says why. The other half is AOTIRPrinterForTestaot.cpp.
#if ENABLE(AOT) && !ASSERT_ENABLED

#define AOT_IR_PRINTER_FOR_TESTAOT 1

#include "AOTIRPrinterTypes.cpp"
#include "AOTSchemaRecordPrinter.cpp"

#endif // ENABLE(AOT) && !ASSERT_ENABLED
