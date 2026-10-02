/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "YarrLinear.h"

#if USE(BUN_JSC_ADDITIONS)

#include "Options.h"
#include <algorithm>
#include <limits>
#include <unicode/utf16.h>
#include <wtf/ASCIICType.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/HashMap.h>
#include <wtf/StackCheck.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr {

WTF_MAKE_TZONE_ALLOCATED_IMPL(LinearProgram);

using Opcode = LinearInstruction::Opcode;

ASCIILiteral linearRefusalName(LinearRefusal refusal)
{
    switch (refusal) {
    case LinearRefusal::None:
        return "none"_s;
    case LinearRefusal::BackReference:
        return "backreference"_s;
    case LinearRefusal::UnboundedLookaround:
        return "lookaround of unbounded length"_s;
    case LinearRefusal::NestingTooDeep:
        return "nesting too deep"_s;
    case LinearRefusal::ProgramTooLarge:
        return "program too large"_s;
    case LinearRefusal::WorkingMemoryTooLarge:
        return "working memory too large"_s;
    case LinearRefusal::LookaroundTooCostly:
        return "lookaround too costly"_s;
    case LinearRefusal::UnsupportedTerm:
        return "unsupported term"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

// ---------------------------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------------------------

class LinearCompiler {
public:
    explicit LinearCompiler(YarrPattern& pattern)
        : m_pattern(pattern)
        , m_program(makeUnique<LinearProgram>())
        , m_maximumProgramSize(std::min(Options::maximumRegExpLinearProgramSize(), maximumProgramSizeLimit))
    {
        // A term that emits nothing still costs the compiler a visit, so the visits have a
        // limit of their own: /(?:(?:(?:){1000}){1000}){1000}/ has no instruction at all.
        m_remainingWork = static_cast<uint64_t>(m_maximumProgramSize) * 8 + 1024;
    }

    std::unique_ptr<LinearProgram> compile(LinearRefusal& refusal)
    {
        compileBody();
        if (!failed() && workingMemoryBound() > Options::maximumRegExpLinearWorkingMemory())
            refuse(LinearRefusal::WorkingMemoryTooLarge);
        if (!failed()) {
            // Without a lookaround the bound is under this limit: the program is.
            m_program->m_maximumStepsPerPosition = maximumStepsPerPosition();
            if (m_program->m_maximumStepsPerPosition > static_cast<uint64_t>(m_maximumProgramSize) * stepsOfWaitingInstruction)
                refuse(LinearRefusal::LookaroundTooCostly);
        }
        refusal = m_refusal;
        if (failed())
            return nullptr;

        computeFirstCharacters();
        for (auto& code : m_program->m_codes)
            code.instructions.shrinkToFit();
        m_program->m_codes.shrinkToFit();
        m_program->m_classes.shrinkToFit();
        return WTF::move(m_program);
    }

private:
    // The lookarounds that can be inside one another. Each level is a matcher of its own on the
    // native stack while the outer ones wait.
    static constexpr unsigned maximumLookaroundDepth = 16;

    // The matcher keeps an instruction's index and a bit in one unsigned.
    static constexpr unsigned maximumProgramSizeLimit = 1u << 30;

    // LinearMatcher::Frame.
    static constexpr uint64_t bytesPerFrame = 3 * sizeof(unsigned);

    static constexpr uint64_t unbounded = std::numeric_limits<uint64_t>::max();

    // How many characters a term or a disjunction can match.
    struct Lengths {
        uint64_t minimum { 0 };
        uint64_t maximum { 0 };
    };

    struct Repeat {
        unsigned minimum { 1 };
        unsigned maximum { 1 }; // quantifyInfinite for no limit
        bool greedy { true };
        // What one iteration can match. An iteration past the minimum has to consume something
        // (RepeatMatcher in the specification), which only needs a check when it could be empty.
        Lengths iteration;
        // The slots of the groups inside, which every iteration starts by clearing.
        bool clears { false };
        unsigned firstClearedGroup { 0 };
        unsigned lastClearedGroup { 0 };
    };

    bool failed() const { return m_refusal != LinearRefusal::None; }

    bool refuse(LinearRefusal refusal)
    {
        if (!failed())
            m_refusal = refusal;
        return false;
    }

    bool charge()
    {
        if (failed())
            return false;
        if (!m_remainingWork)
            return refuse(LinearRefusal::ProgramTooLarge);
        --m_remainingWork;
        return true;
    }

    static uint64_t saturatedSum(uint64_t a, uint64_t b)
    {
        uint64_t sum = a + b;
        return sum < a ? unbounded : sum;
    }

    static uint64_t saturatedProduct(uint64_t a, uint64_t b)
    {
        if (!a || !b)
            return 0;
        if (a == unbounded || b == unbounded || a > unbounded / b)
            return unbounded;
        return a * b;
    }

    LinearCode& code() { return m_program->m_codes[m_currentCode]; }
    unsigned here() { return code().instructions.size(); }

    // Never fails: the instruction that passes the limit is still appended, so that an index
    // emit() returned can be patched, and the caller stops at its next charge().
    unsigned emit(Opcode opcode, unsigned a = 0, unsigned b = 0, uint8_t flags = 0)
    {
        if (m_instructionCount >= m_maximumProgramSize)
            refuse(LinearRefusal::ProgramTooLarge);
        ++m_instructionCount;
        unsigned index = here();
        code().instructions.append(LinearInstruction { opcode, flags, a, b });
        return index;
    }

    void setSplitTargets(unsigned split, unsigned iteration, unsigned exit, bool greedy)
    {
        auto& instruction = code().instructions[split];
        ASSERT(instruction.opcode == Opcode::Split);
        instruction.a = greedy ? iteration : exit;
        instruction.b = greedy ? exit : iteration;
    }

    Lengths lengthsOf(PatternDisjunction* disjunction)
    {
        auto iterator = m_lengths.find(disjunction);
        if (iterator != m_lengths.end())
            return iterator->value;

        if (!m_stackCheck.isSafeToRecurse()) [[unlikely]] {
            refuse(LinearRefusal::NestingTooDeep);
            return { 0, unbounded };
        }

        Lengths result { unbounded, 0 };
        for (auto& alternative : disjunction->m_alternatives) {
            Lengths sum;
            for (auto& term : alternative->m_terms) {
                Lengths one = lengthsOf(term);
                sum.minimum = saturatedSum(sum.minimum, one.minimum);
                sum.maximum = saturatedSum(sum.maximum, one.maximum);
            }
            result.minimum = std::min(result.minimum, sum.minimum);
            result.maximum = std::max(result.maximum, sum.maximum);
        }
        if (disjunction->m_alternatives.isEmpty())
            result = { };

        m_lengths.add(disjunction, result);
        return result;
    }

    Lengths lengthsOf(const PatternTerm& term)
    {
        switch (term.type) {
        case PatternTerm::Type::AssertionBOL:
        case PatternTerm::Type::AssertionEOL:
        case PatternTerm::Type::AssertionWordBoundary:
        case PatternTerm::Type::ParentheticalAssertion:
        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
            return { };
        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
        case PatternTerm::Type::DotStarEnclosure:
            return { 0, unbounded };
        case PatternTerm::Type::PatternCharacter:
        case PatternTerm::Type::CharacterClass: {
            unsigned maximum = term.quantityMaxCount.value();
            return { term.quantityMinCount.value(), maximum == quantifyInfinite ? unbounded : maximum };
        }
        case PatternTerm::Type::ParenthesesSubpattern: {
            Lengths iteration = lengthsOf(term.parentheses.disjunction);
            unsigned maximum = term.quantityMaxCount.value();
            return {
                saturatedProduct(iteration.minimum, term.quantityMinCount.value()),
                saturatedProduct(iteration.maximum, maximum == quantifyInfinite ? unbounded : maximum)
            };
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
        return { };
    }

    unsigned classIndexFor(CharacterClass* characterClass)
    {
        auto result = m_classIndices.add(characterClass, m_program->m_classes.size());
        if (!result.isNewEntry)
            return result.iterator->value;

        // Only the lists say what a class holds: m_table and m_latin1Table are caches of them,
        // and a shared inverted class points at the table of the class it inverts.
        LinearCharacterClass compiled;
        if (characterClass->m_anyCharacter) {
            compiled.latin1.setAll();
            compiled.ranges.append(CharacterRange { 0x100, UCHAR_MAX_VALUE });
            m_program->m_classes.append(WTF::move(compiled));
            return result.iterator->value;
        }

        Vector<CharacterRange> wide;
        auto add = [&](char32_t begin, char32_t end) {
            ASSERT(begin <= end);
            for (char32_t character = begin; character <= end && character < compiled.latin1.size(); ++character)
                compiled.latin1.set(character);
            if (end >= compiled.latin1.size())
                wide.append(CharacterRange { std::max<char32_t>(begin, compiled.latin1.size()), end });
        };
        for (char32_t character : characterClass->m_matches8)
            add(character, character);
        for (auto& range : characterClass->m_ranges8)
            add(range.begin, range.end);
        for (char32_t character : characterClass->m_matches32)
            add(character, character);
        for (auto& range : characterClass->m_ranges32)
            add(range.begin, range.end);

        std::ranges::sort(wide, { }, &CharacterRange::begin);
        for (auto& range : wide) {
            if (!compiled.ranges.isEmpty() && range.begin <= compiled.ranges.last().end + 1) {
                compiled.ranges.last().end = std::max(compiled.ranges.last().end, range.end);
                continue;
            }
            compiled.ranges.append(range);
        }
        compiled.ranges.shrinkToFit();
        m_program->m_classes.append(WTF::move(compiled));
        return result.iterator->value;
    }

    void compileBody()
    {
        m_program->m_codes.append(LinearCode { });
        m_currentCode = 0;
        code().writesSlots = true;

        m_program->m_captureSlotCount = (m_pattern.m_numSubpatterns + 1) * 2;
        m_program->m_slotCount = m_pattern.offsetsSize();
        m_program->m_decodeSurrogatePairs = m_pattern.eitherUnicode();
        m_program->m_sticky = m_pattern.sticky();

        if (m_pattern.m_containsBackreferences) {
            refuse(LinearRefusal::BackReference);
            return;
        }

        // optimizeBOL() lays the body out as the pattern's own alternatives, marked onceThrough,
        // followed by the copies the backtracking engines retry at later positions. The copies
        // match nothing the alternatives do not.
        auto& alternatives = m_pattern.m_body->m_alternatives;
        bool hasOnceThrough = !alternatives.isEmpty() && alternatives[0]->onceThrough();
        Vector<PatternAlternative*, 8> selected;
        for (auto& alternative : alternatives) {
            if (!hasOnceThrough || alternative->onceThrough())
                selected.append(alternative.get());
        }
        // No copy was made: every alternative has to begin at the start of the subject.
        m_program->m_anchoredAtStart = hasOnceThrough && selected.size() == alternatives.size();
        // optimizeDotStarWrappedExpressions() moved a leading ^ into the enclosure and told
        // optimizeBOL() nothing about it.
        if (selected.size() == 1) {
            if (auto* enclosure = dotStarEnclosureOf(*selected[0]); enclosure && enclosure->anchors.bolAnchor && !enclosure->multiline())
                m_program->m_anchoredAtStart = true;
        }

        emit(Opcode::Save, 0);
        if (!compileAlternatives(selected.span(), Forward))
            return;
        emit(Opcode::Save, 1);
        emit(Opcode::Match);
    }

    bool compileDisjunction(PatternDisjunction* disjunction, MatchDirection direction)
    {
        if (!m_stackCheck.isSafeToRecurse()) [[unlikely]]
            return refuse(LinearRefusal::NestingTooDeep);

        Vector<PatternAlternative*, 8> alternatives;
        for (auto& alternative : disjunction->m_alternatives)
            alternatives.append(alternative.get());
        return compileAlternatives(alternatives.span(), direction);
    }

    bool compileAlternatives(std::span<PatternAlternative* const> alternatives, MatchDirection direction)
    {
        Vector<unsigned, 8> jumpsToEnd;
        for (size_t index = 0; index < alternatives.size(); ++index) {
            if (!charge())
                return false;
            bool isLast = index + 1 == alternatives.size();
            unsigned split = 0;
            if (!isLast)
                split = emit(Opcode::Split);
            unsigned begin = here();
            if (!compileAlternative(*alternatives[index], direction))
                return false;
            if (!isLast) {
                jumpsToEnd.append(emit(Opcode::Jump));
                setSplitTargets(split, begin, here(), true);
            }
        }
        unsigned end = here();
        for (unsigned jump : jumpsToEnd)
            code().instructions[jump].a = end;
        return !failed();
    }

    // optimizeDotStarWrappedExpressions() rewrites /^.*X.*$/ (with or without the ^ and the $)
    // to X followed by one term that says which of the two anchors there were. The backtracking
    // engines match X and widen the match to its line. The matcher runs the pattern as it was
    // written, which is the same match.
    static PatternTerm* dotStarEnclosureOf(PatternAlternative& alternative)
    {
        if (alternative.m_terms.isEmpty() || alternative.m_terms.last().type != PatternTerm::Type::DotStarEnclosure)
            return nullptr;
        return &alternative.m_terms.last();
    }

    bool emitDotStar(const PatternTerm& enclosure)
    {
        unsigned classIndex = classIndexFor(enclosure.dotAll() ? m_pattern.anyCharacterClass() : m_pattern.newlineCharacterClass());
        uint8_t flags = enclosure.dotAll() ? 0 : LinearInstruction::Invert;
        Repeat repeat;
        repeat.minimum = 0;
        repeat.maximum = quantifyInfinite;
        repeat.iteration = { 1, 1 };
        return emitRepeated(repeat, [&] {
            emit(Opcode::CharacterClass, classIndex, 0, flags);
            return true;
        });
    }

    bool compileAlternative(PatternAlternative& alternative, MatchDirection direction)
    {
        size_t count = alternative.m_terms.size();
        PatternTerm* enclosure = dotStarEnclosureOf(alternative);
        if (enclosure) {
            ASSERT(direction == Forward);
            --count;
            if (enclosure->anchors.bolAnchor)
                emit(Opcode::AssertBOL, 0, 0, enclosure->multiline() ? LinearInstruction::Multiline : 0);
            if (!emitDotStar(*enclosure))
                return false;
        }

        // A lookbehind matches its terms from the last to the first.
        for (size_t index = 0; index < count; ++index) {
            auto& term = alternative.m_terms[direction == Forward ? index : count - 1 - index];
            if (!compileTerm(term, direction))
                return false;
        }

        if (enclosure) {
            if (!emitDotStar(*enclosure))
                return false;
            if (enclosure->anchors.eolAnchor)
                emit(Opcode::AssertEOL, 0, 0, enclosure->multiline() ? LinearInstruction::Multiline : 0);
        }
        return !failed();
    }

    void emitClear(const Repeat& repeat)
    {
        if (!repeat.clears)
            return;
        emit(Opcode::ClearSlots, repeat.firstClearedGroup * 2, (repeat.lastClearedGroup + 1) * 2);
        if (!m_pattern.hasDuplicateNamedCaptureGroups())
            return;
        for (unsigned group = repeat.firstClearedGroup; group <= repeat.lastClearedGroup; ++group) {
            if (unsigned name = m_pattern.m_duplicateNamedGroupForSubpatternId[group])
                emit(Opcode::StoreSlot, m_pattern.offsetForDuplicateNamedGroupId(name), 0);
        }
    }

    template<typename Functor>
    bool emitRepeated(const Repeat& repeat, const Functor& emitIteration)
    {
        bool cannotConsume = !repeat.iteration.maximum;
        bool canBeEmpty = !repeat.iteration.minimum;

        // Iterations that cannot consume all run at one position and come to one result.
        unsigned mandatory = cannotConsume ? std::min(repeat.minimum, 1u) : repeat.minimum;
        for (unsigned iteration = 0; iteration < mandatory; ++iteration) {
            if (!charge())
                return false;
            emitClear(repeat);
            if (!emitIteration())
                return false;
        }

        // An iteration past the minimum that consumes nothing is not a match of it.
        if (repeat.maximum == repeat.minimum || cannotConsume)
            return !failed();

        auto emitOptionalIteration = [&] {
            if (canBeEmpty)
                emit(Opcode::LoopBegin);
            emitClear(repeat);
            if (!emitIteration())
                return false;
            if (canBeEmpty)
                emit(Opcode::LoopEnd);
            return true;
        };

        if (repeat.maximum == quantifyInfinite) {
            if (!charge())
                return false;
            unsigned split = emit(Opcode::Split);
            unsigned iteration = here();
            if (!emitOptionalIteration())
                return false;
            emit(Opcode::Jump, split);
            setSplitTargets(split, iteration, here(), repeat.greedy);
            return !failed();
        }

        Vector<unsigned, 16> splits;
        for (unsigned iteration = repeat.minimum; iteration < repeat.maximum; ++iteration) {
            if (!charge())
                return false;
            splits.append(emit(Opcode::Split));
            if (!emitOptionalIteration())
                return false;
        }
        unsigned exit = here();
        for (unsigned split : splits)
            setSplitTargets(split, split + 1, exit, repeat.greedy);
        return !failed();
    }

    // `direction` is the direction of the lookaround the term is in. A term does not always
    // carry it (a class in a lookbehind says Forward), so it is not read from the term.
    bool compileTerm(PatternTerm& term, MatchDirection direction)
    {
        if (!charge())
            return false;

        auto repeatOf = [&] {
            Repeat repeat;
            repeat.minimum = term.quantityMinCount.value();
            repeat.maximum = term.quantityMaxCount.value();
            repeat.greedy = term.quantityType != QuantifierType::NonGreedy;
            return repeat;
        };

        switch (term.type) {
        case PatternTerm::Type::AssertionBOL:
            emit(Opcode::AssertBOL, 0, 0, term.multiline() ? LinearInstruction::Multiline : 0);
            return true;

        case PatternTerm::Type::AssertionEOL:
            emit(Opcode::AssertEOL, 0, 0, term.multiline() ? LinearInstruction::Multiline : 0);
            return true;

        case PatternTerm::Type::AssertionWordBoundary: {
            uint8_t flags = 0;
            if (term.invert())
                flags |= LinearInstruction::Invert;
            if (term.ignoreCase() && m_pattern.eitherUnicode())
                flags |= LinearInstruction::UnicodeIgnoreCase;
            emit(Opcode::AssertWordBoundary, 0, 0, flags);
            return true;
        }

        case PatternTerm::Type::PatternCharacter: {
            char32_t character = term.patternCharacter;
            // Under /i the pattern holds a character of its own only when it is an ASCII letter
            // or has no other case; YarrPatternConstructor made a class of every other one.
            bool eitherCase = term.ignoreCase() && isASCIIAlpha(character);
            Repeat repeat = repeatOf();
            repeat.iteration = { 1, 1 };
            return emitRepeated(repeat, [&] {
                if (eitherCase)
                    emit(Opcode::CharacterEither, toASCIILower(character), toASCIIUpper(character));
                else
                    emit(Opcode::Character, character);
                return true;
            });
        }

        case PatternTerm::Type::CharacterClass: {
            unsigned classIndex = classIndexFor(term.characterClass);
            uint8_t flags = term.invert() ? LinearInstruction::Invert : 0;
            Repeat repeat = repeatOf();
            repeat.iteration = { 1, 1 };
            return emitRepeated(repeat, [&] {
                emit(Opcode::CharacterClass, classIndex, 0, flags);
                return true;
            });
        }

        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
            return refuse(LinearRefusal::BackReference);

        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
            // A reference to a group that has not matched yet matches the empty string.
            return true;

        case PatternTerm::Type::ParenthesesSubpattern: {
            PatternDisjunction* disjunction = term.parentheses.disjunction;
            unsigned group = term.parentheses.subpatternId;
            unsigned duplicateName = 0;
            if (term.capture() && m_pattern.hasDuplicateNamedCaptureGroups())
                duplicateName = m_pattern.m_duplicateNamedGroupForSubpatternId[group];

            Repeat repeat = repeatOf();
            repeat.iteration = lengthsOf(disjunction);
            if (failed())
                return false;
            // The one iteration of a plain group starts with its groups clear already. Where
            // X{min,max} was split in two terms that share X's groups, the second term starts
            // with what the first one captured.
            bool isPlain = term.quantityType == QuantifierType::FixedCount && repeat.maximum == 1 && !term.parentheses.isCopy;
            if (term.containsAnyCaptures() && !isPlain) {
                repeat.clears = true;
                repeat.firstClearedGroup = group;
                repeat.lastClearedGroup = term.parentheses.lastSubpatternId;
            }

            return emitRepeated(repeat, [&] {
                // A lookbehind comes to the end of a group first.
                if (term.capture())
                    emit(Opcode::Save, group * 2 + (direction == Forward ? 0 : 1));
                if (!compileDisjunction(disjunction, direction))
                    return false;
                if (term.capture()) {
                    emit(Opcode::Save, group * 2 + (direction == Forward ? 1 : 0));
                    if (duplicateName)
                        emit(Opcode::StoreSlot, m_pattern.offsetForDuplicateNamedGroupId(duplicateName), group);
                }
                return true;
            });
        }

        case PatternTerm::Type::ParentheticalAssertion: {
            auto child = codeForLookaround(term);
            if (!child)
                return false;
            emit(Opcode::Lookaround, *child, 0, term.invert() ? LinearInstruction::Invert : 0);
            return true;
        }

        case PatternTerm::Type::DotStarEnclosure:
            // compileAlternative() compiles the one at the end of the pattern, and
            // optimizeDotStarWrappedExpressions() puts none anywhere else.
            return refuse(LinearRefusal::UnsupportedTerm);
        }

        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }

    // One code per lookaround of the pattern: the copies of a repeated group share it, and with
    // it what the matcher remembers of its last evaluation.
    std::optional<unsigned> codeForLookaround(PatternTerm& term)
    {
        PatternDisjunction* disjunction = term.parentheses.disjunction;
        auto iterator = m_lookaroundCodes.find(disjunction);
        if (iterator != m_lookaroundCodes.end())
            return iterator->value;

        // The matcher runs a lookaround to its end at every position that asserts it, so the
        // match stays linear only when that end is a bounded distance away. How far it can be
        // is for maximumStepsPerPosition() to judge, once the program is complete.
        Lengths lengths = lengthsOf(disjunction);
        if (failed())
            return std::nullopt;
        if (lengths.maximum == unbounded) {
            refuse(LinearRefusal::UnboundedLookaround);
            return std::nullopt;
        }
        if (m_lookaroundDepth >= maximumLookaroundDepth) {
            refuse(LinearRefusal::NestingTooDeep);
            return std::nullopt;
        }

        unsigned parent = m_currentCode;
        unsigned child = m_program->m_codes.size();
        m_program->m_codes.append(LinearCode { });
        m_currentCode = child;
        ++m_lookaroundDepth;

        code().direction = term.matchDirection();
        if (term.containsAnyCaptures()) {
            code().writesSlots = true;
            code().firstSlot = term.parentheses.subpatternId * 2;
            code().endSlot = (term.parentheses.lastSubpatternId + 1) * 2;
            if (m_pattern.hasDuplicateNamedCaptureGroups()) {
                for (unsigned group = term.parentheses.subpatternId; group <= term.parentheses.lastSubpatternId; ++group) {
                    if (m_pattern.m_duplicateNamedGroupForSubpatternId[group])
                        code().writesDuplicateNameSlots = true;
                }
            }
        }

        bool compiled = compileDisjunction(disjunction, term.matchDirection());
        if (compiled)
            emit(Opcode::Match);

        --m_lookaroundDepth;
        m_currentCode = parent;
        if (!compiled || failed())
            return std::nullopt;

        m_lookaroundCodes.add(disjunction, child);
        return child;
    }

    // The most the matcher can hold for this program, whatever the subject is. A position has
    // at most two states per instruction that waits on a character, in two lists, and every
    // state of a code that writes slots has a copy of the slots. addThread() keeps a frame for
    // each instruction it is in the middle of, and one for each slot that instruction wrote. The
    // vectors that hold all this grow by a quarter at a time.
    uint64_t workingMemoryBound() const
    {
        uint64_t bytes = 0;
        for (auto& code : m_program->m_codes) {
            uint64_t slots = code.writesSlots ? m_program->m_slotCount : 0;
            uint64_t waiting = 0;
            uint64_t frames = 0;
            for (auto& instruction : code.instructions) {
                if (instruction.isConsuming())
                    ++waiting;
                uint64_t writes = 0;
                switch (instruction.opcode) {
                case Opcode::Save:
                case Opcode::StoreSlot:
                    writes = 1;
                    break;
                case Opcode::ClearSlots:
                    writes = instruction.b - instruction.a;
                    break;
                case Opcode::Lookaround:
                    writes = m_program->m_codes[instruction.a].writesSlots ? m_program->m_slotCount : 0;
                    break;
                default:
                    break;
                }
                frames += 2 * (1 + writes);
            }
            uint64_t states = waiting * 2 * 2;
            bytes += states * (1 + slots) * sizeof(unsigned);
            bytes += code.instructions.size() * 2 * sizeof(unsigned);
            bytes += frames * bytesPerFrame;
            bytes += 2 * slots * sizeof(unsigned);
        }
        return bytes + bytes / 4;
    }

    // A step of the matcher is a visit of addThread(), or a state run() takes over a
    // character. At one position an instruction is visited at most twice, once per value of
    // "consumed", and an instruction that waits on a character has at most that many states.
    static constexpr uint64_t stepsOfInstruction = 2;
    static constexpr uint64_t stepsOfWaitingInstruction = 4;

    static uint64_t stepsOf(const LinearInstruction& instruction)
    {
        return instruction.isConsuming() ? stepsOfWaitingInstruction : stepsOfInstruction;
    }

    // What a lookaround instruction of one code asks of a lookaround: at how many offsets, and
    // between which.
    struct LookaroundUse {
        uint64_t offsets { 0 };
        unsigned firstOffset { std::numeric_limits<unsigned>::max() };
        unsigned lastOffset { 0 };
    };

    // The most steps one evaluation of a lookaround takes, with the lookarounds inside it.
    // `evaluation` has that number for each of those already.
    //
    // The length of a lookaround is bounded, so its code has no loop and every jump in it goes
    // forward. One pass over the code therefore finds, for each instruction, the fewest and the
    // most characters the matcher can have read when it comes to it. The matcher reads one
    // character per position, so the instruction is visited at that many positions and no more.
    // A lookaround inside is evaluated once per position that asserts it (the matcher keeps the
    // result of the last position), and this one waits for each of those evaluations.
    uint64_t stepsOfOneEvaluation(const LinearCode& code, const Vector<uint64_t>& evaluation, Vector<LookaroundUse>& uses) const
    {
        struct Offsets {
            unsigned first { std::numeric_limits<unsigned>::max() };
            unsigned last { 0 };
        };
        unsigned size = code.instructions.size();
        Vector<Offsets> offsets(size);
        offsets[0] = { 0, 0 };
        Vector<unsigned, 8> lookarounds;

        uint64_t steps = 0;
        for (unsigned index = 0; index < size; ++index) {
            Offsets reached = offsets[index];
            if (reached.first > reached.last)
                continue;

            auto& instruction = code.instructions[index];
            uint64_t positions = reached.last - reached.first + 1;
            steps = saturatedSum(steps, positions * stepsOf(instruction));

            auto flowTo = [&](unsigned target, unsigned consumed) {
                if (target <= index || target >= size)
                    return false;
                offsets[target].first = std::min(offsets[target].first, reached.first + consumed);
                offsets[target].last = std::max(offsets[target].last, reached.last + consumed);
                return true;
            };

            bool goesForward = true;
            switch (instruction.opcode) {
            case Opcode::Match:
                break;
            case Opcode::Character:
            case Opcode::CharacterEither:
            case Opcode::CharacterClass:
                goesForward = flowTo(index + 1, 1);
                break;
            case Opcode::Jump:
                goesForward = flowTo(instruction.a, 0);
                break;
            case Opcode::Split: {
                bool first = flowTo(instruction.a, 0);
                bool second = flowTo(instruction.b, 0);
                goesForward = first && second;
                break;
            }
            case Opcode::Lookaround: {
                auto& use = uses[instruction.a];
                if (!use.offsets)
                    lookarounds.append(instruction.a);
                use.offsets += positions;
                use.firstOffset = std::min(use.firstOffset, reached.first);
                use.lastOffset = std::max(use.lastOffset, reached.last);
                goesForward = flowTo(index + 1, 0);
                break;
            }
            case Opcode::Save:
            case Opcode::ClearSlots:
            case Opcode::StoreSlot:
            case Opcode::LoopBegin:
            case Opcode::LoopEnd:
            case Opcode::AssertBOL:
            case Opcode::AssertEOL:
            case Opcode::AssertWordBoundary:
                goesForward = flowTo(index + 1, 0);
                break;
            }
            ASSERT(goesForward);
            if (!goesForward)
                return unbounded;
        }

        for (unsigned lookaround : lookarounds) {
            auto& use = uses[lookaround];
            uint64_t evaluations = std::min<uint64_t>(use.offsets, use.lastOffset - use.firstOffset + 1);
            steps = saturatedSum(steps, saturatedProduct(evaluations, evaluation[lookaround]));
        }
        return steps;
    }

    // The most steps a position of the subject costs a match: the pattern's own instructions,
    // and one evaluation of each lookaround the pattern asserts. What the lookarounds inside
    // one another cost is the product of what each can read, which is why the compiler has a
    // limit for it.
    uint64_t maximumStepsPerPosition() const
    {
        auto& codes = m_program->m_codes;
        // A code is appended before the codes of the lookarounds inside it.
        Vector<uint64_t> evaluation(FillWith { }, codes.size(), static_cast<uint64_t>(0));
        Vector<LookaroundUse> uses(codes.size());
        for (unsigned index = codes.size() - 1; index; --index)
            evaluation[index] = stepsOfOneEvaluation(codes[index], evaluation, uses);

        uint64_t steps = 0;
        for (auto& instruction : codes[0].instructions) {
            steps = saturatedSum(steps, stepsOf(instruction));
            if (instruction.opcode != Opcode::Lookaround)
                continue;
            auto& use = uses[instruction.a];
            if (use.offsets)
                continue;
            use.offsets = 1;
            steps = saturatedSum(steps, evaluation[instruction.a]);
        }
        return steps;
    }

    // The characters a match can begin with, so that the matcher can pass over the positions
    // of a subject where none can. An assertion is taken to hold: the set may only be too large.
    void computeFirstCharacters()
    {
        auto& program = *m_program;
        auto& instructions = program.m_codes[0].instructions;

        WTF::BitSet<256> first;
        bool canBeWide = false;
        bool canBeEmpty = false;

        Vector<bool> visited(FillWith { }, instructions.size(), false);
        Vector<unsigned, 16> worklist;
        worklist.append(0);
        while (!worklist.isEmpty()) {
            unsigned index = worklist.takeLast();
            if (visited[index])
                continue;
            visited[index] = true;

            auto& instruction = instructions[index];
            auto addCharacter = [&](char32_t character) {
                if (character < first.size())
                    first.set(character);
                else
                    canBeWide = true;
            };
            switch (instruction.opcode) {
            case Opcode::Character:
                addCharacter(instruction.a);
                break;
            case Opcode::CharacterEither:
                addCharacter(instruction.a);
                addCharacter(instruction.b);
                break;
            case Opcode::CharacterClass: {
                auto& characterClass = program.m_classes[instruction.a];
                if (instruction.invert()) {
                    auto inverted = characterClass.latin1;
                    inverted.invert();
                    first.merge(inverted);
                    canBeWide = true;
                } else {
                    first.merge(characterClass.latin1);
                    if (!characterClass.ranges.isEmpty())
                        canBeWide = true;
                }
                break;
            }
            case Opcode::Match:
                canBeEmpty = true;
                break;
            case Opcode::Jump:
                worklist.append(instruction.a);
                break;
            case Opcode::Split:
                worklist.append(instruction.a);
                worklist.append(instruction.b);
                break;
            case Opcode::Save:
            case Opcode::ClearSlots:
            case Opcode::StoreSlot:
            case Opcode::LoopBegin:
            case Opcode::LoopEnd:
            case Opcode::AssertBOL:
            case Opcode::AssertEOL:
            case Opcode::AssertWordBoundary:
            case Opcode::Lookaround:
                worklist.append(index + 1);
                break;
            }
        }

        if (canBeEmpty) {
            first.setAll();
            canBeWide = true;
        }
        program.m_firstCharacters = first;
        program.m_firstCharacterCanBeWide = canBeWide;
    }

    YarrPattern& m_pattern;
    std::unique_ptr<LinearProgram> m_program;
    LinearRefusal m_refusal { LinearRefusal::None };
    StackCheck m_stackCheck;
    unsigned m_currentCode { 0 };
    unsigned m_lookaroundDepth { 0 };
    unsigned m_maximumProgramSize;
    unsigned m_instructionCount { 0 };
    uint64_t m_remainingWork { 0 };
    UncheckedKeyHashMap<PatternDisjunction*, Lengths> m_lengths;
    UncheckedKeyHashMap<PatternDisjunction*, unsigned> m_lookaroundCodes;
    UncheckedKeyHashMap<CharacterClass*, unsigned> m_classIndices;
};

std::unique_ptr<LinearProgram> compileLinear(YarrPattern& pattern, LinearRefusal& refusal)
{
    return LinearCompiler(pattern).compile(refusal);
}

// ---------------------------------------------------------------------------------------------
// Matcher
// ---------------------------------------------------------------------------------------------

template<typename CharType>
class LinearMatcher {
    WTF_MAKE_NONCOPYABLE(LinearMatcher);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    LinearMatcher(const LinearProgram& program, std::span<const CharType> input)
        : m_program(program)
        , m_input(input.data())
        , m_length(input.size())
        , m_decodeSurrogatePairs(program.m_decodeSurrogatePairs && sizeof(CharType) > 1)
    {
        m_lookaroundScratch.resize(program.m_codes.size());
    }

    unsigned match(unsigned start, unsigned* output)
    {
        if (start > m_length)
            return offsetNoMatch;

        bool search = !m_program.m_sticky && !m_program.m_anchoredAtStart;
        if (!run(0, start, search, output)) {
            clearSlots(output);
            return offsetNoMatch;
        }
        return output[0];
    }

    uint64_t steps() const { return m_steps; }

    size_t scratchBytes() const
    {
        size_t bytes = m_scratch.bytes();
        for (auto& scratch : m_lookaroundScratch) {
            if (scratch)
                bytes += sizeof(Scratch) + scratch->bytes();
        }
        return bytes;
    }

private:
    static constexpr char32_t errorCodePoint = 0xFFFFFFFFu;
    static constexpr unsigned notEvaluated = std::numeric_limits<unsigned>::max();

    // The states of one position, in the order backtracking would try them. A state is the
    // instruction it waits on and, in a code that writes slots, its copy of the slots.
    struct ThreadList {
        bool isEmpty() const { return programCounters.isEmpty(); }
        unsigned size() const { return programCounters.size(); }

        void clear()
        {
            programCounters.shrink(0);
            slots.shrink(0);
        }

        void append(unsigned programCounter, std::span<const unsigned> threadSlots)
        {
            programCounters.append(programCounter);
            slots.append(threadSlots);
        }

        std::span<const unsigned> slotsOf(unsigned index, unsigned slotCount) const
        {
            return slots.span().subspan(index * slotCount, slotCount);
        }

        size_t bytes() const { return (programCounters.capacity() + slots.capacity()) * sizeof(unsigned); }

        Vector<unsigned, 16> programCounters;
        Vector<unsigned, 64> slots;
    };

    // What addThread() still has to do once the path it follows ends.
    struct Frame {
        enum class Kind : uint8_t { Explore, RestoreSlot, RestoreConsumed };
        Kind kind;
        unsigned a { 0 };
        unsigned b { 0 };
    };
    static_assert(sizeof(Frame) == 3 * sizeof(unsigned), "LinearCompiler::workingMemoryBound() counts a frame as this much");

    struct Scratch {
        WTF_MAKE_NONCOPYABLE(Scratch);
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Scratch);
    public:
        Scratch() = default;

        size_t bytes() const
        {
            return current.bytes() + next.bytes()
                + visited.capacity() * sizeof(unsigned)
                + stack.capacity() * sizeof(Frame)
                + (working.capacity() + result.capacity()) * sizeof(unsigned);
        }

        ThreadList current;
        ThreadList next;
        // Per instruction and per value of "consumed": the generation that entered it last.
        Vector<unsigned, 128> visited;
        Vector<Frame, 32> stack;
        // The slots of the state addThread() is following.
        Vector<unsigned, 32> working;
        // A lookaround: the slots of its last match, and the position it was evaluated at.
        Vector<unsigned, 32> result;
        unsigned generation { 0 };
        unsigned memoPosition { notEvaluated };
        bool memoMatched { false };
        bool isPrepared { false };
        bool isRunning { false };
    };

    Scratch& scratchFor(unsigned codeIndex)
    {
        if (!codeIndex)
            return m_scratch;
        auto& scratch = m_lookaroundScratch[codeIndex];
        if (!scratch)
            scratch = makeUnique<Scratch>();
        return *scratch;
    }

    void clearSlots(unsigned* slots) const
    {
        for (unsigned slot = 0; slot < m_program.m_captureSlotCount; ++slot)
            slots[slot] = offsetNoMatch;
        for (unsigned slot = m_program.m_captureSlotCount; slot < m_program.m_slotCount; ++slot)
            slots[slot] = 0;
    }

    void prepare(Scratch& scratch, const LinearCode& code)
    {
        if (scratch.isPrepared)
            return;
        scratch.isPrepared = true;
        scratch.visited.fill(0, code.instructions.size() * 2);
        if (code.writesSlots) {
            scratch.working.resize(m_program.m_slotCount);
            if (&scratch != &m_scratch)
                scratch.result.resize(m_program.m_slotCount);
        }
    }

    // Every position gets a generation, and a state is in the set of a position when its
    // visited entry holds the generation.
    void beginPosition(Scratch& scratch)
    {
        if (!++scratch.generation) [[unlikely]] {
            scratch.visited.fill(0);
            scratch.generation = 1;
        }
    }

    ALWAYS_INLINE char32_t readForward(unsigned position, unsigned& width) const
    {
        ASSERT(position < m_length);
        width = 1;
        char32_t character = m_input[position];
        if constexpr (sizeof(CharType) > 1) {
            if (m_decodeSurrogatePairs) {
                if (U16_IS_LEAD(character) && position + 1 < m_length && U16_IS_TRAIL(m_input[position + 1])) {
                    width = 2;
                    return U16_GET_SUPPLEMENTARY(character, m_input[position + 1]);
                }
                // The second half of a pair is not a character: nothing matches it.
                if (U16_IS_TRAIL(character) && position && U16_IS_LEAD(m_input[position - 1]))
                    return errorCodePoint;
            }
        }
        return character;
    }

    ALWAYS_INLINE char32_t readBackward(unsigned position, unsigned& width) const
    {
        ASSERT(position && position <= m_length);
        width = 1;
        char32_t character = m_input[position - 1];
        if constexpr (sizeof(CharType) > 1) {
            if (m_decodeSurrogatePairs && U16_IS_TRAIL(character) && position > 1 && U16_IS_LEAD(m_input[position - 2])) {
                width = 2;
                return U16_GET_SUPPLEMENTARY(m_input[position - 2], character);
            }
        }
        return character;
    }

    static bool isNewline(char32_t character)
    {
        return character == '\n' || character == '\r' || character == 0x2028 || character == 0x2029;
    }

    static bool isWordCharacter(char32_t character, bool unicodeIgnoreCase)
    {
        if (isASCIIAlphanumeric(character) || character == '_')
            return true;
        // U+017F and U+212A fold to s and k.
        return unicodeIgnoreCase && (character == 0x017F || character == 0x212A);
    }

    bool canBeginMatch(unsigned position) const
    {
        if (position == m_length)
            return m_program.m_firstCharacterCanBeWide && m_program.m_firstCharacters.isFull();
        char32_t character = m_input[position];
        if (character < m_program.m_firstCharacters.size())
            return m_program.m_firstCharacters.get(character);
        return m_program.m_firstCharacterCanBeWide;
    }

    // The next position after `position` a match can begin at, or the end of the subject.
    unsigned nextCandidate(unsigned position)
    {
        ASSERT(position < m_length);
        unsigned width;
        readForward(position, width);
        position += width;
        while (position < m_length && !canBeginMatch(position)) {
            ++m_steps;
            ++position;
        }
        return position;
    }

    bool evaluateLookaround(unsigned codeIndex, unsigned position)
    {
        Scratch& scratch = scratchFor(codeIndex);
        if (scratch.memoPosition != position) {
            prepare(scratch, m_program.m_codes[codeIndex]);
            scratch.memoMatched = run(codeIndex, position, false, scratch.result.mutableSpan().data());
            scratch.memoPosition = position;
        }
        return scratch.memoMatched;
    }

    // Adds the state that waits at `programCounter`, with the slots in scratch.working, and
    // every state it leads to without consuming a character, to `list`. The ones backtracking
    // tries first are added first.
    void addThread(const LinearCode& code, Scratch& scratch, ThreadList& list, unsigned programCounter, unsigned position, bool consumed)
    {
        const LinearInstruction* instructions = code.instructions.span().data();
        unsigned* visited = scratch.visited.mutableSpan().data();
        unsigned* working = scratch.working.mutableSpan().data();
        unsigned generation = scratch.generation;
        auto& stack = scratch.stack;
        ASSERT(stack.isEmpty());

        auto setSlot = [&](unsigned slot, unsigned value) {
            if (working[slot] == value)
                return;
            stack.append(Frame { Frame::Kind::RestoreSlot, slot, working[slot] });
            working[slot] = value;
        };

        for (;;) {
            for (;;) {
                unsigned key = (programCounter << 1) | static_cast<unsigned>(consumed);
                if (visited[key] == generation)
                    break;
                visited[key] = generation;
                ++m_steps;

                const LinearInstruction& instruction = instructions[programCounter];
                bool alive = true;
                switch (instruction.opcode) {
                case Opcode::Character:
                case Opcode::CharacterEither:
                case Opcode::CharacterClass:
                case Opcode::Match:
                    list.append(programCounter, scratch.working.span());
                    alive = false;
                    break;

                case Opcode::Jump:
                    programCounter = instruction.a;
                    break;

                case Opcode::Split:
                    stack.append(Frame { Frame::Kind::Explore, instruction.b, 0 });
                    programCounter = instruction.a;
                    break;

                case Opcode::Save:
                    setSlot(instruction.a, position);
                    ++programCounter;
                    break;

                case Opcode::ClearSlots:
                    for (unsigned slot = instruction.a; slot < instruction.b; ++slot)
                        setSlot(slot, offsetNoMatch);
                    ++programCounter;
                    break;

                case Opcode::StoreSlot:
                    setSlot(instruction.a, instruction.b);
                    ++programCounter;
                    break;

                case Opcode::LoopBegin:
                    if (consumed) {
                        stack.append(Frame { Frame::Kind::RestoreConsumed, 0, 0 });
                        consumed = false;
                    }
                    ++programCounter;
                    break;

                case Opcode::LoopEnd:
                    alive = consumed;
                    ++programCounter;
                    break;

                case Opcode::AssertBOL:
                    alive = !position || (instruction.multiline() && isNewline(m_input[position - 1]));
                    ++programCounter;
                    break;

                case Opcode::AssertEOL:
                    alive = position == m_length || (instruction.multiline() && isNewline(m_input[position]));
                    ++programCounter;
                    break;

                case Opcode::AssertWordBoundary: {
                    bool before = position && isWordCharacter(m_input[position - 1], instruction.unicodeIgnoreCase());
                    bool after = position < m_length && isWordCharacter(m_input[position], instruction.unicodeIgnoreCase());
                    alive = (before != after) != instruction.invert();
                    ++programCounter;
                    break;
                }

                case Opcode::Lookaround: {
                    bool matched = evaluateLookaround(instruction.a, position);
                    alive = matched != instruction.invert();
                    if (alive && matched) {
                        // What the groups of a lookahead or lookbehind captured stays captured.
                        const LinearCode& child = m_program.m_codes[instruction.a];
                        if (child.writesSlots) {
                            const auto& result = scratchFor(instruction.a).result;
                            for (unsigned slot = child.firstSlot; slot < child.endSlot; ++slot)
                                setSlot(slot, result[slot]);
                            if (child.writesDuplicateNameSlots) {
                                for (unsigned slot = m_program.m_captureSlotCount; slot < m_program.m_slotCount; ++slot) {
                                    if (result[slot])
                                        setSlot(slot, result[slot]);
                                }
                            }
                        }
                    }
                    ++programCounter;
                    break;
                }
                }

                if (!alive)
                    break;
            }

            for (;;) {
                if (stack.isEmpty())
                    return;
                Frame frame = stack.takeLast();
                if (frame.kind == Frame::Kind::RestoreSlot) {
                    working[frame.a] = frame.b;
                    continue;
                }
                if (frame.kind == Frame::Kind::RestoreConsumed) {
                    consumed = true;
                    continue;
                }
                programCounter = frame.a;
                break;
            }
        }
    }

    void addInitialThread(const LinearCode& code, Scratch& scratch, ThreadList& list, unsigned position)
    {
        if (code.writesSlots)
            clearSlots(scratch.working.mutableSpan().data());
        addThread(code, scratch, list, 0, position, false);
    }

    // Matches `code` from `start`. With `search`, a match can begin at `start` or after it, and
    // the leftmost one is found. The slots of the match are written to `result`.
    bool run(unsigned codeIndex, unsigned start, bool search, unsigned* result)
    {
        const LinearCode& code = m_program.m_codes[codeIndex];
        Scratch& scratch = scratchFor(codeIndex);
        prepare(scratch, code);
        RELEASE_ASSERT(!scratch.isRunning);
        scratch.isRunning = true;

        const LinearInstruction* instructions = code.instructions.span().data();
        bool backward = code.direction == Backward;
        unsigned slotCount = code.writesSlots ? m_program.m_slotCount : 0;

        ThreadList* current = &scratch.current;
        ThreadList* next = &scratch.next;
        current->clear();
        next->clear();

        bool matched = false;
        unsigned position = start;

        beginPosition(scratch);
        addInitialThread(code, scratch, *current, position);

        for (;;) {
            bool atEnd = backward ? !position : position == m_length;

            if (current->isEmpty()) {
                if (matched || !search || atEnd)
                    break;
                position = nextCandidate(position);
                beginPosition(scratch);
                addInitialThread(code, scratch, *current, position);
                continue;
            }

            char32_t character = errorCodePoint;
            unsigned width = 1;
            if (!atEnd)
                character = backward ? readBackward(position, width) : readForward(position, width);
            unsigned nextPosition = backward ? position - width : position + width;

            beginPosition(scratch);
            unsigned count = current->size();
            for (unsigned index = 0; index < count; ++index) {
                ++m_steps;
                unsigned programCounter = current->programCounters[index];
                const LinearInstruction& instruction = instructions[programCounter];
                bool advances = false;
                switch (instruction.opcode) {
                case Opcode::Character:
                    advances = character == instruction.a;
                    break;
                case Opcode::CharacterEither:
                    advances = character == instruction.a || character == instruction.b;
                    break;
                case Opcode::CharacterClass:
                    advances = character != errorCodePoint && m_program.m_classes[instruction.a].contains(character) != instruction.invert();
                    break;
                case Opcode::Match:
                    if (slotCount)
                        memcpySpan(std::span { result, slotCount }, current->slotsOf(index, slotCount));
                    matched = true;
                    // Backtracking stops here: it would not have tried the states that follow.
                    index = count;
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                }
                if (!advances)
                    continue;
                if (slotCount)
                    memcpySpan(scratch.working.mutableSpan(), current->slotsOf(index, slotCount));
                addThread(code, scratch, *next, programCounter + 1, nextPosition, true);
            }

            // Where nothing is captured, any match is the answer.
            if (atEnd || (matched && !slotCount))
                break;

            position = nextPosition;
            if (search && !matched && canBeginMatch(position))
                addInitialThread(code, scratch, *next, position);
            std::swap(current, next);
            next->clear();
        }

        scratch.isRunning = false;
        return matched;
    }

    const LinearProgram& m_program;
    const CharType* m_input;
    unsigned m_length;
    bool m_decodeSurrogatePairs;
    uint64_t m_steps { 0 };
    Scratch m_scratch;
    Vector<std::unique_ptr<Scratch>, 4> m_lookaroundScratch;
};

unsigned LinearProgram::match(StringView input, unsigned start, unsigned* output, Statistics* statistics) const
{
    auto matchWith = [&](auto characters) {
        LinearMatcher<typename decltype(characters)::value_type> matcher(*this, characters);
        unsigned result = matcher.match(start, output);
        if (statistics) {
            statistics->steps = matcher.steps();
            statistics->scratchBytes = matcher.scratchBytes();
        }
        return result;
    };
    if (input.is8Bit())
        return matchWith(input.span8());
    return matchWith(input.span16());
}

size_t LinearProgram::instructionCount() const
{
    size_t count = 0;
    for (auto& code : m_codes)
        count += code.instructions.size();
    return count;
}

size_t LinearProgram::estimatedSizeInBytes() const
{
    size_t bytes = sizeof(LinearProgram) + m_codes.capacity() * sizeof(LinearCode) + m_classes.capacity() * sizeof(LinearCharacterClass);
    for (auto& code : m_codes)
        bytes += code.instructions.capacity() * sizeof(LinearInstruction);
    for (auto& characterClass : m_classes)
        bytes += characterClass.ranges.capacity() * sizeof(CharacterRange);
    return bytes;
}

void LinearProgram::dump(PrintStream& out) const
{
    out.println("Linear program: ", instructionCount(), " instructions, ", m_slotCount, " slots, at most ", m_maximumStepsPerPosition, " steps per position",
        m_sticky ? ", sticky" : "", m_anchoredAtStart ? ", anchored at start" : "", m_decodeSurrogatePairs ? ", decodes surrogate pairs" : "");
    for (unsigned codeIndex = 0; codeIndex < m_codes.size(); ++codeIndex) {
        auto& code = m_codes[codeIndex];
        out.println("  code ", codeIndex, code.direction == Backward ? " (backward)" : "", ":");
        for (unsigned index = 0; index < code.instructions.size(); ++index) {
            auto& instruction = code.instructions[index];
            out.print("    ", index, ": ");
            switch (instruction.opcode) {
            case Opcode::Character:
                out.print("Character ");
                dumpChar32(out, instruction.a);
                break;
            case Opcode::CharacterEither:
                out.print("CharacterEither ");
                dumpChar32(out, instruction.a);
                out.print(" ");
                dumpChar32(out, instruction.b);
                break;
            case Opcode::CharacterClass:
                out.print("CharacterClass #", instruction.a, instruction.invert() ? " inverted" : "");
                break;
            case Opcode::Match:
                out.print("Match");
                break;
            case Opcode::Jump:
                out.print("Jump ", instruction.a);
                break;
            case Opcode::Split:
                out.print("Split ", instruction.a, ", ", instruction.b);
                break;
            case Opcode::Save:
                out.print("Save ", instruction.a);
                break;
            case Opcode::ClearSlots:
                out.print("ClearSlots [", instruction.a, ", ", instruction.b, ")");
                break;
            case Opcode::StoreSlot:
                out.print("StoreSlot ", instruction.a, " = ", instruction.b);
                break;
            case Opcode::LoopBegin:
                out.print("LoopBegin");
                break;
            case Opcode::LoopEnd:
                out.print("LoopEnd");
                break;
            case Opcode::AssertBOL:
                out.print("AssertBOL", instruction.multiline() ? " multiline" : "");
                break;
            case Opcode::AssertEOL:
                out.print("AssertEOL", instruction.multiline() ? " multiline" : "");
                break;
            case Opcode::AssertWordBoundary:
                out.print("AssertWordBoundary", instruction.invert() ? " inverted" : "", instruction.unicodeIgnoreCase() ? " unicode-ignore-case" : "");
                break;
            case Opcode::Lookaround:
                out.print("Lookaround code ", instruction.a, instruction.invert() ? " inverted" : "");
                break;
            }
            out.println();
        }
    }
}

} } // namespace JSC::Yarr

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS)
