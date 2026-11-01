/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Options.h"
#include <wtf/DataLog.h>
#include <wtf/StringPrintStream.h>

#if !OS(WINDOWS)
#include <errno.h>
#include <unistd.h>
#endif

namespace JSC::AOT {

inline void writeStageLine(const UTF8CString& line)
{
#if !OS(WINDOWS)
    if (Options::useOSLog() == OSLogType::None) {
        // dataLog() ends in printf() to a stream without a buffer, and how many writes that takes is the C library's choice.
        WTF::dataFile().atomically([&](PrintStream&) {
            std::span<const char8_t> rest = line.span();
            while (!rest.empty()) {
                ssize_t numberOfBytesWritten = ::write(STDERR_FILENO, rest.data(), rest.size());
                if (numberOfBytesWritten > 0)
                    rest = rest.subspan(static_cast<size_t>(numberOfBytesWritten));
                else if (!numberOfBytesWritten || errno != EINTR)
                    return;
            }
        });
        return;
    }
#endif
    dataLog(line);
}

// dataLogLn() writes each of its operands by itself, and whoever embeds the engine writes to the same descriptor under no lock of ours.
template<typename... Types>
void logStageLine(const Types&... values)
{
    StringPrintStream line;
    line.println(values...);
    writeStageLine(line.toUTF8CString());
}

} // namespace JSC::AOT
