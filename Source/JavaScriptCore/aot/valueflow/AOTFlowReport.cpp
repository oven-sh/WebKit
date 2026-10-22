/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowReport.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTFlowReportInternal.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTResultRules.h"
#include "AOTRuntimeReporter.h"
#include "AOTThreads.h"
#include "AOTTypeTestReport.h"
#include "IntlObject.h"
#include <algorithm>
#include <array>
#include <compare>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CanonicalLocaleUse {
    LocaleUseKind kind { LocaleUseKind::Unknown };
    Vector<uint8_t, 16> tag;
    BuiltinID builtin;
    Witness witness { noWitness };
};

} // anonymous namespace

static std::optional<Vector<uint8_t, 16>> tryCanonicalizeTag(const Program& program, StringID tag)
{
    if (!tag)
        return std::nullopt;
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(tag, buffer);
    if (!isStructurallyValidLanguageTag(text))
        return std::nullopt;
    String canonical = canonicalizeUnicodeLocaleID(text);
    if (canonical.isNull())
        return std::nullopt;
    return Vector<uint8_t, 16>(byteCast<uint8_t>(canonical.utf8().span()));
}

static CanonicalLocaleUse canonicalLocaleUseOf(const Program& program, const FlowLocaleUse& use)
{
    CanonicalLocaleUse canonical { use.kind, { }, use.builtin, use.witness };
    if (use.kind != LocaleUseKind::Constant)
        return canonical;

    if (auto tag = tryCanonicalizeTag(program, use.tag))
        canonical.tag = WTF::move(*tag);
    else
        canonical.kind = LocaleUseKind::Unknown;
    return canonical;
}

static std::strong_ordering compareKindAndTag(const CanonicalLocaleUse& a, const CanonicalLocaleUse& b)
{
    if (a.kind != b.kind)
        return a.kind <=> b.kind;
    return std::lexicographical_compare_three_way(a.tag.begin(), a.tag.end(), b.tag.begin(), b.tag.end());
}

void appendLocaleUses(const Program& program, const FlowResults& flow, FlowReport& report)
{
    Vector<CanonicalLocaleUse> uses;
    uses.reserveInitialCapacity(flow.localeUses().size());
    for (const FlowLocaleUse& use : flow.localeUses())
        uses.append(canonicalLocaleUseOf(program, use));

    std::ranges::sort(uses, [](const CanonicalLocaleUse& a, const CanonicalLocaleUse& b) {
        std::strong_ordering order = compareKindAndTag(a, b);
        return order != 0 ? order < 0 : isLowerWitness(a.witness, b.witness);
    });
    removeRepeatedElements(uses, [](const CanonicalLocaleUse& a, const CanonicalLocaleUse& b) {
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

// An obfuscated image holds no display paths, so its executable makes the ids from the paths of the modules.
uint64_t remainingCheckIDOf(const Program& program, const AOT::CompileOptions& options, Wire::SourceLocation origin, TypeSiteKind kind)
{
    std::span<const uint8_t> path;
    if (hasModule(origin)) {
        const Module& module = program.at(ModuleID(origin.module));
        path = program.string(options.obfuscates() ? module.path : module.displayPath);
    }
    return typeCheckIDOf(path, origin.position, kind);
}

static auto remainingCheckOrderKey(const Wire::RemainingCheckRecord& check)
{
    const Wire::Witness& witness = check.witness;
    return std::tuple { check.id, witness.function, witness.value, witness.location.module, witness.location.position, check.cause };
}

Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program& program, BuildContext& build)
{
    if (!hasFlag(build.options().reports, ReportSet::Flow))
        return { };

    const ParallelLoops& loops = build.parallelLoops();
    Vector<Wire::RemainingCheckRecord> checks = collectInParallel<Wire::RemainingCheckRecord>(loops, program.functions.size(), [&](unsigned index, Vector<Wire::RemainingCheckRecord>& found) {
        if (program.functions[index].hasBody())
            found.appendVector(collectRemainingChecks(program, build.options(), FunctionID(index)));
    });

    sortAndRemoveDuplicates(loops, checks, [](const Wire::RemainingCheckRecord& a, const Wire::RemainingCheckRecord& b) {
        return remainingCheckOrderKey(a) < remainingCheckOrderKey(b);
    }, [](const Wire::RemainingCheckRecord& a, const Wire::RemainingCheckRecord& b) {
        return a.id == b.id;
    });
    return checks;
}

// The block in which the function ends without a choice, if it does.
static BlockID findEndOfStraightEntry(const Function& function)
{
    BlockID block = entryBlock;
    for (size_t steps = 0; steps < function.blocks.size(); ++steps) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind == TerminatorKind::Unreachable)
            return block;
        if (terminator.kind != TerminatorKind::Jump)
            return { };
        block = terminator.next().block;
    }
    return { };
}

Vector<InternalError> findFunctionsThatEndInFirstBlock(const Program& program, const BitVector& isReachable, BuildContext& build)
{
    return collectInParallel<InternalError>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<InternalError>& found) {
        const Function& function = program.functions[index];
        // The model of a built-in is never run.
        if (!function.hasBody() || function.role == FunctionRole::Model || !isReachable.get(index))
            return;
        BlockID block = findEndOfStraightEntry(function);
        if (!block)
            return;
        // Later steps renumber blocks and values, so the operation and its position in the source are taken here.
        InternalError error { .kind = InternalErrorKind::FunctionEndsInFirstBlock, .function = FunctionID(index) };
        const Block& contents = function.at(block);
        if (contents.values.isEmpty())
            error.detail = "it ends at once"_s;
        else {
            const Value& last = function.at(contents.values.last());
            // An operation that only throws is what the program says.
            if (std::optional<Type> natural = tryWidestNaturalTypeOf(last); natural && natural->isNever())
                return;
            error.location = function.locationOf(contents.values.last());
            error.detail = makeString("it ends behind "_s, nameOf(last.opcode));
        }
        found.append(WTF::move(error));
    });
}

void reportFunctionsThatEndInFirstBlock(const Program& program, Vector<InternalError>&& found, BuildContext& build)
{
    if (found.isEmpty())
        return;

    BitVector isInlined(program.functions.size());
    for (const Function& function : program.functions) {
        if (!function.hasBody() || function.inlineFrames.size() < 2)
            continue;
        auto note = [&](Origin origin) {
            if (origin.inlineFrame)
                isInlined.set(function.inlineFrames[origin.inlineFrame].callee.index());
        };
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                note(function.at(id).origin());
            note(block.terminator.origin());
        }
    }
    BitVector hasNoCode = findBodiesWithoutCode(program, build);
    for (InternalError& error : found) {
        uint32_t index = error.function.index();
        if ((program.at(error.function).hasBody() && !hasNoCode.get(index)) || isInlined.get(index))
            build.diagnostics().add(WTF::move(error));
    }
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
            if (level == KnowledgeLevel::Unknown)
                indirectCalls += record.calls[rank];
            else
                directCalls += record.calls[rank];
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

static Vector<AllocationSiteReport> collectAllocationSites(const Program& program, const FlowResults& flow)
{
    return WTF::map(flow.allocationSites(), [&](const AllocationSite& site) {
        return AllocationSiteReport { findFunctionAsRead(&program, site.function), allocationKindOf(program, flow, site) };
    });
}

FlowReportBodyRecords collectFlowReportBodyRecords(const Program& program, BuildContext& build)
{
    FlowReportBodyRecords records;
    runTasksInParallel(build.parallelLoops(), {
        [&] { records.typeTests = collectTypeTestRecords(program, build.parallelLoops()); },
        [&] {
            // A run that ends before flow is applied still has every check in its program.
            if (program.form() == IRForm::AsRead)
                records.remainingChecks = collectRemainingChecks(program, build);
        },
    });
    return records;
}

FlowReport buildFlowReport(const Program& program, const FlowResults& flow, std::span<const FlowSiteLevel> levels, std::span<const Wire::RemainingCheckRecord> checksLeftByFlow, FlowReportBodyRecords&& bodyRecords, BuildContext& build)
{
    FlowReport report;
    report.unseenCode = Vector<Wire::UnseenCodeRecord>(flow.unseenCode());
    if (!flow.hasResults())
        report.unseenCode.append(Wire::UnseenCodeRecord { .cause = UnseenCodeCause::FlowResultDisabled });
    report.unseenCode.append(flow.loadsOfMachineCode());
    report.builtinsUsed = Vector<Wire::BuiltinUseRecord>(flow.builtinUses());
    report.featuresUsed = Vector<Wire::FeatureUseRecord>(flow.featureUses());
    appendLocaleUses(program, flow, report);
    if (build.options().collectsAllocationRemarks())
        report.allocationSites = collectAllocationSites(program, flow);

    ReportSet reports = build.options().reports;
    bool collectsRecords = hasFlag(reports, ReportSet::Flow);
    report.typeTests = WTF::move(bodyRecords.typeTests);
    report.accesses = buildAccessRecords(program, flow, levels, build);
    if (program.form() == IRForm::AsRead)
        report.remainingChecks = WTF::move(bodyRecords.remainingChecks);
    else if (collectsRecords)
        report.remainingChecks = Vector<Wire::RemainingCheckRecord>(checksLeftByFlow);

    report.accessCounts = countAccesses(program, report.accesses.span(), report.typeTests.span());
    countAccessStatistics(report.accessCounts.span(), build.statistics());
    if (!collectsRecords) {
        report.typeTests.clear();
        report.remainingChecks.clear();
        // The compile call counts these records per function for its function reports and drops them afterwards.
        if (!hasFlag(reports, ReportSet::Functions))
            report.accesses.clear();
    }
    return report;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
