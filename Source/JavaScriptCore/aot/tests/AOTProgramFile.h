/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/PrintStream.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

struct ProgramFile {
    enum class Kind : uint8_t { Bytes, Text };

    Kind kind { Kind::Text };
    Vector<uint8_t> bytes;
    String text;
};

ProgramFile::Kind programKindOf(std::span<const uint8_t>);
std::optional<ProgramFile> readProgramFile(const String& path, PrintStream& errors);
std::unique_ptr<Program> createProgram(const ProgramFile&, BuildContext&);

void printDiagnostics(const Diagnostics&, PrintStream&);

Vector<uint8_t> encodeProgramText(StringView text, BuildContext&);

inline constexpr ASCIILiteral ownLibraryLine = "# LIBRARY: own"_s;

String standInLibraryText(unsigned libraryFunction);
String withStandInLibrary(const String& programText);

struct ReaderFuzzRequest {
    uint32_t seed { 1 };
    unsigned count { 1000 };
    Vector<String> programPaths;
};

unsigned fuzzReader(const ReaderFuzzRequest&, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
