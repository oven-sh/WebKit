/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSupportTestCommands.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDiagnostics.h"
#include "AOTLimits.h"
#include "AOTLosses.h"
#include "AOTPassID.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaEnums.h"
#include "AOTTestFile.h"
#include "AOTThreads.h"
#include "AOTWire.h"
#include <algorithm>
#include <iterator>
#include <utility>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Variant.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RecordWord {
    StringView kind { };
    FunctionID function { };
    BlockID block { };
    ValueID value { };
    LayoutID layout { };
    LimitID limit { LimitID::None };
    uint32_t count { 0 };
    SourceLocation location { noSourceLocation };
    PassID pass { PassID::None };
    AllocationSiteID site { };
    std::optional<StringView> reason { };
    AllocationHeapCause cause { AllocationHeapCause::None };
};

enum class RecordPart : uint16_t {
    Function = 1 << 0,
    Block = 1 << 1,
    Value = 1 << 2,
    Layout = 1 << 3,
    Limit = 1 << 4,
    Count = 1 << 5,
    Location = 1 << 6,
    Pass = 1 << 7,
    Site = 1 << 8,
    Reason = 1 << 9,
    Cause = 1 << 10,
};

struct RecordPartSyntax {
    ASCIILiteral prefix;
    RecordPart part;
};

struct IDText {
    char letter;
    uint32_t index;

    void dump(PrintStream& out) const
    {
        if (index == Wire::none)
            out.print('-');
        else
            out.print(letter, index);
    }
};

struct LocationText {
    SourceLocation location;

    void dump(PrintStream& out) const
    {
        if (!hasModule(location))
            out.print('-');
        else
            out.print(location.module, ':', location.position);
    }
};

struct LimitHit {
    LimitID limit;
    Subject subject;
};

template<typename Record>
struct RecordList {
    CommonTestWords commonWords;
    const Program* program { nullptr };
    Vector<Record> records;
};

struct LossesMode {
    ASCIILiteral name;
    void (*run)(TestCommandContext&, std::span<const String> words);
};

using AnyDiagnostic = Variant<UserDiagnostic, InternalError>;

} // anonymous namespace

static constexpr RecordPartSyntax recordPartSyntaxes[] = {
    { "f"_s, RecordPart::Function },
    { "b"_s, RecordPart::Block },
    { "v"_s, RecordPart::Value },
    { "L"_s, RecordPart::Layout },
    { "limit="_s, RecordPart::Limit },
    { "count="_s, RecordPart::Count },
    { "at="_s, RecordPart::Location },
    { "pass="_s, RecordPart::Pass },
    { "site="_s, RecordPart::Site },
    { "reason="_s, RecordPart::Reason },
    { "cause="_s, RecordPart::Cause },
};

static constexpr OptionSet<RecordPart> diagnosticParts {
    RecordPart::Function,
    RecordPart::Block,
    RecordPart::Value,
    RecordPart::Layout,
    RecordPart::Limit,
    RecordPart::Count,
    RecordPart::Location,
    RecordPart::Pass,
};

static constexpr OptionSet<RecordPart> limitHitParts {
    RecordPart::Function,
    RecordPart::Value,
    RecordPart::Layout,
    RecordPart::Location,
};

static constexpr OptionSet<RecordPart> declineParts {
    RecordPart::Function,
    RecordPart::Value,
    RecordPart::Limit,
    RecordPart::Count,
    RecordPart::Location,
    RecordPart::Reason,
};

static constexpr OptionSet<RecordPart> allocationRemarkParts {
    RecordPart::Limit,
    RecordPart::Location,
    RecordPart::Site,
    RecordPart::Cause,
};

template<typename Target, typename T>
static bool assignIfParsed(Target& target, const std::optional<T>& parsed)
{
    if (!parsed)
        return false;
    target = *parsed;
    return true;
}

template<typename IDType>
static std::optional<IDType> parseID(StringView text)
{
    auto index = parseTestNumber<uint32_t>(text);
    if (!index || *index == Wire::none)
        return std::nullopt;
    return IDType(*index);
}

static std::optional<FunctionID> parseFunctionID(StringView text)
{
    auto id = parseID<FunctionID>(text);
    if (!id || ProvisionalFunctions::contains(*id))
        return std::nullopt;
    return id;
}

static std::optional<SourceLocation> parseLocation(StringView text)
{
    size_t colon = text.find(':');
    if (colon == notFound)
        return std::nullopt;
    auto module = parseTestNumber<uint32_t>(text.left(colon));
    auto position = parseTestNumber<uint32_t>(text.substring(colon + 1));
    if (!module || *module == Wire::none || !position)
        return std::nullopt;
    return SourceLocation { *module, *position };
}

static bool parseRecordPart(RecordPart part, StringView text, RecordWord& record)
{
    switch (part) {
    case RecordPart::Function:
        return assignIfParsed(record.function, parseFunctionID(text));
    case RecordPart::Block:
        return assignIfParsed(record.block, parseID<BlockID>(text));
    case RecordPart::Value:
        return assignIfParsed(record.value, parseID<ValueID>(text));
    case RecordPart::Layout:
        return assignIfParsed(record.layout, parseID<LayoutID>(text));
    case RecordPart::Limit:
        return assignIfParsed(record.limit, parseEnum<LimitID>(text));
    case RecordPart::Count:
        return assignIfParsed(record.count, parseTestNumber<uint32_t>(text));
    case RecordPart::Location:
        return assignIfParsed(record.location, parseLocation(text));
    case RecordPart::Pass:
        return assignIfParsed(record.pass, passNamed(text));
    case RecordPart::Site:
        return assignIfParsed(record.site, parseID<AllocationSiteID>(text));
    case RecordPart::Reason:
        record.reason = text;
        return true;
    case RecordPart::Cause:
        return assignIfParsed(record.cause, parseEnum<AllocationHeapCause>(text));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<RecordWord> parseRecordWord(StringView word, OptionSet<RecordPart> allowedParts)
{
    RecordWord record;
    OptionSet<RecordPart> seenParts;
    bool isKind = true;
    for (StringView text : word.splitAllowingEmptyEntries(',')) {
        if (std::exchange(isKind, false)) {
            record.kind = text;
            continue;
        }
        auto* syntax = std::ranges::find_if(recordPartSyntaxes, [&](const RecordPartSyntax& candidate) {
            return text.startsWith(candidate.prefix);
        });
        if (syntax == std::end(recordPartSyntaxes) || !allowedParts.contains(syntax->part) || seenParts.contains(syntax->part))
            return std::nullopt;
        seenParts.add(syntax->part);
        if (!parseRecordPart(syntax->part, text.substring(syntax->prefix.length()), record))
            return std::nullopt;
    }
    return record;
}

static Subject subjectOf(const RecordWord& record)
{
    return {
        .function = record.function,
        .block = record.block,
        .value = record.value,
        .layout = record.layout,
        .location = record.location,
    };
}

static UserDiagnostic makeUserDiagnostic(UserDiagnosticKind kind, const RecordWord& record)
{
    return {
        .kind = kind,
        .function = record.function,
        .block = record.block,
        .value = record.value,
        .layout = record.layout,
        .limit = record.limit,
        .count = record.count,
        .location = record.location,
    };
}

static InternalError makeInternalError(InternalErrorKind kind, const RecordWord& record)
{
    return {
        .kind = kind,
        .function = record.function,
        .block = record.block,
        .value = record.value,
        .layout = record.layout,
        .limit = record.limit,
        .count = record.count,
        .location = record.location,
        .pass = record.pass,
    };
}

static std::optional<AnyDiagnostic> parseDiagnostic(StringView word)
{
    auto record = parseRecordWord(word, diagnosticParts);
    if (!record)
        return std::nullopt;
    auto name = parseDiagnosticName(record->kind);
    if (!name)
        return std::nullopt;
    return WTF::switchOn(*name,
        [&](UserDiagnosticKind kind) -> std::optional<AnyDiagnostic> {
            if (record->pass != PassID::None)
                return std::nullopt;
            return AnyDiagnostic { makeUserDiagnostic(kind, *record) };
        },
        [&](InternalErrorKind kind) -> std::optional<AnyDiagnostic> {
            return AnyDiagnostic { makeInternalError(kind, *record) };
        });
}

static std::optional<LimitHit> parseLimitHit(StringView word)
{
    auto record = parseRecordWord(word, limitHitParts);
    if (!record)
        return std::nullopt;
    auto limit = parseEnum<LimitID>(record->kind);
    if (!limit || *limit == LimitID::None)
        return std::nullopt;
    return LimitHit { *limit, subjectOf(*record) };
}

static std::optional<uint32_t> parseDeclineReason(DeclineKind kind, StringView text)
{
    return WTF::switchOn(decodeDeclineReason(kind, 0),
        [](std::monostate) -> std::optional<uint32_t> {
            return std::nullopt;
        },
        [&](OptionSet<AllocationEscapeReason> flags) -> std::optional<uint32_t> {
            for (StringView name : text.splitAllowingEmptyEntries('+')) {
                auto flag = parseEnum<AllocationEscapeReason>(name);
                if (!flag)
                    return std::nullopt;
                flags.add(*flag);
            }
            return flags.toRaw();
        },
        [&]<typename Reason>(Reason) -> std::optional<uint32_t> {
            auto member = parseEnum<Reason>(text);
            if (!member)
                return std::nullopt;
            return static_cast<uint32_t>(*member);
        });
}

static std::optional<Wire::DeclineRecord> parseDecline(StringView word)
{
    auto record = parseRecordWord(word, declineParts);
    if (!record)
        return std::nullopt;
    auto kind = parseEnum<DeclineKind>(record->kind);
    if (!kind)
        return std::nullopt;
    std::optional<uint32_t> reason = record->reason ? parseDeclineReason(*kind, *record->reason) : 0;
    if (!reason)
        return std::nullopt;
    return Wire::DeclineRecord {
        .kind = *kind,
        .limit = record->limit,
        .count = record->count,
        .reason = *reason,
        .witness = witnessOf(subjectOf(*record)),
    };
}

static std::optional<AllocationRemark> parseAllocationRemark(StringView word)
{
    auto record = parseRecordWord(word, allocationRemarkParts);
    if (!record || !record->site)
        return std::nullopt;
    auto outcome = parseEnum<AllocationOutcome>(record->kind);
    if (!outcome)
        return std::nullopt;
    return AllocationRemark {
        .site = record->site,
        .location = record->location,
        .outcome = *outcome,
        .cause = record->cause,
        .limit = record->limit,
    };
}

template<typename Record>
static std::optional<RecordList<Record>> parseRecordList(TestCommandContext& context, std::span<const String> words, std::optional<Record> (*parseRecord)(StringView))
{
    RecordList<Record> list;
    list.program = context.program.get();
    for (const String& word : words) {
        if (takeCommonTestWord(word, list.commonWords))
            continue;
        if (word == "no-program"_s) {
            list.program = nullptr;
            continue;
        }
        auto record = parseRecord(word);
        if (!record) {
            printBadArgument(context.out, word);
            return std::nullopt;
        }
        list.records.append(*record);
    }
    shuffle(list.records, list.commonWords.seed);
    return list;
}

template<typename Record>
static void printDiagnosticLine(PrintStream& out, const Record& record, ASCIILiteral lastColumn, const String& message)
{
    out.println(nameOf(record.kind), ' ',
        IDText { 'f', record.function.index() }, ' ',
        IDText { 'b', record.block.index() }, ' ',
        IDText { 'v', record.value.index() }, ' ',
        IDText { 'L', record.layout.index() }, ' ',
        nameOf(record.limit), ' ', record.count, ' ',
        LocationText { record.location }, ' ',
        lastColumn, " | "_s, message);
}

static void printDiagnostic(PrintStream& out, const UserDiagnostic& diagnostic, const String& message)
{
    printDiagnosticLine(out, diagnostic, nameOf(severityOf(diagnostic.kind)), message);
}

static void printDiagnostic(PrintStream& out, const InternalError& error, const String& message)
{
    printDiagnosticLine(out, error, error.pass == PassID::None ? "-"_s : nameOf(error.pass), message);
}

static void checkTemplate(PrintStream& out, ASCIILiteral kind, const String& message, unsigned& numberOfBadTemplates)
{
    if (!message.isEmpty() && !message.contains('{') && !message.contains('}'))
        return;
    ++numberOfBadTemplates;
    out.println("bad template "_s, kind, ": "_s, message);
}

static void runDiagnosticTemplates(PrintStream& out)
{
    out.println("empty sink has errors: "_s, yesOrNo(Diagnostics().hasErrors()));

    const RecordWord fullRecord {
        .function = FunctionID(0u),
        .block = BlockID(0u),
        .value = ValueID(0u),
        .layout = LayoutID(0u),
        .limit = LimitID::MaxLayoutsInType,
        .count = 1,
        .location = { 0, 0 },
        .pass = PassID::VerifyProgram,
    };
    unsigned numberOfBadTemplates = 0;
    bool errorsMatchSeverity = true;
    for (UserDiagnosticKind kind : allMembersOf<UserDiagnosticKind>) {
        Diagnostics sink;
        sink.add(makeUserDiagnostic(kind, fullRecord));
        if (sink.hasErrors() != failsBuild(kind))
            errorsMatchSeverity = false;
        sink.finish(nullptr);
        checkTemplate(out, nameOf(kind), sink.messageOfUserDiagnostic(0), numberOfBadTemplates);
    }
    for (InternalErrorKind kind : allMembersOf<InternalErrorKind>) {
        Diagnostics sink;
        sink.add(makeInternalError(kind, fullRecord));
        if (!sink.hasErrors())
            errorsMatchSeverity = false;
        sink.finish(nullptr);
        checkTemplate(out, nameOf(kind), sink.messageOfInternalError(0), numberOfBadTemplates);
    }
    out.println("templates: "_s, numberOfEnumerators<UserDiagnosticKind>, " user kinds, "_s, numberOfEnumerators<InternalErrorKind>, " internal kinds, "_s, numberOfBadTemplates, " bad"_s);
    printCheck(out, "hasErrors agrees with severity"_s, errorsMatchSeverity);
}

void runDiagnosticsTestCommand(TestCommandContext& context)
{
    if (isOnlyWord(context.arguments, "templates"_s))
        return runDiagnosticTemplates(context.out);

    auto list = parseRecordList(context, context.arguments, parseDiagnostic);
    if (!list)
        return;

    Diagnostics sink;
    ParallelLoops loops(list->commonWords.threads);
    forEachIndexInParallel(loops, list->records.size(), [&](unsigned index) {
        WTF::switchOn(list->records[index], [&]<typename Record>(const Record& record) {
            if (index % 2)
                sink.append(Vector<Record> { record });
            else
                sink.add(record);
        });
    });

    PrintStream& out = context.out;
    out.println("has errors: "_s, yesOrNo(sink.hasErrors()));
    sink.finish(list->program);

    const Vector<UserDiagnostic>& userDiagnostics = sink.userDiagnostics();
    out.println("user records "_s, userDiagnostics.size());
    for (size_t index = 0; index < userDiagnostics.size(); ++index)
        printDiagnostic(out, userDiagnostics[index], sink.messageOfUserDiagnostic(index));

    const Vector<InternalError>& internalErrors = sink.internalErrors();
    out.println("internal records "_s, internalErrors.size());
    for (size_t index = 0; index < internalErrors.size(); ++index)
        printDiagnostic(out, internalErrors[index], sink.messageOfInternalError(index));

    out.println("of "_s, list->records.size(), " added"_s);
}

static void runLimitHits(TestCommandContext& context, std::span<const String> words)
{
    auto list = parseRecordList(context, words, parseLimitHit);
    if (!list)
        return;

    ParallelLoops loops(list->commonWords.threads);
    forEachIndexInParallel(loops, list->records.size(), [&](unsigned index) {
        const LimitHit& hit = list->records[index];
        context.build.gaveUp(hit.limit, hit.subject);
    });

    for (const Wire::LossRecord& loss : context.build.takeLosses(list->program)) {
        context.out.println(nameOf(loss.limit), ' ', nameOf(loss.stage), " x"_s, loss.count, ' ',
            IDText { 'f', loss.witness.function }, ' ',
            IDText { 'v', loss.witness.value }, ' ',
            IDText { 'L', loss.layout }, ' ',
            LocationText { loss.witness.location });
    }
}

static void runDeclines(TestCommandContext& context, std::span<const String> words)
{
    auto list = parseRecordList(context, words, parseDecline);
    if (!list)
        return;

    ParallelLoops loops(list->commonWords.threads);
    forEachIndexInParallel(loops, list->records.size(), [&](unsigned index) {
        context.build.addDecline(list->records[index]);
    });

    Vector<Wire::DeclineRecord> declines = context.build.takeDeclines(list->program);
    context.out.println("declines "_s, declines.size(), " of "_s, list->records.size(), " added"_s);
    for (const Wire::DeclineRecord& decline : declines) {
        context.out.println(nameOf(decline.kind), ' ',
            IDText { 'f', decline.witness.function }, ' ',
            IDText { 'v', decline.witness.value }, ' ',
            LocationText { decline.witness.location }, ' ',
            nameOf(decline.limit), " ["_s, decline.count, ']', DeclineReasonField { decline });
    }
}

static void runAllocationRemarks(TestCommandContext& context, std::span<const String> words)
{
    auto list = parseRecordList(context, words, parseAllocationRemark);
    if (!list)
        return;

    ParallelLoops loops(list->commonWords.threads);
    forEachIndexInParallel(loops, list->records.size(), [&](unsigned index) {
        context.build.addAllocationRemark(list->records[index]);
    });

    for (const AllocationRemark& remark : context.build.takeAllocationRemarks()) {
        context.out.println(IDText { 's', remark.site.index() }, ' ', nameOf(remark.outcome), " x"_s, remark.copies, ' ',
            LocationText { remark.location }, ' ', nameOf(remark.limit), ' ', nameOf(remark.cause));
    }
}

void runLossesTestCommand(TestCommandContext& context)
{
    static constexpr LossesMode modes[] = {
        { "limit-hits"_s, runLimitHits },
        { "declines"_s, runDeclines },
        { "allocation-remarks"_s, runAllocationRemarks },
    };

    if (context.arguments.empty())
        return printMissingArgument(context.out);
    for (const LossesMode& mode : modes) {
        if (context.arguments[0] == mode.name)
            return mode.run(context, context.arguments.subspan(1));
    }
    printBadArgument(context.out, context.arguments[0]);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
