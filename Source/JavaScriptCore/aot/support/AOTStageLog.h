/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/DataLog.h>
#include <wtf/StringPrintStream.h>

namespace JSC::AOT {

// dataLogLn() writes each of its operands by itself, and whoever embeds the engine writes to the same descriptor under no lock of ours.
template<typename... Types>
void logStageLine(const Types&... values)
{
    StringPrintStream line;
    line.println(values...);
    dataLog(line.toUTF8CString());
}

} // namespace JSC::AOT
