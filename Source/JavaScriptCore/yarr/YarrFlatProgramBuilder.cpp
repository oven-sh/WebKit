/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "YarrFlatProgram.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <tuple>
#include <wtf/ASCIICType.h>
#include <wtf/Hasher.h>
#include <wtf/StdUnorderedMap.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace Yarr {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlatProgram);

class FlatProgramBuilder {
public:
    FlatProgramBuilder(const YarrPattern& pattern, FlatProgramForm form)
        : m_pattern(pattern)
        , m_countsTurns(form == FlatProgramForm::CountsTurns)
    {
    }

    std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> build()
    {
        bool runsOnlyOn8BitSubjects = false;
        if (m_pattern.eitherUnicode() && !consumesSingleCodeUnits(*m_pattern.m_body, Subjects::All)) {
            if (!consumesSingleCodeUnits(*m_pattern.m_body, Subjects::With8BitCharacters))
                return makeUnexpected(FlatProgramDecline::UnicodeMode);
            runsOnlyOn8BitSubjects = true;
        }
        if (m_pattern.m_containsModifiers)
            return makeUnexpected(FlatProgramDecline::Modifiers);
        if (m_pattern.m_containsLookbehinds)
            return makeUnexpected(FlatProgramDecline::Lookbehind);
        if (m_pattern.hasDuplicateNamedCaptureGroups())
            return makeUnexpected(FlatProgramDecline::DuplicateNamedGroups);
        if (m_pattern.containsUnsignedLengthPattern())
            return makeUnexpected(FlatProgramDecline::UnsignedLengthPattern);

        auto program = makeUnique<FlatProgram>();
        m_program = program.get();
        program->numSubpatterns = m_pattern.m_numSubpatterns;
        if (m_pattern.hasEndAnchoredFixedSize())
            program->endAnchoredFixedSize = m_pattern.m_endAnchoredFixedSize;
        program->triesOnlyOnce = m_pattern.sticky();
        program->runsOnlyOn8BitSubjects = runsOnlyOn8BitSubjects;

        auto& alternatives = m_pattern.m_body->m_alternatives;
        unsigned count = alternatives.size();
        if (alternatives[0]->onceThrough()) {
            count = 0;
            while (count < alternatives.size() && alternatives[count]->onceThrough())
                ++count;
            program->triesOnlyOnce |= count == alternatives.size();
            // From the second place on the bytecode runs the copies behind these, which lack what begins with ^.
            if (m_countsTurns && count != alternatives.size())
                return makeUnexpected(FlatProgramDecline::CopiedGroup);
        }

        if (!emitAlternatives(alternatives.span().first(count), { }))
            return makeUnexpected(*m_decline);
        emit(Kind::Match);
        if (m_countsTurns && !std::ranges::contains(program->ops, Kind::CountTurn, &FlatOp::kind))
            return makeUnexpected(FlatProgramDecline::NoTurnsToCount);
        if (hasExceededIndexLimit())
            return makeUnexpected(FlatProgramDecline::TooManySetsOrRegisters);
        program->numRegisters = m_nextRegister;

        removeUnneededLogging(*program);
        if (!m_countsTurns)
            computeStartScan(alternatives.span().first(count));

        if (!program->text.isEmpty()) {
            for (unsigned i = 0; i < sizeof(uint64_t); ++i)
                program->text.append(0);
        }
        program->ops.shrinkToFit();
        program->characterSets.shrinkToFit();
        program->firstSets.shrinkToFit();
        program->dispatchClasses.shrinkToFit();
        program->dispatchEntries.shrinkToFit();
        program->wideRanges.shrinkToFit();
        program->text.shrinkToFit();
        program->wideText.shrinkToFit();
        return program;
    }

private:
    static bool isSingleCodeUnit(char32_t character) { return U_IS_BMP(character) && !U_IS_SURROGATE(character); }

    static bool holdsSingleCodeUnits(const CharacterClass& characterClass)
    {
        if (characterClass.hasStrings() || characterClass.hasNonBMPCharacters())
            return false;
        bool hasSurrogate = std::ranges::any_of(characterClass.m_matches32, [](char32_t character) {
            return U_IS_SURROGATE(character);
        });
        hasSurrogate |= std::ranges::any_of(characterClass.m_ranges32, [](const CharacterRange& range) {
            return range.begin <= 0xdfff && range.end >= 0xd800;
        });
        return !hasSurrogate;
    }

    enum class Subjects : bool { All, With8BitCharacters };

    // In unicode mode a step consumes a code point. Where no step can consume a surrogate, a code point that is consumed is one code unit, and the program for code units answers the same.
    // A subject of 8-bit characters has no surrogate for a class to consume, whatever the class holds.
    // The parser has made classes of the letters that have other cases. RegExp::matchInline keeps a match from beginning inside a surrogate pair.
    static bool consumesSingleCodeUnits(const PatternDisjunction& disjunction, Subjects subjects, unsigned depth = 0)
    {
        if (depth > maxNestingDepth)
            return false;
        for (auto& alternative : disjunction.m_alternatives) {
            for (const PatternTerm& term : alternative->m_terms) {
                switch (term.type) {
                case PatternTerm::Type::AssertionBOL:
                case PatternTerm::Type::AssertionEOL:
                    break;
                case PatternTerm::Type::AssertionWordBoundary:
                case PatternTerm::Type::NumberedBackReference:
                case PatternTerm::Type::NamedBackReference:
                case PatternTerm::Type::NumberedForwardReference:
                case PatternTerm::Type::NamedForwardReference:
                    if (term.ignoreCase())
                        return false;
                    break;
                case PatternTerm::Type::PatternCharacter:
                    if (!isSingleCodeUnit(term.patternCharacter))
                        return false;
                    break;
                case PatternTerm::Type::CharacterClass:
                    if (subjects == Subjects::With8BitCharacters) {
                        if (term.characterClass->hasStrings())
                            return false;
                        break;
                    }
                    if (term.invert() || !holdsSingleCodeUnits(*term.characterClass))
                        return false;
                    break;
                case PatternTerm::Type::ParenthesesSubpattern:
                case PatternTerm::Type::ParentheticalAssertion:
                    if (!consumesSingleCodeUnits(*term.parentheses.disjunction, subjects, depth + 1))
                        return false;
                    break;
                case PatternTerm::Type::DotStarEnclosure:
                    return false;
                }
            }
        }
        return true;
    }

    using Kind = FlatOp::Kind;
    using Alternatives = std::span<const std::unique_ptr<PatternAlternative>>;
    using SetKey = std::tuple<const CharacterClass*, bool, char16_t, char16_t>;

    static constexpr unsigned maxNestingDepth = 64;
    static constexpr unsigned maxTermsVisitedForEmptiness = 64;
    static constexpr unsigned maxSharedPrefixDepth = 32;
    static constexpr size_t maxIndexCount = std::numeric_limits<uint16_t>::max();

    struct EmitContext {
        uint64_t neededAfter { 0 };
        unsigned nestingDepth { 0 };
        unsigned sharedPrefixDepth { 0 };
        // FlatProgramForm::CountsTurns: PatternTerm::inputPosition counts from here in the alternatives that are emitted, and neededAfter is what ByteCompiler::emitDisjunction() has tested for beyond them.
        unsigned initialInputPosition { 0 };
        // FlatProgramForm::CountsTurns: an alternative that needs no more than this, and neededAfter, has been tested for.
        unsigned testedFor { 0 };
    };

    // What is left of an alternative from one of its terms on.
    struct Branch {
        const PatternAlternative* alternative;
        unsigned begin;
    };

    // Branches that begin with the same step, in their order. stepTerms is 0 where the first member begins with no step that could be shared.
    struct BranchGroup {
        Vector<Branch, 2> members;
        std::optional<FirstSetBits> first;
        unsigned stepTerms { 0 };
    };

    struct InternedKeyHash {
        size_t operator()(const SetKey& key) const { return computeHash(key); }
        size_t operator()(const FirstSetBits& set) const { return computeHash(set.bits, set.atEnd); }
    };

    bool decline(FlatProgramDecline reason)
    {
        if (!m_decline)
            m_decline = reason;
        return false;
    }

    bool hasExceededIndexLimit() const
    {
        return m_nextRegister > maxIndexCount || m_program->characterSets.size() > maxIndexCount || m_program->firstSets.size() > maxIndexCount;
    }

    unsigned emit(Kind kind, uint32_t argument = 0, uint32_t target = 0)
    {
        FlatOp op { };
        op.kind = kind;
        op.argument = argument;
        op.target = target;
        m_program->ops.append(op);
        return m_program->ops.size() - 1;
    }

    unsigned emitWithSet(Kind kind, unsigned set, uint32_t argument = 0, uint32_t target = 0)
    {
        unsigned op = emit(kind, argument, target);
        m_program->ops[op].set = static_cast<uint16_t>(set);
        return op;
    }

    unsigned emitWithRegister(Kind kind, unsigned registerIndex, uint32_t argument = 0, uint32_t target = 0)
    {
        unsigned op = emit(kind, argument, target);
        m_program->ops[op].registerIndex = static_cast<uint16_t>(registerIndex);
        return op;
    }

    unsigned here() const { return m_program->ops.size(); }
    void setTarget(unsigned op) { m_program->ops[op].target = here(); }

    static uint64_t minimumSize(const PatternTerm& term)
    {
        switch (term.type) {
        case PatternTerm::Type::PatternCharacter:
        case PatternTerm::Type::CharacterClass:
            return static_cast<unsigned>(term.quantityMinCount);
        case PatternTerm::Type::ParenthesesSubpattern:
            return static_cast<uint64_t>(static_cast<unsigned>(term.quantityMinCount)) * term.parentheses.disjunction->m_minimumSize;
        case PatternTerm::Type::AssertionBOL:
        case PatternTerm::Type::AssertionEOL:
        case PatternTerm::Type::AssertionWordBoundary:
        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
        case PatternTerm::Type::ParentheticalAssertion:
        case PatternTerm::Type::DotStarEnclosure:
            return 0;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void setNeeded(unsigned op, uint64_t needed)
    {
        m_program->ops[op].needed = static_cast<uint32_t>(std::min<uint64_t>(needed, std::numeric_limits<uint32_t>::max()));
    }

    unsigned emitWithNeeded(Kind kind, uint64_t needed)
    {
        unsigned op = emit(kind);
        setNeeded(op, needed);
        return op;
    }

    unsigned emitNeedsInput(uint64_t needed) { return emitWithNeeded(Kind::NeedsInput, needed); }
    unsigned emitNeedsInputElseJump(uint64_t needed) { return emitWithNeeded(Kind::NeedsInputElseJump, needed); }

    static uint64_t neededToEnter(const PatternDisjunction& disjunction, uint64_t neededAfter)
    {
        return disjunction.m_minimumSize && neededAfter ? disjunction.m_minimumSize + neededAfter : 0;
    }

    static bool hasAllLatin1Characters(const FirstSetBits& set)
    {
        return !~(set.bits[0] & set.bits[1] & set.bits[2] & set.bits[3]);
    }

    static void addAll(FirstSetBits& set, const FirstSetBits& other)
    {
        for (unsigned i = 0; i < set.bits.size(); ++i)
            set.bits[i] |= other.bits[i];
        set.atEnd |= other.atEnd;
    }

    std::optional<FirstSetBits> latin1Bits(CharacterSetID id, bool& hasWideMembers) const
    {
        if (id == CharacterSetID::Unknown)
            return std::nullopt;
        const CharacterSet& set = m_pattern.m_firstFollowSets.set(id);
        FirstSetBits result;
        result.atEnd = set.includesEndOfInput;
        for (auto [begin, end] : set.ranges) {
            if (begin <= 0xff)
                addLatin1Range(result.bits, begin, std::min<char32_t>(end, 0xff));
            hasWideMembers |= end > 0xff;
        }
        return result;
    }

    // The analysis gives up on a set that takes a walk over many terms, as the first set of a group of a hundred alternatives does. Each of those alternatives has its own.
    std::optional<FirstSetBits> firstBits(const PatternAlternative& alternative, bool& hasWideMembers, unsigned depth = 0) const
    {
        if (auto known = latin1Bits(alternative.m_firstSet, hasWideMembers))
            return known;
        if (depth >= maxNestingDepth || alternative.m_terms.isEmpty() || alternative.matchDirection() != Forward)
            return std::nullopt;
        const PatternTerm& term = alternative.m_terms[0];
        if (term.type != PatternTerm::Type::ParenthesesSubpattern || !term.quantityMinCount || term.matchDirection() != Forward)
            return std::nullopt;
        FirstSetBits result;
        for (auto& nested : term.parentheses.disjunction->m_alternatives) {
            auto first = firstBits(*nested, hasWideMembers, depth + 1);
            if (!first)
                return std::nullopt;
            addAll(result, *first);
        }
        return result;
    }

    static constexpr unsigned endOfInput = 256;

    template<typename Functor>
    static void forEachMember(const FirstSetBits& set, const Functor& functor)
    {
        for (unsigned word = 0; word < set.bits.size(); ++word) {
            for (uint64_t bits = set.bits[word]; bits; bits &= bits - 1)
                functor(word * 64 + std::countr_zero(bits));
        }
        if (set.atEnd)
            functor(endOfInput);
    }

    void fillDispatchTable(std::span<const unsigned> choices, std::span<const std::optional<FirstSetBits>> first)
    {
        using PerClass = std::array<uint16_t, FlatProgram::dispatchClassesPerTable>;
        PerClass classes { };
        PerClass sizes { };
        PerClass memberCounts { };
        PerClass classOfMembers { };
        Vector<uint16_t, 16> touched;
        unsigned classCount = 1;
        sizes[0] = FlatProgram::dispatchClassesPerTable;
        for (auto& set : first) {
            if (!set)
                continue;
            forEachMember(*set, [&](unsigned member) {
                if (!memberCounts[classes[member]]++)
                    touched.append(classes[member]);
            });
            forEachMember(*set, [&](unsigned member) {
                uint16_t whole = classes[member];
                if (memberCounts[whole] == sizes[whole])
                    return;
                if (!classOfMembers[whole])
                    classOfMembers[whole] = classCount++;
                classes[member] = classOfMembers[whole];
            });
            for (uint16_t whole : touched) {
                if (uint16_t part = std::exchange(classOfMembers[whole], 0)) {
                    sizes[part] = memberCounts[whole];
                    sizes[whole] -= memberCounts[whole];
                }
                memberCounts[whole] = 0;
            }
            touched.shrink(0);
        }
        auto& ops = m_program->ops;
        auto& entries = m_program->dispatchEntries;
        unsigned firstClass = m_program->dispatchClasses.size();
        m_program->dispatchClasses.append(std::span<const uint16_t> { classes });
        unsigned firstRow = entries.size();
        entries.grow(firstRow + choices.size() * classCount);

        auto forEachClassThatBegins = [&](const std::optional<FirstSetBits>& set, const auto& functor) {
            if (set) {
                forEachMember(*set, [&](unsigned member) {
                    functor(classes[member]);
                });
                return;
            }
            for (unsigned i = 0; i < classCount; ++i)
                functor(i);
        };

        Vector<uint32_t, 64> next;
        next.fill(FlatProgram::noAlternativeBeginsHere, classCount);
        forEachClassThatBegins(first[choices.size()], [&](unsigned i) {
            next[i] = ops[choices.back()].target + FlatProgram::firstDispatchTarget;
        });
        for (unsigned alternative = choices.size(); alternative--;) {
            auto row = entries.mutableSpan().subspan(firstRow + alternative * classCount, classCount);
            for (unsigned i = 0; i < classCount; ++i)
                row[i] = next[i];
            uint32_t here = choices[alternative] + FlatProgram::firstDispatchTarget;
            forEachClassThatBegins(first[alternative], [&](unsigned i) {
                if (next[i] == here)
                    return;
                row[i] = next[i] == FlatProgram::noAlternativeBeginsHere ? FlatProgram::onlyThisAlternativeBeginsHere : FlatProgram::thisAndLaterAlternativesBeginHere;
                next[i] = here;
            });
            ops[choices[alternative]].argument = firstRow + alternative * classCount;
            ops[choices[alternative]].needed = firstClass;
        }
    }

    unsigned internFirstSet(const FirstSetBits& set)
    {
        auto [entry, isNew] = m_firstSetIndexes.try_emplace(set, m_program->firstSets.size());
        if (isNew)
            m_program->firstSets.append(set);
        return entry->second;
    }

    void computeStartScan(Alternatives alternatives)
    {
        if (m_program->triesOnlyOnce)
            return;
        FirstSetBits start;
        bool hasWideMembers = false;
        for (auto& alternative : alternatives) {
            auto first = firstBits(*alternative, hasWideMembers);
            if (!first)
                return;
            addAll(start, *first);
        }
        if (start.atEnd)
            return;

        m_program->startScan = FlatStartScan::Set;
        m_program->startSet = start;
        m_program->startMayBeWide = hasWideMembers;
        unsigned memberCount = 0;
        unsigned lastMember = 0;
        for (unsigned i = 0; i < start.bits.size(); ++i) {
            memberCount += std::popcount(start.bits[i]);
            if (start.bits[i])
                lastMember = i * 64 + std::countr_zero(start.bits[i]);
        }
        if (memberCount == 1 && !hasWideMembers) {
            m_program->startScan = FlatStartScan::OneCharacter;
            m_program->onlyStartCharacter = static_cast<Latin1Character>(lastMember);
        }
    }

    unsigned internSet(const CharacterClass* characterClass, bool invert, char16_t first, char16_t second)
    {
        ASSERT(characterClass || !invert);
        auto [entry, isNew] = m_setIndexes.try_emplace(SetKey { characterClass, invert, first, second }, m_program->characterSets.size());
        if (!isNew)
            return entry->second;

        CharacterSetBits set = characterClass ? buildCharacterSetBits(*characterClass, invert, m_program->wideRanges) : buildCharacterSetBits(first, second, m_program->wideRanges);
        m_program->characterSets.append(set);
        return entry->second;
    }

    unsigned newlineSet()
    {
        return internSet(m_pattern.newlineCharacterClass(), false, 0, 0);
    }

    static bool isCased(const PatternTerm& term)
    {
        return term.ignoreCase() && isASCIIAlpha(term.patternCharacter);
    }

    unsigned setForTerm(const PatternTerm& term)
    {
        if (term.type == PatternTerm::Type::CharacterClass)
            return internSet(term.characterClass, term.invert(), 0, 0);
        char16_t character = term.patternCharacter;
        if (isCased(term))
            return internSet(nullptr, false, toASCIILower(character), toASCIIUpper(character));
        return internSet(nullptr, false, character, character);
    }

    struct CaptureRange {
        unsigned first;
        unsigned last;
    };

    static std::optional<CaptureRange> captureRange(const PatternTerm& term)
    {
        if (!term.containsAnyCaptures())
            return std::nullopt;
        return CaptureRange { term.parentheses.subpatternId, term.parentheses.lastSubpatternId };
    }

    unsigned emitAlternativeChoice(const std::optional<FirstSetBits>& first, const FirstSetBits& later)
    {
        if (!first || hasAllLatin1Characters(*first))
            return emit(Kind::Choice);
        unsigned choice = emit(Kind::ChoiceByFirst);
        m_program->ops[choice].firstSet = static_cast<uint16_t>(internFirstSet(*first));
        m_program->ops[choice].laterFirstSet = static_cast<uint16_t>(internFirstSet(later));
        return choice;
    }

    std::optional<FirstSetBits> firstBits(const Branch& branch, bool& hasWideMembers) const
    {
        if (!branch.begin)
            return firstBits(*branch.alternative, hasWideMembers);
        FirstSetBits result;
        auto& terms = branch.alternative->m_terms;
        for (unsigned i = branch.begin; i < terms.size(); ++i) {
            const PatternTerm& term = terms[i];
            if (!isAtom(term))
                return std::nullopt;
            if (!term.quantityMaxCount)
                continue;
            auto set = latin1Bits(term.m_matchedSet, hasWideMembers);
            if (!set)
                return std::nullopt;
            addAll(result, *set);
            if (term.quantityMinCount)
                return result;
        }
        return std::nullopt;
    }

    static bool intersects(const FirstSetBits& a, const FirstSetBits& b)
    {
        return (a.bits[0] & b.bits[0]) || (a.bits[1] & b.bits[1]) || (a.bits[2] & b.bits[2]) || (a.bits[3] & b.bits[3]) || (a.atEnd && b.atEnd);
    }

    // A step takes its characters in one way only: a fixed number of them, or as many as there are. So (step a | step b) is step (a | b).
    unsigned termsOfStepAt(const Branch& branch)
    {
        auto& terms = branch.alternative->m_terms;
        if (branch.begin >= terms.size() || !isAtom(terms[branch.begin]))
            return 0;
        auto run = runAt(*branch.alternative, branch.begin);
        if (!run)
            return 1;
        return run->kind == Kind::PossessiveRun ? run->terms : 0;
    }

    static bool beginWithSameTerms(const Branch& a, const Branch& b, unsigned count)
    {
        for (unsigned i = 0; i < count; ++i) {
            const PatternTerm& one = a.alternative->m_terms[a.begin + i];
            const PatternTerm& other = b.alternative->m_terms[b.begin + i];
            if (!matchesSameCharacters(one, other) || one.quantityType != other.quantityType || one.quantityMinCount != other.quantityMinCount || one.quantityMaxCount != other.quantityMaxCount)
                return false;
        }
        return true;
    }

    // A branch joins an earlier group over the heads of the groups between them only if none of those can begin where it can: then it does not matter which of them is tried first.
    Vector<BranchGroup, 8> groupBranches(std::span<const Branch> branches, bool sharesSteps)
    {
        Vector<BranchGroup, 8> groups;
        for (const Branch& branch : branches) {
            bool hasWideMembers = false;
            auto first = firstBits(branch, hasWideMembers);
            unsigned stepTerms = sharesSteps ? termsOfStepAt(branch) : 0;
            BranchGroup* home = nullptr;
            bool mayPassOthers = first && !hasWideMembers && !first->atEnd;
            for (unsigned i = stepTerms ? groups.size() : 0; i--;) {
                BranchGroup& group = groups[i];
                if (group.stepTerms == stepTerms && beginWithSameTerms(group.members[0], branch, stepTerms)) {
                    home = &group;
                    break;
                }
                if (!mayPassOthers || !group.first || intersects(*group.first, *first))
                    break;
            }
            if (!home) {
                groups.append(BranchGroup { { branch }, first, stepTerms });
                continue;
            }
            home->members.append(branch);
            if (home->first && first)
                addAll(*home->first, *first);
            else
                home->first = std::nullopt;
        }
        return groups;
    }

    bool emitBranchGroup(const BranchGroup& group, EmitContext context)
    {
        const Branch& leader = group.members[0];
        if (group.members.size() == 1)
            return emitAlternative(*leader.alternative, context, leader.begin);

        unsigned shared = group.stepTerms;
        for (;;) {
            unsigned stepTerms = termsOfStepAt({ leader.alternative, leader.begin + shared });
            if (!stepTerms)
                break;
            bool isShared = true;
            for (unsigned i = 1; isShared && i < group.members.size(); ++i) {
                Branch rest { group.members[i].alternative, group.members[i].begin + shared };
                isShared = termsOfStepAt(rest) == stepTerms && beginWithSameTerms({ leader.alternative, leader.begin + shared }, rest, stepTerms);
            }
            if (!isShared)
                break;
            shared += stepTerms;
        }

        for (unsigned i = leader.begin, end = leader.begin + shared; i < end;) {
            if (auto run = runAt(*leader.alternative, i)) {
                emitRun(*run, 0);
                i += run->terms;
            } else
                i += emitFixedAtoms(*leader.alternative, i, end);
        }
        auto rest = group.members.map<Vector<Branch, 8>>([&](const Branch& member) {
            return Branch { member.alternative, member.begin + shared };
        });
        ++context.sharedPrefixDepth;
        return emitBranches(rest.span(), context);
    }

    bool emitBranches(std::span<const Branch> branches, EmitContext context)
    {
        if (hasExceededIndexLimit())
            return decline(FlatProgramDecline::TooManySetsOrRegisters);
        auto groups = groupBranches(branches, branches.size() > 2 && context.sharedPrefixDepth < maxSharedPrefixDepth);
        unsigned count = groups.size();
        if (count == 1)
            return emitBranchGroup(groups[0], context);

        Vector<std::optional<FirstSetBits>, 8> first(count);
        Vector<FirstSetBits, 8> later(count);
        for (unsigned i = 0; i < count; ++i)
            first[i] = groups[i].first;
        bool usesTable = count > 2;
        FirstSetBits unknown;
        unknown.bits.fill(~0ull);
        unknown.atEnd = true;
        for (unsigned i = count - 1; i--;) {
            later[i] = later[i + 1];
            addAll(later[i], first[i + 1].value_or(unknown));
        }

        unsigned smallestMinimumSize = branches[0].alternative->m_minimumSize;
        for (auto& branch : branches)
            smallestMinimumSize = std::min(smallestMinimumSize, branch.alternative->m_minimumSize);

        auto emitChecked = [&](const BranchGroup& group) {
            const Branch& leader = group.members[0];
            if (group.members.size() == 1 && !leader.begin && context.neededAfter && leader.alternative->m_minimumSize > smallestMinimumSize)
                emitNeedsInput(leader.alternative->m_minimumSize + context.neededAfter);
            return emitBranchGroup(group, context);
        };

        Vector<unsigned, 8> jumpsToEnd;
        Vector<unsigned, 8> choices;
        for (unsigned i = 0; i + 1 < count; ++i) {
            unsigned choice = usesTable ? emit(Kind::ChoiceByTable) : emitAlternativeChoice(first[i], later[i]);
            choices.append(choice);
            if (!emitChecked(groups[i]))
                return false;
            jumpsToEnd.append(emit(Kind::Jump));
            setTarget(choice);
        }
        if (usesTable)
            fillDispatchTable(choices.span(), first.span());
        if (!emitChecked(groups[count - 1]))
            return false;
        for (unsigned jump : jumpsToEnd)
            setTarget(jump);
        return true;
    }

    bool emitAlternativesInOrder(Alternatives alternatives, EmitContext context)
    {
        Vector<unsigned, 8> jumpsToEnd;
        for (auto& alternative : alternatives) {
            bool isLast = &alternative == &alternatives.back();
            unsigned choice = isLast ? 0 : emit(Kind::Choice);
            if (alternative->m_minimumSize > context.testedFor)
                emitNeedsInput(alternative->m_minimumSize + context.neededAfter);
            if (!emitAlternative(*alternative, context))
                return false;
            if (isLast)
                break;
            jumpsToEnd.append(emit(Kind::Jump));
            setTarget(choice);
        }
        for (unsigned jump : jumpsToEnd)
            setTarget(jump);
        return true;
    }

    bool emitAlternatives(Alternatives alternatives, EmitContext context)
    {
        if (context.nestingDepth >= maxNestingDepth)
            return decline(FlatProgramDecline::NestedTooDeep);
        ++context.nestingDepth;
        if (m_countsTurns)
            return emitAlternativesInOrder(alternatives, context);
        if (alternatives.size() == 1)
            return emitAlternative(*alternatives[0], context);
        Vector<Branch, 8> branches;
        branches.reserveInitialCapacity(alternatives.size());
        for (auto& alternative : alternatives)
            branches.append(Branch { alternative.get(), 0 });
        return emitBranches(branches.span(), context);
    }

    bool emitGroupBody(const PatternTerm& term, EmitContext context)
    {
        unsigned slot = term.parentheses.subpatternId << 1;
        auto& alternatives = term.parentheses.disjunction->m_alternatives;
        if (term.capture() && alternatives.size() == 1 && alternatives[0]->matchDirection() == Forward) {
            auto& terms = alternatives[0]->m_terms;
            if (terms.isEmpty()) {
                emit(Kind::LoggedCaptureEmpty, slot);
                return true;
            }
            if (auto run = runAt(*alternatives[0], 0); run && run->terms == terms.size() && run->kind == Kind::PossessiveRun && run->max == quantifyInfinite) {
                emitWithSet(Kind::LoggedCapturedRun, run->set, slot, run->min);
                return true;
            }
        }
        if (term.capture())
            emit(Kind::LoggedCaptureStart, slot);
        if (!emitAlternatives(alternatives.span(), context))
            return false;
        if (term.capture())
            emit(Kind::LoggedCaptureEnd, slot + 1);
        return true;
    }

    bool emitOptionalGroup(const PatternTerm& term, EmitContext context)
    {
        bool canBeEmpty = !term.parentheses.disjunction->m_minimumSize;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;

        auto emitChecked = [&] {
            if (canBeEmpty)
                emitWithRegister(Kind::LoggedSavePosition, start);
            if (!emitGroupBody(term, context))
                return false;
            if (canBeEmpty)
                emitWithRegister(Kind::FailIfEmpty, start);
            return true;
        };

        uint64_t needed = neededToEnter(*term.parentheses.disjunction, context.neededAfter);
        if (term.quantityType == QuantifierType::Greedy) {
            std::optional<unsigned> check;
            if (needed)
                check = emitNeedsInputElseJump(needed);
            unsigned choice = emit(Kind::Choice);
            if (!emitChecked())
                return false;
            setTarget(choice);
            if (check)
                setTarget(*check);
            return true;
        }
        unsigned choice = emit(Kind::Choice);
        unsigned jump = emit(Kind::Jump);
        setTarget(choice);
        if (needed)
            emitNeedsInput(needed);
        if (!emitChecked())
            return false;
        setTarget(jump);
        return true;
    }

    struct Loop {
        unsigned min;
        unsigned max;
        bool isLazy;
        bool canBeEmpty;
        uint64_t neededForAnotherIteration;
        std::optional<CaptureRange> captures;
        bool countsTurns { false };
    };

    template<typename Body>
    bool emitLoop(const Loop& loop, const Body& emitBody)
    {
        auto [min, max, isLazy, canBeEmpty, neededForAnotherIteration, captures, countsTurns] = loop;
        bool needsCounter = min > 1 || max != quantifyInfinite;
        unsigned counter = needsCounter ? m_nextRegister++ : 0;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;

        std::optional<unsigned> jumpToBody;
        std::optional<unsigned> jumpToMandatory;
        Vector<unsigned, 3> jumpsToExit;
        if (needsCounter)
            emitWithRegister(Kind::LoggedSetRegister, counter, 0);
        else if (min) {
            if (canBeEmpty)
                emitWithRegister(Kind::LoggedSetRegister, start, offsetNoMatch);
            if (countsTurns)
                emit(Kind::CountTurn);
            jumpToBody = emit(Kind::Jump);
        }

        unsigned head = here();
        if (needsCounter && min)
            jumpToMandatory = emitWithRegister(Kind::JumpIfBelow, counter, min);
        if (min == max)
            jumpsToExit.append(emit(Kind::Jump));
        else {
            if (max != quantifyInfinite)
                jumpsToExit.append(emitWithRegister(Kind::JumpIfNotBelow, counter, max));
            if (isLazy) {
                unsigned choice = emit(Kind::Choice);
                jumpsToExit.append(emit(Kind::Jump));
                setTarget(choice);
                if (countsTurns)
                    emit(Kind::CountTurn);
                if (neededForAnotherIteration)
                    emitNeedsInput(neededForAnotherIteration);
                if (canBeEmpty)
                    emitWithRegister(Kind::LoggedSavePosition, start);
            } else {
                if (countsTurns)
                    emit(Kind::CountTurn);
                if (canBeEmpty)
                    jumpsToExit.append(emitWithRegister(Kind::GreedyLoopChoice, start));
                else {
                    if (neededForAnotherIteration)
                        jumpsToExit.append(emitNeedsInputElseJump(neededForAnotherIteration));
                    jumpsToExit.append(emit(Kind::Choice));
                }
            }
        }
        if (jumpToMandatory) {
            if (canBeEmpty || countsTurns) {
                std::optional<unsigned> skip;
                if (min != max)
                    skip = emit(Kind::Jump);
                setTarget(*jumpToMandatory);
                if (countsTurns)
                    emit(Kind::CountTurn);
                if (canBeEmpty)
                    emitWithRegister(Kind::LoggedSetRegister, start, offsetNoMatch);
                if (skip)
                    setTarget(*skip);
            } else
                setTarget(*jumpToMandatory);
        }
        if (jumpToBody)
            setTarget(*jumpToBody);
        if (needsCounter)
            emitWithRegister(Kind::LoggedIncrement, counter);
        if (captures)
            emit(Kind::LoggedClearCaptures, captures->first, captures->last);
        if (!emitBody())
            return false;
        if (canBeEmpty)
            emitWithRegister(Kind::LoopBackUnlessEmpty, start, 0, head);
        else
            emit(Kind::LoopBack, 0, head);
        for (unsigned jump : jumpsToExit)
            setTarget(jump);
        return true;
    }

    enum class InputConsumption : uint8_t { Unknown, None };

    static InputConsumption computeInputConsumption(const PatternDisjunction& disjunction, unsigned& termsLeftToVisit)
    {
        for (auto& alternative : disjunction.m_alternatives) {
            for (auto& term : alternative->m_terms) {
                if (!termsLeftToVisit)
                    return InputConsumption::Unknown;
                --termsLeftToVisit;
                switch (term.type) {
                case PatternTerm::Type::PatternCharacter:
                case PatternTerm::Type::CharacterClass:
                    if (term.quantityMaxCount)
                        return InputConsumption::Unknown;
                    break;
                case PatternTerm::Type::ParenthesesSubpattern:
                    if (term.quantityMaxCount && computeInputConsumption(*term.parentheses.disjunction, termsLeftToVisit) == InputConsumption::Unknown)
                        return InputConsumption::Unknown;
                    break;
                case PatternTerm::Type::NumberedBackReference:
                case PatternTerm::Type::NamedBackReference:
                case PatternTerm::Type::DotStarEnclosure:
                    return InputConsumption::Unknown;
                case PatternTerm::Type::AssertionBOL:
                case PatternTerm::Type::AssertionEOL:
                case PatternTerm::Type::AssertionWordBoundary:
                case PatternTerm::Type::NumberedForwardReference:
                case PatternTerm::Type::NamedForwardReference:
                case PatternTerm::Type::ParentheticalAssertion:
                    break;
                }
            }
        }
        return InputConsumption::None;
    }

    bool emitGroup(const PatternTerm& term, EmitContext context)
    {
        if (term.parentheses.isCopy)
            return decline(FlatProgramDecline::CopiedGroup);
        if (term.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        unsigned min = term.quantityMinCount;
        unsigned max = term.quantityMaxCount;
        const PatternDisjunction& disjunction = *term.parentheses.disjunction;
        if (!max)
            return true;
        if (m_countsTurns) {
            bool isTestedForByParent = max == 1 && term.quantityType == QuantifierType::FixedCount;
            context.initialInputPosition = term.inputPosition - (isTestedForByParent ? disjunction.m_minimumSize : 0);
            if (isTestedForByParent || (!min && neededToEnter(disjunction, context.neededAfter)))
                context.testedFor = disjunction.m_minimumSize;
        } else if (min != 1 || max != 1) {
            unsigned termsLeftToVisit = maxTermsVisitedForEmptiness;
            if (computeInputConsumption(disjunction, termsLeftToVisit) == InputConsumption::None)
                return !min || emitGroupBody(term, context);
        }
        if (max == 1)
            return min ? emitGroupBody(term, context) : emitOptionalGroup(term, context);
        Loop loop { min, max, term.quantityType == QuantifierType::NonGreedy, !disjunction.m_minimumSize, neededToEnter(disjunction, context.neededAfter), captureRange(term), m_countsTurns && jitCountsTurnsOf(term) };
        return emitLoop(loop, [&] {
            return emitGroupBody(term, context);
        });
    }

    bool emitLookahead(const PatternTerm& term, EmitContext context)
    {
        if (term.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        Alternatives alternatives = term.parentheses.disjunction->m_alternatives.span();
        context.initialInputPosition = term.inputPosition;
        unsigned frameDepthRegister = m_nextRegister++;
        if (term.invert()) {
            unsigned begin = emit(Kind::NegativeLookaheadBegin, frameDepthRegister);
            if (!emitAlternatives(alternatives, context))
                return false;
            emit(Kind::NegativeLookaheadEnd, frameDepthRegister);
            setTarget(begin);
            return true;
        }

        unsigned positionRegister = m_nextRegister++;
        emitWithRegister(Kind::LookaheadBegin, positionRegister, frameDepthRegister);
        if (!emitAlternatives(alternatives, context))
            return false;
        emitWithRegister(term.containsAnyCaptures() ? Kind::LookaheadEndKeepingCaptures : Kind::LookaheadEnd, positionRegister, frameDepthRegister);
        return true;
    }

    struct Run {
        Kind kind;
        unsigned set;
        unsigned firstSet;
        unsigned min;
        unsigned max;
        unsigned terms;
    };

    static bool isAtom(const PatternTerm& term)
    {
        return (term.type == PatternTerm::Type::PatternCharacter || term.type == PatternTerm::Type::CharacterClass) && term.matchDirection() == Forward;
    }

    static bool matchesSameCharacters(const PatternTerm& a, const PatternTerm& b)
    {
        if (a.type != b.type)
            return false;
        if (a.type == PatternTerm::Type::PatternCharacter)
            return a.patternCharacter == b.patternCharacter && isCased(a) == isCased(b);
        return a.characterClass == b.characterClass && a.invert() == b.invert();
    }

    std::optional<Run> runAt(const PatternAlternative& alternative, unsigned i)
    {
        auto& terms = alternative.m_terms;
        if (!isAtom(terms[i]))
            return std::nullopt;
        unsigned min = 0;
        unsigned counted = i;
        if (terms[i].quantityType == QuantifierType::FixedCount) {
            min = terms[i].quantityMaxCount;
            counted = i + 1;
            if (counted == terms.size() || !isAtom(terms[counted]) || terms[counted].quantityType == QuantifierType::FixedCount || !matchesSameCharacters(terms[i], terms[counted]))
                return std::nullopt;
        }
        const PatternTerm& term = terms[counted];
        unsigned set = setForTerm(term);
        unsigned extra = term.quantityMaxCount;
        unsigned max = extra == quantifyInfinite || extra > quantifyInfinite - min ? quantifyInfinite : min + extra;
        bool isGreedy = term.quantityType == QuantifierType::Greedy;
        Run run { isGreedy ? Kind::GreedyRun : Kind::LazyRun, set, 0, min, max, counted + 1 - i };
        if (m_countsTurns) {
            // Only tests of the next term's that fail are left out: no turn begins before one.
            if (counted + 1 == terms.size())
                return run;
            const PatternTerm& next = terms[counted + 1];
            if (!isAtom(next) || next.quantityType != QuantifierType::FixedCount || !next.quantityMaxCount)
                return run;
            auto hasWideMembers = [](const CharacterSetBits& bits) {
                return bits.wideRangeCount || bits.invertsWideRanges;
            };
            CharacterSetBits following = m_program->characterSets[setForTerm(next)];
            CharacterSetBits members = m_program->characterSets[set];
            bool areComplements = term.type == PatternTerm::Type::CharacterClass && next.type == PatternTerm::Type::CharacterClass && term.characterClass == next.characterClass && term.invert() != next.invert();
            bool areDisjoint = areComplements || !hasWideMembers(members) || !hasWideMembers(following);
            for (unsigned word = 0; word < members.bits.size(); ++word)
                areDisjoint &= !(members.bits[word] & following.bits[word]);
            if (areDisjoint) {
                run.kind = Kind::PossessiveRun;
                return run;
            }
            if (hasWideMembers(following))
                return run;
            run.kind = isGreedy ? Kind::GreedyRunBeforeKnownCharacters : Kind::LazyRunBeforeKnownCharacters;
            run.firstSet = internFirstSet({ following.bits, false });
            return run;
        }
        if ((isGreedy && term.possessive()) || m_pattern.m_firstFollowSets.areDisjoint(term.m_matchedSet, term.m_followSet)) {
            run.kind = Kind::PossessiveRun;
            return run;
        }
        bool hasWideMembers = false;
        auto following = latin1Bits(term.m_followSet, hasWideMembers);
        if (!following || hasWideMembers || hasAllLatin1Characters(*following))
            return run;
        run.kind = isGreedy ? Kind::GreedyRunBeforeKnownCharacters : Kind::LazyRunBeforeKnownCharacters;
        run.firstSet = internFirstSet(*following);
        return run;
    }

    void emitRun(const Run& run, uint64_t neededAfter)
    {
        unsigned op = emitWithSet(run.kind, run.set, run.max, run.min);
        m_program->ops[op].firstSet = static_cast<uint16_t>(run.firstSet);
        if (run.kind != Kind::PossessiveRun)
            setNeeded(op, neededAfter);
        else if (m_countsTurns && neededAfter)
            emitNeedsInput(neededAfter);
    }

    void emitFixedAtom(const PatternTerm& term)
    {
        unsigned count = term.quantityMaxCount;
        if (!count)
            return;
        if (count > 1)
            emitWithSet(Kind::SetCount, setForTerm(term), count);
        else if (term.type == PatternTerm::Type::PatternCharacter && !isCased(term))
            emit(Kind::Character, term.patternCharacter);
        else
            emitWithSet(Kind::Set, setForTerm(term));
    }

    static bool isTextCharacter(const PatternAlternative& alternative, unsigned i, bool wide)
    {
        auto& terms = alternative.m_terms;
        if (i >= terms.size())
            return false;
        const PatternTerm& term = terms[i];
        if (term.type != PatternTerm::Type::PatternCharacter || term.matchDirection() != Forward || term.quantityType != QuantifierType::FixedCount || term.quantityMaxCount != 1)
            return false;
        if (isLatin1(term.patternCharacter) == wide)
            return false;
        bool beginsRun = i + 1 < terms.size() && isAtom(terms[i + 1]) && terms[i + 1].quantityType != QuantifierType::FixedCount && matchesSameCharacters(term, terms[i + 1]);
        return !beginsRun;
    }

    unsigned emitFixedAtoms(const PatternAlternative& alternative, unsigned i, unsigned limit = std::numeric_limits<unsigned>::max())
    {
        auto& terms = alternative.m_terms;
        const PatternTerm& term = terms[i];
        bool isWide = term.type == PatternTerm::Type::PatternCharacter && !isLatin1(term.patternCharacter);
        unsigned end = i;
        bool hasCase = false;
        while (end < limit && isTextCharacter(alternative, end, isWide)) {
            hasCase |= isCased(terms[end]);
            ++end;
        }
        if (end - i < 2) {
            emitFixedAtom(term);
            return 1;
        }
        if (isWide) {
            unsigned offset = m_program->wideText.size();
            for (unsigned j = i; j < end; ++j)
                m_program->wideText.append(static_cast<char16_t>(terms[j].patternCharacter));
            emit(Kind::WideText, offset, end - i);
        } else {
            unsigned offset = m_program->text.size();
            for (unsigned j = i; j < end; ++j) {
                auto character = static_cast<Latin1Character>(terms[j].patternCharacter);
                m_program->text.append(isCased(terms[j]) ? toASCIILower(character) : character);
            }
            if (hasCase) {
                for (unsigned j = i; j < end; ++j)
                    m_program->text.append(isCased(terms[j]) ? 0x20 : 0);
            }
            emit(hasCase ? Kind::CasedText : Kind::Text, offset, end - i);
        }
        return end - i;
    }

    bool emitBackReference(const PatternTerm& term)
    {
        if (term.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        auto emitReference = [&] {
            emit(term.ignoreCase() ? Kind::BackReferenceIgnoringCase : Kind::BackReference, term.backReferenceSubpatternId << 1);
            return true;
        };
        unsigned max = term.quantityMaxCount;
        if (!max)
            return true;
        bool isFixed = term.quantityType == QuantifierType::FixedCount;
        if (isFixed && max == 1)
            return emitReference();
        return emitLoop({ isFixed ? max : 0, max, term.quantityType == QuantifierType::NonGreedy, true, 0, std::nullopt }, emitReference);
    }

    bool emitAlternative(const PatternAlternative& alternative, EmitContext context, unsigned begin = 0)
    {
        if (alternative.matchDirection() != Forward)
            return decline(FlatProgramDecline::Lookbehind);
        auto& terms = alternative.m_terms;
        uint64_t neededAfter = context.neededAfter;
        for (unsigned i = begin; i < terms.size(); ++i)
            neededAfter += minimumSize(terms[i]);
        unsigned accounted = begin;
        auto accountThrough = [&](unsigned last) {
            while (accounted <= last)
                neededAfter -= minimumSize(terms[accounted++]);
            if (m_countsTurns)
                neededAfter = context.neededAfter + context.initialInputPosition + alternative.m_minimumSize - terms[last].inputPosition;
        };
        for (unsigned i = begin; i < terms.size(); ++i) {
            if (hasExceededIndexLimit())
                return decline(FlatProgramDecline::TooManySetsOrRegisters);
            const PatternTerm& term = terms[i];
            accountThrough(i);
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
                if (term.multiline())
                    emitWithSet(Kind::LineStart, newlineSet());
                else
                    emit(Kind::InputStart);
                break;
            case PatternTerm::Type::AssertionEOL:
                if (term.multiline())
                    emitWithSet(Kind::LineEnd, newlineSet());
                else
                    emit(Kind::InputEnd);
                break;
            case PatternTerm::Type::AssertionWordBoundary:
                emitWithSet(term.invert() ? Kind::NotWordBoundary : Kind::WordBoundary, internSet(m_pattern.wordcharCharacterClass(), false, 0, 0));
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass:
                if (term.matchDirection() != Forward)
                    return decline(FlatProgramDecline::Lookbehind);
                if (auto run = runAt(alternative, i)) {
                    accountThrough(i + run->terms - 1);
                    emitRun(*run, neededAfter);
                    i += run->terms - 1;
                    break;
                }
                i += emitFixedAtoms(alternative, i) - 1;
                break;
            case PatternTerm::Type::ParenthesesSubpattern:
                if (!emitGroup(term, { neededAfter, context.nestingDepth, context.sharedPrefixDepth, 0, 0 }))
                    return false;
                break;
            case PatternTerm::Type::ParentheticalAssertion:
                if (!emitLookahead(term, { 0, context.nestingDepth, context.sharedPrefixDepth, 0, 0 }))
                    return false;
                break;
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                if (!emitBackReference(term))
                    return false;
                if (m_countsTurns && neededAfter)
                    emitNeedsInput(neededAfter);
                break;
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                break;
            case PatternTerm::Type::DotStarEnclosure:
                return decline(FlatProgramDecline::DotStarEnclosure);
            }
        }
        return true;
    }

    const YarrPattern& m_pattern;
    FlatProgram* m_program { nullptr };
    std::optional<FlatProgramDecline> m_decline;
    StdUnorderedMap<SetKey, unsigned, InternedKeyHash> m_setIndexes;
    StdUnorderedMap<FirstSetBits, unsigned, InternedKeyHash> m_firstSetIndexes;
    unsigned m_nextRegister { 0 };
    const bool m_countsTurns;
};

size_t FlatProgram::estimatedSizeInBytes() const
{
    return ops.capacity() * sizeof(FlatOp) + characterSets.capacity() * sizeof(CharacterSetBits) + firstSets.capacity() * sizeof(FirstSetBits)
        + dispatchClasses.capacity() * sizeof(uint16_t) + dispatchEntries.capacity() * sizeof(uint32_t)
        + wideRanges.capacity() * sizeof(CharacterRange) + text.capacity() + wideText.capacity() * sizeof(char16_t);
}

// As YarrGenerator::disjunctionContainsBacktrackableContent().
static bool containsBacktrackableContent(const PatternDisjunction& disjunction)
{
    if (disjunction.m_alternatives.size() > 1)
        return true;
    for (auto& alternative : disjunction.m_alternatives) {
        for (auto& term : alternative->m_terms) {
            if (term.quantityType != QuantifierType::FixedCount)
                return true;
            if (term.type == PatternTerm::Type::NumberedBackReference || term.type == PatternTerm::Type::NamedBackReference)
                return true;
            if ((term.type == PatternTerm::Type::ParenthesesSubpattern || term.type == PatternTerm::Type::ParentheticalAssertion) && containsBacktrackableContent(*term.parentheses.disjunction))
                return true;
        }
    }
    return false;
}

bool jitCountsTurnsOf(const PatternTerm& term)
{
    ASSERT(term.type == PatternTerm::Type::ParenthesesSubpattern);
    if ((term.quantityMaxCount == 1 && !term.parentheses.isCopy) || term.parentheses.isTerminal)
        return false;
    if (term.quantityType != QuantifierType::FixedCount)
        return true;
    return term.parentheses.disjunction->m_alternatives.size() != 1 || (term.quantityMaxCount > 1 && containsBacktrackableContent(*term.parentheses.disjunction));
}

std::expected<std::unique_ptr<FlatProgram>, FlatProgramDecline> compileFlatProgram(const YarrPattern& pattern, FlatProgramForm form)
{
    ASSERT(pattern.m_executionMode == ExecutionMode::IncludeSubpatterns);
    return FlatProgramBuilder(pattern, form).build();
}

ASCIILiteral description(FlatProgramDecline reason)
{
    switch (reason) {
    case FlatProgramDecline::UnicodeMode:
        return "unicode mode"_s;
    case FlatProgramDecline::Modifiers:
        return "modifiers"_s;
    case FlatProgramDecline::Lookbehind:
        return "lookbehind"_s;
    case FlatProgramDecline::DuplicateNamedGroups:
        return "duplicate named groups"_s;
    case FlatProgramDecline::UnsignedLengthPattern:
        return "unsigned length pattern"_s;
    case FlatProgramDecline::CopiedGroup:
        return "copied group"_s;
    case FlatProgramDecline::DotStarEnclosure:
        return "enclosed by .*"_s;
    case FlatProgramDecline::NestedTooDeep:
        return "nested too deep"_s;
    case FlatProgramDecline::TooManySetsOrRegisters:
        return "too many sets or registers"_s;
    case FlatProgramDecline::NoTurnsToCount:
        return "no turns to count"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::Yarr
