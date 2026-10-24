/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHost.h"

#if ENABLE(AOT) && !defined(BUN_HARDENED)

#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTModuleTable.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/FilePrintStream.h>
#include <wtf/HexNumber.h>
#include <wtf/Lock.h>
#include <wtf/ProcessID.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

struct PerfMapSymbol {
    size_t offset;
    String name;
};

} // anonymous namespace

static String symbolNameOf(const Image& image, uint32_t function)
{
    if (image.isObfuscated())
        return makeString('f', function);
    const ProgramTables& tables = image.programTables();
    FunctionDescription description(image.rootFunctionOf(function));
    String name = description.codeType() == ModuleCode ? String("(module)"_s) : writtenFunctionName(description.sourceFunction());
    if (name.isEmpty())
        name = "(anonymous)"_s;
    auto position = image.positionOf(function, CallSiteIndex(0));
    if (!position || !position->source)
        return name;
    return makeString(name, ' ', ModuleTable(tables).pathTextForLineColumns(position->source), ':', position->lineColumn.line);
}

void writePerfMap()
{
    const Image* image = Image::current();
    if (!image)
        return;
    // Each entry point of an executable has an image of its own, and a thread that starts one adds its functions to the file.
    static Lock lock;
    static bool hasWrittenInThisProcess;
    Locker locker { lock };
    auto file = FilePrintStream::open(makeString("/tmp/perf-"_s, getCurrentProcessID(), ".map"_s).utf8(), std::exchange(hasWrittenInThisProcess, true) ? "a"_s : "w"_s);
    if (!file)
        return;
    auto& header = image->header();
    const uint8_t* code = image->code().data();
    Vector<PerfMapSymbol> symbols;
    for (uint32_t function = 0; function < header.counts.functions; ++function) {
        if (!image->sizeOfCodeOf(function))
            continue;
        symbols.append({ static_cast<size_t>(image->codeOf(function) - code), symbolNameOf(*image, function) });
    }
    for (unsigned copy = 0; copy < image->sharedCodeCopies().size(); ++copy)
        symbols.append({ static_cast<size_t>(image->sharedCodeCopy(copy) - code), String("(stubs of compiled code)"_s) });
    AOT::sort(symbols, { }, &PerfMapSymbol::offset);
    for (size_t i = 0; i < symbols.size(); ++i) {
        size_t end = i + 1 < symbols.size() ? symbols[i + 1].offset : header.code.functionsEnd;
        file->println(hex(reinterpret_cast<uintptr_t>(code) + symbols[i].offset, Lowercase), " ", hex(end - symbols[i].offset, Lowercase), " ", symbols[i].name);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
