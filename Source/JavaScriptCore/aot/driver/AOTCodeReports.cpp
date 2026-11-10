/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStages.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompiledCode.h"
#include "AOTFormsSaid.h"
#include "AOTIRProgram.h"
#include "AOTImageBuilder.h"
#include "AOTLinkedFunctions.h"
#include "AOTOpcode.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "Options.h"
#include <algorithm>
#include <limits>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static_assert(sizeof(Wire::ModuleBytesRecord::codeBytes) == sizeof(uint64_t) && sizeof(Wire::ModuleBytesRecord::tableBytes) == sizeof(uint64_t) && sizeof(Wire::FormBytesRecord::bytes) == sizeof(uint64_t));

namespace {

struct CodeReportFormTotal {
    uint64_t count { 0 };
    uint64_t bytes { 0 };
    uint64_t schemaMismatchCount { 0 };
};

using CodeReportFormTotals = UncheckedKeyHashMap<uint64_t, CodeReportFormTotal, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

struct CodeReportForm {
    explicit CodeReportForm(uint64_t key)
        : opcode(static_cast<Typed::Opcode>(key >> 32))
        , state(static_cast<Typed::StateOfKnowledge>(key >> 16 & 0xFFFF))
        , place(static_cast<Typed::Place>(key >> 8 & 0xFF))
        , form(static_cast<Wire::CodeForm>(key >> 1 & 0x7F))
        , isNamed(!(key & 1))
    {
    }

    Typed::Opcode opcode;
    Typed::StateOfKnowledge state;
    Typed::Place place;
    Wire::CodeForm form;
    bool isNamed;
};

} // anonymous namespace

static uint64_t keyOfCodeReportForm(const BytesOfForm& entry)
{
    return static_cast<uint64_t>(entry.opcode) << 32 | static_cast<uint64_t>(entry.state) << 16 | static_cast<uint64_t>(entry.place) << 8 | static_cast<uint64_t>(entry.form) << 1 | !entry.isNamed;
}

static Vector<Wire::ModuleBytesRecord> measureCodeReportRunsOfModules(const Typed::Program& program, const BuiltImage& built, const ParallelLoops& loops)
{
    const LinkedFunctions& units = built.functions;
    return collectInParallel<Wire::ModuleBytesRecord>(loops, static_cast<unsigned>(units.size()), [&](unsigned position, Vector<Wire::ModuleBytesRecord>& runs) {
        const CompiledCode& code = units[position];
        Typed::ModuleID writtenIn = program.at(code.unit.function).module;
        if (!writtenIn)
            return;
        uint32_t module = writtenIn.index();
        if (runs.isEmpty() || runs.last().module != module)
            runs.append(Wire::ModuleBytesRecord { .module = module });
        Wire::ModuleBytesRecord& run = runs.last();
        ++run.functions;
        run.codeBytes += code.body.bytes.size();
        run.tableBytes += built.sideTableBytes[position];
    });
}

static void writeCodeReportBytesByModule(const Typed::Program& program, const BuiltImage& built, const ParallelLoops& loops, CompileOutput& output)
{
    RELEASE_ASSERT(built.sideTableBytes.size() == built.functions.size());

    Vector<Wire::ModuleBytesRecord> records;
    records.fill(Wire::ModuleBytesRecord { }, program.modules.size());
    for (const Wire::ModuleBytesRecord& run : measureCodeReportRunsOfModules(program, built, loops)) {
        Wire::ModuleBytesRecord& record = records[run.module];
        record.module = run.module;
        record.functions += run.functions;
        record.codeBytes += run.codeBytes;
        record.tableBytes += run.tableBytes;
    }
    records.removeAllMatching([](const Wire::ModuleBytesRecord& record) {
        return !record.functions;
    });
    output.setRecords(Wire::ResultSection::BytesByModule, records.span());
}

static CodeReportFormTotals sumCodeReportBytesByForm(const LinkedFunctions& units, const ParallelLoops& loops)
{
    Vector<CodeReportFormTotals> totalsOfThreads(loops.width());

    ChunksOfIndices chunks = divideIntoChunks(static_cast<unsigned>(units.size()));
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned chunk, unsigned worker) {
        CodeReportFormTotals& totals = totalsOfThreads[worker];
        for (unsigned position = chunks.firstIndexOf[chunk]; position < chunks.firstIndexOf[chunk + 1]; ++position) {
            for (const BytesOfForm& entry : units[position].measurements->bytesByForm) {
                CodeReportFormTotal& total = totals.add(keyOfCodeReportForm(entry), CodeReportFormTotal { }).iterator->value;
                total.count += entry.count;
                total.bytes += entry.bytes;
                total.schemaMismatchCount += entry.schemaMismatchCount;
            }
        }
    });

    CodeReportFormTotals totals = WTF::move(totalsOfThreads[0]);
    for (size_t worker = 1; worker < totalsOfThreads.size(); ++worker) {
        for (const auto& [key, totalOfThread] : totalsOfThreads[worker]) {
            CodeReportFormTotal& total = totals.add(key, CodeReportFormTotal { }).iterator->value;
            total.count += totalOfThread.count;
            total.bytes += totalOfThread.bytes;
            total.schemaMismatchCount += totalOfThread.schemaMismatchCount;
        }
    }
    return totals;
}

static void printCodeReportNameOfForm(PrintStream& out, const CodeReportForm& row)
{
    out.print(nameOf(row.opcode));
    if (row.state != Typed::StateOfKnowledge::Always)
        out.print(" ", nameOf(row.state));
    if (row.place != Typed::Place::Anywhere)
        out.print(" in ", nameOf(row.place));
}

static bool codeReportStopsAtFormsThatAreNotNamed()
{
    return ASSERT_ENABLED || ASAN_ENABLED || Options::verifyAOTMachineCode();
}

static void writeCodeReportBytesByForm(const LinkedFunctions& units, BuildContext& context, CompileOutput& output)
{
    CodeReportFormTotals totals = sumCodeReportBytesByForm(units, context.parallelLoops());

    Vector<uint64_t> keys = copyToVector(totals.keys());
    AOT::sort(keys, [](uint64_t a, uint64_t b) {
        Typed::Opcode opcodeOfA = CodeReportForm(a).opcode;
        Typed::Opcode opcodeOfB = CodeReportForm(b).opcode;
        if (opcodeOfA != opcodeOfB)
            return std::ranges::lexicographical_compare(nameOf(opcodeOfA).span(), nameOf(opcodeOfB).span());
        return a < b;
    });

    Vector<Wire::FormBytesRecord> records;
    StringPrintStream formsThatAreNotNamed;
    for (uint64_t key : keys) {
        CodeReportForm row(key);
        StringPrintStream name;
        printCodeReportNameOfForm(name, row);
        if (!row.isNamed) {
            formsThatAreNotNamed.print(formsThatAreNotNamed.length() ? ", " : "", name.toString(), " ", nameOf(Typed::formReportedAs(row.form)));
            name.print(" (not named in the schema)");
        }
        const CodeReportFormTotal& total = totals.find(key)->value;
        if (row.isNamed && total.schemaMismatchCount)
            name.print(" (the code of ", total.schemaMismatchCount, " shows another form)");
        if (row.place != Typed::Place::Anywhere && row.place != Typed::Place::Loop)
            name.print(" (the design lets a loop alone pick a form)");
        ASCIICString text = name.toASCIICString();
        records.append(Wire::FormBytesRecord {
            .operation = output.appendText(byteCast<uint8_t>(text.span())),
            .form = row.form,
            .count = static_cast<uint32_t>(std::min<uint64_t>(total.count, std::numeric_limits<uint32_t>::max())),
            .bytes = total.bytes,
        });
    }
    output.setRecords(Wire::ResultSection::BytesByForm, records.span());

    if (formsThatAreNotNamed.length() && codeReportStopsAtFormsThatAreNotNamed())
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::OpcodeWithoutCode, .detail = makeString("emitters say forms that the schema does not name: "_s, formsThatAreNotNamed.toString()) });
}

void writeCodeReportSections(const Typed::Program& program, const BuiltImage& built, BuildContext& context, CompileOutput& output)
{
    if (built.image.isEmpty())
        return;

    const ParallelLoops& loops = context.parallelLoops();
    ReportSet reports = context.options().reports;
    writeCodeReportBytesByModule(program, built, loops, output);
    if (hasFlag(reports, ReportSet::Flow))
        writeCodeReportBytesByForm(built.functions, context, output);
    if (hasFlag(reports, ReportSet::Symbols))
        output.setSection(Wire::ResultSection::Symbols, encodeSymbols(program, built, context));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
