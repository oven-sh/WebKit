/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)

namespace JSC {

struct ExceptionEventLocation;

// Exception scope verification (Options::validateExceptionChecks()) crashes at the first exception check that is missing, which
// is right for code that is clean and no way to clean code that never was. With a log, each missing check is written to a
// file, once for each process, and the program goes on.
//
// A line has, separated by tabs: the scope that may have thrown (file:line, function), the scope that found the exception
// unchecked (the same), and the stacks of both moments, innermost frame first, as frame:returnAddress pairs in hexadecimal. The
// function that lacks the check is the innermost one that is in both stacks, and the return addresses below it tell which
// of its calls went unchecked, and how far it got. Tools/Scripts/unchecked-exceptions reads such a file.
class UncheckedExceptionLog {
public:
    static bool isEnabled();
    static void noteSimulatedThrow();
    static void report(const ExceptionEventLocation& throwLocation, const ExceptionEventLocation& location);
};

} // namespace JSC

#endif // ENABLE(EXCEPTION_SCOPE_VERIFICATION)
