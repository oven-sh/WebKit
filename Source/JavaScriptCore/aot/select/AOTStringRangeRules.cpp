/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTSelectionContext.h"
#include "AOTStringRuleSupport.h"
#include "AOTStringTable.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class UseKind : uint8_t {
    Other,
    Read,
    Length,
    CompareCodeUnit,
    TestEmpty,
    FoldToFalse,
    Nested,
    SwitchOnCodeUnit,
    TestPresence,
};

struct RangeUse {
    UseKind kind { UseKind::Other };
    ValueID user { };
    BlockID block { };
    uint32_t operandIndex { 0 };
    char16_t codeUnit { 0 };
    bool comparesUnsigned { false };
    bool holdsForString { false };
    CharacterRead read { };
};

struct RangeUses {
    Vector<RangeUse, 4> uses;
    Vector<ValueID, 2> upcasts;
};

enum class RangeForm : uint8_t { Unchanged, CodeUnit, Erased, RangeOfBase };

struct RangeValue {
    ValueID value;
    StringRangeSource source;
    std::optional<uint32_t> enclosing { };
    Vector<StringRangeSource, 2> chain { };
    RangeForm form { RangeForm::Unchanged };
};

enum class Action : uint8_t {
    ReadCharacters,
    ComputeLength,
    CompareOwnCodeUnit,
    CompareSharedCodeUnit,
    TestSharedCodeUnit,
    TestEmpty,
    FoldToFalse,
    BecomeCodeUnit,
    BecomeRangeOfBase,
    Erase,
};

struct Match {
    SelectionGroup group;
    Action action;
    ProgramPoint point;
    Vector<StringRangeSource, 2> chain { };
    ValueID base { };
    CharacterRead read { };
    uint32_t operandIndex { 0 };
    char16_t codeUnit { 0 };
    bool comparesUnsigned { false };
    bool holdsForString { false };
    ValueID sharedUnit { };
    BlockID branchBlock { };
    Vector<BlockID, 1> switchBlocks { };
};

class RangeMatcher {
    WTF_MAKE_NONCOPYABLE(RangeMatcher);
public:
    RangeMatcher(SelectionContext&, Vector<RangeValue>&&);

    Vector<Match> run();

private:
    void sortRanges();
    void findChains();
    void matchRange(uint32_t index);

    RangeUses collectUses(uint32_t index);
    RangeUse classify(uint32_t index, const StringRangeSource&, const Use&);
    void classifyOperand(uint32_t index, const StringRangeSource&, const Use&, RangeUse&);
    bool classifyTypeTest(const Use&, RangeUse&) const;
    void appendNarrowedStrings(const RangeUse& test, Vector<ValueID, 4>&) const;
    void classifyComparison(const StringRangeSource&, const Use&, const LiteralArgument&, RangeUse&) const;
    UseKind classifyTerminator(const StringRangeSource&, BlockID) const;

    bool tryClaim(ValueID);
    bool tryClaimWithUpcasts(ValueID range, std::span<const ValueID> upcasts);
    bool tryClaimWithTests(ValueID range, const RangeUses&);
    void claimUsers(RangeUses&);
    RangeForm chooseForm(const RangeValue&, RangeUses&);

    Match createMatch(ValueID root, Action, const RangeValue&) const;
    void addMatches(const RangeValue&, const RangeUses&);

    SelectionContext& m_context;
    const Program& m_program;
    const Function& m_function;
    const UseLists& m_uses;
    Vector<RangeValue> m_ranges;
    UncheckedKeyHashMap<ValueID, uint32_t> m_indexes;
    Vector<Match> m_matches;
};

} // anonymous namespace

static std::optional<char16_t> singleCodeUnitOf(const Program& program, StringID string)
{
    Vector<char16_t> units = decodeProgramString(program.string(string));
    if (units.size() != 1)
        return std::nullopt;
    return units[0];
}

static bool readsLength(const Program& program, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::GetStringLength)
        return true;
    if (user.opcode != Opcode::CallBuiltin || user.builtinCallForm() != BuiltinCallForm::Whole || !builtinOperandAt(user, operandIndex).isReceiver())
        return false;
    return inlineImplementationOf(program, user) == InlineBuiltin::StringLength;
}

static bool treatsUndefinedAsEmptyString(const Program& program, const CharacterRead& read)
{
    if (read.rangeRead == Opcode::StringRangeEqualsLiteral)
        return !program.string(read.literal).empty();
    // "undefined" begins with a digit of every radix above 30.
    return read.rangeRead == Opcode::StringRangeParseInt && !read.radix;
}

static bool replacesUser(UseKind kind)
{
    switch (kind) {
    case UseKind::Read:
    case UseKind::Length:
    case UseKind::CompareCodeUnit:
    case UseKind::TestEmpty:
    case UseKind::FoldToFalse:
        return true;
    case UseKind::Other:
    case UseKind::Nested:
    case UseKind::SwitchOnCodeUnit:
    case UseKind::TestPresence:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Action> actionOf(UseKind kind, RangeForm form)
{
    switch (kind) {
    case UseKind::Read:
        return Action::ReadCharacters;
    case UseKind::Length:
        return Action::ComputeLength;
    case UseKind::CompareCodeUnit:
        return form == RangeForm::CodeUnit ? Action::CompareSharedCodeUnit : Action::CompareOwnCodeUnit;
    case UseKind::TestEmpty:
        return Action::TestEmpty;
    case UseKind::FoldToFalse:
        return Action::FoldToFalse;
    case UseKind::TestPresence:
        return Action::TestSharedCodeUnit;
    case UseKind::Other:
    case UseKind::Nested:
    case UseKind::SwitchOnCodeUnit:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canShareCodeUnit(const StringRangeSource& source, const RangeUses& uses)
{
    if (!source.isSingleUnit() || !uses.upcasts.isEmpty())
        return false;

    unsigned numberOfComparisons = 0;
    bool needsSharedUnit = false;
    for (const RangeUse& use : uses.uses) {
        switch (use.kind) {
        case UseKind::CompareCodeUnit:
            ++numberOfComparisons;
            break;
        case UseKind::SwitchOnCodeUnit:
        case UseKind::TestPresence:
            needsSharedUnit = true;
            break;
        case UseKind::TestEmpty:
        case UseKind::FoldToFalse:
            break;
        case UseKind::Other:
        case UseKind::Read:
        case UseKind::Length:
        case UseKind::Nested:
            return false;
        }
    }
    return needsSharedUnit || numberOfComparisons >= 2;
}

static Vector<RangeValue> collectRanges(SelectionContext& context)
{
    const Program& program = context.program();
    const Function& function = context.function();
    Vector<RangeValue> ranges;
    for (Opcode opcode : { Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::CallBuiltin, Opcode::ElemGet }) {
        for (ValueID id : context.candidates(opcode)) {
            if (context.isOnSlowPath(id))
                continue;
            if (std::optional<StringRangeSource> source = stringRangeSourceOf(program, function, function.at(id)))
                ranges.append(RangeValue { .value = id, .source = *source });
        }
    }
    return ranges;
}

RangeMatcher::RangeMatcher(SelectionContext& context, Vector<RangeValue>&& ranges)
    : m_context(context)
    , m_program(context.program())
    , m_function(context.function())
    , m_uses(context.analyses().useLists())
    , m_ranges(WTF::move(ranges))
{
}

void RangeMatcher::sortRanges()
{
    const CFG& cfg = m_context.analyses().cfg();
    std::ranges::sort(m_ranges, { }, [&](const RangeValue& range) {
        ProgramPoint point = m_uses.pointOf(range.value);
        return std::pair { cfg.reversePostOrderIndex(point.block), point.index };
    });
    for (uint32_t index = 0; index < m_ranges.size(); ++index)
        m_indexes.add(m_ranges[index].value, index);
}

void RangeMatcher::findChains()
{
    for (uint32_t index = 0; index < m_ranges.size(); ++index) {
        RangeValue& range = m_ranges[index];
        auto enclosing = m_indexes.find(definitionThroughUpcasts(m_function, range.source.string));
        // Negative bounds mean undefined, and adding the start of an enclosing range to them would lose that.
        bool encodesAbsenceInBounds = range.source.bounds && range.source.mayBeUndefined;
        if (enclosing != m_indexes.end() && !encodesAbsenceInBounds) {
            RELEASE_ASSERT(enclosing->value < index);
            const RangeValue& outer = m_ranges[enclosing->value];
            if (!outer.source.mayBeUndefined && !m_context.pass().exceeds(LimitID::MaxSelectionGroupMembers, outer.chain.size() + 1, range.value)) {
                range.enclosing = enclosing->value;
                range.chain = outer.chain;
            }
        }
        range.chain.append(range.source);
    }
}

void RangeMatcher::classifyComparison(const StringRangeSource& source, const Use& use, const LiteralArgument& literal, RangeUse& result) const
{
    Opcode opcode = m_function.at(use.user).opcode;
    bool comparesOneUnit = literal.text.length() == 1;
    if (relationalOperators.contains(opcode)) {
        if (!comparesOneUnit)
            return;
        // -1 stands for an empty range: it is below every code unit, as the empty string is below the literal.
        // No relation holds for undefined: read as an unsigned number, -1 is above every code unit.
        bool isGreaterComparison = opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
        bool rangeIsOnLeft = OperandPosition::left(opcode) == use.operandIndex;
        bool holdsForMinusOne = isGreaterComparison != rangeIsOnLeft;
        result.comparesUnsigned = source.mayBeUndefined && holdsForMinusOne;
    } else if (!comparesOneUnit) {
        bool mayBeEmptyString = !source.mayBeUndefined;
        result.kind = mayBeEmptyString && literal.text.isEmpty() ? UseKind::TestEmpty : UseKind::FoldToFalse;
        return;
    }
    result.kind = UseKind::CompareCodeUnit;
    result.codeUnit = literal.text[0];
}

UseKind RangeMatcher::classifyTerminator(const StringRangeSource& source, BlockID block) const
{
    const Terminator& terminator = m_function.at(block).terminator;
    if (terminator.kind != TerminatorKind::SwitchString || !source.isSingleUnit() || m_context.passesProofs(block))
        return UseKind::Other;

    bool comparesOneUnit = std::ranges::all_of(terminator.strings(), [&](StringID string) {
        return singleCodeUnitOf(m_program, string).has_value();
    });
    return comparesOneUnit ? UseKind::SwitchOnCodeUnit : UseKind::Other;
}

// Tells the character from undefined. The narrowed values that its branch passes to the targets are still the range.
bool RangeMatcher::classifyTypeTest(const Use& use, RangeUse& result) const
{
    const Value& test = m_function.at(use.user);
    if (test.opcode != Opcode::TypeTest)
        return false;
    const Type& tested = m_program.types.at(test.testedType());
    bool admitsEveryString = Type::fromPrimitives(Primitive::AnyString).isSubsetOf(tested, m_program);
    bool admitsNoString = !(tested.primitives & Primitive::AnyString);
    bool admitsUndefined = !!(tested.primitives & Primitive::Undefined);
    if (admitsUndefined ? !admitsNoString : !admitsEveryString)
        return false;

    std::optional<Use> branch = m_uses.singleUse(use.user);
    if (!branch || branch->kind != Use::Kind::TerminatorOperand || m_function.at(branch->block).terminator.kind != TerminatorKind::Branch)
        return false;
    const Terminator& terminator = m_function.at(branch->block).terminator;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        const Target& target = terminator.targets[targetIndex];
        if (!target.proofs.isEmpty() && m_context.analyses().cfg().singleIncomingEdge(target.block) != Edge { branch->block, targetIndex })
            return false;
    }

    result.kind = UseKind::TestPresence;
    result.block = branch->block;
    result.holdsForString = !admitsUndefined;
    return true;
}

void RangeMatcher::appendNarrowedStrings(const RangeUse& test, Vector<ValueID, 4>& strings) const
{
    Edge edge { test.block, test.holdsForString ? 0u : 1u };
    auto& parameters = m_function.at(targetOf(m_function, edge).block).parameters;
    for (size_t index = numberOfExplicitArguments(m_function, edge); index < parameters.size(); ++index)
        strings.append(parameters[index]);
}

void RangeMatcher::classifyOperand(uint32_t index, const StringRangeSource& source, const Use& use, RangeUse& result)
{
    if (auto inner = m_indexes.find(use.user); inner != m_indexes.end()) {
        const RangeValue& innerRange = m_ranges[inner->value];
        if (innerRange.enclosing == index && innerRange.form != RangeForm::Unchanged)
            result.kind = UseKind::Nested;
        return;
    }
    if (m_context.isClaimed(use.user))
        return;

    if (source.mayBeUndefined && classifyTypeTest(use, result))
        return;
    if (source.isSingleUnit()) {
        if (std::optional<LiteralArgument> literal = comparedLiteralOf(m_context, use)) {
            classifyComparison(source, use, *literal, result);
            return;
        }
    }
    if (readsLength(m_program, m_function.at(use.user), use.operandIndex)) {
        if (!source.mayBeUndefined)
            result.kind = UseKind::Length;
        return;
    }

    std::optional<CharacterRead> read = characterReadOf(m_context, use);
    if (!read || (source.mayBeUndefined && !treatsUndefinedAsEmptyString(m_program, *read)))
        return;
    result.kind = UseKind::Read;
    result.read = *read;
}

RangeUse RangeMatcher::classify(uint32_t index, const StringRangeSource& source, const Use& use)
{
    RangeUse result { .user = use.user, .block = use.block, .operandIndex = use.operandIndex };
    switch (use.kind) {
    case Use::Kind::Operand:
        classifyOperand(index, source, use, result);
        return result;
    case Use::Kind::TerminatorOperand:
        result.kind = classifyTerminator(source, use.block);
        return result;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return result;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RangeUses RangeMatcher::collectUses(uint32_t index)
{
    RangeUses result;
    auto collect = [&](const StringRangeSource& source, Vector<ValueID, 4>&& worklist, Vector<ValueID, 4>& narrowedStrings) {
        while (!worklist.isEmpty()) {
            for (const Use& use : m_uses.usesOf(worklist.takeLast())) {
                if (use.kind == Use::Kind::Operand && m_function.at(use.user).opcode == Opcode::Upcast) {
                    result.upcasts.append(use.user);
                    worklist.append(use.user);
                    continue;
                }
                result.uses.append(classify(index, source, use));
                if (result.uses.last().kind == UseKind::TestPresence)
                    appendNarrowedStrings(result.uses.last(), narrowedStrings);
            }
        }
    };

    StringRangeSource source = m_ranges[index].source;
    Vector<ValueID, 4> narrowedStrings;
    collect(source, { m_ranges[index].value }, narrowedStrings);
    source.mayBeUndefined = false;
    Vector<ValueID, 4> none;
    collect(source, WTF::move(narrowedStrings), none);
    RELEASE_ASSERT(none.isEmpty());
    return result;
}

bool RangeMatcher::tryClaim(ValueID id)
{
    return m_context.tryClaim(SelectionRuleID::SubstringView, groupOfOne(id)) == ClaimResult::Claimed;
}

bool RangeMatcher::tryClaimWithUpcasts(ValueID range, std::span<const ValueID> upcasts)
{
    Vector<SelectionGroup, 4> groups;
    groups.append(groupOfOne(range));
    for (ValueID upcast : upcasts)
        groups.append(groupOfOne(upcast));
    return m_context.tryClaimAll(SelectionRuleID::SubstringView, groups.span()) == ClaimResult::Claimed;
}

bool RangeMatcher::tryClaimWithTests(ValueID range, const RangeUses& uses)
{
    Vector<SelectionGroup, 4> groups;
    groups.append(groupOfOne(range));
    for (const RangeUse& use : uses.uses) {
        if (use.kind == UseKind::TestPresence)
            groups.append(groupOfOne(use.user));
    }
    return m_context.tryClaimAll(SelectionRuleID::SubstringView, groups.span()) == ClaimResult::Claimed;
}

void RangeMatcher::claimUsers(RangeUses& uses)
{
    for (RangeUse& use : uses.uses) {
        if (replacesUser(use.kind) && !tryClaim(use.user))
            use.kind = UseKind::Other;
    }
}

RangeForm RangeMatcher::chooseForm(const RangeValue& range, RangeUses& uses)
{
    if (canShareCodeUnit(range.source, uses) && tryClaimWithTests(range.value, uses))
        return RangeForm::CodeUnit;

    bool hasOtherUse = false;
    for (RangeUse& use : uses.uses) {
        if (use.kind == UseKind::SwitchOnCodeUnit || use.kind == UseKind::TestPresence)
            use.kind = UseKind::Other;
        hasOtherUse |= use.kind == UseKind::Other;
    }
    if (!hasOtherUse && tryClaimWithUpcasts(range.value, uses.upcasts.span()))
        return RangeForm::Erased;
    if (range.chain.size() > 1 && tryClaim(range.value))
        return RangeForm::RangeOfBase;
    return RangeForm::Unchanged;
}

Match RangeMatcher::createMatch(ValueID root, Action action, const RangeValue& range) const
{
    return { .group = groupOfOne(root), .action = action, .point = m_uses.pointOf(root), .chain = range.chain, .base = range.chain.first().string };
}

void RangeMatcher::addMatches(const RangeValue& range, const RangeUses& uses)
{
    for (const RangeUse& use : uses.uses) {
        std::optional<Action> action = actionOf(use.kind, range.form);
        if (!action)
            continue;
        Match match = createMatch(use.user, *action, range);
        match.read = use.read;
        match.operandIndex = use.operandIndex;
        match.codeUnit = use.codeUnit;
        match.comparesUnsigned = use.comparesUnsigned;
        match.holdsForString = use.holdsForString;
        match.sharedUnit = range.value;
        match.branchBlock = use.block;
        m_matches.append(WTF::move(match));
    }

    switch (range.form) {
    case RangeForm::Unchanged:
        return;
    case RangeForm::CodeUnit: {
        Match match = createMatch(range.value, Action::BecomeCodeUnit, range);
        for (const RangeUse& use : uses.uses) {
            if (use.kind == UseKind::SwitchOnCodeUnit)
                match.switchBlocks.append(use.block);
        }
        m_matches.append(WTF::move(match));
        return;
    }
    case RangeForm::Erased:
        m_matches.append(createMatch(range.value, Action::Erase, range));
        for (ValueID upcast : uses.upcasts)
            m_matches.append(createMatch(upcast, Action::Erase, range));
        return;
    case RangeForm::RangeOfBase:
        m_matches.append(createMatch(range.value, Action::BecomeRangeOfBase, range));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RangeMatcher::matchRange(uint32_t index)
{
    RangeUses uses = collectUses(index);
    claimUsers(uses);

    RangeValue& range = m_ranges[index];
    range.form = chooseForm(range, uses);
    addMatches(range, uses);
}

Vector<Match> RangeMatcher::run()
{
    sortRanges();
    findChains();
    // An enclosed range comes after the enclosing one, which is still read unless the enclosed one got another form.
    for (uint32_t index = m_ranges.size(); index--;)
        matchRange(index);
    return std::exchange(m_matches, { });
}

static std::optional<StringRangeBounds> insertBounds(GroupRewriter& rewriter, const Match& match, std::span<const StringRangeSource> chain)
{
    std::optional<StringRangeBounds> bounds;
    for (const StringRangeSource& source : chain)
        bounds = insertStringRange(rewriter.builder(), match.point, source, bounds);
    return bounds;
}

static StringRangeBounds insertBounds(GroupRewriter& rewriter, const Match& match)
{
    return *insertBounds(rewriter, match, match.chain.span());
}

static void replaceRootWith(GroupRewriter& rewriter, Value&& replacement)
{
    Value& root = rewriter.replaceRoot(replacement.opcode);
    replacement.type = root.type;
    replacement.setOrigin(root.origin());
    root = WTF::move(replacement);
}

static Value makeCodeUnitRead(GroupRewriter& rewriter, const Match& match)
{
    const StringRangeSource& unit = match.chain.last();
    RELEASE_ASSERT(unit.isSingleUnit());
    if (unit.method == StringRangeMethod::SingleIndex && unit.first) {
        std::optional<StringRangeBounds> enclosing = insertBounds(rewriter, match, match.chain.span().first(match.chain.size() - 1));
        return makeStringRangeRead(Opcode::StringRangeCodeUnitOrMinusOne, match.base, enclosing, unit.first);
    }

    StringRangeBounds bounds = insertBounds(rewriter, match);
    ValueID zero = rewriter.insert(makeInteger(0));
    return makeStringRangeRead(Opcode::StringRangeCodeUnitOrMinusOne, match.base, bounds, zero);
}

static void setComparedOperands(Value& comparison, const Match& match, ValueID unit, ValueID codeUnit)
{
    bool unitIsOnLeft = OperandPosition::left(comparison.opcode) == match.operandIndex;
    comparison.setLeftOperand(unitIsOnLeft ? unit : codeUnit);
    comparison.setRightOperand(unitIsOnLeft ? codeUnit : unit);
}

static void readCharacters(GroupRewriter& rewriter, const Match& match)
{
    CharacterRead read = match.read;
    if (!read.position && OperandPosition::position(read.rangeRead))
        read.position = rewriter.insert(makeInteger(0));
    replaceRootWith(rewriter, makeCharacterRead(read, match.base, insertBounds(rewriter, match)));
    rewriter.leaveUnclaimed(match.group.root);
}

static void compareCodeUnit(GroupRewriter& rewriter, const Match& match, ValueID unit)
{
    ValueID root = match.group.root;
    Opcode opcode = rewriter.original(root).opcode;
    ValueID codeUnit = rewriter.insert(makeInteger(match.codeUnit));
    if (match.comparesUnsigned)
        unit = rewriter.insert(IRBuilder::make(Opcode::ToUint32, { unit }));
    setComparedOperands(rewriter.replaceRoot(opcode), match, unit, codeUnit);
    rewriter.leaveUnclaimed(root);
}

static void compareOwnCodeUnit(GroupRewriter& rewriter, const Match& match)
{
    ValueID unit = rewriter.insert(makeCodeUnitRead(rewriter, match));
    rewriter.leaveUnclaimed(unit);
    compareCodeUnit(rewriter, match, unit);
}

static void compareSharedCodeUnit(GroupRewriter& rewriter, const Match& match)
{
    compareCodeUnit(rewriter, match, match.sharedUnit);
}

// An integer comparison narrows no string, so the parameters that the test narrowed are removed. The strings have no uses left, and the value on the other edge is undefined.
static void testSharedCodeUnit(GroupRewriter& rewriter, const Match& match, ValueSubstitution& substitution)
{
    ValueID root = match.group.root;
    ValueID zero = rewriter.insert(makeInteger(0));
    ValueID undefinedValue = rewriter.insert(IRBuilder::make(Opcode::Undefined));
    Value& comparison = rewriter.replaceRoot(match.holdsForString ? Opcode::GreaterEq : Opcode::Less);
    comparison.setLeftOperand(match.sharedUnit);
    comparison.setRightOperand(zero);
    rewriter.leaveUnclaimed(root);

    Function& function = rewriter.function();
    uint32_t stringTargetIndex = match.holdsForString ? 0 : 1;
    for (uint32_t targetIndex = 0; targetIndex < function.at(match.branchBlock).terminator.targets.size(); ++targetIndex) {
        Edge edge { match.branchBlock, targetIndex };
        BlockID target = targetOf(function, edge).block;
        BitVector narrowedParameters;
        for (size_t index = numberOfExplicitArguments(function, edge); index < function.at(target).parameters.size(); ++index) {
            if (targetIndex != stringTargetIndex)
                substitution.replace(function.at(target).parameters[index], undefinedValue);
            narrowedParameters.set(index);
        }
        rewriter.builder().removeParameters(target, narrowedParameters);
    }
}

static void switchOnCodeUnit(const Program& program, Terminator& terminator, ValueID unit)
{
    Vector<int64_t, 8> cases;
    for (StringID string : terminator.strings())
        cases.append(*singleCodeUnitOf(program, string));

    Terminator next(TerminatorKind::Switch);
    next.setSubjectOperand(unit);
    next.setCases(cases.span());
    next.setOrigin(terminator.origin());
    next.targets = WTF::move(terminator.targets);
    terminator = WTF::move(next);
}

static void becomeCodeUnit(const Program& program, GroupRewriter& rewriter, const Match& match)
{
    ValueID root = match.group.root;
    replaceRootWith(rewriter, makeCodeUnitRead(rewriter, match));
    rewriter.leaveUnclaimed(root);
    for (BlockID block : match.switchBlocks)
        switchOnCodeUnit(program, rewriter.function().at(block).terminator, root);
}

static void becomeRangeOfBase(GroupRewriter& rewriter, const Match& match)
{
    ValueID root = match.group.root;
    const StringRangeSource& source = match.chain.last();
    StringRangeBounds bounds = insertBounds(rewriter, match);
    if (source.bounds) {
        Value& substring = rewriter.function().at(root);
        substring.setStringOperand(match.base);
        substring.setStartOperand(bounds.start);
        substring.setEndOperand(bounds.end);
    } else if (source.mayBeUndefined)
        replaceRootWith(rewriter, makeStringRangeRead(Opcode::StringRangeSubstringOrUndefined, match.base, bounds, { }));
    else
        replaceRootWith(rewriter, IRBuilder::make(Opcode::Substring, { match.base, bounds.start, bounds.end }));
    rewriter.leaveUnclaimed(root);
}

static void rewriteMatch(const Program& program, GroupRewriter& rewriter, const Match& match, ValueSubstitution& substitution)
{
    switch (match.action) {
    case Action::ReadCharacters:
        readCharacters(rewriter, match);
        return;
    case Action::ComputeLength: {
        constexpr int64_t lengthMask = Range::length().maximum;
        static_assert(!(lengthMask & (lengthMask + 1)), "Masking keeps every length and gives the difference the type of the read it replaces, which the result rule of Sub cannot prove.");

        StringRangeBounds bounds = insertBounds(rewriter, match);
        ValueID difference = rewriter.insert(IRBuilder::make(Opcode::Sub, { bounds.end, bounds.start }));
        ValueID mask = rewriter.insert(makeInteger(lengthMask));
        replaceRootWith(rewriter, IRBuilder::make(Opcode::BitAnd, { difference, mask }));
        return;
    }
    case Action::CompareOwnCodeUnit:
        compareOwnCodeUnit(rewriter, match);
        return;
    case Action::CompareSharedCodeUnit:
        compareSharedCodeUnit(rewriter, match);
        return;
    case Action::TestSharedCodeUnit:
        testSharedCodeUnit(rewriter, match, substitution);
        return;
    case Action::TestEmpty: {
        StringRangeBounds bounds = insertBounds(rewriter, match);
        replaceRootWith(rewriter, IRBuilder::make(Opcode::StrictEq, { bounds.start, bounds.end }));
        return;
    }
    case Action::FoldToFalse:
        rewriter.replaceRoot(Opcode::Boolean).setBoolean(false);
        return;
    case Action::BecomeCodeUnit:
        becomeCodeUnit(program, rewriter, match);
        return;
    case Action::BecomeRangeOfBase:
        becomeRangeOfBase(rewriter, match);
        return;
    case Action::Erase:
        rewriter.eraseRoot();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void selectSubstringView(SelectionContext& context)
{
    Vector<RangeValue> ranges = collectRanges(context);
    if (ranges.isEmpty())
        return;

    // The effects of a comparison depend on its operand types, so a range becomes a code unit before the comparisons that share the unit are rewritten.
    // A session rewrites from the last position to the first, so the comparisons of a narrowed string are rewritten before its test drops the string.
    Vector<Match> sharedUnitComparisons;
    Vector<Match> otherMatches;
    for (Match& match : RangeMatcher(context, WTF::move(ranges)).run()) {
        bool sharesUnit = match.action == Action::CompareSharedCodeUnit || match.action == Action::TestSharedCodeUnit;
        (sharesUnit ? sharedUnitComparisons : otherMatches).append(WTF::move(match));
    }

    ValueSubstitution substitution(context.function());
    auto rewrite = [&](GroupRewriter& rewriter, const Match& match) {
        rewriteMatch(context.program(), rewriter, match, substitution);
    };
    context.rewriteGroups(SelectionRuleID::SubstringView, otherMatches, rewrite);
    context.rewriteGroups(SelectionRuleID::SubstringView, sharedUnitComparisons, rewrite);
    substitution.apply(context.pass().function, &context.analyses());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
