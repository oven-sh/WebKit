/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpRequests.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTCFG.h"
#include "AOTFlowResults.h"
#include "AOTIR.h"
#include "AOTIRPrinter.h"
#include "AOTKnownValue.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpSummary.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/StackCheck.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using namespace Typed;

WTF_MAKE_TZONE_ALLOCATED_IMPL(RegExpRequests);

static_assert(Value::opcodesWithPattern == Value::opcodesWithRegExpFlags);
static_assert(Value::opcodesWithPattern == Value::opcodesWithRegExpRecord);

namespace {

struct CallsWithUnknownText;

struct CollectionInputs {
    const Program& program;
    const FlowResults& flow;
    BuildContext& context;
    CallsWithUnknownText& callsWithUnknownText;
};

struct FoundRegExp {
    RegExpRequestOrigin origin;
    StringID pattern;
    OptionSet<Yarr::Flags> flags;
    RegExpSite site;
};

struct RegExpConstructorCall {
    BuiltinCall builtinCall;
    AccessSiteID accessSite;
    RegExpSite site;
    BlockID block;
    SourceLocation location;
};

using FlagSets = Vector<OptionSet<Yarr::Flags>, 8>;

struct CallWithUnknownText {
    RegExpSite site;
    BlockID block;
    SourceLocation location;
    ValueID pattern;
    FlagSets flagSets;
    bool flagsAreComplete;
};

struct CallsWithUnknownText {
    Lock lock;
    Vector<CallWithUnknownText> calls;
};

// The text that an operand has if every variable holds what the only store to it in the program stores. Nothing is proven: the run time finds a record by the text it is given.
class TextFromConstants {
public:
    TextFromConstants(const Program& program, StringArena& strings, BuildContext& context)
        : m_program(program)
        , m_strings(strings)
        , m_context(context)
    {
        m_stores = collectInParallel<Store>(context.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Store>& stores) {
            for (const Value& value : program.functions[index].values) {
                if (value.opcode == Opcode::PutModuleVariable)
                    stores.append(Store { value.variable(), FunctionID(index), value.valueOperand() });
            }
        });
        std::ranges::sort(m_stores, { }, [](const Store& store) {
            return std::tuple { store.variable, store.function, store.stored };
        });
    }

    StringID textOf(const CallWithUnknownText& call)
    {
        m_subject = Subject { .function = call.site.function, .block = call.block, .value = call.site.value };
        KnownValue text = evaluate(Activation { call.site.function }, call.pattern);
        return text.kind() == KnownValue::Kind::String ? text.asString() : StringID();
    }

private:
    struct Store {
        VariableID variable;
        FunctionID function;
        ValueID stored;
    };

    struct Activation {
        FunctionID function;
        const Activation* caller { nullptr };
        const Value* call { nullptr };
    };

    bool isActive(const Activation& activation, FunctionID function) const
    {
        for (const Activation* active = &activation; active; active = active->caller) {
            if (active->function == function)
                return true;
        }
        return false;
    }

    KnownValue evaluate(const Activation& activation, ValueID id)
    {
        if (!id || !m_stackCheck.isSafeToRecurse())
            return { };
        const Function& function = m_program.at(activation.function);
        if (KnownValue known = knownValueOf(m_program, function, id); known.isKnown())
            return known;

        const Value& value = function.at(id);
        switch (value.opcode) {
        case Opcode::GetModuleVariable:
            return valueOf(value.variable());
        case Opcode::AssumeType:
            return evaluate(activation, value.valueOperand());
        case Opcode::Parameter:
            return argumentFor(activation, id);
        case Opcode::CallDirect:
            return resultOf(activation, value);
        default:
            return opcodesCreatingKnownStrings.contains(value.opcode) ? concatenate(activation, value) : KnownValue();
        }
    }

    KnownValue valueOf(VariableID variable)
    {
        auto known = m_variables.add(variable.index(), KnownValue());
        if (!known.isNewEntry)
            return known.iterator->value;
        auto stores = std::ranges::equal_range(m_stores, variable, { }, &Store::variable);
        if (stores.size() != 1)
            return { };
        KnownValue stored = evaluate(Activation { stores.front().function }, stores.front().stored);
        m_variables.set(variable.index(), stored);
        return stored;
    }

    KnownValue argumentFor(const Activation& activation, ValueID parameter)
    {
        if (!activation.call)
            return { };
        const auto& parameters = m_program.at(activation.function).blocks.first().parameters;
        std::span<const ValueID> arguments = activation.call->argumentsOperands();
        size_t index = parameters.find(parameter);
        if (index == notFound || parameters.size() != arguments.size())
            return { };
        return evaluate(*activation.caller, arguments[index]);
    }

    KnownValue resultOf(const Activation& activation, const Value& call)
    {
        FunctionID callee = call.function();
        const Function& function = m_program.at(callee);
        if (!function.hasBody() || function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || isActive(activation, callee))
            return { };
        ValueID returned;
        for (const Block& block : function.blocks) {
            ValueID result;
            switch (block.terminator.kind) {
            case TerminatorKind::Return:
                result = block.terminator.valueOperand();
                break;
            case TerminatorKind::ReturnCall:
                result = block.terminator.callOperand();
                break;
            default:
                continue;
            }
            if (!result || returned)
                return { };
            returned = result;
        }
        return evaluate(Activation { callee, &activation, &call }, returned);
    }

    KnownValue concatenate(const Activation& activation, const Value& value)
    {
        Vector<KnownValue, 8> operands;
        uint64_t bytes = 0;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            KnownValue operand = evaluate(activation, value.operand(index));
            if (operand.kind() != KnownValue::Kind::String)
                return { };
            bytes += m_program.string(operand.asString()).size();
            operands.append(operand);
        }
        if (m_context.exceeds(LimitID::RegExpCandidateTextLength, bytes, m_subject))
            return { };
        uint32_t next = 0;
        return evaluateStringOperation(m_program, m_strings, m_program.at(activation.function), value, [&](ValueID) {
            return operands[next++];
        });
    }

    const Program& m_program;
    StringArena& m_strings;
    BuildContext& m_context;
    StackCheck m_stackCheck;
    Subject m_subject;
    Vector<Store> m_stores;
    UncheckedKeyHashMap<uint32_t, KnownValue, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_variables;
};

} // anonymous namespace

static auto keyOf(const RegExpRequest& request)
{
    return std::pair { request.pattern.index(), request.flags.toRaw() };
}

static auto orderKey(const FoundRegExp& found)
{
    return std::tuple { found.origin, found.pattern, found.flags.toRaw(), found.site };
}

static bool isAscendingListOf(RegExpRequestOrigin origin, std::span<const RegExpRequest> requests)
{
    bool hasOrigin = std::ranges::all_of(requests, [&](const RegExpRequest& request) {
        return request.origin == origin;
    });
    return hasOrigin && std::ranges::adjacent_find(requests, std::ranges::greater_equal { }, keyOf) == requests.end();
}

static std::optional<size_t> findRequest(std::span<const RegExpRequest> ascendingRequests, StringID pattern, OptionSet<Yarr::Flags> significantFlags)
{
    std::pair key { pattern.index(), significantFlags.toRaw() };
    auto found = std::ranges::lower_bound(ascendingRequests, key, { }, keyOf);
    if (found == ascendingRequests.end() || keyOf(*found) != key)
        return std::nullopt;
    return static_cast<size_t>(found - ascendingRequests.begin());
}

RegExpRequests::RegExpRequests(Vector<RegExpRequest>&& literals, Vector<RegExpRequest>&& candidates)
    : m_requests(WTF::move(literals))
    , m_numberOfLiterals(static_cast<uint32_t>(m_requests.size()))
{
    RELEASE_ASSERT(isAscendingListOf(RegExpRequestOrigin::Literal, m_requests.span()));
    RELEASE_ASSERT(isAscendingListOf(RegExpRequestOrigin::Candidate, candidates.span()));
    m_requests.appendVector(WTF::move(candidates));
}

RegExpRecordID RegExpRequests::recordOf(StringID pattern, OptionSet<Yarr::Flags> flags) const
{
    std::optional<size_t> index = findRequest(literals(), pattern, ImageRegExp::significantFlags(flags));
    RELEASE_ASSERT(index);
    return RegExpRecordID(*index);
}

static bool callsRegExpConstructor(const Program& program, BuiltinOverloadID overload)
{
    const BuiltinTable& builtins = program.builtins();
    return builtins.canonicalID(builtins.at(overload).builtin) == Builtins::RegExp;
}

static bool mayPassArgument(const BuiltinCall& call, uint32_t argumentIndex)
{
    switch (call.form) {
    case BuiltinCallForm::List:
        return true;
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
    case BuiltinCallForm::AnyReceiver:
        return argumentIndex < call.arguments.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlagSets parseFlagSets(const Program& program, const SinkCandidates& candidates)
{
    FlagSets flagSets;
    // Unknown flags may be "", "g" or "d", which all find the record without significant flags.
    if (candidates.mayBeUndefined || !candidates.isComplete)
        flagSets.append(OptionSet<Yarr::Flags> { });
    Vector<char16_t> buffer;
    for (StringID text : candidates.strings) {
        std::optional<OptionSet<Yarr::Flags>> flags = Yarr::parseFlags(program.strings.viewOf(text, buffer));
        if (!flags)
            continue;
        flagSets.appendIfNotContains(ImageRegExp::significantFlags(*flags));
    }
    return flagSets;
}

static void reportTextKnownAtRunTime(BuildContext& context, RegExpSite site, BlockID block, SourceLocation location)
{
    context.diagnostics().add(UserDiagnostic {
        .kind = UserDiagnosticKind::RegExpWithoutCode,
        .function = site.function,
        .block = block,
        .value = site.value,
        .count = static_cast<uint32_t>(RegExpDeclineReason::TextKnownAtRunTime),
        .location = location,
    });
}

static void addCandidates(const Program& program, BuildContext& context, StringID pattern, const FlagSets& flagSets, RegExpSite site, Vector<FoundRegExp>& found)
{
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(pattern, buffer);
    for (OptionSet<Yarr::Flags> flagSet : flagSets) {
        const RegExpSummary& summary = context.regExpSummaries().summaryOf(pattern, flagSet, text);
        // Plain characters are searched for without code, and the run time sees that they are plain when it parses them.
        if (!summary.isValid() || !summary.atom.isNull())
            continue;
        found.append(FoundRegExp { RegExpRequestOrigin::Candidate, pattern, flagSet, site });
    }
}

static void collectCandidates(const CollectionInputs& inputs, const RegExpConstructorCall& call, Vector<FoundRegExp>& found)
{
    const SinkCandidates notPassed { .strings = { }, .mayBeUndefined = true, .isComplete = true };
    auto candidatesOf = [&](uint32_t argumentIndex) -> const SinkCandidates& {
        return mayPassArgument(call.builtinCall, argumentIndex) ? inputs.flow.sinkCandidates(call.accessSite, argumentIndex) : notPassed;
    };
    const SinkCandidates& patterns = candidatesOf(0);
    const SinkCandidates& flags = candidatesOf(1);

    const StringTable& strings = inputs.program.strings;
    FlagSets flagSets = parseFlagSets(inputs.program, flags);
    auto addPattern = [&](StringID pattern) {
        addCandidates(inputs.program, inputs.context, pattern, flagSets, call.site, found);
    };
    for (StringID pattern : patterns.strings)
        addPattern(pattern);
    if (patterns.mayBeUndefined && !patterns.strings.contains(strings.emptyString()))
        addPattern(strings.emptyString());

    if (patterns.isComplete && flags.isComplete)
        return;
    bool passesPatternAsOperand = call.builtinCall.form != BuiltinCallForm::List && !call.builtinCall.arguments.empty();
    if (patterns.isComplete || !passesPatternAsOperand) {
        reportTextKnownAtRunTime(inputs.context, call.site, call.block, call.location);
        return;
    }
    Locker locker { inputs.callsWithUnknownText.lock };
    inputs.callsWithUnknownText.calls.append(CallWithUnknownText { call.site, call.block, call.location, call.builtinCall.arguments[0], WTF::move(flagSets), flags.isComplete });
}

static void addCandidatesFromConstants(const Program& program, StringArena& strings, BuildContext& context, Vector<CallWithUnknownText>&& calls, Vector<FoundRegExp>& found)
{
    if (calls.isEmpty())
        return;
    std::ranges::sort(calls, { }, &CallWithUnknownText::site);
    TextFromConstants constants(program, strings, context);
    for (const CallWithUnknownText& call : calls) {
        StringID pattern = constants.textOf(call);
        if (pattern)
            addCandidates(program, context, pattern, call.flagSets, call.site, found);
        if (!pattern || !call.flagsAreComplete)
            reportTextKnownAtRunTime(context, call.site, call.block, call.location);
    }
}

static void collectFromFunction(const CollectionInputs& inputs, FunctionID id, Vector<FoundRegExp>& found)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    if (!function.hasBody())
        return;

    Vector<RegExpConstructorCall, 4> calls;
    // Placement asks for the record of a literal in every block, reachable or not.
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID blockID(blockIndex);
        for (ValueID valueID : function.at(blockID).values) {
            const Value& value = function.at(valueID);
            RegExpSite site { id, valueID };
            if (Value::opcodesWithPattern.contains(value.opcode))
                found.append(FoundRegExp { RegExpRequestOrigin::Literal, value.pattern(), ImageRegExp::significantFlags(toYarrFlags(value.regExpFlags())), site });
            else if (builtinCallOpcodes.contains(value.opcode) && callsRegExpConstructor(program, value.overload()))
                calls.append(RegExpConstructorCall { builtinCallOf(program, value), value.accessSite(), site, blockID, function.locationOf(valueID) });
        }
    }
    if (calls.isEmpty())
        return;

    CFG cfg(function);
    for (const RegExpConstructorCall& call : calls) {
        if (cfg.isReachable(call.block))
            collectCandidates(inputs, call, found);
    }
}

static void addToRequests(Vector<RegExpRequest>& ascendingRequests, const FoundRegExp& found)
{
    if (ascendingRequests.isEmpty() || ascendingRequests.last().pattern != found.pattern || ascendingRequests.last().flags != found.flags)
        ascendingRequests.append(RegExpRequest { .pattern = found.pattern, .flags = found.flags, .origin = found.origin });
    Vector<RegExpSite, 1>& sites = ascendingRequests.last().sites;
    if (sites.isEmpty() || sites.last() != found.site)
        sites.append(found.site);
}

static void mergeCandidatesIntoLiterals(Vector<RegExpRequest>& literals, Vector<RegExpRequest>& candidates)
{
    candidates.removeAllMatching([&](const RegExpRequest& candidate) {
        std::optional<size_t> literal = findRequest(literals.span(), candidate.pattern, candidate.flags);
        if (!literal)
            return false;
        literals[*literal].sites.appendVector(candidate.sites);
        return true;
    });
}

static void internMetadataStrings(RegExpRequest& request, const Program& program, StringArena& strings, BuildContext& context)
{
    Vector<char16_t> buffer;
    StringView text = program.strings.viewOf(request.pattern, buffer);
    const RegExpSummary& summary = context.regExpSummaries().summaryOf(request.pattern, request.flags, text);
    switch (summary.metadata) {
    case RegExpMetadata::Unknown:
        return;
    case RegExpMetadata::Known:
        break;
    }

    if (!summary.atom.isNull())
        request.atom = StringView(summary.atom) == text ? request.pattern : strings.intern(StringView(summary.atom));
    request.captureGroupNames = summary.captureGroupNames.map([&](const String& name) {
        return name.isNull() ? StringID() : strings.intern(StringView(name));
    });
    request.namedGroups = summary.namedGroups.map([&](const RegExpNamedGroupSummary& group) {
        return RegExpNamedGroupStrings { strings.intern(StringView(group.name)), group.parenIndices };
    });
}

RegExpRequests collectRegExpRequests(const Program& program, const FlowResults& flow, StringArena& strings, BuildContext& context)
{
    CallsWithUnknownText callsWithUnknownText;
    CollectionInputs inputs { program, flow, context, callsWithUnknownText };
    Vector<FoundRegExp> found = collectInParallel<FoundRegExp>(context.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FoundRegExp>& foundInFunction) {
        collectFromFunction(inputs, FunctionID(index), foundInFunction);
    });
    addCandidatesFromConstants(program, strings, context, WTF::move(callsWithUnknownText.calls), found);
    std::ranges::sort(found, { }, orderKey);

    Vector<RegExpRequest> literals;
    Vector<RegExpRequest> candidates;
    for (const FoundRegExp& entry : found) {
        switch (entry.origin) {
        case RegExpRequestOrigin::Literal:
            addToRequests(literals, entry);
            break;
        case RegExpRequestOrigin::Candidate:
            addToRequests(candidates, entry);
            break;
        }
    }
    mergeCandidatesIntoLiterals(literals, candidates);

    for (RegExpRequest& request : literals)
        internMetadataStrings(request, program, strings, context);
    for (RegExpRequest& request : candidates)
        internMetadataStrings(request, program, strings, context);

    context.statistics().countChanges(PassID::CollectRegExpLiterals, literals.size() + candidates.size());
    return RegExpRequests(WTF::move(literals), WTF::move(candidates));
}

static ASCIILiteral originName(RegExpRequestOrigin origin)
{
    switch (origin) {
    case RegExpRequestOrigin::Literal:
        return "literal"_s;
    case RegExpRequestOrigin::Candidate:
        return "candidate"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RegExpRequests::dump(PrintStream& out, const Program& program) const
{
    out.print("regexp requests: ", m_requests.size(), "\n");
    for (size_t index = 0; index < m_requests.size(); ++index) {
        const RegExpRequest& request = m_requests[index];
        out.print("  ", index, " ", originName(request.origin), " ");
        printString(out, program, request.pattern);
        Yarr::FlagsString characters = Yarr::flagsString(request.flags);
        out.print(" flags=");
        if (request.flags.isEmpty())
            out.print("-");
        else
            out.print(StringView(characters.span()));
        out.print(" sites=");
        CommaPrinter comma;
        for (const RegExpSite& site : request.sites)
            out.print(comma, "f", site.function.index(), ":v", site.value.index());
        out.print("\n");
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
