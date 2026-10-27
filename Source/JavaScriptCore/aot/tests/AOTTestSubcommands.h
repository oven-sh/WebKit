/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr int exitCodeForSuccess = 0;
inline constexpr int exitCodeForFailure = 1;
inline constexpr int exitCodeForInvalidCommandLine = 2;

int runPrintSubcommand(std::span<const String> arguments);
int runCompileSubcommand(std::span<const String> arguments);
int runFuzzReaderSubcommand(std::span<const String> arguments);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
