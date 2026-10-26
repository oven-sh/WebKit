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
#include "AOTSort.h"
#include "AOTStringRuleSupport.h"
#include "AOTStringTable.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class UseKind : uint8_t { Other, Read, Length, CompareCodeUnit, TestEmpty, FoldToFalse, Nested, SwitchOnCodeUnit, TestPresence };

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

// The root is the user of a use that gets another form. Where the use is of kind Other, the root is the range itself or one of its upcasts.
struct StringRangeMatch {
    SelectionGroup group;
    ProgramPoint point;
    Vector<StringRangeSource, 2> chain;
    ValueID range;
    RangeForm form;
    RangeUse use;
    Vector<BlockID, 1> switchBlocks { };

    ValueID base() const { return chain.first().string; }
    bool sharesCodeUnit() const { return use.kind == UseKind::TestPresence || (use.kind == UseKind::CompareCodeUnit && form == RangeForm::CodeUnit); }
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

static bool getsAnotherForm(UseKind kind)
{
    switch (kind) {
    case UseKind::Read:
    case UseKind::Length:
    case UseKind::CompareCodeUnit:
    case UseKind::TestEmpty:
    case UseKind::FoldToFalse:
    case UseKind::TestPresence:
        return true;
    case UseKind::Other:
    case UseKind::Nested:
    case UseKind::SwitchOnCodeUnit:
        return false;
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

namespace {

struct RangeMatcher {
    SelectionContext& context;
    const Program& program;
    const Function& function;
    const UseLists& useLists;
    Vector<RangeValue> ranges;
    UncheckedKeyHashMap<ValueID, uint32_t> indexes { };
    Vector<StringRangeMatch> matches { };

    void findChain(uint32_t index)
    {
        RangeValue& range = ranges[index];
        auto enclosing = indexes.find(definitionThroughUpcasts(function, range.source.string));
        // Negative bounds mean undefined, and adding the start of an enclosing range to them would lose that.
        bool encodesAbsenceInBounds = range.source.bounds && range.source.mayBeUndefined;
        if (enclosing != indexes.end() && !encodesAbsenceInBounds) {
            RELEASE_ASSERT(enclosing->value < index);
            const RangeValue& outer = ranges[enclosing->value];
            if (!outer.source.mayBeUndefined && !context.pass().exceeds(LimitID::MaxSelectionGroupMembers, outer.chain.size() + 1, range.value)) {
                range.enclosing = enclosing->value;
                range.chain = outer.chain;
            }
        }
        range.chain.append(range.source);
    }

    void classifyComparison(const StringRangeSource& source, const Use& use, const LiteralArgument& literal, RangeUse& result) const
    {
        Opcode opcode = function.at(use.user).opcode;
        bool comparesOneUnit = literal.text.length() == 1;
        if (relationalOperators.contains(opcode)) {
            if (!comparesOneUnit)
                return;
            // -1 stands for an empty range: it is below every code unit, as the empty string is below the literal.
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

    UseKind classifyTerminator(const StringRangeSource& source, BlockID block) const
    {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::SwitchString || !source.isSingleUnit() || context.passesProofs(block))
            return UseKind::Other;

        bool comparesOneUnit = std::ranges::all_of(terminator.strings(), [&](StringID string) {
            return singleCodeUnitOf(program, string).has_value();
        });
        return comparesOneUnit ? UseKind::SwitchOnCodeUnit : UseKind::Other;
    }

    // Tells the character from undefined. The narrowed values that its branch passes to the targets are still the range.
    bool classifyTypeTest(const Use& use, RangeUse& result) const
    {
        const Value& test = function.at(use.user);
        if (test.opcode != Opcode::TypeTest)
            return false;
        const Type& tested = program.types.at(test.testedType());
        bool admitsEveryString = Type::fromPrimitives(Primitive::AnyString).isSubsetOf(tested, program);
        bool admitsNoString = !(tested.primitives & Primitive::AnyString);
        bool admitsUndefined = !!(tested.primitives & Primitive::Undefined);
        if (admitsUndefined ? !admitsNoString : !admitsEveryString)
            return false;

        std::optional<Use> branch = useLists.singleUse(use.user);
        if (!branch || branch->kind != Use::Kind::TerminatorOperand || function.at(branch->block).terminator.kind != TerminatorKind::Branch)
            return false;
        const Terminator& terminator = function.at(branch->block).terminator;
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
            const Target& target = terminator.targets[targetIndex];
            if (!target.proofs.isEmpty() && context.analyses().cfg().singleIncomingEdge(target.block) != Edge { branch->block, targetIndex })
                return false;
        }

        result.kind = UseKind::TestPresence;
        result.block = branch->block;
        result.holdsForString = !admitsUndefined;
        return true;
    }

    void classifyOperand(uint32_t index, const StringRangeSource& source, const Use& use, RangeUse& result)
    {
        if (auto inner = indexes.find(use.user); inner != indexes.end()) {
            const RangeValue& innerRange = ranges[inner->value];
            if (innerRange.enclosing == index && innerRange.form != RangeForm::Unchanged)
                result.kind = UseKind::Nested;
            return;
        }
        if (context.isClaimed(use.user))
            return;

        if (source.mayBeUndefined && classifyTypeTest(use, result))
            return;
        if (source.isSingleUnit()) {
            if (std::optional<LiteralArgument> literal = comparedLiteralOf(context, use)) {
                classifyComparison(source, use, *literal, result);
                return;
            }
        }
        if (readsLength(program, function.at(use.user), use.operandIndex)) {
            if (!source.mayBeUndefined)
                result.kind = UseKind::Length;
            return;
        }

        std::optional<CharacterRead> read = characterReadOf(context, use);
        if (!read || (source.mayBeUndefined && !treatsUndefinedAsEmptyString(program, *read)))
            return;
        result.kind = UseKind::Read;
        result.read = *read;
    }

    RangeUse classify(uint32_t index, const StringRangeSource& source, const Use& use)
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

    RangeUses collectUses(uint32_t index)
    {
        RangeUses result;
        auto collect = [&](const StringRangeSource& source, Vector<ValueID, 4>&& worklist, Vector<ValueID, 4>& narrowedStrings) {
            while (!worklist.isEmpty()) {
                for (const Use& use : useLists.usesOf(worklist.takeLast())) {
                    if (use.kind == Use::Kind::Operand && isCastThatProvesNothing(function.at(use.user).opcode)) {
                        result.upcasts.append(use.user);
                        worklist.append(use.user);
                        continue;
                    }
                    result.uses.append(classify(index, source, use));
                    const RangeUse& test = result.uses.last();
                    if (test.kind != UseKind::TestPresence)
                        continue;
                    Edge edge { test.block, test.holdsForString ? 0u : 1u };
                    auto& parameters = function.at(targetOf(function, edge).block).parameters;
                    for (size_t parameter = numberOfExplicitArguments(function, edge); parameter < parameters.size(); ++parameter)
                        narrowedStrings.append(parameters[parameter]);
                }
            }
        };

        StringRangeSource source = ranges[index].source;
        Vector<ValueID, 4> narrowedStrings;
        collect(source, { ranges[index].value }, narrowedStrings);
        source.mayBeUndefined = false;
        Vector<ValueID, 4> none;
        collect(source, WTF::move(narrowedStrings), none);
        RELEASE_ASSERT(none.isEmpty());
        return result;
    }

    bool tryClaim(ValueID id)
    {
        return context.tryClaim(SelectionRuleID::SubstringView, groupOfOne(id)) == ClaimResult::Claimed;
    }

    bool tryClaimWith(ValueID range, std::span<const ValueID> others)
    {
        Vector<SelectionGroup, 4> groups { groupOfOne(range) };
        for (ValueID other : others)
            groups.append(groupOfOne(other));
        return context.tryClaimAll(SelectionRuleID::SubstringView, groups.span()) == ClaimResult::Claimed;
    }

    RangeForm chooseForm(const RangeValue& range, RangeUses& uses)
    {
        Vector<ValueID, 4> tests;
        for (const RangeUse& use : uses.uses) {
            if (use.kind == UseKind::TestPresence)
                tests.append(use.user);
        }
        if (canShareCodeUnit(range.source, uses) && tryClaimWith(range.value, tests.span()))
            return RangeForm::CodeUnit;

        bool hasOtherUse = false;
        for (RangeUse& use : uses.uses) {
            if (use.kind == UseKind::SwitchOnCodeUnit || use.kind == UseKind::TestPresence)
                use.kind = UseKind::Other;
            hasOtherUse |= use.kind == UseKind::Other;
        }
        if (!hasOtherUse && tryClaimWith(range.value, uses.upcasts.span()))
            return RangeForm::Erased;
        if (range.chain.size() > 1 && tryClaim(range.value))
            return RangeForm::RangeOfBase;
        return RangeForm::Unchanged;
    }

    void matchRange(uint32_t index)
    {
        RangeUses uses = collectUses(index);
        for (RangeUse& use : uses.uses) {
            if (replacesUser(use.kind) && !tryClaim(use.user))
                use.kind = UseKind::Other;
        }

        RangeValue& range = ranges[index];
        range.form = chooseForm(range, uses);

        auto addMatch = [&](ValueID root, const RangeUse& use) {
            matches.append(StringRangeMatch { groupOfOne(root), useLists.pointOf(root), range.chain, range.value, range.form, use });
        };
        for (const RangeUse& use : uses.uses) {
            if (getsAnotherForm(use.kind))
                addMatch(use.user, use);
        }

        switch (range.form) {
        case RangeForm::Unchanged:
            return;
        case RangeForm::CodeUnit:
            addMatch(range.value, { });
            for (const RangeUse& use : uses.uses) {
                if (use.kind == UseKind::SwitchOnCodeUnit)
                    matches.last().switchBlocks.append(use.block);
            }
            return;
        case RangeForm::Erased:
            addMatch(range.value, { });
            for (ValueID upcast : uses.upcasts)
                addMatch(upcast, { });
            return;
        case RangeForm::RangeOfBase:
            addMatch(range.value, { });
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void run()
    {
        const CFG& cfg = context.analyses().cfg();
        AOT::sort(ranges, { }, [&](const RangeValue& range) {
            ProgramPoint point = useLists.pointOf(range.value);
            return std::pair { cfg.reversePostOrderIndex(point.block), point.index };
        });
        for (uint32_t index = 0; index < ranges.size(); ++index)
            indexes.add(ranges[index].value, index);
        for (uint32_t index = 0; index < ranges.size(); ++index)
            findChain(index);
        // An enclosed range comes after the enclosing one, which is still read unless the enclosed one got another form.
        for (uint32_t index = ranges.size(); index--;)
            matchRange(index);
    }
};

} // anonymous namespace

static std::optional<StringRangeBounds> insertBounds(GroupRewriter& rewriter, const StringRangeMatch& match, std::span<const StringRangeSource> chain)
{
    std::optional<StringRangeBounds> bounds;
    for (const StringRangeSource& source : chain)
        bounds = insertStringRange(rewriter.builder(), match.point, source, bounds);
    return bounds;
}

static StringRangeBounds insertBounds(GroupRewriter& rewriter, const StringRangeMatch& match)
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

static Value makeCodeUnitRead(GroupRewriter& rewriter, const StringRangeMatch& match)
{
    const StringRangeSource& unit = match.chain.last();
    RELEASE_ASSERT(unit.isSingleUnit());
    if (unit.method == StringRangeMethod::SingleIndex && unit.first) {
        std::optional<StringRangeBounds> enclosing = insertBounds(rewriter, match, match.chain.span().first(match.chain.size() - 1));
        return makeStringRangeRead(Opcode::StringRangeCodeUnitOrMinusOne, match.base(), enclosing, unit.first);
    }

    StringRangeBounds bounds = insertBounds(rewriter, match);
    ValueID zero = rewriter.insert(makeInteger(0));
    return makeStringRangeRead(Opcode::StringRangeCodeUnitOrMinusOne, match.base(), bounds, zero);
}

static void compareCodeUnit(GroupRewriter& rewriter, const StringRangeMatch& match)
{
    ValueID root = match.group.root;
    ValueID unit = match.range;
    if (!match.sharesCodeUnit()) {
        unit = rewriter.insert(makeCodeUnitRead(rewriter, match));
        rewriter.leaveUnclaimed(unit);
    }
    ValueID codeUnit = rewriter.insert(makeInteger(match.use.codeUnit));
    if (match.use.comparesUnsigned)
        unit = rewriter.insert(IRBuilder::make(Opcode::ToUint32, { unit }));

    Value& comparison = rewriter.replaceRoot(rewriter.original(root).opcode);
    bool unitIsOnLeft = OperandPosition::left(comparison.opcode) == match.use.operandIndex;
    comparison.setLeftOperand(unitIsOnLeft ? unit : codeUnit);
    comparison.setRightOperand(unitIsOnLeft ? codeUnit : unit);
    rewriter.leaveUnclaimed(root);
}

// An integer comparison narrows no string, so the parameters that the test narrowed are removed. The strings have no uses left, and the value on the other edge is undefined.
static void testSharedCodeUnit(GroupRewriter& rewriter, const StringRangeMatch& match, ValueSubstitution& substitution)
{
    ValueID zero = rewriter.insert(makeInteger(0));
    ValueID undefinedValue = rewriter.insert(IRBuilder::make(Opcode::Undefined));
    Value& comparison = rewriter.replaceRoot(match.use.holdsForString ? Opcode::GreaterEq : Opcode::Less);
    comparison.setLeftOperand(match.range);
    comparison.setRightOperand(zero);
    rewriter.leaveUnclaimed(match.group.root);

    Function& function = rewriter.function();
    BlockID branchBlock = match.use.block;
    uint32_t stringTargetIndex = match.use.holdsForString ? 0 : 1;
    for (uint32_t targetIndex = 0; targetIndex < function.at(branchBlock).terminator.targets.size(); ++targetIndex) {
        Edge edge { branchBlock, targetIndex };
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

static void becomeRangeOfBase(GroupRewriter& rewriter, const StringRangeMatch& match)
{
    const StringRangeSource& source = match.chain.last();
    StringRangeBounds bounds = insertBounds(rewriter, match);
    if (source.bounds) {
        Value& substring = rewriter.function().edit(match.group.root);
        substring.setStringOperand(match.base());
        substring.setStartOperand(bounds.start);
        substring.setEndOperand(bounds.end);
    } else if (source.mayBeUndefined)
        replaceRootWith(rewriter, makeStringRangeRead(Opcode::StringRangeSubstringOrUndefined, match.base(), bounds, { }));
    else
        replaceRootWith(rewriter, IRBuilder::make(Opcode::Substring, { match.base(), bounds.start, bounds.end }));
}

static void rewriteRange(const Program& program, GroupRewriter& rewriter, const StringRangeMatch& match)
{
    ValueID root = match.group.root;
    switch (match.form) {
    case RangeForm::CodeUnit:
        replaceRootWith(rewriter, makeCodeUnitRead(rewriter, match));
        rewriter.leaveUnclaimed(root);
        for (BlockID block : match.switchBlocks)
            switchOnCodeUnit(program, rewriter.function().edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, root);
        return;
    case RangeForm::Erased:
        rewriter.eraseRoot();
        return;
    case RangeForm::RangeOfBase:
        becomeRangeOfBase(rewriter, match);
        rewriter.leaveUnclaimed(root);
        return;
    case RangeForm::Unchanged:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void rewriteMatch(const Program& program, GroupRewriter& rewriter, const StringRangeMatch& match, ValueSubstitution& substitution)
{
    switch (match.use.kind) {
    case UseKind::Read: {
        CharacterRead read = match.use.read;
        if (!read.position && OperandPosition::position(read.rangeRead))
            read.position = rewriter.insert(makeInteger(0));
        replaceRootWith(rewriter, makeCharacterRead(read, match.base(), insertBounds(rewriter, match)));
        rewriter.leaveUnclaimed(match.group.root);
        return;
    }
    case UseKind::Length: {
        constexpr int64_t lengthMask = Range::length().maximum;
        static_assert(!(lengthMask & (lengthMask + 1)), "Masking keeps every length and gives the difference the type of the read it replaces, which the result rule of Sub cannot prove.");

        StringRangeBounds bounds = insertBounds(rewriter, match);
        ValueID difference = rewriter.insert(IRBuilder::make(Opcode::Sub, { bounds.end, bounds.start }));
        ValueID mask = rewriter.insert(makeInteger(lengthMask));
        replaceRootWith(rewriter, IRBuilder::make(Opcode::BitAnd, { difference, mask }));
        return;
    }
    case UseKind::CompareCodeUnit:
        compareCodeUnit(rewriter, match);
        return;
    case UseKind::TestPresence:
        testSharedCodeUnit(rewriter, match, substitution);
        return;
    case UseKind::TestEmpty: {
        StringRangeBounds bounds = insertBounds(rewriter, match);
        replaceRootWith(rewriter, IRBuilder::make(Opcode::StrictEq, { bounds.start, bounds.end }));
        return;
    }
    case UseKind::FoldToFalse:
        rewriter.replaceRoot(Opcode::Boolean).setBoolean(false);
        return;
    case UseKind::Other:
        rewriteRange(program, rewriter, match);
        return;
    case UseKind::Nested:
    case UseKind::SwitchOnCodeUnit:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void selectSubstringView(SelectionContext& context)
{
    Vector<RangeValue> ranges = collectRanges(context);
    if (ranges.isEmpty())
        return;
    RangeMatcher matcher { context, context.program(), context.function(), context.analyses().useLists(), WTF::move(ranges) };
    matcher.run();

    // The effects of a comparison depend on its operand types, so a range becomes a code unit before the comparisons that share the unit are rewritten.
    Vector<StringRangeMatch> sharedUnitComparisons;
    Vector<StringRangeMatch> otherMatches;
    for (StringRangeMatch& match : matcher.matches)
        (match.sharesCodeUnit() ? sharedUnitComparisons : otherMatches).append(WTF::move(match));

    ValueSubstitution substitution(context.function());
    auto rewrite = [&](GroupRewriter& rewriter, const StringRangeMatch& match) {
        rewriteMatch(context.program(), rewriter, match, substitution);
    };
    context.rewriteGroups(SelectionRuleID::SubstringView, otherMatches, rewrite);
    context.rewriteGroups(SelectionRuleID::SubstringView, sharedUnitComparisons, rewrite);
    substitution.apply(context.pass().function, &context.analyses());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
