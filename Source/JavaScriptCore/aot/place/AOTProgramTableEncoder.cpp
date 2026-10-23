/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTFlowResults.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxFieldOffset, hostArchitecture()) == 1u << SlotData::offsetBits);

TableEncoder::TableEncoder(const ProgramTableInputs& inputs, BuildContext& build)
    : inputs(inputs)
    , program(*inputs.pipeline.program)
    , flow(*inputs.pipeline.flow)
    , slots(*inputs.pipeline.slots)
    , placement(*inputs.pipeline.placement)
    , identifiers(build.identifierNumbering())
    , build(build)
    , writer(build)
{
    namedStrings.ensureSize(program.strings.size());
    stringsWithOtherUses.ensureSize(program.strings.size());
    writtenTables.ensureSize(numberOfProgramTables);
    uint32_t headerOffset = writer.allocate(sizeof(ProgramTables), 16);
    RELEASE_ASSERT(!headerOffset);
}

uint32_t TableEncoder::stringNumber(StringID id)
{
    uint32_t number = shownStringNumber(id);
    stringsWithOtherUses.quickSet(number);
    return number;
}

uint32_t TableEncoder::shownStringNumber(StringID id)
{
    if (!id)
        id = program.strings.emptyString();
    namedStrings.quickSet(id.index());
    return id.index();
}

uint32_t TableEncoder::stringConstantNumber(StringID id)
{
    stringNumber(id);
    if (std::optional<uint32_t> number = inputs.stringConstants.numberOf(id))
        return *number;
    fail(ProgramTableRule::StringWithoutConstantNumber);
    return 0;
}

uint32_t TableEncoder::identifierNumber(PropertyKeyID key) const
{
    if (std::optional<uint32_t> number = identifiers.numberOf(key))
        return *number;
    fail(ProgramTableRule::NameWithoutIdentifier);
    return 0;
}

uint32_t TableEncoder::identifierNumberOfText(StringID string) const
{
    if (std::optional<uint32_t> number = identifiers.numberOfText(string))
        return *number;
    fail(ProgramTableRule::NameWithoutIdentifier);
    return 0;
}

uint32_t TableEncoder::typeOffset(TypeID id) const
{
    if (uint32_t offset = typeOffsets[(id ? id : TypeTable::anything).index()])
        return offset;
    fail(ProgramTableRule::TypeNotEncoded);
    return typeOffsets[TypeTable::anything.index()];
}

uint32_t TableEncoder::dataNodeOf(DataID id) const
{
    uint32_t node = dataNodes[id.index()];
    if (node != UINT32_MAX)
        return node;
    fail(ProgramTableRule::DataNotEncoded);
    return 0;
}

FunctionID TableEncoder::findEntrypoint(FunctionID function) const
{
    if (!function)
        return { };
    return inputs.pipeline.derivedFunctions->find(derivedFunctionKey(FunctionRole::GenericEntrypoint, function));
}

uint32_t TableEncoder::entrypointOf(FunctionID function) const
{
    if (!function)
        return UINT32_MAX;
    FunctionID entrypoint = findEntrypoint(function);
    if (!entrypoint) {
        fail(ProgramTableRule::FunctionWithoutEntrypoint, { }, function);
        return UINT32_MAX;
    }
    return entrypoint.index();
}

uint32_t TableEncoder::checkedOffset(uint32_t offset) const
{
    if (offset <= SlotData::maxOffset)
        return offset;
    build.reportLimitExceeded(LimitID::MaxFieldOffset, offset);
    return 0;
}

void TableEncoder::fail(ProgramTableRule rule, LayoutID layout, FunctionID function) const
{
    build.diagnostics().add(InternalError { .kind = InternalErrorKind::ProgramTableCheckFailed, .function = function, .layout = layout, .count = static_cast<uint32_t>(rule) });
}

static void fillHeader(TableEncoder& encoder)
{
    ProgramTables& header = encoder.header;
    const ShapeTable& shapes = encoder.placement.shapes();
    ClosureFunctionRange closureFunctions = encoder.placement.closureFunctions();
    header.magic = ProgramTables::expectedMagic;
    header.flags = encoder.flow.programInstances() == ProgramInstances::AtMostOne ? 0 : ProgramTables::mayRunInMultipleInstances;
    if (!encoder.flow.mayReadLegacyRegExpStatics())
        header.flags |= ProgramTables::neverReadsLegacyRegExpStatics;
    header.instanceSizes = encoder.inputs.instanceSizes;
    header.numberOfShapes = shapes.numberOfShapes();
    header.firstEnvironmentShape = shapes.firstEnvironmentShape();
    header.closureFunctions = { closureFunctions.first.index(), closureFunctions.end.index(), closureFunctions.omittedWordCount };
}

BodyFactsForTables collectBodyFactsForTables(const Program& program, BuildContext& build)
{
    BodyFactsForTables facts;
    if (build.recordedOptions().policy() == MismatchPolicy::Report)
        facts.usedTypeSites = findUsedTypeSites(program, build.parallelLoops());
    facts.functionsThatOnlyReturn = findModuleBodiesThatOnlyReturn(program);
    facts.importSites = findImportSites(program, build.parallelLoops());
    return facts;
}

EncodedProgramTables encodeProgramTables(const ProgramTableInputs& inputs, BuildContext& build)
{
    struct TableGroup {
        void (*encode)(TableEncoder&);
        StatisticID statistic;
    };
    static constexpr TableGroup groups[] = {
        { encodeTypeTables, StatisticID::ProgramTableBytesTypes },
        { encodeDataTable, StatisticID::ProgramTableBytesData },
        { encodeLayoutTables, StatisticID::ProgramTableBytesLayouts },
        { encodePropertyRunTable, StatisticID::ProgramTableBytesLayouts },
        { encodeFieldTable, StatisticID::ProgramTableBytesFieldTable },
        { encodeMethodTable, StatisticID::ProgramTableBytesOther },
        { encodeStaticsTables, StatisticID::ProgramTableBytesStatics },
        { encodeFunctionTables, StatisticID::ProgramTableBytesFunctions },
        { encodeModuleTables, StatisticID::ProgramTableBytesModules },
        { encodeRequestTable, StatisticID::ProgramTableBytesRequests },
        { encodeConstantTables, StatisticID::ProgramTableBytesOther },
        { encodeGlobalBindingTable, StatisticID::ProgramTableBytesOther },
        // The last: the image builder takes it off the tables and puts it in front of the constants of the code.
        { encodeShapeSetBits, StatisticID::ProgramTableBytesLayouts },
    };

    TableEncoder encoder(inputs, build);
    build.statistics().count(StatisticID::ProgramTableBytesOther, encoder.writer.size());
    for (auto& group : groups) {
        size_t sizeBefore = encoder.writer.size();
        group.encode(encoder);
        if (build.diagnostics().hasErrors())
            return { };
        build.statistics().count(group.statistic, encoder.writer.size() - sizeBefore);
    }
    fillHeader(encoder);
    encoder.writer.at<ProgramTables>(0) = encoder.header;

    Vector<uint8_t> bytes = encoder.writer.take();
    BitVector stringsOnlyShown = encoder.namedStrings;
    stringsOnlyShown.exclude(encoder.stringsWithOtherUses);
    return { WTF::move(bytes), WTF::move(encoder.namedStrings), WTF::move(stringsOnlyShown) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
