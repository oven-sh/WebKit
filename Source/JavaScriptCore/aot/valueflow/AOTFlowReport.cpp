/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowReport.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTCFG.h"
#include "AOTCheckCause.h"
#include "AOTFlowReportInternal.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeTestReport.h"
#include "IntlObject.h"

namespace JSC { namespace AOT { namespace Typed {

static void appendLocaleUses(const Program& program, const FlowResults& flow, FlowReport& report)
{
    struct CanonicalLocaleUse {
        LocaleUseKind kind { LocaleUseKind::Unknown };
        Vector<uint8_t, 16> tag;
        BuiltinID builtin;
        Witness witness { noWitness };
    };
    Vector<CanonicalLocaleUse> uses;
    uses.reserveInitialCapacity(flow.localeUses().size());
    for (const FlowLocaleUse& use : flow.localeUses()) {
        uses.append(CanonicalLocaleUse { use.kind, { }, use.builtin, use.witness });
        if (use.kind != LocaleUseKind::Constant)
            continue;
        String canonical;
        if (use.tag) {
            Vector<char16_t> buffer;
            StringView text = program.strings.viewOf(use.tag, buffer);
            if (isStructurallyValidLanguageTag(text))
                canonical = canonicalizeUnicodeLocaleID(text);
        }
        if (canonical.isNull())
            uses.last().kind = LocaleUseKind::Unknown;
        else
            uses.last().tag = Vector<uint8_t, 16>(byteCast<uint8_t>(canonical.utf8().span()));
    }

    auto compareKindAndTag = [](const CanonicalLocaleUse& a, const CanonicalLocaleUse& b) -> std::strong_ordering {
        if (a.kind != b.kind)
            return a.kind <=> b.kind;
        return std::lexicographical_compare_three_way(a.tag.begin(), a.tag.end(), b.tag.begin(), b.tag.end());
    };
    AOT::sort(uses, [&](const CanonicalLocaleUse& a, const CanonicalLocaleUse& b) {
        std::strong_ordering order = compareKindAndTag(a, b);
        return order != 0 ? order < 0 : isLowerWitness(a.witness, b.witness);
    });
    removeRepeatedElements(uses, [&](const CanonicalLocaleUse& a, const CanonicalLocaleUse& b) {
        return compareKindAndTag(a, b) == 0;
    });

    for (const CanonicalLocaleUse& use : uses) {
        Wire::TextRange tag { static_cast<uint32_t>(report.localeTags.size()), static_cast<uint32_t>(use.tag.size()) };
        report.localeTags.append(use.tag.span());
        Wire::LocaleUseRecord record { .kind = use.kind, .tag = tag, .witness = use.witness };
        if (use.builtin)
            record.builtin = use.builtin.index();
        report.localesUsed.append(record);
    }
}

static void appendRemainingChecks(const Program& program, const AOT::CompileOptions& options, FunctionID id, Vector<Wire::RemainingCheckRecord>& checks)
{
    const Function& function = program.at(id);
    CFG cfg(function);
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            const Value& check = function.at(value);
            if (check.opcode != Opcode::Check && !std::ranges::contains(replacedOperationsOf(check.opcode), Opcode::Check))
                continue;
            CheckCause cause = PayloadWord::cause(check.opcode) ? check.cause() : CheckCause::Unknown;
            if (cause == CheckCause::Unknown && check.opcode == Opcode::Check)
                cause = computeUnknownObjectCause(function, check.valueOperand());

            Wire::SourceLocation location = function.locationOf(value);
            Wire::SourceLocation origin = location;
            TypeSiteKind siteKind = TypeSiteKind::Unknown;
            if (TypeSiteID site = PayloadWord::site(check.opcode) ? check.site() : TypeSiteID()) {
                origin = { program.at(site).module.index(), program.at(site).position };
                siteKind = program.at(site).kind;
            }
            std::span<const uint8_t> path;
            if (hasModule(origin)) {
                const Module& module = program.at(ModuleID(origin.module));
                path = program.string(options.obfuscates() ? module.path : module.displayPath);
            }
            checks.append(Wire::RemainingCheckRecord {
                .id = typeCheckIDOf(path, origin.position, siteKind),
                .witness = { findFunctionAsRead(&program, check.inlineFrame ? function.inlineFrames[check.inlineFrame].callee : id).index(), value.index(), location },
                .origin = origin,
                .cause = cause,
                .siteKind = static_cast<uint32_t>(siteKind),
            });
        }
    }
}

Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program& program, BuildContext& build)
{
    if (!hasFlag(build.options().reports, ReportSet::Flow))
        return { };

    const ParallelLoops& loops = build.parallelLoops();
    Vector<Wire::RemainingCheckRecord> checks = collectInParallel<Wire::RemainingCheckRecord>(loops, program.functions.size(), [&](unsigned index, Vector<Wire::RemainingCheckRecord>& found) {
        if (program.functions[index].hasBody())
            appendRemainingChecks(program, build.options(), FunctionID(index), found);
    });

    auto orderKey = [](const Wire::RemainingCheckRecord& check) {
        const Wire::Witness& witness = check.witness;
        return std::tuple { check.id, witness.function, witness.value, witness.location.module, witness.location.position, check.cause };
    };
    sortAndRemoveDuplicates(loops, checks, [&](const Wire::RemainingCheckRecord& a, const Wire::RemainingCheckRecord& b) {
        return orderKey(a) < orderKey(b);
    }, [](const Wire::RemainingCheckRecord& a, const Wire::RemainingCheckRecord& b) {
        return a.id == b.id;
    });
    return checks;
}

static StatisticID statisticOf(KnowledgeLevel level)
{
    switch (level) {
    case KnowledgeLevel::Unknown:
        return StatisticID::AccessesUnknown;
    case KnowledgeLevel::Expected:
        return StatisticID::AccessesGuarded;
    case KnowledgeLevel::Proven:
        return StatisticID::AccessesProven;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void countAccessStatistics(std::span<const Wire::AccessCountRecord> counts, Statistics& statistics)
{
    std::array<uint64_t, numberOfEnumerators<KnowledgeLevel>> accesses { };
    uint64_t directCalls = 0;
    uint64_t indirectCalls = 0;
    for (const Wire::AccessCountRecord& record : counts) {
        for (KnowledgeLevel level : allMembersOf<KnowledgeLevel>) {
            unsigned rank = rankOf(level);
            accesses[rank] += record.accesses[rank];
            (level == KnowledgeLevel::Unknown ? indirectCalls : directCalls) += record.calls[rank];
        }
    }

    for (KnowledgeLevel level : allMembersOf<KnowledgeLevel>)
        statistics.count(statisticOf(level), accesses[rankOf(level)]);
    statistics.count(StatisticID::DirectCalls, directCalls);
    statistics.count(StatisticID::IndirectCalls, indirectCalls);
}

static AllocationKind allocationKindOf(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::ModuleNamespace:
        return AllocationKind::Object;
    case LayoutKind::Environment:
        return AllocationKind::Environment;
    case LayoutKind::Closure:
        return AllocationKind::Closure;
    case LayoutKind::CoroutineFrame:
        return AllocationKind::Generator;
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return AllocationKind::Other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AllocationKind allocationKindOf(BuiltinClassID builtinClass)
{
    if (builtinClass == BuiltinClasses::Object)
        return AllocationKind::Object;
    if (builtinClass == BuiltinClasses::Array)
        return AllocationKind::Array;
    if (builtinClass == BuiltinClasses::Arguments)
        return AllocationKind::Arguments;
    if (builtinClass == BuiltinClasses::Generator || builtinClass == BuiltinClasses::AsyncGenerator)
        return AllocationKind::Generator;
    if (builtinClass == BuiltinClasses::Promise)
        return AllocationKind::Promise;
    if (builtinClass == BuiltinClasses::BoundFunction)
        return AllocationKind::BoundFunction;
    if (builtinClass == BuiltinClasses::Function || builtinClass == BuiltinClasses::AsyncFunction || builtinClass == BuiltinClasses::GeneratorFunction || builtinClass == BuiltinClasses::AsyncGeneratorFunction)
        return AllocationKind::Closure;
    if (builtinClass == BuiltinClasses::String)
        return AllocationKind::StringRange;
    if (builtinClass == BuiltinClasses::Map || builtinClass == BuiltinClasses::Set || builtinClass == BuiltinClasses::WeakMap || builtinClass == BuiltinClasses::WeakSet)
        return AllocationKind::Collection;
    return AllocationKind::Other;
}

static AllocationKind allocationKindOf(const Program& program, const FlowResults& flow, const AllocationSite& site)
{
    if (site.allocationClass && flow.at(site.allocationClass).closureFunction)
        return AllocationKind::Closure;
    if (site.layout) {
        AllocationKind kind = allocationKindOf(program.at(site.layout).kind);
        if (kind != AllocationKind::Other)
            return kind;
    }
    return site.builtinClass ? allocationKindOf(site.builtinClass) : AllocationKind::Other;
}

static Vector<BuiltinID> findBuiltinsCalledDirectly(const Program& program, const ParallelLoops& loops)
{
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinID> called = collectInParallel<BuiltinID>(loops, program.functions.size(), [&](unsigned index, Vector<BuiltinID>& found) {
        FunctionID id(index);
        size_t firstOfFunction = found.size();
        auto note = [&](BuiltinOverloadID overload) {
            BuiltinID builtin = builtins.canonicalID(builtins.at(overload).builtin);
            if (!std::ranges::contains(found.subspan(firstOfFunction), builtin))
                found.append(builtin);
        };
        forEachListedValue(program, id, builtinCallOpcodes, [&](BlockID, uint32_t, ValueID, const Value& value) {
            note(value.overload());
        });
        if (!mayEndBlockWith(program, id, { TerminatorKind::TryCallInline }))
            return;
        for (const Block& block : program.at(id).blocks) {
            if (block.terminator.kind == TerminatorKind::TryCallInline)
                note(block.terminator.overload());
        }
    });
    AOT::sort(called);
    removeRepeatedElements(called);
    return called;
}

// A direct call has no way out but the ones its overload declares, so the code behind it has to be in the executable.
static void countDirectCallsAsUses(const Program& program, std::span<const BuiltinID> called, FlowReport& report, BuildContext& build)
{
    const BuiltinTable& builtins = program.builtins();
    for (BuiltinID builtin : called) {
        if (!program.mayBeTrimmed(builtin))
            continue;
        if (isGuardedByRemovedFeature(builtins, builtin, build.options().removedFeatures)) {
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::CallsTrimmedBuiltin, .detail = builtins.textOf(builtin) });
            continue;
        }
        report.featuresUsed.append(Wire::FeatureUseRecord { static_cast<uint32_t>(*guardOf(builtins, builtin)), builtin.index(), noWitness });
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(builtin)) {
            if (!line.condition && !line.isGuard)
                report.featuresUsed.append(Wire::FeatureUseRecord { static_cast<uint32_t>(line.feature), builtin.index(), noWitness });
        }
    }
}

FlowReportBodyRecords collectFlowReportBodyRecords(const Program& program, BuildContext& build)
{
    FlowReportBodyRecords records;
    runTasksInParallel(build.parallelLoops(), {
        [&] { records.typeTests = collectTypeTestRecords(program, build.parallelLoops()); },
        [&] { records.builtinsCalledDirectly = findBuiltinsCalledDirectly(program, build.parallelLoops()); },
        [&] {
            if (program.form() == IRForm::AsRead)
                records.remainingChecks = collectRemainingChecks(program, build);
        },
    });
    return records;
}

FlowReport reportUsesOfLibraryCode(std::span<const Wire::UnseenCodeRecord> unseenCode, std::span<const Wire::UnseenCodeRecord> loadsOfMachineCode, std::span<const Wire::BuiltinUseRecord> builtinUses, std::span<const RuntimeFunctionID> libraryFunctionsLacked)
{
    FlowReport report;
    report.unseenCode = Vector<Wire::UnseenCodeRecord>(unseenCode);
    report.unseenCode.append(loadsOfMachineCode);
    report.builtinsUsed = Vector<Wire::BuiltinUseRecord>(builtinUses);
    for (RuntimeFunctionID function : libraryFunctionsLacked)
        report.libraryFunctionsLacked.append(Wire::LackedFunctionRecord { .function = function.index() });
    return report;
}

FlowReport reportUsesOfLibraryCode(const FlowResults& flow)
{
    if (flow.hasResults())
        return reportUsesOfLibraryCode(flow.unseenCode(), flow.loadsOfMachineCode(), flow.builtinUses(), flow.libraryFunctionsLacked());
    Vector<Wire::UnseenCodeRecord> unseenCode(flow.unseenCode());
    unseenCode.append(Wire::UnseenCodeRecord { .cause = UnseenCodeCause::FlowResultDisabled });
    return reportUsesOfLibraryCode(unseenCode.span(), flow.loadsOfMachineCode(), flow.builtinUses(), flow.libraryFunctionsLacked());
}

FlowReport buildFlowReport(const Program& program, const FlowResults& flow, const GradedSites& sites, std::span<const Wire::RemainingCheckRecord> checksLeftByFlow, FlowReportBodyRecords&& bodyRecords, BuildContext& build)
{
    FlowReport report = reportUsesOfLibraryCode(flow);
    report.featuresUsed = Vector<Wire::FeatureUseRecord>(flow.featureUses());
    countDirectCallsAsUses(program, bodyRecords.builtinsCalledDirectly.span(), report, build);
    appendLocaleUses(program, flow, report);
    for (const FlowWorkerEntry& entry : flow.workerEntries()) {
        std::span<const uint8_t> text = program.string(entry.path);
        report.workerEntries.append(Wire::WorkerEntryRecord { .path = { static_cast<uint32_t>(report.workerEntryPaths.size()), static_cast<uint32_t>(text.size()) }, .isGuess = entry.isGuess, .hasOptions = entry.hasOptions, .witness = entry.witness });
        report.workerEntryPaths.append(text);
    }
    if (build.options().collectsAllocationRemarks()) {
        report.allocationSites = WTF::map(flow.allocationSites(), [&](const AllocationSite& site) {
            return AllocationSiteReport { findFunctionAsRead(&program, site.function), allocationKindOf(program, flow, site) };
        });
    }

    ReportSet reports = build.options().reports;
    bool collectsRecords = hasFlag(reports, ReportSet::Flow);
    report.typeTests = WTF::move(bodyRecords.typeTests);
    report.accesses = buildAccessRecords(program, flow, sites.levels.span(), build);
    report.accesses.appendVector(sites.generalOperations);
    report.otherOrigins = Vector<Wire::OtherOriginsRecord>(flow.otherOrigins());
    if (program.form() == IRForm::AsRead)
        report.remainingChecks = WTF::move(bodyRecords.remainingChecks);
    else if (collectsRecords)
        report.remainingChecks = Vector<Wire::RemainingCheckRecord>(checksLeftByFlow);

    report.accessCounts = countAccesses(program, report.accesses.span(), report.typeTests.span());
    countAccessStatistics(report.accessCounts.span(), build.statistics());
    if (!collectsRecords) {
        report.typeTests.clear();
        report.remainingChecks.clear();
        if (!hasFlag(reports, ReportSet::Functions))
            report.accesses.clear();
    }
    return report;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
