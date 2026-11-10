/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStages.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTObfuscatedSites.h"
#include "AOTRemovableFeature.h"
#include "AOTRuntimeTable.h"
#include "AOTSort.h"
#include "IntlObject.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

template<typename Record>
static void setRecordsOfResultSection(CompileOutput& output, Wire::ResultSection section, const Vector<Record>& records)
{
    output.setRecords<Record>(section, records.span());
}

void writeDiagnosticSections(const Diagnostics& diagnostics, CompileOutput& output)
{
    const Vector<UserDiagnostic>& userDiagnostics = diagnostics.userDiagnostics();
    Vector<Wire::UserDiagnosticRecord> userDiagnosticRecords;
    userDiagnosticRecords.reserveInitialCapacity(userDiagnostics.size());
    for (size_t index = 0; index < userDiagnostics.size(); ++index) {
        const UserDiagnostic& diagnostic = userDiagnostics[index];
        userDiagnosticRecords.append(Wire::UserDiagnosticRecord {
            .kind = diagnostic.kind,
            .function = diagnostic.function.index(),
            .block = diagnostic.block.index(),
            .value = diagnostic.value.index(),
            .layout = diagnostic.layout.index(),
            .limit = diagnostic.limit,
            .count = diagnostic.count,
            .location = diagnostic.location,
            .message = output.appendText(diagnostics.messageOfUserDiagnostic(index)),
            .origin = diagnostic.origin,
            .siteKind = static_cast<uint32_t>(diagnostic.siteKind),
            .minimum = diagnostic.minimum,
            .maximum = diagnostic.maximum,
        });
    }
    setRecordsOfResultSection(output, Wire::ResultSection::UserDiagnostics, userDiagnosticRecords);

    const Vector<InternalError>& internalErrors = diagnostics.internalErrors();
    Vector<Wire::InternalErrorRecord> internalErrorRecords;
    internalErrorRecords.reserveInitialCapacity(internalErrors.size());
    for (size_t index = 0; index < internalErrors.size(); ++index) {
        const InternalError& error = internalErrors[index];
        internalErrorRecords.append(Wire::InternalErrorRecord {
            .kind = error.kind,
            .function = error.function.index(),
            .block = error.block.index(),
            .value = error.value.index(),
            .layout = error.layout.index(),
            .limit = error.limit,
            .count = error.count,
            .location = error.location,
            .message = output.appendText(diagnostics.messageOfInternalError(index)),
        });
    }
    setRecordsOfResultSection(output, Wire::ResultSection::InternalErrors, internalErrorRecords);
}

void writeStatisticSection(const Statistics& statistics, CompileOutput& output)
{
    Vector<uint64_t> values = statistics.values();
    RELEASE_ASSERT(values.size() == numberOfStatistics);
    setRecordsOfResultSection(output, Wire::ResultSection::Statistics, values);
}

static void writeEntryModuleIDSection(const Typed::Program& program, CompileOutput& output)
{
    Vector<uint32_t> ids;
    ids.reserveInitialCapacity(program.entries.size());
    for (const Typed::Entry& entry : program.entries)
        ids.append(entry.module.index() + 1);
    setRecordsOfResultSection(output, Wire::ResultSection::EntryModuleIds, ids);
}

static void writeUnseenCodeSection(const Typed::Facts& facts, CompileOutput& output)
{
    Vector<Wire::UnseenCodeRecord> records(facts.unseenCode());
    records.append(facts.loadsOfMachineCode());
    setRecordsOfResultSection(output, Wire::ResultSection::UnseenCode, records);
}

static void writeLocaleUseSection(const Typed::Program& program, const Typed::Facts& facts, CompileOutput& output)
{
    struct CanonicalUse {
        Wire::LocaleUseKind kind { Wire::LocaleUseKind::Unknown };
        Vector<uint8_t, 16> tag;
        BuiltinID builtin;
        Wire::Witness witness { Typed::noWitness };
    };

    Vector<CanonicalUse> uses;
    uses.reserveInitialCapacity(facts.localeUses().size());
    for (const Typed::LocaleUse& use : facts.localeUses()) {
        uses.append(CanonicalUse { use.kind, { }, use.builtin, use.witness });
        if (use.kind != Wire::LocaleUseKind::Constant)
            continue;
        String canonical;
        if (use.tag) {
            Vector<char16_t> buffer;
            StringView text = program.strings.viewOf(use.tag, buffer);
            if (isStructurallyValidLanguageTag(text))
                canonical = canonicalizeUnicodeLocaleID(text);
        }
        if (canonical.isEmpty())
            uses.last().kind = Wire::LocaleUseKind::Unknown;
        else
            uses.last().tag = Vector<uint8_t, 16>(byteCast<uint8_t>(canonical.utf8().span()));
    }

    auto compareKindAndTag = [](const CanonicalUse& a, const CanonicalUse& b) -> std::strong_ordering {
        if (a.kind != b.kind)
            return a.kind <=> b.kind;
        return std::lexicographical_compare_three_way(a.tag.begin(), a.tag.end(), b.tag.begin(), b.tag.end());
    };
    AOT::sort(uses, [&](const CanonicalUse& a, const CanonicalUse& b) {
        std::strong_ordering order = compareKindAndTag(a, b);
        if (order != 0)
            return order < 0;
        if (Typed::isLowerWitness(a.witness, b.witness))
            return true;
        if (Typed::isLowerWitness(b.witness, a.witness))
            return false;
        return a.builtin < b.builtin;
    });
    removeRepeatedElements(uses, [&](const CanonicalUse& a, const CanonicalUse& b) {
        return compareKindAndTag(a, b) == 0;
    });

    Vector<Wire::LocaleUseRecord> records;
    records.reserveInitialCapacity(uses.size());
    for (const CanonicalUse& use : uses) {
        Wire::LocaleUseRecord record { .kind = use.kind, .witness = use.witness };
        if (!use.tag.isEmpty()) {
            record.tag = output.appendText(use.tag.span());
            if (record.tag.length != use.tag.size())
                record.kind = Wire::LocaleUseKind::Unknown;
        }
        if (use.builtin)
            record.builtin = use.builtin.index();
        records.append(record);
    }
    setRecordsOfResultSection(output, Wire::ResultSection::LocalesUsed, records);
}

static void writeWorkerSections(const Typed::Program& program, const Typed::Facts& facts, CompileOutput& output)
{
    Vector<Wire::WorkerEntryRecord> entries;
    entries.reserveInitialCapacity(facts.workerEntries().size());
    for (const Typed::WorkerEntry& entry : facts.workerEntries()) {
        entries.append(Wire::WorkerEntryRecord {
            .path = entry.path ? output.appendText(program.string(entry.path)) : Wire::TextRange { },
            .witness = entry.witness,
        });
    }
    setRecordsOfResultSection(output, Wire::ResultSection::WorkerEntries, entries);

    Vector<Wire::WorkerStartRecord> startsWithoutName;
    for (const Wire::Witness& start : facts.workerStartsWithoutName())
        startsWithoutName.append(Wire::WorkerStartRecord { .witness = start });
    setRecordsOfResultSection(output, Wire::ResultSection::WorkerStartsWithoutName, startsWithoutName);

    const Vector<Wire::Witness>& workerURLs = facts.tables().program.workerURLs;
    Vector<Wire::WorkerURLRecord> urls;
    urls.reserveInitialCapacity(workerURLs.size());
    for (const Wire::Witness& witness : workerURLs)
        urls.append(Wire::WorkerURLRecord { .witness = witness });
    setRecordsOfResultSection(output, Wire::ResultSection::WorkerURLs, urls);
}

static void writeLackedFunctionSection(const Typed::Facts& facts, CompileOutput& output)
{
    Vector<Wire::MissingFunctionRecord> records;
    records.reserveInitialCapacity(facts.missingLibraryFunctions().size());
    for (auto function : facts.missingLibraryFunctions())
        records.append(Wire::MissingFunctionRecord { .function = function.index() });
    setRecordsOfResultSection(output, Wire::ResultSection::MissingLibraryFunctions, records);
}

static void writeLackedModuleSection(const Typed::Facts& facts, CompileOutput& output)
{
    const Typed::ProgramFactTables& tables = facts.tables().program;

    Vector<Wire::MissingModuleRecord> modules;
    modules.reserveInitialCapacity(tables.missingLibraryModules.size());
    for (auto module : tables.missingLibraryModules)
        modules.append(Wire::MissingModuleRecord { .module = module.index() });
    setRecordsOfResultSection(output, Wire::ResultSection::MissingLibraryModules, modules);
}

void writeFactSections(const Typed::Program& program, const Typed::Facts& facts, CompileOutput& output)
{
    writeEntryModuleIDSection(program, output);
    writeUnseenCodeSection(facts, output);
    output.setRecords<Wire::BuiltinUseRecord>(Wire::ResultSection::BuiltinsUsed, facts.builtinUses());
    writeLocaleUseSection(program, facts, output);
    writeWorkerSections(program, facts, output);
    writeLackedFunctionSection(facts, output);
    writeLackedModuleSection(facts, output);
}

void writeFeatureSection(const Typed::Facts& facts, const BuiltImage* image, CompileOutput& output)
{
    using FeatureSet = WTF::BitSet<removableFeatureIndexLimit>;

    Vector<Wire::FeatureUseRecord> records(facts.featureUses());
    if (image) {
        FeatureSet featuresOfImage;
        auto add = [&](RemovableFeature feature) {
            featuresOfImage.set(static_cast<size_t>(feature));
        };

        image->entriesUsed.forEachSetBit([&](size_t index) {
            if (std::optional<RemovableFeature> feature = featureOf(static_cast<Entry>(index)))
                add(*feature);
        });

        const ImageContents& contents = image->contents;
        if (contents.numberOfRegExpsWithCode || contents.numberOfRegExpsWithoutCode)
            add(RemovableFeature::RegExp);
        if (contents.numberOfRegExpsWithUnicodePropertyLookup)
            add(RemovableFeature::RegExpUnicodeProperties);
        if (contents.numberOfWebAssemblyModules)
            add(RemovableFeature::WebAssembly);

        FeatureSet featuresNamedByFacts;
        for (const Wire::FeatureUseRecord& record : records) {
            if ((record.kind == Wire::FeatureUseKind::Named || record.kind == Wire::FeatureUseKind::FoundByName) && record.feature < FeatureSet::size())
                featuresNamedByFacts.set(record.feature);
        }
        featuresOfImage.exclude(featuresNamedByFacts);

        featuresOfImage.forEachSetBit([&](size_t feature) {
            records.append(Wire::FeatureUseRecord {
                .feature = static_cast<uint32_t>(feature),
                .builtin = Wire::none,
                .witness = Typed::noWitness,
                .kind = Wire::FeatureUseKind::Named,
            });
        });
    }
    AOT::stableSort(records, { }, &Wire::FeatureUseRecord::feature);
    setRecordsOfResultSection(output, Wire::ResultSection::FeaturesUsed, records);
}

void writeObfuscatedSiteSection(const Typed::Program& program, const BuiltImage& image, BuildContext& build, CompileOutput& output)
{
    if (!build.options().obfuscates())
        return;

    ObfuscatedSites sites = collectObfuscatedSites(image.functions, program, build);
    std::span<const uint8_t> names = sites.names.span();
    Wire::TextRange appended = output.appendText(names);
    bool didAppend = appended.length == names.size();
    for (Wire::ObfuscatedSiteRecord& record : sites.records)
        record.name = didAppend ? Wire::TextRange { appended.offset + record.name.offset, record.name.length } : Wire::TextRange { };
    setRecordsOfResultSection(output, Wire::ResultSection::ObfuscatedSites, sites.records);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
