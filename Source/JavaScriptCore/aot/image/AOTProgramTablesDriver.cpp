/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTThreads.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static_assert(!static_cast<unsigned>(TablePartID::Header), "The run time reads the header at the start of the tables.");

using TableGroupNamer = void (*)(const TableInputs&, const GatheredMentions&, NamesOfTables&);

static ASCIILiteral phaseNameOfTableGroup(TablePartID part)
{
    switch (part) {
    case TablePartID::Header:
        return "image: header of program tables"_s;
    case TablePartID::Types:
        return "image: type tables"_s;
    case TablePartID::Data:
        return "image: data table"_s;
    case TablePartID::Layouts:
        return "image: layout tables"_s;
    case TablePartID::FieldTable:
        return "image: field table"_s;
    case TablePartID::Statics:
        return "image: statics tables"_s;
    case TablePartID::Functions:
        return "image: function tables"_s;
    case TablePartID::FunctionSources:
        return "image: function sources"_s;
    case TablePartID::Modules:
        return "image: module tables"_s;
    case TablePartID::Requests:
        return "image: request tables"_s;
    case TablePartID::Constants:
        return "image: constant tables"_s;
    case TablePartID::ShapeSets:
        return "image: shape sets"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr bool runsBehindTypesAndData(TablePartID part)
{
    return part != TablePartID::Types && part != TablePartID::Data && part != TablePartID::FunctionSources;
}

static bool checksJoinedProgramTables()
{
    return ASSERT_ENABLED || ASAN_ENABLED;
}

NamesOfTables nameWhatTablesName(const TableInputs& inputs, const GatheredMentions& mentions, BuildContext& build)
{
    std::array<std::optional<NamesOfTables>, numberOfTableParts> namesOfGroups;
    auto name = [&](TablePartID part, TableGroupNamer namer) {
        CurrentWorkScope currentWork(phaseNameOfTableGroup(part));
        StepTimes timesOfGroup(build, phaseNameOfTableGroup(part));
        namer(inputs, mentions, namesOfGroups[static_cast<unsigned>(part)].emplace(inputs.program));
        timesOfGroup.didFinish("name"_s);
    };
    runLanes(build.parallelLoops(), {
        [&] { name(TablePartID::Modules, nameWhatModuleTablesName); },
        [&] { name(TablePartID::Functions, nameWhatFunctionTablesName); },
        [&] { name(TablePartID::Layouts, nameWhatLayoutTablesName); },
        [&] { name(TablePartID::Types, nameWhatTypeTablesName); },
        [&] { name(TablePartID::Data, nameWhatDataTableNames); },
        [&] { name(TablePartID::Statics, nameWhatStaticsTablesName); },
        [&] { name(TablePartID::Constants, nameWhatConstantTablesName); },
        [&] { name(TablePartID::Requests, nameWhatRequestTablesName); },
    });

    NamesOfTables merged(inputs.program);
    for (const std::optional<NamesOfTables>& names : namesOfGroups) {
        if (names)
            merged.merge(*names);
    }
    nameVariablesThatHeapSnapshotsShow(inputs, mentions, build.parallelLoops(), merged);
    return merged;
}

static ProgramTables makeHeaderOfProgramTables(const TableInputs& inputs, const ImageNumbers& numbers)
{
    const Typed::ShapeTable& shapes = inputs.plan.shapes();
    const FunctionNumbers& functions = numbers.functions();
    uint32_t firstClosureFunction = functions.firstClosureFunction().index();

    ProgramTables header { };
    header.magic = ProgramTables::expectedMagic;
    if (!inputs.facts.hasOneInstance())
        header.flags |= ProgramTables::mayRunInMultipleInstances;
    if (!inputs.facts.mayReadLegacyRegExpStatics())
        header.flags |= ProgramTables::neverReadsLegacyRegExpStatics;
    header.instanceSizes = numbers.instanceSizes();
    header.numberOfShapes = shapes.numberOfShapes();
    header.firstEnvironmentShape = shapes.firstEnvironmentShape();
    header.numberOfLayoutIndices = static_cast<uint32_t>(inputs.program.layouts.size());
    header.numberOfFieldTableRows = shapes.firstEnvironmentShape() - 1;
    header.numberOfFieldTableColumns = numbers.identifiers().countWithFieldTableColumn();
    header.numberOfClosureCells = numbers.numberOfClosureCells();
    header.sizeOfStructuresOfHeirs = numbers.sizeOfStructuresOfHeirs();
    header.closureFunctions = { firstClosureFunction, functions.endOfClosureFunctions().index(), firstClosureFunction };
    return header;
}

static Vector<uint32_t> typeOffsetsInJoinedTables(std::span<const TablePart> parts, const FirstTableParts& firstParts)
{
    uint32_t start = static_cast<uint32_t>(startInJoinedTables(parts, TablePartID::Types));
    return firstParts.typeOffsets.map([start](uint32_t offsetInPart) -> uint32_t {
        return offsetInPart ? start + offsetInPart : 0;
    });
}

Vector<uint8_t> encodeProgramTables(const TableInputs& inputs, ImageNumbers& numbers, std::array<uint64_t, numberOfTableParts>& bytesOfParts, BuildContext& build)
{
    static constexpr ASCIILiteral nameOfSteps = "image: program tables"_s;
    CurrentWorkScope workOfDriver(nameOfSteps);
    StepTimes times(build, nameOfSteps);
    Diagnostics& diagnostics = build.diagnostics();
    bytesOfParts.fill(0);

    FirstTableParts firstParts;
    Vector<TableEncoder, numberOfTableParts> encoders;
    for (unsigned index = 0; index < numberOfTableParts; ++index) {
        auto part = static_cast<TablePartID>(index);
        encoders.constructAndAppend(inputs, numbers, runsBehindTypesAndData(part) ? &firstParts : nullptr, part, build);
    }
    auto encode = [&](TablePartID part, const auto& encodeGroup) {
        CurrentWorkScope currentWork(phaseNameOfTableGroup(part));
        StepTimes timesOfGroup(build, phaseNameOfTableGroup(part));
        encodeGroup(encoders[static_cast<unsigned>(part)]);
        timesOfGroup.didFinish("encode"_s);
    };

    runLanes(build.parallelLoops(), {
        [&] {
            encode(TablePartID::Types, [&](TableEncoder& encoder) { encodeTypeTables(encoder, firstParts); });
            times.didFinish("encode types"_s);
            if (diagnostics.hasErrors())
                return;
            if (firstParts.typeOffsets.size() != numbers.typesNeededAtRunTime().size()) {
                encoders[static_cast<unsigned>(TablePartID::Types)].fail("TypeOffsetsDoNotMatchTypesNeededAtRunTime"_s);
                return;
            }
            runLanes(build.parallelLoops(), {
                [&] {
                    firstParts.fieldsOfLayoutRecords = collectFieldsOfLayoutRecords(encoders[static_cast<unsigned>(TablePartID::Layouts)]);
                    times.didFinish("collect the fields of layout records"_s);
                    if (diagnostics.hasErrors())
                        return;
                    runLanes(build.parallelLoops(), {
                        [&] { encode(TablePartID::Layouts, encodeLayoutTables); },
                        [&] { encode(TablePartID::Functions, encodeFunctionProgramTables); },
                        [&] { encode(TablePartID::FieldTable, encodeFieldTable); },
                        [&] { encode(TablePartID::Modules, encodeModuleTables); },
                        [&] { encode(TablePartID::Constants, encodeConstantTables); },
                        [&] { encode(TablePartID::Statics, [](TableEncoder& encoder) { encodeStaticsTables(encoder); encodeMethodTableDependents(encoder); }); },
                        [&] { encode(TablePartID::ShapeSets, encodeShapeSetBits); },
                    });
                },
                [&] {
                    encode(TablePartID::Data, [&](TableEncoder& encoder) { encodeDataTable(encoder, firstParts); });
                    if (!diagnostics.hasErrors())
                        encode(TablePartID::Requests, encodeRequestTables);
                },
            });
            times.didFinish("encode the other groups"_s);
        },
        [&] {
            if (!diagnostics.hasErrors())
                encode(TablePartID::FunctionSources, encodeFunctionSources);
        },
    });
    times.didFinish("the end of the function sources lane"_s);
    if (diagnostics.hasErrors())
        return { };

    Vector<TablePart, numberOfTableParts> parts;
    for (TableEncoder& encoder : encoders)
        parts.append(WTF::move(encoder.part));
    ProgramTables header = makeHeaderOfProgramTables(inputs, numbers);
    Vector<uint8_t> tables = joinTableParts(parts.mutableSpan(), header, build);
    times.didFinish("join"_s);
    if (diagnostics.hasErrors())
        return { };

    for (unsigned index = 0; index < numberOfTableParts; ++index)
        bytesOfParts[index] = parts[index].size();
    numbers.setOffsetsOfTypes(typeOffsetsInJoinedTables(parts.span(), firstParts));
    numbers.setNodesOfData(WTF::move(firstParts.dataNodes));
    destroyInBackground(build.backgroundJobs(), WTF::move(parts));
    destroyInBackground(build.backgroundJobs(), WTF::move(firstParts.fieldsOfLayoutRecords));
    times.didFinish("hand on the parts"_s);

    if (checksJoinedProgramTables()) {
        checkProgramTables(inputs, numbers, tables.span(), build);
        times.didFinish("check"_s);
        if (diagnostics.hasErrors())
            return { };
    }
    return tables;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
