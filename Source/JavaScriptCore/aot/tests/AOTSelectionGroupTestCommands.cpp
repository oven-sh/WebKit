/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionTestCommands.h"

#if ENABLE(AOT)

#include "AOTComparisonChains.h"
#include "AOTIRPrinter.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTLoopEntry.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSupportTestCommands.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <ranges>
#include <utility>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class RewriteKind : uint8_t { SlowPath, TwoWay, Arms, TestAndBranch, TestAndBranchAsValue, Unreachable, Replace };

struct ClaimRequest {
    SelectionRuleID rule { SelectionRuleID::PropertyPair };
    bool claimsTogether { false };
    Vector<SelectionGroup, 2> groups;
    std::optional<RewriteKind> rewrite;
    StringView rewriteWord;
    ValueID succeeded;
    Vector<ValueID, 2> fastValues;
    uint32_t numberOfArms { 0 };
    std::optional<Opcode> replacement;
};

struct ClaimedGroup {
    SelectionGroup group;
};

struct LoopEntryRequest {
    std::optional<BlockID> user;
    std::optional<ValueID> operand;
    StringView operandWord;
    bool emits { false };
};

struct ChainRequest {
    std::optional<ChainKind> kind;
    std::optional<uint64_t> minCases;
    std::optional<uint64_t> maxCases;
    bool converts { false };
};

} // anonymous namespace

static std::optional<FunctionID> parseFunctionWord(TestCommandContext& context)
{
    if (context.arguments.empty()) {
        printMissingArgument(context.out);
        return std::nullopt;
    }
    std::optional<FunctionID> id;
    if (std::optional<StringView> text = valueForKey(context.arguments.front(), "function"_s))
        id = parseFunctionWithBody(*context.program, *text);
    if (!id)
        printBadArgument(context.out, context.arguments.front());
    return id;
}

static bool parseWordsAfterFunction(TestCommandContext& context, NOESCAPE const Invocable<bool(StringView)> auto& parseWord)
{
    for (const String& word : context.arguments.subspan(1)) {
        if (!parseWord(word)) {
            printBadArgument(context.out, word);
            return false;
        }
    }
    return true;
}

static const SelectionRule* findRule(StringView name)
{
    for (const SelectionRule& rule : selectionRules()) {
        if (name == nameOf(rule.id))
            return &rule;
    }
    return nullptr;
}

static std::optional<SelectionGroup> parseGroup(const Function& function, StringView text)
{
    SelectionGroup group;
    if (text.endsWith('!')) {
        group.hasSlowPath = true;
        text = text.left(text.length() - 1);
    }
    for (StringView memberText : text.splitAllowingEmptyEntries(',')) {
        GroupMember member;
        if (memberText.endsWith('+')) {
            member.isUsedOutside = true;
            memberText = memberText.left(memberText.length() - 1);
        }
        std::optional<ValueID> value = parseListedValue(function, memberText);
        if (!value)
            return std::nullopt;
        member.value = *value;
        group.members.append(member);
    }
    if (group.members.isEmpty() || group.members.last().isUsedOutside)
        return std::nullopt;
    group.root = group.members.last().value;
    return group;
}

static bool parseSlowPath(const Function& function, StringView text, ClaimRequest& request)
{
    size_t colon = text.find(':');
    bool hasFastValues = colon != notFound;
    std::optional<ValueID> succeeded = parseListedValue(function, hasFastValues ? text.left(colon) : text);
    if (!succeeded)
        return false;
    request.succeeded = *succeeded;
    if (!hasFastValues)
        return true;

    for (StringView valueText : text.substring(colon + 1).splitAllowingEmptyEntries(',')) {
        std::optional<ValueID> value = parseListedValue(function, valueText);
        if (!value)
            return false;
        request.fastValues.append(*value);
    }
    return true;
}

static std::optional<RewriteKind> parseBareRewriteWord(StringView word)
{
    if (word == "two-way"_s)
        return RewriteKind::TwoWay;
    if (word == "test-and-branch"_s)
        return RewriteKind::TestAndBranch;
    if (word == "test-and-branch-as-value"_s)
        return RewriteKind::TestAndBranchAsValue;
    if (word == "unreachable"_s)
        return RewriteKind::Unreachable;
    return std::nullopt;
}

static bool parseRewriteWord(const Function& function, StringView word, ClaimRequest& request)
{
    if (request.rewrite)
        return false;
    request.rewriteWord = word;

    if (std::optional<StringView> text = valueForKey(word, "slow-path"_s)) {
        request.rewrite = RewriteKind::SlowPath;
        return parseSlowPath(function, *text, request);
    }
    if (std::optional<StringView> text = valueForKey(word, "arms"_s)) {
        request.rewrite = RewriteKind::Arms;
        request.numberOfArms = parseTestNumber<uint8_t>(*text).value_or(0);
        return !!request.numberOfArms;
    }
    if (std::optional<StringView> text = valueForKey(word, "replace"_s)) {
        request.rewrite = RewriteKind::Replace;
        request.replacement = parseEnum<Opcode>(*text);
        return request.replacement.has_value();
    }
    request.rewrite = parseBareRewriteWord(word);
    return request.rewrite.has_value();
}

static bool parseClaimWord(const Function& function, StringView word, ClaimRequest& request)
{
    if (word == "together"_s) {
        request.claimsTogether = true;
        return true;
    }
    if (std::optional<StringView> name = valueForKey(word, "rule"_s)) {
        const SelectionRule* rule = findRule(*name);
        if (!rule)
            return false;
        request.rule = rule->id;
        return true;
    }
    if (std::optional<StringView> members = valueForKey(word, "group"_s)) {
        std::optional<SelectionGroup> group = parseGroup(function, *members);
        if (!group)
            return false;
        request.groups.append(WTF::move(*group));
        return true;
    }
    return parseRewriteWord(function, word, request);
}

static std::optional<ClaimRequest> parseClaimRequest(TestCommandContext& context, const Function& function)
{
    ClaimRequest request;
    bool isParsed = parseWordsAfterFunction(context, [&](StringView word) {
        return parseClaimWord(function, word, request);
    });
    if (!isParsed)
        return std::nullopt;
    if (request.groups.isEmpty()) {
        printMissingArgument(context.out);
        return std::nullopt;
    }
    return request;
}

static bool claimGroups(PrintStream& out, SelectionContext& selection, const ClaimRequest& request)
{
    if (request.claimsTogether) {
        ClaimResult result = selection.tryClaimAll(request.rule, request.groups.span());
        out.println("groups: "_s, nameOf(result));
        return result == ClaimResult::Claimed;
    }

    bool isFirstGroupClaimed = false;
    for (size_t index = 0; index < request.groups.size(); ++index) {
        ClaimResult result = selection.tryClaim(request.rule, request.groups[index]);
        out.println("group "_s, index, ": "_s, nameOf(result));
        if (!index)
            isFirstGroupClaimed = result == ClaimResult::Claimed;
    }
    return isFirstGroupClaimed;
}

static bool hasResult(const Value& value)
{
    return resultRuleOf(value.opcode) != ResultRule::None;
}

static bool hasMemberUsedOutside(const SelectionGroup& group)
{
    return group.members.containsIf([](const GroupMember& member) {
        return member.isUsedOutside;
    });
}

static Vector<ValueID, 4> carriedValuesOf(const Function& function, const SelectionGroup& group)
{
    Vector<ValueID, 4> carriedValues;
    if (hasResult(function.at(group.root)))
        carriedValues.append(group.root);
    for (const GroupMember& member : group.members) {
        if (member.isUsedOutside)
            carriedValues.append(member.value);
    }
    return carriedValues;
}

static bool canEmitSlowPath(SelectionContext& selection, const ClaimRequest& request)
{
    const SelectionGroup& group = request.groups.first();
    if (!selection.typeOf(request.succeeded).isBoolean())
        return false;
    if (request.fastValues.size() != carriedValuesOf(selection.function(), group).size())
        return false;

    FunctionAnalyses& analyses = selection.analyses();
    ProgramPoint firstMember = analyses.useLists().pointOf(group.members.first().value);
    auto isAvailable = [&](ValueID value) {
        return analyses.dominates(value, firstMember);
    };
    return isAvailable(request.succeeded) && std::ranges::all_of(request.fastValues, isAvailable);
}

static bool hasInlineImplementation(const Program& program, const Value& value)
{
    return value.opcode == Opcode::CallBuiltin && inlineImplementationOf(program, value);
}

static bool canTakeOperandsOf(Opcode replacement, const Value& original)
{
    uint32_t numberOfOperands = original.numberOfOperands();
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(replacement);
    if (numberOfOperands > numberOfNamedOperands && !hasVariadicOperand(replacement))
        return false;

    for (uint32_t index = 0; index < numberOfNamedOperands; ++index) {
        bool hasOperand = index < numberOfOperands && original.operand(index);
        if (!hasOperand && !operandInfoOf(replacement, index).isOptional)
            return false;
    }
    return true;
}

static bool canRewrite(SelectionContext& selection, const ClaimRequest& request)
{
    const SelectionGroup& group = request.groups.first();
    const Value& root = selection.function().at(group.root);
    bool isGroupOfOne = group.members.size() == 1;

    switch (*request.rewrite) {
    case RewriteKind::SlowPath:
        return group.hasSlowPath && canEmitSlowPath(selection, request);
    case RewriteKind::TwoWay:
        return group.hasSlowPath && !hasMemberUsedOutside(group) && hasInlineImplementation(selection.program(), root);
    case RewriteKind::Arms:
        return group.hasSlowPath && !hasMemberUsedOutside(group);
    case RewriteKind::TestAndBranch:
        return isGroupOfOne && branchesOf(root.opcode) != Branches::No && selection.isOnlyBranchedOn(group.root);
    case RewriteKind::TestAndBranchAsValue:
        return group.hasSlowPath && isGroupOfOne && branchesOf(root.opcode) != Branches::No;
    case RewriteKind::Unreachable:
        return isGroupOfOne;
    case RewriteKind::Replace:
        return !hasMemberUsedOutside(group) && boundToOf(*request.replacement) == BoundTo::Nothing && canTakeOperandsOf(*request.replacement, root);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitSlowPath(GroupRewriter& rewriter, const ClaimRequest& request)
{
    if (!request.fastValues.isEmpty()) {
        Vector<ValueID, 4> carriedValues = carriedValuesOf(rewriter.function(), rewriter.group());
        Value results = IRBuilder::make(Opcode::Results);
        results.setValuesOperands(request.fastValues.span());
        ValueID producer = rewriter.insert(WTF::move(results));
        for (uint32_t index = 0; index < carriedValues.size(); ++index)
            rewriter.replaceWithResult(carriedValues[index], producer, index);
    }
    rewriter.emitSlowPath(request.succeeded);
}

static Terminator makeTryCallInline(const Program& program, const Value& call)
{
    InlineBuiltin builtin = *inlineImplementationOf(program, call);
    const BuiltinImplementation& implementation = overloadOf(program, call).code.implementation;
    Vector<uint32_t, 4> parameters;
    for (uint8_t parameter : std::span(implementation.inlineParameters).first(numberOfParametersOf(builtin)))
        parameters.append(parameter);

    Terminator fast(TerminatorKind::TryCallInline);
    fast.setInlineBuiltin(builtin);
    fast.setInlineParameters(parameters.span());
    fast.setOverload(call.overload());
    if (ValueID receiver = call.receiverOperand())
        fast.setReceiverOperand(receiver);
    fast.setArgumentsOperands(call.argumentsOperands());
    fast.setAccessSite(call.accessSite());
    fast.setAllocationSite(call.allocationSite());
    return fast;
}

static void emitArms(GroupRewriter& rewriter, uint32_t numberOfArms)
{
    IRBuilder& builder = rewriter.builder();
    bool rootHasResult = hasResult(rewriter.original(rewriter.group().root));

    rewriter.beginArms();
    ValueID subject = builder.appendInteger(0);
    Vector<BlockID, 4> arms;
    for (uint32_t index = 0; index < numberOfArms; ++index)
        arms.append(rewriter.addArm(BlockFrequency::Normal));

    Vector<std::pair<int64_t, Target>> cases;
    for (uint32_t index = 0; index + 1 < numberOfArms; ++index)
        cases.append(std::pair { static_cast<int64_t>(index), Target { arms[index], { }, { } } });
    builder.emitSwitch(subject, WTF::move(cases), Target { arms.last(), { }, { } });

    for (BlockID arm : arms) {
        builder.moveToEndOf(arm);
        ValueID rootCopy = rewriter.appendOriginals();
        rewriter.endArm(rootHasResult ? std::optional(rootCopy) : std::nullopt);
    }
}

static void rewriteGroup(GroupRewriter& rewriter, const Program& program, const ClaimRequest& request)
{
    const Value& root = rewriter.original(rewriter.group().root);
    switch (*request.rewrite) {
    case RewriteKind::SlowPath:
        emitSlowPath(rewriter, request);
        return;
    case RewriteKind::TwoWay:
        rewriter.emitTwoWay(makeTryCallInline(program, root));
        return;
    case RewriteKind::Arms:
        emitArms(rewriter, request.numberOfArms);
        return;
    case RewriteKind::TestAndBranch:
        rewriter.emitTestAndBranch();
        return;
    case RewriteKind::TestAndBranchAsValue:
        rewriter.emitTestAndBranchAsValue();
        return;
    case RewriteKind::Unreachable:
        rewriter.endBlockAsUnreachable();
        return;
    case RewriteKind::Replace:
        copyOperands(rewriter.replaceRoot(*request.replacement), 0, root, 0);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Diagnostics rewriteFirstGroup(SelectionContext& selection, const ClaimRequest& request)
{
    BuildContext& build = selection.build();
    Diagnostics reportedEarlier = build.takeDiagnostics();

    Vector<ClaimedGroup> matches;
    matches.append(ClaimedGroup { request.groups.first() });
    selection.rewriteGroups(request.rule, matches, [&](GroupRewriter& rewriter, const ClaimedGroup&) {
        rewriteGroup(rewriter, selection.program(), request);
    });

    return std::exchange(build.diagnostics(), WTF::move(reportedEarlier));
}

static void printInternalErrors(TestCommandContext& context, Diagnostics&& reported)
{
    reported.finish(context.program.get());
    for (const InternalError& error : reported.internalErrors())
        context.out.println("error "_s, nameOf(error.kind));
    context.build.diagnostics().append(Vector<UserDiagnostic>(reported.userDiagnostics()));
}

static void printRetypedValues(PrintStream& out, const SelectionContext& selection)
{
    if (!selection.narrowedValues().empty()) {
        out.print("retyped:"_s);
        for (ValueID id : selection.narrowedValues())
            out.print(" v"_s, id.index());
        out.print("\n"_s);
    }
    if (selection.hasWidenedValue())
        out.println("widened"_s);
}

static void printClaimCounts(PrintStream& out, const SelectionContext& selection)
{
    std::span<const uint32_t> counts = selection.claimCounts();
    out.print("claims:"_s);
    for (unsigned index = 0; index < numberOfClaimResults; ++index) {
        if (counts[index])
            out.print(" "_s, nameOf(static_cast<ClaimResult>(index)), "="_s, counts[index]);
    }
    out.print("\n"_s);
}

void runClaimTestCommand(TestCommandContext& context)
{
    std::optional<FunctionID> id = parseFunctionWord(context);
    if (!id)
        return;
    std::optional<ClaimRequest> request = parseClaimRequest(context, context.program->at(*id));
    if (!request)
        return;

    SelectionTestSession session(context, *id);
    SelectionContext selection(session.pass());
    if (claimGroups(context.out, selection, *request) && request->rewrite) {
        if (!canRewrite(selection, *request)) {
            printBadArgument(context.out, request->rewriteWord);
            return;
        }

        Diagnostics reported = rewriteFirstGroup(selection, *request);
        printFunction(context.out, *context.program, *id, session.function());
        printInternalErrors(context, WTF::move(reported));
        verifyFunction(*context.program, *id, session.function(), context.form, context.build);
        printRetypedValues(context.out, selection);
    }
    printClaimCounts(context.out, selection);
}

static ASCIILiteral nameOf(LoopEntryResult result)
{
    switch (result) {
    case LoopEntryResult::Unknown:
        return "Unknown"_s;
    case LoopEntryResult::NoSingleEntry:
        return "NoSingleEntry"_s;
    case LoopEntryResult::HandlerDiffers:
        return "HandlerDiffers"_s;
    case LoopEntryResult::Emitted:
        return "Emitted"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool parseLoopEntryWord(const Function& function, StringView word, LoopEntryRequest& request)
{
    if (word == "emit"_s) {
        request.emits = true;
        return true;
    }
    if (std::optional<StringView> text = valueForKey(word, "user"_s)) {
        request.user = parseBlock(function, *text);
        return request.user.has_value();
    }
    if (std::optional<StringView> text = valueForKey(word, "operand"_s)) {
        request.operand = parseListedValue(function, *text);
        request.operandWord = word;
        return request.operand.has_value();
    }
    return false;
}

static std::optional<LoopEntryRequest> parseLoopEntryRequest(TestCommandContext& context, const Function& function)
{
    LoopEntryRequest request;
    bool isParsed = parseWordsAfterFunction(context, [&](StringView word) {
        return parseLoopEntryWord(function, word, request);
    });
    if (!isParsed)
        return std::nullopt;
    if (!request.user || !request.operand) {
        printMissingArgument(context.out);
        return std::nullopt;
    }
    return request;
}

static LoopEntryResult emitEntryUpcast(SelectionContext& selection, const InvariantOperand& invariant)
{
    FunctionPassContext& pass = selection.pass();
    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    return emitOnLoopEntry(selection, builder, invariant.header, [&](IRBuilder&) {
        Value upcast = IRBuilder::make(Opcode::Upcast, { invariant.value });
        upcast.setCastType(pass.function.at(invariant.value).type);
        builder.append(WTF::move(upcast));
    });
}

void runLoopEntryTestCommand(TestCommandContext& context)
{
    std::optional<FunctionID> id = parseFunctionWord(context);
    if (!id)
        return;
    std::optional<LoopEntryRequest> request = parseLoopEntryRequest(context, context.program->at(*id));
    if (!request)
        return;

    SelectionTestSession session(context, *id);
    SelectionContext selection(session.pass());
    if (selection.analyses().useLists().locationOf(*request->operand).placement == ValuePlacement::Detached) {
        printBadArgument(context.out, request->operandWord);
        return;
    }

    std::optional<InvariantOperand> invariant = findOutermostInvariantLoop(selection, *request->user, *request->operand);
    if (!invariant) {
        context.out.println("none"_s);
        return;
    }
    context.out.println("invariant: header=b"_s, invariant->header.index(), " value=v"_s, invariant->value.index());
    if (!request->emits)
        return;

    context.out.println(nameOf(emitEntryUpcast(selection, *invariant)));
    printFunction(context.out, *context.program, *id, session.function());
}

static std::optional<ChainKind> parseChainKind(StringView name)
{
    if (name == "Integers"_s)
        return ChainKind::Integers;
    if (name == "Numbers"_s)
        return ChainKind::Numbers;
    if (name == "Strings"_s)
        return ChainKind::Strings;
    return std::nullopt;
}

static bool parseChainWord(StringView word, ChainRequest& request)
{
    if (word == "convert"_s) {
        request.converts = true;
        return true;
    }
    if (std::optional<StringView> name = valueForKey(word, "kind"_s)) {
        request.kind = parseChainKind(*name);
        return request.kind.has_value();
    }
    if (std::optional<StringView> text = valueForKey(word, "min"_s)) {
        request.minCases = parseTestNumber<uint64_t>(*text);
        return request.minCases.has_value();
    }
    if (std::optional<StringView> text = valueForKey(word, "max"_s)) {
        request.maxCases = parseTestNumber<uint64_t>(*text);
        return request.maxCases.has_value();
    }
    return false;
}

static std::optional<ChainRequest> parseChainRequest(TestCommandContext& context)
{
    ChainRequest request;
    bool isParsed = parseWordsAfterFunction(context, [&](StringView word) {
        return parseChainWord(word, request);
    });
    if (!isParsed)
        return std::nullopt;
    if (!request.kind) {
        printMissingArgument(context.out);
        return std::nullopt;
    }
    return request;
}

static LimitID maxCasesLimitOf(ChainKind kind)
{
    switch (kind) {
    case ChainKind::Integers:
    case ChainKind::Numbers:
        return LimitID::MaxSwitchCases;
    case ChainKind::Strings:
        return LimitID::MaxSwitchStringCases;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ChainLimits limitsOf(const FunctionPassContext& pass, const ChainRequest& request)
{
    LimitID maxCasesLimit = maxCasesLimitOf(*request.kind);
    return {
        .minCases = request.minCases.value_or(pass.limit(LimitID::MinSwitchCases)),
        .maxCases = std::max<uint64_t>(request.maxCases.value_or(pass.limit(maxCasesLimit)), 1),
        .maxCasesLimit = maxCasesLimit,
    };
}

static void printCaseConstant(PrintStream& out, const Program& program, ChainKind kind, const ChainCase& chainCase)
{
    switch (kind) {
    case ChainKind::Integers:
    case ChainKind::Numbers:
        out.print(chainCase.number);
        return;
    case ChainKind::Strings:
        printString(out, program, chainCase.string);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printChain(PrintStream& out, const FunctionPassContext& pass, ChainKind kind, const ComparisonChain& chain)
{
    out.print("head=b"_s, chain.head.index(), " subject=v"_s, chain.subject.index(), " links="_s);
    CommaPrinter linkSeparator(","_s);
    for (BlockID link : chain.links)
        out.print(linkSeparator, "b"_s, link.index());

    out.print(" cases="_s);
    CommaPrinter caseSeparator(" "_s);
    for (const ChainCase& chainCase : chain.cases) {
        out.print(caseSeparator);
        printCaseConstant(out, pass.program, kind, chainCase);
        out.print(":b"_s, targetOf(pass.function, chainCase.edge).block.index());
    }
    out.println(" fall-through=b"_s, targetOf(pass.function, chain.fallThrough).block.index());
}

static void convertChains(FunctionPassContext& pass, ChainKind kind, const Vector<ComparisonChain>& chains)
{
    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    for (const ComparisonChain& chain : chains | std::views::reverse)
        convertChainToSwitch(builder, pass.analyses, kind, chain);
}

void runComparisonChainsTestCommand(TestCommandContext& context)
{
    std::optional<FunctionID> id = parseFunctionWord(context);
    if (!id)
        return;
    std::optional<ChainRequest> request = parseChainRequest(context);
    if (!request)
        return;

    SelectionTestSession session(context, *id);
    FunctionPassContext& pass = session.pass();
    ChainKind kind = *request->kind;
    Vector<ComparisonChain> chains = findComparisonChains(pass.analyses, kind, limitsOf(pass, *request), [](ValueID) {
        return true;
    });
    for (const ComparisonChain& chain : chains)
        printChain(context.out, pass, kind, chain);
    if (!request->converts)
        return;

    convertChains(pass, kind, chains);
    compactFunction(pass.function);
    pass.analyses.didChangeControlFlow();
    printFunction(context.out, pass.program, pass.id, pass.function);
    verifyFunction(pass.program, pass.id, pass.function, context.form, context.build);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
