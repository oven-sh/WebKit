/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCatalog.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptions.h"
#include "AOTFieldTable.h"
#include "AOTFlowResults.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRegExpRequests.h"
#include "AOTRegExpSummary.h"
#include "AOTRemovableFeature.h"
#include "AOTSlotPlan.h"
#include "AOTStringUses.h"
#include "AOTWasmModules.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <span>

namespace JSC { namespace AOT {

StubOptions StubOptions::from(const RecordedOptions& recorded, const Typed::Program& program, const Typed::FlowResults& flow)
{
    return {
        .useWideReferences = recorded.useWideReferences(),
        .forceGCSlowPaths = recorded.has(TestingSwitch::ForceGCSlowPaths),
        .imageHasWebAssembly = mayHaveWebAssemblyModules(program),
        .hasOneInstance = flow.programInstances() == Typed::ProgramInstances::AtMostOne,
        .callsTrapsOfProxies = flow.mayRunUnseenCode() || !flow.loadsOfMachineCode().empty() || !flow.usesOf(Builtins::Proxy).isEmpty() || !flow.usesOf(Builtins::ProxyRevocable).isEmpty(),
        .matchesRegExps = flow.mayRunUnseenCode() || std::ranges::any_of(flow.featureUses(), [](const auto& use) { return use.feature == static_cast<uint32_t>(RemovableFeature::RegExp); }),
        .validates = recorded.validates,
        .checksProofs = recorded.validates || recorded.has(TestingSwitch::CheckProofsAtRunTime),
        .countsAllocations = recorded.countsBranches,
        .isPositionIndependent = recorded.form() == ImageForm::PositionIndependent,
        .layoutSeed = recorded.layoutSeed,
        .targetFeatures = CPUFeatureSet::fromBits(static_cast<uint32_t>(recorded.targetFeatures)),
    };
}

RegExpLiteralConstants StubPolicy::regExpLiteral(std::optional<uint32_t> record, bool isGlobal) const
{
    RegExpLiteralConstants constants;
    if (record && *record < regExpLiterals.size())
        constants = regExpLiterals[*record];
    constants.isGlobal = isGlobal;
    constants.usesLastIndex = constants.usesLastIndex || isGlobal;
    return constants;
}

StubPolicy chooseStubPolicy(const KeyUses& uses, const IdentifierNumbering& numbering, BuildContext& build)
{
    static constexpr std::array accessesWithBoundStubs { Typed::Access::Read, Typed::Access::Store, Typed::Access::Define };

    uint64_t minUses = build.limit(LimitID::MinUsesForOwnStub);
    StubPolicy policy;
    for (size_t key = 0; key < uses.byKey.size(); ++key) {
        const KeyUse& use = uses.byKey[key];
        for (Typed::Access access : accessesWithBoundStubs) {
            size_t accessIndex = static_cast<size_t>(access);
            if (use.usesByAccess[accessIndex] < minUses)
                continue;
            if (std::optional<uint32_t> number = numbering.numberOf(PropertyKeyID(key)); number && RowDisplacementTable::hasColumn(*number))
                policy.namesWithOwnStubByAccess[accessIndex].set(*number);
        }
    }
    return policy;
}

void addExpectedFieldTableEntries(StubPolicy& policy, const Typed::Program& program, const Typed::SlotPlan& slots, const IdentifierNumbering& numbering)
{
    const BitVector& names = policy.namesWithOwnStub(Typed::Access::Read);
    if (names.isEmpty())
        return;
    using Counts = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    Counts layoutsWithName;
    Counts layoutsWithEntry;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Typed::LayoutID id(index);
        if (!slots.holdsCompiledObjects(id))
            continue;
        const auto& fields = program.at(id).fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            std::optional<uint32_t> name = numbering.numberOf(fields[field].key);
            if (!name || !names.get(*name))
                continue;
            layoutsWithName.add(*name, 0).iterator->value++;
            const Typed::PlacedSlot& slot = slots.slot(id, field);
            bool isValue = slot.representation == SlotRepresentation::JSValue || slot.representation == SlotRepresentation::Cell;
            uint32_t word = slot.offset / sizeof(EncodedJSValue);
            if (!slot.hasStorage() || !isValue || slot.offset % sizeof(EncodedJSValue) || word > FieldTableEntry::maxOperand)
                continue;
            FieldTableEntry::Form form = slot.isOutOfLine() ? FieldTableEntry::Form::OutOfLineValue : FieldTableEntry::Form::InlineValue;
            layoutsWithEntry.add(FieldTableEntry::create(*name, form, word).bits, 0).iterator->value++;
        }
    }
    // A wrong guess costs two instructions. A right one saves three in the cell and nineteen in the Extension.
    for (auto [bits, count] : layoutsWithEntry) {
        uint32_t name = bits >> FieldTableEntry::nameShift;
        if (count * 2 > layoutsWithName.get(name))
            policy.expectedFieldTableEntries.add(name, bits);
    }
}

void chooseOrderOfFieldTableForms(StubPolicy& policy, const Typed::Program& program, const Typed::SlotPlan& slots, const KeyUses& uses)
{
    // By key: in how many layouts the field is a JSValue in the cell, a JSValue in the Extension, a double in the cell, anything else.
    enum Place : uint8_t { InCell, InExtension, DoubleInCell, Elsewhere, NumberOfPlaces };
    Vector<std::array<uint32_t, NumberOfPlaces>> layoutsByKey(FillWith { }, uses.byKey.size(), std::array<uint32_t, NumberOfPlaces> { });
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Typed::LayoutID id(index);
        if (!slots.holdsCompiledObjects(id))
            continue;
        const auto& fields = program.at(id).fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            size_t key = fields[field].key.index();
            const Typed::PlacedSlot& slot = slots.slot(id, field);
            if (key >= layoutsByKey.size() || !slot.hasStorage())
                continue;
            bool isValue = slot.representation == SlotRepresentation::JSValue || slot.representation == SlotRepresentation::Cell;
            bool isDoubleInCell = traitsOf(slot.representation).valueKind == SlotValueKind::Double && !slot.isOutOfLine();
            ++layoutsByKey[key][isDoubleInCell ? DoubleInCell : !isValue ? Elsewhere : slot.isOutOfLine() ? InExtension : InCell];
        }
    }
    // A place that reads the name counts for each form by the share of the layouts that have it.
    std::array<double, NumberOfPlaces> reads { };
    for (size_t key = 0; key < layoutsByKey.size(); ++key) {
        const auto& layouts = layoutsByKey[key];
        double total = layouts[InCell] + layouts[InExtension] + layouts[DoubleInCell] + layouts[Elsewhere];
        if (!total)
            continue;
        // A read under a key that the build does not know may ask for any name, so each name counts once by itself.
        const auto& usesOfKey = uses.byKey[key].usesByAccess;
        double places = 1 + usesOfKey[static_cast<size_t>(Typed::Access::Read)] + usesOfKey[static_cast<size_t>(Typed::Access::Has)];
        for (unsigned place = 0; place < NumberOfPlaces; ++place)
            reads[place] += places * (layouts[place] / total);
    }
    // It saves two tests on the one side and costs one on the other.
    policy.readsTestOutOfLineValuesSecond = 2 * reads[InExtension] > reads[Elsewhere] + reads[DoubleInCell];
    // A double is the last form asked for: that saves six tests and costs one.
    policy.readsTestDoublesSecond = 6 * reads[DoubleInCell] > reads[Elsewhere] + reads[InExtension];
}

void addRegExpLiterals(StubPolicy& policy, const RegExpRequests& requests, bool legacyStaticsMayBeRead, BuildContext& context)
{
    std::span<const RegExpRequest> allRequests = requests.all();
    policy.regExpLiterals.grow(allRequests.size());
    for (size_t record = 0; record < allRequests.size(); ++record) {
        const RegExpRequest& request = allRequests[record];
        RegExpLiteralConstants& constants = policy.regExpLiterals[record];
        const RegExpSummary* summary = context.regExpSummaries().summaryIfComputed(request.pattern, request.flags);
        if (summary && summary->metadata == RegExpMetadata::Known) {
            constants.minimumSize = summary->minimumSize;
            constants.numberOfGroups = summary->numSubpatterns;
            constants.hasNamedGroups = summary->hasNamedGroups();
        }
        // The flags of a request are the ones that change the code, and g is none of them.
        constants.usesLastIndex = request.flags.contains(Yarr::Flags::Sticky);
        constants.isGlobal = false;
        constants.matchesAtCodePointBoundaries = request.flags.containsAny({ Yarr::Flags::Unicode, Yarr::Flags::UnicodeSets });
        constants.recordsMatch = legacyStaticsMayBeRead;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
