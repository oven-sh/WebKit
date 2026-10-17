/*
 * Copyright (C) 2009-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Peter Varga (pvarga@inf.u-szeged.hu), University of Szeged
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
#include "YarrInterpreter.h"

#include "ConcurrentJSLock.h"
#include "Options.h"
#include "SuperSampler.h"
#include "Yarr.h"
#include "YarrCanonicalize.h"
#include <numeric>
#include <wtf/BitVector.h>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/DataLog.h>
#include <wtf/StackCheck.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BytecodePattern);
WTF_MAKE_TZONE_ALLOCATED_IMPL(FastProgram);
WTF_MAKE_TZONE_ALLOCATED_IMPL(ByteDisjunction);

class ByteTermDumper {
public:
    ByteTermDumper(YarrPattern* pattern = nullptr)
        : m_pattern(pattern)
    {
        if (pattern)
            m_compileMode = pattern->compileMode();
    }

    ByteTermDumper(CompileMode compileMode)
        : m_compileMode(compileMode)
    {
    }

    void dumpTerm(size_t idx, ByteTerm);
    void dumpDisjunction(ByteDisjunction*, unsigned nesting = 0);
    bool unicode() const { return m_compileMode == CompileMode::Unicode; }
    bool unicodeSets() const { return m_compileMode == CompileMode::UnicodeSets; }
    bool eitherUnicode() const { return unicode() || unicodeSets(); }

private:
    YarrPattern* m_pattern { nullptr };
    unsigned m_nesting { 0 };
    unsigned m_lineIndent { 0 };
    CompileMode m_compileMode { CompileMode::Legacy };
    bool m_recursiveDump { false };
};

template<typename CharType>
class Interpreter {
public:
    static constexpr bool verbose = false;
    static constexpr char32_t errorCodePoint = 0xFFFFFFFFu;

    struct ParenthesesDisjunctionContext;

    struct BackTrackInfoParentheses {
        uintptr_t begin;
        uintptr_t matchAmount;
        ParenthesesDisjunctionContext* lastContext;
    };

    static inline void LEGACY_NODELETE appendParenthesesDisjunctionContext(BackTrackInfoParentheses* backTrack, ParenthesesDisjunctionContext* context)
    {
        context->next = backTrack->lastContext;
        backTrack->lastContext = context;
        ++backTrack->matchAmount;
    }

    static inline void LEGACY_NODELETE popParenthesesDisjunctionContext(BackTrackInfoParentheses* backTrack)
    {
        RELEASE_ASSERT(backTrack->matchAmount);
        RELEASE_ASSERT(backTrack->lastContext);
        backTrack->lastContext = backTrack->lastContext->next;
        --backTrack->matchAmount;
    }

    struct DisjunctionContext
    {
        DisjunctionContext() = default;

        void* LEGACY_NODELETE operator new(size_t, void* where)
        {
            return where;
        }

        static size_t allocationSize(unsigned numberOfFrames)
        {
            constexpr size_t alignment = std::max(alignof(DisjunctionContext), sizeof(uintptr_t));
            size_t rawSize = sizeof(DisjunctionContext) - sizeof(uintptr_t) + Checked<size_t>(numberOfFrames) * sizeof(uintptr_t);
            size_t roundedSize = roundUpToMultipleOf<alignment>(rawSize);
            RELEASE_ASSERT(roundedSize >= rawSize);
            return roundedSize;
        }

        int term { 0 };
        unsigned matchBegin;
        unsigned matchEnd;
#if ASSERT_ENABLED
        constexpr static uint64_t magicNumber = 0x01aabbccddeeff01;
        uint64_t m_magicNumber { magicNumber };
#endif
        uintptr_t frame[1];
    };

    DisjunctionContext* allocDisjunctionContext(ByteDisjunction* disjunction)
    {
        size_t size = DisjunctionContext::allocationSize(disjunction->m_frameSize);
        auto* newAllocatorPool = allocatorPool->ensureCapacity(size);
        if (!newAllocatorPool) [[unlikely]]
            return nullptr;
        allocatorPool = newAllocatorPool;
        return new (allocatorPool->alloc(size)) DisjunctionContext();
    }

    void LEGACY_NODELETE freeDisjunctionContext(DisjunctionContext* context)
    {
#if ASSERT_ENABLED
        ASSERT(context->m_magicNumber == DisjunctionContext::magicNumber);
        context->m_magicNumber = 0;
#endif
        static_assert(std::is_trivially_destructible_v<DisjunctionContext>);
        allocatorPool = allocatorPool->dealloc(context);
    }

    struct ParenthesesDisjunctionContext
    {
        ParenthesesDisjunctionContext(BytecodePattern* pattern, unsigned* output, ByteTerm& term, unsigned numDuplicateNamedGroups, BitVector& duplicateNamedGroups)
            : m_pattern(pattern)
            , m_numNestedSubpatterns(term.atom.parenthesesDisjunction->m_numSubpatterns)
            , m_duplicateNamedGroups(duplicateNamedGroups)

        {
            m_numBackupIds = m_numNestedSubpatterns * 2 + numDuplicateNamedGroups;
            unsigned firstSubpatternId = term.subpatternId();

            for (unsigned i = 0; i < (m_numNestedSubpatterns << 1); ++i) {
                subpatternAndGroupIdBackup[i] = output[(firstSubpatternId << 1) + i];
                output[(firstSubpatternId << 1) + i] = offsetNoMatch;
            }

            unsigned nameGroupIdx = 0;
            for (unsigned duplicateNamedGroupId : m_duplicateNamedGroups) {
                subpatternAndGroupIdBackup[backupOffsetForDuplicateNamedGroup(nameGroupIdx)] = output[m_pattern->offsetForDuplicateNamedGroupId(duplicateNamedGroupId)];
                output[pattern->offsetForDuplicateNamedGroupId(duplicateNamedGroupId)] = 0;
                ++nameGroupIdx;
            }

            new (getDisjunctionContext()) DisjunctionContext();
        }

        void* LEGACY_NODELETE operator new(size_t, void* where)
        {
            return where;
        }

        void restoreOutput(unsigned* output, unsigned firstSubpatternId)
        {
            for (unsigned i = 0; i < (m_numNestedSubpatterns << 1); ++i)
                output[(firstSubpatternId << 1) + i] = subpatternAndGroupIdBackup[i];

            unsigned nameGroupIdx = 0;
            for (unsigned duplicateNamedGroupId : m_duplicateNamedGroups) {
                output[m_pattern->offsetForDuplicateNamedGroupId(duplicateNamedGroupId)] = subpatternAndGroupIdBackup[backupOffsetForDuplicateNamedGroup(nameGroupIdx)];
                ++nameGroupIdx;
            }
        }

        DisjunctionContext* getDisjunctionContext()
        {
            return std::bit_cast<DisjunctionContext*>(std::bit_cast<uintptr_t>(this) + allocationSize(m_numBackupIds));
        }

        unsigned LEGACY_NODELETE backupOffsetForDuplicateNamedGroup(unsigned duplicateNamedGroup)
        {
            unsigned offset = (m_numNestedSubpatterns << 1) + duplicateNamedGroup;
            ASSERT(offset < m_numBackupIds);
            return offset;
        }

        static size_t allocationSize(unsigned numberOfSubpatterns, unsigned numDuplicateNamedGroups)
        {
            Checked<size_t> numBackupIds = (Checked<size_t>(numberOfSubpatterns) * 2U) + Checked<size_t>(numDuplicateNamedGroups);
            return allocationSize(numBackupIds.value());
        }

        static size_t allocationSize(unsigned numBackupIds)
        {
            constexpr size_t alignment = std::max(alignof(ParenthesesDisjunctionContext), sizeof(unsigned));
            size_t rawSize = sizeof(ParenthesesDisjunctionContext) + Checked<size_t>(numBackupIds) * sizeof(unsigned);
            size_t roundedSize = roundUpToMultipleOf<alignment>(rawSize);
            RELEASE_ASSERT(roundedSize >= rawSize);
            return roundedSize;
        }

        ParenthesesDisjunctionContext* next { nullptr };
        BytecodePattern* m_pattern;
        unsigned m_numNestedSubpatterns;
        size_t m_numBackupIds;
        BitVector m_duplicateNamedGroups;
#if ASSERT_ENABLED
        constexpr static uint64_t magicNumber = 0x02aabbccddeeff02;
        uint64_t m_magicNumber { magicNumber };
#endif
        unsigned subpatternAndGroupIdBackup[1];
    };

    ParenthesesDisjunctionContext* allocParenthesesDisjunctionContext(ByteDisjunction* disjunction, unsigned* output, ByteTerm& term)
    {
        BitVector duplicateNamedCaptureGroups;
        unsigned firstSubpatternId = term.subpatternId();
        unsigned numNestedSubpatterns = term.atom.parenthesesDisjunction->m_numSubpatterns;
        unsigned numDuplicateNamedGroups = 0;

        if (pattern->hasDuplicateNamedCaptureGroups()) {
            for (unsigned i = 0; i < numNestedSubpatterns; ++i) {
                unsigned subpatternId = firstSubpatternId + i;
                unsigned duplicateNamedGroup = pattern->m_duplicateNamedGroupForSubpatternId[subpatternId];
                if (duplicateNamedGroup)
                    duplicateNamedCaptureGroups.set(duplicateNamedGroup);
            }

            numDuplicateNamedGroups = duplicateNamedCaptureGroups.bitCount();
        }

        size_t size = Checked<size_t>(ParenthesesDisjunctionContext::allocationSize(numNestedSubpatterns, numDuplicateNamedGroups)) + DisjunctionContext::allocationSize(disjunction->m_frameSize);
        auto* newAllocatorPool = allocatorPool->ensureCapacity(size);
        if (!newAllocatorPool) [[unlikely]]
            return nullptr;
        allocatorPool = newAllocatorPool;
        return new (allocatorPool->alloc(size)) ParenthesesDisjunctionContext(pattern, output, term, numDuplicateNamedGroups, duplicateNamedCaptureGroups);
    }

    void freeParenthesesDisjunctionContext(ParenthesesDisjunctionContext* context)
    {
#if ASSERT_ENABLED
        ASSERT(context->m_magicNumber == ParenthesesDisjunctionContext::magicNumber);
        context->m_magicNumber = 0;
#endif
        context->~ParenthesesDisjunctionContext();
        allocatorPool = allocatorPool->dealloc(context);
    }

    class InputStream {
    public:
        InputStream(std::span<const CharType> input, unsigned start, bool decodeSurrogatePairs)
            : input(input.data())
            , pos(start)
            , length(input.size())
            , decodeSurrogatePairs(decodeSurrogatePairs)
        {
        }

        void LEGACY_NODELETE next()
        {
            ++pos;
        }

        void LEGACY_NODELETE rewind(unsigned amount)
        {
            ASSERT(pos >= amount);
            pos -= amount;
        }

        char32_t LEGACY_NODELETE read()
        {
            ASSERT(pos < length);
            if (pos < length)
                return input[pos];
            return errorCodePoint;
        }

        char32_t LEGACY_NODELETE readChecked(unsigned negativePositionOffest)
        {
            RELEASE_ASSERT(pos >= negativePositionOffest);
            unsigned p = pos - negativePositionOffest;
            ASSERT(p < length);
            auto result = input[p];
            if (U16_IS_LEAD(result) && decodeSurrogatePairs && p + 1 < length && U16_IS_TRAIL(input[p + 1])) {
                if (atEnd())
                    return errorCodePoint;
                next();
                return U16_GET_SUPPLEMENTARY(result, input[p + 1]);
            } else if (decodeSurrogatePairs && p > 0 && U16_IS_TRAIL(result) && U16_IS_LEAD(input[p - 1]))
                return errorCodePoint;
            return result;
        }

        char32_t LEGACY_NODELETE readCheckedDontAdvance(unsigned negativePositionOffest)
        {
            RELEASE_ASSERT(pos >= negativePositionOffest);
            unsigned p = pos - negativePositionOffest;
            ASSERT(p < length);
            auto result = input[p];
            if (U16_IS_LEAD(result) && decodeSurrogatePairs && p + 1 < length && U16_IS_TRAIL(input[p + 1]))
                return U16_GET_SUPPLEMENTARY(result, input[p + 1]);
            if (U16_IS_TRAIL(result) && decodeSurrogatePairs && p > 0 && U16_IS_LEAD(input[p - 1]))
                return errorCodePoint;
            return result;
        }

        // readForCharacterDump() is only for use by the DUMP_CURR_CHAR macro.
        // We don't want any side effects like the next() in readChecked() above.
        char32_t LEGACY_NODELETE readForCharacterDump(unsigned negativePositionOffest)
        {
            RELEASE_ASSERT(pos >= negativePositionOffest);
            unsigned p = pos - negativePositionOffest;
            ASSERT(p < length);
            auto result = input[p];
            if (U16_IS_LEAD(result) && decodeSurrogatePairs && p + 1 < length && U16_IS_TRAIL(input[p + 1])) {
                if (atEnd())
                    return errorCodePoint;
                return U16_GET_SUPPLEMENTARY(result, input[p + 1]);
            }
            return result;
        }
        
        char32_t LEGACY_NODELETE tryReadBackward(unsigned negativePositionOffest)
        {
            if (pos < negativePositionOffest)
                return errorCodePoint;
            unsigned p = pos - negativePositionOffest;
            ASSERT(p < length);
            auto result = input[p];
            if (U16_IS_TRAIL(result) && decodeSurrogatePairs && p > 0 && U16_IS_LEAD(input[p - 1])) {
                rewind(1);
                return U16_GET_SUPPLEMENTARY(input[p - 1], result);
            }
            return result;
        }

        char32_t LEGACY_NODELETE readSurrogatePairChecked(unsigned negativePositionOffset)
        {
            RELEASE_ASSERT(pos >= negativePositionOffset);
            unsigned p = pos - negativePositionOffset;
            ASSERT(p < length);
            if (p + 1 >= length)
                return errorCodePoint;
            auto first = input[p];
            auto second = input[p + 1];
            if (U16_IS_LEAD(first) && U16_IS_TRAIL(second))
                return U16_GET_SUPPLEMENTARY(first, second);
            return errorCodePoint;
        }

        char32_t LEGACY_NODELETE reread(unsigned from)
        {
            ASSERT(from < length);
            auto result = input[from];
            if (decodeSurrogatePairs) {
                if (U16_IS_LEAD(result) && from + 1 < length && U16_IS_TRAIL(input[from + 1]))
                    return U16_GET_SUPPLEMENTARY(result, input[from + 1]);
                if (U16_IS_TRAIL(result) && from > 0 && U16_IS_LEAD(input[from - 1]))
                    return errorCodePoint;
            }
            return result;
        }

        char32_t prev()
        {
            ASSERT(!(pos > length));
            if (pos && length)
                return input[pos - 1];
            return errorCodePoint;
        }

        unsigned LEGACY_NODELETE getPos()
        {
            return pos;
        }

        void LEGACY_NODELETE setPos(unsigned p)
        {
            pos = p;
        }

        bool atStart()
        {
            return pos == 0;
        }

        bool LEGACY_NODELETE atEnd()
        {
            return pos == length;
        }

        unsigned LEGACY_NODELETE end()
        {
            return length;
        }

        bool LEGACY_NODELETE checkInput(unsigned count)
        {
            if (((pos + count) <= length) && ((pos + count) >= pos)) {
                pos += count;
                return true;
            }
            return false;
        }

        void LEGACY_NODELETE uncheckInput(unsigned count)
        {
            RELEASE_ASSERT(pos >= count);
            pos -= count;
        }

        bool LEGACY_NODELETE tryUncheckInput(unsigned count)
        {
            if (count > pos)
                return false;
            pos -= count;
            return true;
        }

        bool LEGACY_NODELETE atStart(unsigned negativePositionOffset)
        {
            return pos == negativePositionOffset;
        }

        bool LEGACY_NODELETE atEnd(unsigned negativePositionOffest)
        {
            RELEASE_ASSERT(pos >= negativePositionOffest);
            return (pos - negativePositionOffest) == length;
        }

        bool LEGACY_NODELETE isAvailableInput(unsigned offset)
        {
            return (((pos + offset) <= length) && ((pos + offset) >= pos));
        }

        bool LEGACY_NODELETE isValidNegativeInputOffset(unsigned offset)
        {
            return (pos >= offset) && ((pos - offset) < length);
        }

        void dump(PrintStream& out) const
        {
            const unsigned maxPrintLen = 50;

            out.print(sizeof(CharType) == 1 ? "8bit" : "16bit", " len: ", length, " \"");
            unsigned printLen = std::min(length, maxPrintLen);
            for (unsigned i = 0; i < printLen; ++i)
                out.print(CharacterDump(input[i]));
            if (length > maxPrintLen)
                out.print("...");
            out.print("\"");
        }

    private:
        const CharType* input;
        unsigned pos;
        unsigned length;
        bool decodeSurrogatePairs;
    };

    bool testCharacterClass(CharacterClass* characterClass, char32_t ch)
    {
        auto linearSearchMatches = [ch](const Vector<char32_t>& matches) {
            for (unsigned i = 0; i < matches.size(); ++i) {
                if (ch == matches[i])
                    return true;
            }

            return false;
        };

        auto binarySearchMatches = [ch](const Vector<char32_t>& matches) {
            size_t low = 0;
            size_t high = matches.size() - 1;

            while (low <= high) {
                size_t mid = std::midpoint(low, high);
                int diff = ch - matches[mid];
                if (!diff)
                    return true;

                if (diff < 0) {
                    if (mid == low)
                        return false;
                    high = mid - 1;
                } else
                    low = mid + 1;
            }
            return false;
        };

        auto linearSearchRanges = [ch](const Vector<CharacterRange>& ranges) {
            for (unsigned i = 0; i < ranges.size(); ++i) {
                if ((ch >= ranges[i].begin) && (ch <= ranges[i].end))
                    return true;
            }

            return false;
        };

        auto binarySearchRanges = [ch](const Vector<CharacterRange>& ranges) {
            size_t low = 0;
            size_t high = ranges.size() - 1;

            while (low <= high) {
                size_t mid = std::midpoint(low, high);
                int rangeBeginDiff = ch - ranges[mid].begin;
                if (rangeBeginDiff >= 0 && ch <= ranges[mid].end)
                    return true;

                if (rangeBeginDiff < 0) {
                    if (mid == low)
                        return false;
                    high = mid - 1;
                } else
                    low = mid + 1;
            }
            return false;
        };

        if (characterClass->m_anyCharacter)
            return true;

        if (characterClass->m_table && ch < CharacterClass::tableSize)
            return static_cast<bool>(characterClass->m_table[ch]);

        if (characterClass->m_latin1Table && isLatin1(ch))
            return static_cast<bool>(characterClass->m_latin1Table->data[ch]);

        const size_t thresholdForBinarySearch = 6;

        if (!isLatin1(ch)) {
            if (characterClass->m_matches32.size()) {
                if (characterClass->m_matches32.size() > thresholdForBinarySearch) {
                    if (binarySearchMatches(characterClass->m_matches32))
                        return true;
                } else if (linearSearchMatches(characterClass->m_matches32))
                    return true;
            }

            if (characterClass->m_ranges32.size()) {
                if (characterClass->m_ranges32.size() > thresholdForBinarySearch) {
                    if (binarySearchRanges(characterClass->m_ranges32))
                        return true;
                } else if (linearSearchRanges(characterClass->m_ranges32))
                    return true;
            }
        } else {
            if (characterClass->m_matches8.size()) {
                if (characterClass->m_matches8.size() > thresholdForBinarySearch) {
                    if (binarySearchMatches(characterClass->m_matches8))
                        return true;
                } else if (linearSearchMatches(characterClass->m_matches8))
                    return true;
            }

            if (characterClass->m_ranges8.size()) {
                if (characterClass->m_ranges8.size() > thresholdForBinarySearch) {
                    if (binarySearchRanges(characterClass->m_ranges8))
                        return true;
                } else if (linearSearchRanges(characterClass->m_ranges8))
                    return true;
            }
        }

        return false;
    }

    bool LEGACY_NODELETE checkCharacter(ByteTerm& term, unsigned negativeInputOffset)
    {
        ASSERT(term.isCharacterType());
        if (term.matchDirection() == Forward)
            return term.atom.patternCharacter == static_cast<char32_t>(input.readChecked(negativeInputOffset));

        return term.atom.patternCharacter == static_cast<char32_t>(input.tryReadBackward(negativeInputOffset));
    }

    bool LEGACY_NODELETE checkSurrogatePair(ByteTerm& term, unsigned negativeInputOffset)
    {
        ASSERT(term.isCharacterType());
        return term.atom.patternCharacter == static_cast<char32_t>(input.readSurrogatePairChecked(negativeInputOffset));
    }

    bool LEGACY_NODELETE checkCasedCharacter(ByteTerm& term, unsigned negativeInputOffset)
    {
        ASSERT(term.isCasedCharacterType());
        char32_t ch = term.matchDirection() == Forward ? input.readChecked(negativeInputOffset) : input.tryReadBackward(negativeInputOffset);
        return (term.atom.casedCharacter.lo == ch) || (term.atom.casedCharacter.hi == ch);
    }

    bool checkCharacterClass(ByteTerm& term, unsigned negativeInputOffset)
    {
        ASSERT(term.isCharacterClass());

        auto inputChar = term.matchDirection() == Forward ? input.readChecked(negativeInputOffset) : input.tryReadBackward(negativeInputOffset);
        if (inputChar == errorCodePoint)
            return false;

        bool match = testCharacterClass(term.atom.characterClass, static_cast<char32_t>(inputChar));
        return term.invert() ? !match : match;
    }
    
    bool checkCharacterClassDontAdvanceInputForNonBMP(ByteTerm& term, unsigned negativeInputOffset)
    {
        ASSERT(term.isCharacterClass());
        CharacterClass* characterClass = term.atom.characterClass;

        if (term.matchDirection() == Backward && negativeInputOffset > input.getPos())
            return false;

        auto readCharacter = characterClass->hasOnlyNonBMPCharacters() ? input.readSurrogatePairChecked(negativeInputOffset) :  input.readChecked(negativeInputOffset);

        if (readCharacter == errorCodePoint)
            return false;

        return testCharacterClass(characterClass, static_cast<char32_t>(readCharacter));
    }

    bool LEGACY_NODELETE tryConsumeBackReference(int matchBegin, int matchEnd, ByteTerm& term)
    {
        unsigned matchSize = (unsigned)(matchEnd - matchBegin);

        if (term.matchDirection() == Forward) {
            if (!input.checkInput(matchSize))
                return false;
        }

        unsigned savedPos = input.getPos();

        for (unsigned i = 0; i < matchSize; ++i) {
            unsigned negativeInputOffset = term.inputPosition + matchSize - i;
            if (term.matchDirection() == Backward && negativeInputOffset > input.getPos()) {
                input.setPos(savedPos);
                return false;
            }

            char32_t oldCh = input.reread(matchBegin + i);
            char32_t ch;
            if (!U_IS_BMP(oldCh)) {
                ch = input.readSurrogatePairChecked(negativeInputOffset);
                ++i;
            } else
                ch = term.matchDirection() == Forward ? input.readCheckedDontAdvance(negativeInputOffset) : input.tryReadBackward(negativeInputOffset);

            if (oldCh != errorCodePoint && ch != errorCodePoint) {
                if (oldCh == ch)
                    continue;

                if (term.ignoreCase()) {
                    // See ES 6.0, 21.2.2.8.2 for the definition of Canonicalize(). For non-Unicode
                    // patterns, Unicode values are never allowed to match against ASCII ones.
                    // For Unicode, we need to check all canonical equivalents of a character.
                    if (isLegacyCompilation() && (isASCII(oldCh) || isASCII(ch))) {
                        if (toASCIIUpper(oldCh) == toASCIIUpper(ch))
                            continue;
                    } else if (areCanonicallyEquivalent(oldCh, ch, isEitherUnicodeCompilation() ? CanonicalMode::Unicode : CanonicalMode::UCS2))
                        continue;
                }
            }

            if (term.matchDirection() == Forward)
                input.uncheckInput(matchSize);
            else
                input.setPos(savedPos);

            return false;
        }

        if (term.matchDirection() == Backward)
            input.uncheckInput(matchSize);

        return true;
    }

    bool matchAssertionBOL(ByteTerm& term)
    {
        return (input.atStart(term.inputPosition)) || (term.multiline() && testCharacterClass(pattern->newlineCharacterClass, input.readCheckedDontAdvance(term.inputPosition + 1)));
    }

    bool matchAssertionEOL(ByteTerm& term)
    {
        if (term.inputPosition)
            return (input.atEnd(term.inputPosition)) || (term.multiline() && testCharacterClass(pattern->newlineCharacterClass, input.readCheckedDontAdvance(term.inputPosition)));

        return (input.atEnd()) || (term.multiline() && testCharacterClass(pattern->newlineCharacterClass, input.read()));
    }

    bool matchAssertionWordBoundary(ByteTerm& term)
    {
        unsigned inputOffset = term.inputPosition;

        auto boundaryCharacterClass = term.ignoreCase() ? pattern->ignoreCaseWordcharCharacterClass : pattern->wordcharCharacterClass;

        bool prevIsWordchar = !input.atStart(inputOffset) && testCharacterClass(boundaryCharacterClass, input.readCheckedDontAdvance(inputOffset + 1));
        bool readIsWordchar;
        if (inputOffset)
            readIsWordchar = !input.atEnd(inputOffset) && testCharacterClass(boundaryCharacterClass, input.readCheckedDontAdvance(inputOffset));
        else
            readIsWordchar = !input.atEnd() && testCharacterClass(boundaryCharacterClass, input.read());

        bool wordBoundary = prevIsWordchar != readIsWordchar;
        return term.invert() ? !wordBoundary : wordBoundary;
    }

    bool LEGACY_NODELETE backtrackPatternCharacter(ByteTerm& term, DisjunctionContext* context)
    {
        BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount:
            break;

        case QuantifierType::Greedy:
            if (backTrack->matchAmount) {
                --backTrack->matchAmount;
                if (term.matchDirection() == Forward)
                    input.uncheckInput(U16_LENGTH(term.atom.patternCharacter));
                else {
                    if (!input.checkInput(U16_LENGTH(term.atom.patternCharacter)))
                        break;
                }
                return true;
            }
            break;

        case QuantifierType::NonGreedy:
            if (term.matchDirection() == Forward) {
                if ((backTrack->matchAmount < term.atom.quantityMaxCount) && input.checkInput(1)) {
                    ++backTrack->matchAmount;
                    if (checkCharacter(term, term.inputPosition + 1))
                        return true;
                }
                input.setPos(backTrack->begin);
                break;
            }
            // matchDirection Backward
            unsigned position = input.getPos();

            if (position < term.inputPosition)
                break;

            if ((backTrack->matchAmount < term.atom.quantityMaxCount) && input.tryUncheckInput(1)) {
                ++backTrack->matchAmount;
                if (checkCharacter(term, term.inputPosition))
                    return true;
            }
            input.setPos(backTrack->begin);
            break;

        }

        return false;
    }

    bool LEGACY_NODELETE backtrackPatternCasedCharacter(ByteTerm& term, DisjunctionContext* context)
    {
        BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount:
            break;

        case QuantifierType::Greedy:
            if (backTrack->matchAmount) {
                --backTrack->matchAmount;
                if (term.matchDirection() == Forward)
                    input.uncheckInput(1);
                else {
                    if (!input.checkInput(1))
                        break;
                }
                return true;
            }
            break;

        case QuantifierType::NonGreedy:
            if (term.matchDirection() == Forward) {
                if ((backTrack->matchAmount < term.atom.quantityMaxCount) && input.checkInput(1)) {
                    ++backTrack->matchAmount;
                    if (checkCasedCharacter(term, term.inputPosition + 1))
                        return true;
                }
                input.uncheckInput(backTrack->matchAmount);
                break;
            }
            // matchDirection Backward
            unsigned position = input.getPos();

            if (position < term.inputPosition)
                break;

            if ((backTrack->matchAmount < term.atom.quantityMaxCount) && input.tryUncheckInput(1)) {
                ++backTrack->matchAmount;
                if (checkCasedCharacter(term, term.inputPosition))
                    return true;
            }
            input.setPos(backTrack->begin);
            break;
        }

        return false;
    }

    bool matchCharacterClass(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::CharacterClass);
        BackTrackInfoCharacterClass* backTrack = reinterpret_cast<BackTrackInfoCharacterClass*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount: {
            if (term.matchDirection() == Forward) {
                if (isEitherUnicodeCompilation()) {
                    backTrack->begin = input.getPos();
                    unsigned matchAmount = 0;
                    for (matchAmount = 0; matchAmount < term.atom.quantityMaxCount; ++matchAmount) {
                        if (term.invert()) {
                            if (!checkCharacterClass(term, term.inputPosition - matchAmount)) {
                                input.setPos(backTrack->begin);
                                return false;
                            }
                        } else {
                            unsigned matchOffset = matchAmount * (term.atom.characterClass->hasOnlyNonBMPCharacters() ? 2 : 1);
                            if (!checkCharacterClassDontAdvanceInputForNonBMP(term, term.inputPosition - matchOffset)) {
                                input.setPos(backTrack->begin);
                                return false;
                            }
                        }
                    }

                    return true;
                }

                for (unsigned matchAmount = 0; matchAmount < term.atom.quantityMaxCount; ++matchAmount) {
                    if (!checkCharacterClass(term, term.inputPosition - matchAmount))
                        return false;
                }
                return true;
            }

            // matchDirection is Backward
            if (isEitherUnicodeCompilation()) {
                backTrack->begin = input.getPos();
                bool pairsOnly = !term.invert() && term.atom.characterClass->hasOnlyNonBMPCharacters();
                for (unsigned matchAmount = 0; matchAmount < term.atom.quantityMaxCount; ++matchAmount) {
                    unsigned matchOffset = term.atom.quantityMaxCount - 1 - matchAmount;
                    if (pairsOnly) {
                        if (!checkCharacterClassDontAdvanceInputForNonBMP(term, term.inputPosition - 2 * matchOffset)) {
                            input.setPos(backTrack->begin);
                            return false;
                        }
                    } else if (!checkCharacterClass(term, term.inputPosition - matchOffset)) {
                        input.setPos(backTrack->begin);
                        return false;
                    }
                }

                return true;
            }

            if (input.getPos() < term.inputPosition)
                return false;

            for (unsigned matchAmount = 0; matchAmount < term.atom.quantityMaxCount; ++matchAmount) {
                if (!checkCharacterClass(term, term.inputPosition - term.atom.quantityMaxCount + matchAmount + 1))
                    return false;
            }
            return true;
        }

        case QuantifierType::Greedy: {
            unsigned position = input.getPos();
            unsigned matchAmount = 0;
            if (term.matchDirection() == Forward) {
                while ((matchAmount < term.atom.quantityMaxCount) && input.checkInput(1)) {
                    if (!checkCharacterClass(term, term.inputPosition + 1)) {
                        input.setPos(position);
                        break;
                    }
                    ++matchAmount;
                    position = input.getPos();
                }
                backTrack->matchAmount = matchAmount;
                return true;
            }

            // matchDirection = Backward
            if (input.getPos() < term.inputPosition)
                return false;

            backTrack->begin = position;
            while ((matchAmount < term.atom.quantityMaxCount) && input.tryUncheckInput(1)) {
                if (!checkCharacterClass(term, term.inputPosition)) {
                    input.setPos(position);
                    break;
                }
                ++matchAmount;
                position = input.getPos();
            }
            backTrack->matchAmount = matchAmount;
            return true;
        }

        case QuantifierType::NonGreedy:
            backTrack->begin = input.getPos();
            backTrack->matchAmount = 0;
            return true;
        }

        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }

    bool backtrackCharacterClass(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::CharacterClass);
        BackTrackInfoCharacterClass* backTrack = reinterpret_cast<BackTrackInfoCharacterClass*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount:
            if (isEitherUnicodeCompilation())
                input.setPos(backTrack->begin);
            break;

        case QuantifierType::Greedy:
            if (backTrack->matchAmount) {
                if (isEitherUnicodeCompilation()) {
                    // Unmatch one codepoint
                    if (term.matchDirection() == Forward) {
                        --backTrack->matchAmount;
                        input.uncheckInput(1);
                        input.tryReadBackward(term.inputPosition);
                        return true;
                    }
                    // matchDirection Backwards
                    --backTrack->matchAmount;
                    if (!backTrack->matchAmount) {
                        input.setPos(backTrack->begin);
                        return true;
                    }
                    input.readChecked(term.inputPosition);
                    input.checkInput(1);
                    return true;
                }
                --backTrack->matchAmount;
                if (term.matchDirection() == Forward)
                    input.uncheckInput(1);
                else
                    input.checkInput(1);
                return true;
            }
            break;

        case QuantifierType::NonGreedy:
            if (term.matchDirection() == Forward) {
                if ((backTrack->matchAmount < term.atom.quantityMaxCount) && input.checkInput(1)) {
                    ++backTrack->matchAmount;
                    if (checkCharacterClass(term, term.inputPosition + 1))
                        return true;
                }
                input.setPos(backTrack->begin);
                break;
            }
            // matchDirection Backward
            if ((backTrack->matchAmount < term.atom.quantityMaxCount) && input.tryUncheckInput(1)) {
                ++backTrack->matchAmount;
                if (checkCharacterClass(term, term.inputPosition))
                    return true;
            }
            input.setPos(backTrack->begin);
            break;
        }

        return false;
    }

    bool matchBackReference(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::BackReference);
        BackTrackInfoBackReference* backTrack = reinterpret_cast<BackTrackInfoBackReference*>(context->frame + term.frameLocation);

        // Initialize backtracking info first before we check for possible null matches.
        switch (term.atom.quantityType) {
        case QuantifierType::NonGreedy:
            backTrack->matchAmount = 0;
            [[fallthrough]];

        case QuantifierType::FixedCount:
            backTrack->begin = input.getPos();
            break;

        case QuantifierType::Greedy:
            backTrack->matchAmount = 0;
            break;
        }

        unsigned subpatternId;

        if (auto duplicateNamedGroupId = term.duplicateNamedGroupId()) {
            subpatternId = output[pattern->offsetForDuplicateNamedGroupId(duplicateNamedGroupId)];
            if (subpatternId < 1) {
                // If we don't have a subpattern that matched, then the string to match is empty.
                return true;
            }
        } else
            subpatternId = term.subpatternId();

        unsigned matchBegin = output[(subpatternId << 1)];
        unsigned matchEnd = output[(subpatternId << 1) + 1];

        // If the end position of the referenced match hasn't set yet then the backreference in the same parentheses where it references to that.
        // In this case the result of match is empty string like when it references to a parentheses with zero-width match.
        // Eg.: /(a\1)/
        if (matchEnd == offsetNoMatch)
            return true;

        if (matchBegin == offsetNoMatch)
            return true;

        ASSERT(matchBegin <= matchEnd);

        if (matchBegin == matchEnd)
            return true;

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount: {
            for (unsigned matchAmount = 0; matchAmount < term.atom.quantityMaxCount; ++matchAmount) {
                if (!tryConsumeBackReference(matchBegin, matchEnd, term)) {
                    input.setPos(backTrack->begin);
                    return false;
                }
            }
            return true;
        }

        case QuantifierType::Greedy: {
            unsigned matchAmount = 0;
            while ((matchAmount < term.atom.quantityMaxCount) && tryConsumeBackReference(matchBegin, matchEnd, term))
                ++matchAmount;
            backTrack->matchAmount = matchAmount;
            return true;
        }

        case QuantifierType::NonGreedy:
            return true;
        }

        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }

    bool LEGACY_NODELETE backtrackBackReference(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::BackReference);
        BackTrackInfoBackReference* backTrack = reinterpret_cast<BackTrackInfoBackReference*>(context->frame + term.frameLocation);

        unsigned subpatternId;

        if (auto duplicateNamedGroupId = term.duplicateNamedGroupId()) {
            subpatternId = output[pattern->offsetForDuplicateNamedGroupId(duplicateNamedGroupId)];
            if (subpatternId < 1) {
                // If we don't have a subpattern that matched, then the string to match is empty.
                return false;
            }
        } else
            subpatternId = term.subpatternId();

        unsigned matchBegin = output[(subpatternId << 1)];
        unsigned matchEnd = output[(subpatternId << 1) + 1];

        if (matchBegin == offsetNoMatch || matchEnd == offsetNoMatch)
            return false;

        ASSERT(matchBegin <= matchEnd);

        if (matchBegin == matchEnd)
            return false;

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount:
            // for quantityMaxCount == 1, could rewind.
            input.setPos(backTrack->begin);
            break;

        case QuantifierType::Greedy:
            if (backTrack->matchAmount) {
                --backTrack->matchAmount;
                if (term.matchDirection() == Backward)
                    return input.checkInput(matchEnd - matchBegin);
                input.rewind(matchEnd - matchBegin);
                return true;
            }
            break;

        case QuantifierType::NonGreedy:
            if ((backTrack->matchAmount < term.atom.quantityMaxCount) && tryConsumeBackReference(matchBegin, matchEnd, term)) {
                ++backTrack->matchAmount;
                return true;
            }
            input.setPos(backTrack->begin);
            break;
        }

        return false;
    }

    void recordParenthesesMatch(ByteTerm& term, ParenthesesDisjunctionContext* context)
    {
        if (term.capture()) {
            unsigned subpatternId = term.subpatternId();
            // For Backward matches, the captured indexes are recorded end then start.
            output[(subpatternId << 1) + term.matchDirection()] = context->getDisjunctionContext()->matchBegin - term.inputPosition;
            output[(subpatternId << 1) + 1 - term.matchDirection()] = context->getDisjunctionContext()->matchEnd - term.inputPosition;

            if (term.duplicateNamedGroupId()) {
                // Record which of the duplicate named subpatterns matched.
                output[pattern->offsetForDuplicateNamedGroupId(term.duplicateNamedGroupId())] = subpatternId;
            }
        }
    }
    void resetMatches(ByteTerm& term, ParenthesesDisjunctionContext* context)
    {
        unsigned firstSubpatternId = term.subpatternId();
        context->restoreOutput(output, firstSubpatternId);
    }

    JSRegExpResult parenthesesDoBacktrack(ByteTerm& term, BackTrackInfoParentheses* backTrack)
    {
        while (backTrack->matchAmount) {
            ParenthesesDisjunctionContext* context = backTrack->lastContext;

            JSRegExpResult result = matchDisjunction(term.atom.parenthesesDisjunction, context->getDisjunctionContext(), /* btrack= */ true);
            if (result == JSRegExpResult::Match)
                return JSRegExpResult::Match;

            resetMatches(term, context);
            popParenthesesDisjunctionContext(backTrack);
            freeParenthesesDisjunctionContext(context);

            if (result != JSRegExpResult::NoMatch)
                return result;
        }

        return JSRegExpResult::NoMatch;
    }

    bool LEGACY_NODELETE matchParenthesesOnceBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpatternOnceBegin);
        ASSERT(term.atom.quantityMaxCount == 1);

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierType::Greedy: {
            // set this speculatively; if we get to the parens end this will be true.
            backTrack->begin = input.getPos();
            break;
        }
        case QuantifierType::NonGreedy: {
            backTrack->begin = notFound;
            context->term += term.atom.parenthesesWidth;
            return true;
        }
        case QuantifierType::FixedCount:
            break;
        }

        if (term.capture()) {
            unsigned subpatternId = term.subpatternId();
            // For Backward matches, the captured indexes are recorded end then start.
            output[(subpatternId << 1) + term.matchDirection()] = input.getPos() - term.inputPosition;
        }

        return true;
    }

    bool LEGACY_NODELETE matchParenthesesOnceEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpatternOnceEnd);
        ASSERT(term.atom.quantityMaxCount == 1);

        if (term.capture()) {
            unsigned subpatternId = term.subpatternId();
            // For Backward matches, the captured indexes are recorded end then start.
            output[(subpatternId << 1) + 1 - term.matchDirection()] = input.getPos() - term.inputPosition;

            if (term.duplicateNamedGroupId()) {
                // Record which of the duplicate named subpatterns matched.
                output[pattern->offsetForDuplicateNamedGroupId(term.duplicateNamedGroupId())] = subpatternId;
            }
        }

        if (term.atom.quantityType == QuantifierType::FixedCount)
            return true;

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);
        return backTrack->begin != input.getPos();
    }

    bool backtrackParenthesesOnceBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpatternOnceBegin);
        ASSERT(term.atom.quantityMaxCount == 1);

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);

        if (term.capture()) {
            unsigned subpatternId = term.subpatternId();
            output[(subpatternId << 1)] = offsetNoMatch;
            output[(subpatternId << 1) + 1] = offsetNoMatch;

            if (term.duplicateNamedGroupId()) {
                // Clear matching subpatternId.
                output[pattern->offsetForDuplicateNamedGroupId(term.duplicateNamedGroupId())] = 0;
            }
        }

        switch (term.atom.quantityType) {
        case QuantifierType::Greedy:
            // if we backtrack to this point, there is another chance - try matching nothing.
            ASSERT(backTrack->begin != notFound);
            backTrack->begin = notFound;
            context->term += term.atom.parenthesesWidth;
            return true;
        case QuantifierType::NonGreedy:
            ASSERT(backTrack->begin != notFound);
            [[fallthrough]];
        case QuantifierType::FixedCount:
            break;
        }

        return false;
    }

    bool backtrackParenthesesOnceEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpatternOnceEnd);
        ASSERT(term.atom.quantityMaxCount == 1);

        BackTrackInfoParenthesesOnce* backTrack = reinterpret_cast<BackTrackInfoParenthesesOnce*>(context->frame + term.frameLocation);

        switch (term.atom.quantityType) {
        case QuantifierType::Greedy:
            if (backTrack->begin == notFound) {
                context->term -= term.atom.parenthesesWidth;
                return false;
            }
            [[fallthrough]];
        case QuantifierType::NonGreedy:
            if (backTrack->begin == notFound) {
                backTrack->begin = input.getPos();
                if (term.capture()) {
                    // Technically this access to inputPosition should be accessing the begin term's
                    // inputPosition, but for repeats other than fixed these values should be
                    // the same anyway! (We don't pre-check for greedy or non-greedy matches.)
                    ASSERT((&term - term.atom.parenthesesWidth)->type == ByteTerm::Type::ParenthesesSubpatternOnceBegin);
                    ASSERT((&term - term.atom.parenthesesWidth)->inputPosition == term.inputPosition);
                    unsigned subpatternId = term.subpatternId();
                    // For Backward matches, the captured indexes are recorded end then start.
                    output[(subpatternId << 1) + term.matchDirection()] = input.getPos() - term.inputPosition;
                }
                context->term -= term.atom.parenthesesWidth;
                return true;
            }
            [[fallthrough]];
        case QuantifierType::FixedCount:
            break;
        }

        return false;
    }

    bool LEGACY_NODELETE matchParenthesesTerminalBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpatternTerminalBegin);
        ASSERT(term.atom.quantityType == QuantifierType::Greedy);
        ASSERT(!term.atom.quantityMinCount || term.atom.quantityMinCount == 1);
        ASSERT(term.atom.quantityMaxCount == quantifyInfinite);
        ASSERT(!term.capture());

        BackTrackInfoParenthesesTerminal* backTrack = reinterpret_cast<BackTrackInfoParenthesesTerminal*>(context->frame + term.frameLocation);
        backTrack->begin = input.getPos();
        backTrack->entryPosition = input.getPos();
        return true;
    }

    bool LEGACY_NODELETE matchParenthesesTerminalEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpatternTerminalEnd);
        ASSERT(!term.atom.quantityMinCount || term.atom.quantityMinCount == 1);

        BackTrackInfoParenthesesTerminal* backTrack = reinterpret_cast<BackTrackInfoParenthesesTerminal*>(context->frame + term.frameLocation);
        if (backTrack->begin == input.getPos()) {
            // An empty iteration cannot be repeated, so it is only ever acceptable as the single
            // iteration a minimum of one demands, and only before anything has been consumed.
            // Clearing entryPosition both records that the minimum is now met and rejects any
            // further empty iteration.
            if (!term.atom.quantityMinCount || backTrack->entryPosition != input.getPos())
                return false;
            backTrack->entryPosition = notFound;
        }

        backTrack->begin = input.getPos();

        // Successful match! Okay, what's next? - loop around and try to match more!
        // Loop back to the body's first term rather than to ParenthesesSubpatternTerminalBegin,
        // whose stores initialize the whole group and must not run again per iteration.
        context->term -= term.atom.parenthesesWidth;
        return true;
    }

    bool LEGACY_NODELETE backtrackParenthesesTerminalBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpatternTerminalBegin);
        ASSERT(term.atom.quantityType == QuantifierType::Greedy);
        ASSERT(!term.atom.quantityMinCount || term.atom.quantityMinCount == 1);
        ASSERT(term.atom.quantityMaxCount == quantifyInfinite);
        ASSERT(!term.capture());

        // If we backtrack to this point, we have failed to match this iteration of the parens.
        // Nothing follows a terminal group, so an already satisfied minimum makes that failure an
        // acceptable end of the match; an unsatisfied one fails the match.
        if (term.atom.quantityMinCount) {
            BackTrackInfoParenthesesTerminal* backTrack = reinterpret_cast<BackTrackInfoParenthesesTerminal*>(context->frame + term.frameLocation);
            if (backTrack->entryPosition == input.getPos())
                return false;
        }

        context->term += term.atom.parenthesesWidth;
        return true;
    }

    bool LEGACY_NODELETE backtrackParenthesesTerminalEnd(ByteTerm&, DisjunctionContext*)
    {
        // 'Terminal' parentheses are at the end of the regex, and as such a match past end
        // should always be returned as a successful match - we should never backtrack to here.
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }

    bool LEGACY_NODELETE matchParentheticalAssertionBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParentheticalAssertionBegin);
        ASSERT(term.atom.quantityMaxCount == 1);

        BackTrackInfoParentheticalAssertion* backTrack = reinterpret_cast<BackTrackInfoParentheticalAssertion*>(context->frame + term.frameLocation);

        backTrack->begin = input.getPos();

        return true;
    }

    bool LEGACY_NODELETE matchParentheticalAssertionEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParentheticalAssertionEnd);
        ASSERT(term.atom.quantityMaxCount == 1);

        BackTrackInfoParentheticalAssertion* backTrack = reinterpret_cast<BackTrackInfoParentheticalAssertion*>(context->frame + term.frameLocation);

        input.setPos(backTrack->begin);

        // We've reached the end of the parens; if they are inverted, this is failure.
        if (term.invert()) {
            if (term.containsAnyCaptures()) {
                for (unsigned subpattern = term.subpatternId(); subpattern <= term.lastSubpatternId(); subpattern++) {
                    output[subpattern << 1] = offsetNoMatch;
                    output[(subpattern << 1) + 1] = offsetNoMatch;
                }
            }
            context->term -= term.atom.parenthesesWidth;
            return false;
        }

        return true;
    }

    bool LEGACY_NODELETE backtrackParentheticalAssertionBegin(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParentheticalAssertionBegin);
        ASSERT(term.atom.quantityMaxCount == 1);

        if (term.matchDirection() == Backward) {
            BackTrackInfoParentheticalAssertion* backTrack = reinterpret_cast<BackTrackInfoParentheticalAssertion*>(context->frame + term.frameLocation);
            input.setPos(backTrack->begin);
        }

        // We've failed to match parens; if they are inverted, this is win!
        if (term.invert()) {
            context->term += term.atom.parenthesesWidth;
            return true;
        }

        return false;
    }

    bool LEGACY_NODELETE backtrackParentheticalAssertionEnd(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParentheticalAssertionEnd);
        ASSERT(term.atom.quantityMaxCount == 1);

        BackTrackInfoParentheticalAssertion* backTrack = reinterpret_cast<BackTrackInfoParentheticalAssertion*>(context->frame + term.frameLocation);

        input.setPos(backTrack->begin);

        if (term.containsAnyCaptures()) {
            for (unsigned subpattern = term.subpatternId(); subpattern <= term.lastSubpatternId(); subpattern++) {
                output[subpattern << 1] = offsetNoMatch;
                output[(subpattern << 1) + 1] = offsetNoMatch;
            }
        }

        context->term -= term.atom.parenthesesWidth;
        return false;
    }

    JSRegExpResult matchParentheses(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpattern);

        BackTrackInfoParentheses* backTrack = reinterpret_cast<BackTrackInfoParentheses*>(context->frame + term.frameLocation);
        ByteDisjunction* disjunctionBody = term.atom.parenthesesDisjunction;

        backTrack->begin = input.getPos();
        backTrack->matchAmount = 0;
        backTrack->lastContext = nullptr;

        ASSERT(term.atom.quantityType != QuantifierType::FixedCount || term.atom.quantityMinCount == term.atom.quantityMaxCount);

        unsigned minimumMatchCount = term.atom.quantityMinCount;

        // Handle fixed matches and the minimum part of a variable length match.
        if (minimumMatchCount) {
            // Match the mandatory iterations, backtracking earlier ones as needed.
            JSRegExpResult result = refillParenthesesContextsToMinCount(term, backTrack, disjunctionBody);
            if (result != JSRegExpResult::Match)
                return result;

            ParenthesesDisjunctionContext* context = backTrack->lastContext;
            recordParenthesesMatch(term, context);
        }

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount: {
            ASSERT(backTrack->matchAmount == term.atom.quantityMaxCount);
            return JSRegExpResult::Match;
        }

        case QuantifierType::Greedy: {
            JSRegExpResult result = extendParenthesesContextsToMaxCount(term, backTrack, disjunctionBody);
            if (result != JSRegExpResult::Match)
                return result;

            if (backTrack->matchAmount) {
                ParenthesesDisjunctionContext* context = backTrack->lastContext;
                recordParenthesesMatch(term, context);
            }
            return JSRegExpResult::Match;
        }

        case QuantifierType::NonGreedy:
            return JSRegExpResult::Match;
        }

        RELEASE_ASSERT_NOT_REACHED();
        return JSRegExpResult::ErrorNoMatch;
    }

    // Rules for backtracking differ depending on whether this is greedy or non-greedy.
    //
    // Greedy matches never should try just adding more - you should already have done
    // the 'more' cases.  Always backtrack, at least a leetle bit.  However cases where
    // you backtrack an item off the list needs checking, since we'll never have matched
    // the one less case.  Tracking forwards, still add as much as possible.
    //
    // Non-greedy, we've already done the one less case, so don't match on popping.
    // We haven't done the one more case, so always try to add that.
    //
    JSRegExpResult backtrackParentheses(ByteTerm& term, DisjunctionContext* context)
    {
        ASSERT(term.type == ByteTerm::Type::ParenthesesSubpattern);

        BackTrackInfoParentheses* backTrack = reinterpret_cast<BackTrackInfoParentheses*>(context->frame + term.frameLocation);
        ByteDisjunction* disjunctionBody = term.atom.parenthesesDisjunction;

        switch (term.atom.quantityType) {
        case QuantifierType::FixedCount: {
            ASSERT(backTrack->matchAmount == term.atom.quantityMaxCount);
            ASSERT(term.atom.quantityMinCount == term.atom.quantityMaxCount);

            JSRegExpResult result = parenthesesDoBacktrack(term, backTrack);
            if (result != JSRegExpResult::Match)
                return result;

            // For FixedCount min == max, so refilling to min refills to max; the
            // helper handles the inner content-backtrack-on-failure loop too.
            result = refillParenthesesContextsToMinCount(term, backTrack, disjunctionBody);
            if (result != JSRegExpResult::Match)
                return result;

            ASSERT(backTrack->matchAmount == term.atom.quantityMaxCount);
            ParenthesesDisjunctionContext* context = backTrack->lastContext;
            recordParenthesesMatch(term, context);
            return JSRegExpResult::Match;
        }

        case QuantifierType::Greedy: {
            if (!backTrack->matchAmount)
                return JSRegExpResult::NoMatch;

            ParenthesesDisjunctionContext* context = backTrack->lastContext;
            // Per RepeatMatcher, only iterations beyond the mandatory minimum must be
            // non-empty; a mandatory iteration (count <= min) may match zero-length, so
            // retry its content allowing an empty match in that case.
            JSRegExpResult result = (backTrack->matchAmount <= term.atom.quantityMinCount)
                ? matchDisjunction(disjunctionBody, context->getDisjunctionContext(), /* btrack= */ true)
                : matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(), /* btrack= */ true);
            if (result == JSRegExpResult::Match) {
                result = extendParenthesesContextsToMaxCount(term, backTrack, disjunctionBody);
                if (result != JSRegExpResult::Match)
                    return result;
            } else {
                resetMatches(term, context);
                popParenthesesDisjunctionContext(backTrack);
                freeParenthesesDisjunctionContext(context);

                if (result != JSRegExpResult::NoMatch)
                    return result;

                // When matchAmount falls below the minimum, do not give up immediately:
                // try parenthesesDoBacktrack on the remaining contexts to find an
                // alternative match distribution that allows us to reach min, then
                // refill back up to min.
                //
                // For example, /((a+){2,3}){2,3}$/  matched against  "aaaaaa",
                // `a+` will drain all input and this makes {2,3}'s `2` count failed.
                // But instead of immediately saying "this is failed", we should backtrack `a+`,
                // reducing drained count of `a`, and then the parentheses succeeds.
                if (backTrack->matchAmount < term.atom.quantityMinCount) {
                    result = parenthesesDoBacktrack(term, backTrack);
                    if (result != JSRegExpResult::Match)
                        return result;

                    // Now content-backtracking succeeded. Refill back up to min-count.
                    result = refillParenthesesContextsToMinCount(term, backTrack, disjunctionBody);
                    if (result != JSRegExpResult::Match)
                        return result;

                    // And now we should expand the matching up to max count, then we can say Greedy is fully backtracked.
                    result = extendParenthesesContextsToMaxCount(term, backTrack, disjunctionBody);
                    if (result != JSRegExpResult::Match)
                        return result;
                }
            }

            if (backTrack->matchAmount) {
                ParenthesesDisjunctionContext* context = backTrack->lastContext;
                recordParenthesesMatch(term, context);
            }
            return JSRegExpResult::Match;
        }

        case QuantifierType::NonGreedy: {
            // If we've not reached the limit, try to add one more match.
            if (backTrack->matchAmount < term.atom.quantityMaxCount) {
                ParenthesesDisjunctionContext* context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
                if (!context) [[unlikely]]
                    return JSRegExpResult::ErrorNoMemory;
                JSRegExpResult result = matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(), /* btrack= */ false);
                if (result == JSRegExpResult::Match) {
                    appendParenthesesDisjunctionContext(backTrack, context);
                    recordParenthesesMatch(term, context);
                    return JSRegExpResult::Match;
                }

                resetMatches(term, context);
                freeParenthesesDisjunctionContext(context);

                if (result != JSRegExpResult::NoMatch)
                    return result;
            }

            // Nope - okay backtrack looking for an alternative.
            while (backTrack->matchAmount) {
                ParenthesesDisjunctionContext* context = backTrack->lastContext;
                // Mandatory iterations (count <= min) may match zero-length; only extra
                // iterations must be non-empty (RepeatMatcher).
                JSRegExpResult result = (backTrack->matchAmount <= term.atom.quantityMinCount)
                    ? matchDisjunction(disjunctionBody, context->getDisjunctionContext(), /* btrack= */ true)
                    : matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(), /* btrack= */ true);
                if (result == JSRegExpResult::Match) {
                    // A successful content backtrack may have left us below the mandatory
                    // minimum (we popped iterations above while searching for an alternative).
                    // NonGreedy still requires at least min iterations, so refill back up to
                    // min before accepting — mirroring the Greedy case. Without this the
                    // interpreter would return a match with fewer than quantityMinCount
                    // iterations (e.g. /(?:xy|x){2,3}?yxw/ wrongly matching "xyxw").
                    result = refillParenthesesContextsToMinCount(term, backTrack, disjunctionBody);
                    if (result != JSRegExpResult::Match)
                        return result;

                    // successful backtrack! we're back in the game!
                    if (backTrack->matchAmount) {
                        context = backTrack->lastContext;
                        recordParenthesesMatch(term, context);
                    }
                    return JSRegExpResult::Match;
                }

                // pop a match off the stack
                resetMatches(term, context);
                popParenthesesDisjunctionContext(backTrack);
                freeParenthesesDisjunctionContext(context);

                if (result != JSRegExpResult::NoMatch)
                    return result;
            }

            return JSRegExpResult::NoMatch;
        }
        }

        RELEASE_ASSERT_NOT_REACHED();
        return JSRegExpResult::ErrorNoMatch;
    }

    JSRegExpResult refillParenthesesContextsToMinCount(ByteTerm& term, BackTrackInfoParentheses* backTrack, ByteDisjunction* disjunctionBody)
    {
        while (backTrack->matchAmount < term.atom.quantityMinCount) {
            ParenthesesDisjunctionContext* context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
            if (!context) [[unlikely]]
                return JSRegExpResult::ErrorNoMemory;
            JSRegExpResult result = matchDisjunction(disjunctionBody, context->getDisjunctionContext(), /* btrack= */ false);
            if (result == JSRegExpResult::Match) {
                appendParenthesesDisjunctionContext(backTrack, context);
                continue;
            }
            resetMatches(term, context);
            freeParenthesesDisjunctionContext(context);
            if (result != JSRegExpResult::NoMatch)
                return result;
            result = parenthesesDoBacktrack(term, backTrack);
            if (result != JSRegExpResult::Match)
                return result;
        }
        return JSRegExpResult::Match;
    }

    // Greedy extension appends as many additional iterations as possible up
    // to quantityMaxCount, each required to match at least one character.
    JSRegExpResult extendParenthesesContextsToMaxCount(ByteTerm& term, BackTrackInfoParentheses* backTrack, ByteDisjunction* disjunctionBody)
    {
        while (backTrack->matchAmount < term.atom.quantityMaxCount) {
            ParenthesesDisjunctionContext* context = allocParenthesesDisjunctionContext(disjunctionBody, output, term);
            if (!context) [[unlikely]]
                return JSRegExpResult::ErrorNoMemory;
            JSRegExpResult result = matchNonZeroDisjunction(disjunctionBody, context->getDisjunctionContext(), /* btrack= */ false);
            if (result == JSRegExpResult::Match) {
                appendParenthesesDisjunctionContext(backTrack, context);
                continue;
            }
            resetMatches(term, context);
            freeParenthesesDisjunctionContext(context);
            if (result != JSRegExpResult::NoMatch)
                return result;
            break;
        }
        return JSRegExpResult::Match;
    }

    bool matchDotStarEnclosure(ByteTerm& term, DisjunctionContext* context)
    {
        auto isNewline = [&](unsigned position) {
            return testCharacterClass(pattern->newlineCharacterClass, input.reread(position));
        };
        // A leading ^ holds at position 0, or under /m just past a line terminator.
        auto isLineStart = [&](unsigned position) {
            return !position || (term.multiline() && isNewline(position - 1));
        };
        // [startOffset, noNewlineBefore) is known to hold no line terminator. It only grows during
        // a match, so an enclosure rejected for want of a line start (a /g RegExp resumed mid-line)
        // does not rescan that span for every later occurrence of the wrapped expression. Without
        // /m a ^ rejected once stays rejected (later occurrences begin no earlier), recorded as
        // bolUnsatisfiable so those fail at once instead of re-walking their line.
        ASSERT(noNewlineBefore >= startOffset || noNewlineBefore == bolUnsatisfiable);
        if (noNewlineBefore == bolUnsatisfiable)
            return false;
        auto rejectForBOL = [&]() {
            if (!term.multiline())
                noNewlineBefore = bolUnsatisfiable;
            return false;
        };
        unsigned expressionBegin = context->matchBegin;

        if (term.dotAll()) {
            // Under /s the leading .* reaches back to where matching began and the trailing
            // .* runs to the end. A leading ^ must still hold at the match start: the start
            // offset itself, or under /m the first line start after it that does not pass
            // the wrapped expression.
            unsigned matchBegin = startOffset;
            if (term.anchors.m_bol && !isLineStart(matchBegin)) {
                if (!term.multiline())
                    return rejectForBOL();
                matchBegin = std::max(matchBegin, noNewlineBefore);
                while (matchBegin < expressionBegin && !isNewline(matchBegin))
                    ++matchBegin;
                if (matchBegin >= expressionBegin) {
                    noNewlineBefore = std::max(noNewlineBefore, expressionBegin);
                    return false; // no line start in [startOffset, expression]; a later occurrence may have one
                }
                ++matchBegin; // just past the newline
            }
            context->matchBegin = matchBegin;
            context->matchEnd = input.end();
            return true;
        }

        // Walk back from the expression to the start of its line, but not into the span already
        // known to be newline-free.
        unsigned matchBegin = expressionBegin;
        unsigned bound = std::max(startOffset, noNewlineBefore);
        if (matchBegin > bound) {
            for (matchBegin--; true; matchBegin--) {
                if (isNewline(matchBegin)) {
                    ++matchBegin;
                    break;
                }

                if (matchBegin == bound) {
                    noNewlineBefore = expressionBegin;
                    matchBegin = startOffset;
                    break;
                }
            }
        } else
            matchBegin = startOffset;

        // The walk back stopped just past a newline or at the start offset; the latter is a
        // line start only at 0 or (under /m) when the character before it is a newline, so a
        // /g RegExp resumed mid-line ("bxxaxx" from lastIndex 3) does not satisfy ^ there.
        if (term.anchors.m_bol && !isLineStart(matchBegin))
            return rejectForBOL();

        unsigned matchEnd = input.getPos();

        for (; (matchEnd != input.end()) && !isNewline(matchEnd); matchEnd++) { }

        if (term.anchors.m_eol && matchEnd != input.end() && !term.multiline())
            return false;

        context->matchBegin = matchBegin;
        context->matchEnd = matchEnd;
        return true;
    }

#define MATCH_NEXT() { if (verbose) { dataLog(" - Match\n"); if (btrack) { dataLog("  Matching\n"); btrack = false; } }  ++context->term; goto matchAgain; }
#define BACKTRACK() { if (verbose) { dataLog(" - Backtrack\n"); if (!btrack) { dataLog("  Backtracking\n"); btrack = true; } } --context->term; goto backtrack; }
#define currentTerm() (disjunction->terms[context->term])

#define DUMP_TERM() \
{ \
    if (verbose) { \
        termDumper.dumpTerm(context->term, currentTerm()); \
        dataLog(" pos:", input.getPos()); \
    } \
}

#define DUMP_EXTRA(...) \
{ \
    dataLogIf(verbose, " ", __VA_ARGS__); \
}

#define DUMP_EXTRA_IF(predicate, ...) \
{ \
    dataLogIf(verbose && predicate, " ", __VA_ARGS__); \
}

#define DUMP_CURR_CHAR() \
{ \
    if (verbose) { \
        unsigned offset = currentTerm().inputPosition; \
        if (currentTerm().matchDirection() == Backward && currentTerm().atom.quantityType == QuantifierType::FixedCount) \
            offset -= currentTerm().atom.quantityMaxCount - 1; \
        dataLog(" off:", offset, " got:'"); \
        char32_t ch = errorCodePoint; \
        if (input.isValidNegativeInputOffset(offset)) \
            ch = input.readForCharacterDump(offset); \
        if (ch == errorCodePoint) \
            dataLog("<illegal>"); \
        else if (ch < 0x10000 && (ch < 0xd800 || ch > 0xdfff))\
            dataLog(static_cast<char16_t>(ch)); \
        else \
            dataLogF("\\u{%x}", ch); \
        dataLog("'"); \
    } \
}

    JSRegExpResult matchDisjunction(ByteDisjunction* disjunction, DisjunctionContext* context, bool btrack)
    {
        if (!isSafeToRecurse()) [[unlikely]]
            return JSRegExpResult::ErrorNoMemory;

        if (!--remainingMatchCount) {
            dataLogLnIf(verbose, "      Reached match limit - Returning ErrorHitLimit");
            return JSRegExpResult::ErrorHitLimit;
        }

        ByteTermDumper termDumper(compileMode);

        if (btrack)
            BACKTRACK();

        context->matchBegin = input.getPos();
        context->term = 0;

    matchAgain:
        ASSERT(context->term < static_cast<int>(disjunction->terms.size()));

        DUMP_TERM();

        switch (currentTerm().type) {
        case ByteTerm::Type::SubpatternBegin:
            DUMP_EXTRA_IF(currentTerm().capture(), "id:", currentTerm().subpatternId());
            MATCH_NEXT();
        case ByteTerm::Type::SubpatternEnd:
            DUMP_EXTRA_IF(currentTerm().capture(), "id:", currentTerm().subpatternId(), " - Return Match\n");
            context->matchEnd = input.getPos();
            return JSRegExpResult::Match;

        case ByteTerm::Type::BodyAlternativeBegin:
            MATCH_NEXT();
        case ByteTerm::Type::BodyAlternativeDisjunction:
        case ByteTerm::Type::BodyAlternativeEnd:
            context->matchEnd = input.getPos();
            DUMP_EXTRA("- Return Match\n");
            return JSRegExpResult::Match;

        case ByteTerm::Type::AlternativeBegin:
            MATCH_NEXT();
        case ByteTerm::Type::AlternativeDisjunction:
        case ByteTerm::Type::AlternativeEnd: {
            int offset = currentTerm().alternative.end;
            BackTrackInfoAlternative* backTrack = reinterpret_cast<BackTrackInfoAlternative*>(context->frame + currentTerm().frameLocation);
            backTrack->offset = offset;
            context->term += offset;
            MATCH_NEXT();
        }

        case ByteTerm::Type::AssertionBOL:
            if (matchAssertionBOL(currentTerm()))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::AssertionEOL:
            if (matchAssertionEOL(currentTerm()))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::AssertionWordBoundary:
            if (matchAssertionWordBoundary(currentTerm()))
                MATCH_NEXT();
            BACKTRACK();

        case ByteTerm::Type::PatternCharacterOnce:
        case ByteTerm::Type::PatternCharacterFixed: {
            DUMP_CURR_CHAR();
            if (currentTerm().matchDirection() == Forward) {
                if (isEitherUnicodeCompilation()) {
                    if (!U_IS_BMP(currentTerm().atom.patternCharacter)) {
                        for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityMaxCount; ++matchAmount) {
                            if (!checkSurrogatePair(currentTerm(), currentTerm().inputPosition - 2 * matchAmount))
                                BACKTRACK();
                        }
                        MATCH_NEXT();
                    }
                }

                unsigned position = input.getPos(); // May need to back out reading a surrogate pair.

                for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityMaxCount; ++matchAmount) {
                    if (!checkCharacter(currentTerm(), currentTerm().inputPosition - matchAmount)) {
                        input.setPos(position);
                        BACKTRACK();
                    }
                }
            } else {
                auto& term = currentTerm();

                if (isEitherUnicodeCompilation()) {
                    if (!U_IS_BMP(term.atom.patternCharacter)) {
                        for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityMaxCount; ++matchAmount) {
                            auto inputPosition = term.inputPosition - 2 * matchAmount;
                            if (input.getPos() < inputPosition)
                                BACKTRACK();
                            if (!checkSurrogatePair(term, inputPosition))
                                BACKTRACK();
                        }
                        MATCH_NEXT();
                    }
                }

                if (input.getPos() < term.inputPosition)
                    BACKTRACK();

                unsigned position = input.getPos(); // May need to back out reading a surrogate pair.

                for (unsigned matchAmount = 0; matchAmount < term.atom.quantityMaxCount; ++matchAmount) {
                    if (!checkCharacter(term, term.inputPosition + matchAmount + 1 - term.atom.quantityMaxCount)) {
                        input.setPos(position);
                        BACKTRACK();
                    }
                }
            }
            MATCH_NEXT();
        }
        case ByteTerm::Type::PatternCharacterGreedy: {
            DUMP_CURR_CHAR();
            BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + currentTerm().frameLocation);
            unsigned matchAmount = 0;
            unsigned position = input.getPos(); // May need to back out reading a surrogate pair.
            if (currentTerm().matchDirection() == Forward) {
                while ((matchAmount < currentTerm().atom.quantityMaxCount) && input.checkInput(1)) {
                    if (!checkCharacter(currentTerm(), currentTerm().inputPosition + 1)) {
                        input.setPos(position);
                        break;
                    }
                    ++matchAmount;
                    position = input.getPos();
                }
            } else {
                auto& term = currentTerm();
                if (input.getPos() < term.inputPosition)
                    BACKTRACK();

                while ((matchAmount < term.atom.quantityMaxCount) && input.tryUncheckInput(1)) {
                    if (!checkCharacter(currentTerm(), term.inputPosition)) {
                        input.setPos(position);
                        break;
                    }
                    ++matchAmount;
                    position = input.getPos();
                }
            }
            backTrack->matchAmount = matchAmount;

            MATCH_NEXT();
        }

        case ByteTerm::Type::PatternCasedCharacterNonGreedy:
            // Case insensitive matching of unicode characters is handled as Type::CharacterClass.
            ASSERT(!isEitherUnicodeCompilation() || U_IS_BMP(currentTerm().atom.patternCharacter));
            [[fallthrough]];
        case ByteTerm::Type::PatternCharacterNonGreedy: {
            DUMP_CURR_CHAR();
            BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + currentTerm().frameLocation);
            backTrack->begin = input.getPos();
            backTrack->matchAmount = 0;
            MATCH_NEXT();
        }

        case ByteTerm::Type::PatternCasedCharacterOnce:
        case ByteTerm::Type::PatternCasedCharacterFixed: {
            DUMP_CURR_CHAR();
            if (isEitherUnicodeCompilation()) {
                // Case insensitive matching of unicode characters is handled as Type::CharacterClass.
                ASSERT(U_IS_BMP(currentTerm().atom.patternCharacter));

                unsigned position = input.getPos(); // May need to back out reading a surrogate pair.

                if (currentTerm().matchDirection() == Forward) {
                    for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityMaxCount; ++matchAmount) {
                        if (!checkCasedCharacter(currentTerm(), currentTerm().inputPosition - matchAmount)) {
                            input.setPos(position);
                            BACKTRACK();
                        }
                    }
                } else {
                    auto& term = currentTerm();

                    if (input.getPos() < term.inputPosition)
                        BACKTRACK();

                    for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityMaxCount; ++matchAmount) {
                        if (!checkCasedCharacter(term, term.inputPosition - term.atom.quantityMaxCount + matchAmount + 1)) {
                            input.setPos(position);
                            BACKTRACK();
                        }
                    }
                }
                MATCH_NEXT();
            }

            for (unsigned matchAmount = 0; matchAmount < currentTerm().atom.quantityMaxCount; ++matchAmount) {
                if (!checkCasedCharacter(currentTerm(), currentTerm().inputPosition - matchAmount))
                    BACKTRACK();
            }
            MATCH_NEXT();
        }
        case ByteTerm::Type::PatternCasedCharacterGreedy: {
            DUMP_CURR_CHAR();
            BackTrackInfoPatternCharacter* backTrack = reinterpret_cast<BackTrackInfoPatternCharacter*>(context->frame + currentTerm().frameLocation);

            // Case insensitive matching of unicode characters is handled as Type::CharacterClass.
            ASSERT(!isEitherUnicodeCompilation() || U_IS_BMP(currentTerm().atom.patternCharacter));

            if (currentTerm().matchDirection() == Forward) {
                unsigned matchAmount = 0;
                while ((matchAmount < currentTerm().atom.quantityMaxCount) && input.checkInput(1)) {
                    if (!checkCasedCharacter(currentTerm(), currentTerm().inputPosition + 1)) {
                        input.uncheckInput(1);
                        break;
                    }
                    ++matchAmount;
                }
                backTrack->matchAmount = matchAmount;

                MATCH_NEXT();
            } else {
                auto& term = currentTerm();

                if (input.getPos() < term.inputPosition)
                    BACKTRACK();

                unsigned position = input.getPos();
                unsigned matchAmount = 0;
                while ((matchAmount < term.atom.quantityMaxCount) && input.tryUncheckInput(1)) {
                    if (!checkCasedCharacter(term, term.inputPosition)) {
                        input.setPos(position);
                        break;
                    }

                    ++matchAmount;
                    position = input.getPos();
                }
                backTrack->matchAmount = matchAmount;

                MATCH_NEXT();
            }
        }

        case ByteTerm::Type::CharacterClass:
            DUMP_CURR_CHAR();
            if (matchCharacterClass(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::BackReference:
            if (matchBackReference(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpattern: {
            DUMP_EXTRA("\n");
            JSRegExpResult result = matchParentheses(currentTerm(), context);

            if (result == JSRegExpResult::Match) {
                DUMP_EXTRA("     ParenthesesSubpattern");
                MATCH_NEXT();
            }  else if (result != JSRegExpResult::NoMatch)
                return result;

            DUMP_EXTRA("     ParenthesesSubpattern");
            BACKTRACK();
        }
        case ByteTerm::Type::ParenthesesSubpatternOnceBegin:
            if (matchParenthesesOnceBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpatternOnceEnd:
            if (matchParenthesesOnceEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpatternTerminalBegin:
            if (matchParenthesesTerminalBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpatternTerminalEnd:
            if (matchParenthesesTerminalEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParentheticalAssertionBegin:
            if (matchParentheticalAssertionBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParentheticalAssertionEnd:
            if (matchParentheticalAssertionEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();

        case ByteTerm::Type::CheckInput:
            DUMP_EXTRA("count:", currentTerm().checkInputCount);
            if (input.checkInput(currentTerm().checkInputCount))
                MATCH_NEXT();
            BACKTRACK();

        case ByteTerm::Type::UncheckInput:
            DUMP_EXTRA("count:", currentTerm().checkInputCount);
            input.uncheckInput(currentTerm().checkInputCount);
            MATCH_NEXT();

        case ByteTerm::Type::HaveCheckedInput:
            DUMP_EXTRA("count:", currentTerm().checkInputCount);
            if (input.isValidNegativeInputOffset(currentTerm().checkInputCount))
                MATCH_NEXT();
            BACKTRACK();

        case ByteTerm::Type::DotStarEnclosure:
            if (matchDotStarEnclosure(currentTerm(), context)) {
                DUMP_EXTRA("- Return Match\n");
                return JSRegExpResult::Match;
            }
            BACKTRACK();
        }

        // We should never fall-through to here.
        RELEASE_ASSERT_NOT_REACHED();

    backtrack:
        ASSERT(context->term < static_cast<int>(disjunction->terms.size()));

        DUMP_TERM();

        switch (currentTerm().type) {
        case ByteTerm::Type::SubpatternBegin:
            DUMP_EXTRA("id:", currentTerm().subpatternId(), " - Return NoMatch\n");
            return JSRegExpResult::NoMatch;
        case ByteTerm::Type::SubpatternEnd:
            RELEASE_ASSERT_NOT_REACHED();

        case ByteTerm::Type::BodyAlternativeBegin:
        case ByteTerm::Type::BodyAlternativeDisjunction: {
            int offset = currentTerm().alternative.next;
            context->term += offset;
            if (offset > 0)
                MATCH_NEXT();

            if (input.atEnd() || pattern->sticky()) {
                DUMP_EXTRA("- Return NoMatch\n");
                return JSRegExpResult::NoMatch;
            }

            input.next();

            context->matchBegin = input.getPos();

            while (currentTerm().alternative.onceThrough) {
                int next = currentTerm().alternative.next;
                if (next <= 0) {
                    DUMP_EXTRA("- Return NoMatch\n");
                    return JSRegExpResult::NoMatch;
                }
                context->term += next;
            }

            MATCH_NEXT();
        }
        case ByteTerm::Type::BodyAlternativeEnd:
            RELEASE_ASSERT_NOT_REACHED();

        case ByteTerm::Type::AlternativeBegin:
        case ByteTerm::Type::AlternativeDisjunction: {
            int offset = currentTerm().alternative.next;
            context->term += offset;
            if (offset > 0)
                MATCH_NEXT();
            BACKTRACK();
        }
        case ByteTerm::Type::AlternativeEnd: {
            // We should never backtrack back into an alternative of the main body of the regex.
            BackTrackInfoAlternative* backTrack = reinterpret_cast<BackTrackInfoAlternative*>(context->frame + currentTerm().frameLocation);
            unsigned offset = backTrack->offset;
            context->term -= offset;
            BACKTRACK();
        }

        case ByteTerm::Type::AssertionBOL:
        case ByteTerm::Type::AssertionEOL:
        case ByteTerm::Type::AssertionWordBoundary:
            BACKTRACK();

        case ByteTerm::Type::PatternCharacterOnce:
        case ByteTerm::Type::PatternCharacterFixed:
        case ByteTerm::Type::PatternCharacterGreedy:
        case ByteTerm::Type::PatternCharacterNonGreedy:
            if (backtrackPatternCharacter(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::PatternCasedCharacterOnce:
        case ByteTerm::Type::PatternCasedCharacterFixed:
        case ByteTerm::Type::PatternCasedCharacterGreedy:
        case ByteTerm::Type::PatternCasedCharacterNonGreedy:
            if (backtrackPatternCasedCharacter(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::CharacterClass:
            if (backtrackCharacterClass(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::BackReference:
            if (backtrackBackReference(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpattern: {
            JSRegExpResult result = backtrackParentheses(currentTerm(), context);

            if (result == JSRegExpResult::Match) {
                MATCH_NEXT();
            } else if (result != JSRegExpResult::NoMatch)
                return result;

            BACKTRACK();
        }
        case ByteTerm::Type::ParenthesesSubpatternOnceBegin:
            if (backtrackParenthesesOnceBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpatternOnceEnd:
            if (backtrackParenthesesOnceEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpatternTerminalBegin:
            if (backtrackParenthesesTerminalBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParenthesesSubpatternTerminalEnd:
            if (backtrackParenthesesTerminalEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParentheticalAssertionBegin:
            if (backtrackParentheticalAssertionBegin(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();
        case ByteTerm::Type::ParentheticalAssertionEnd:
            if (backtrackParentheticalAssertionEnd(currentTerm(), context))
                MATCH_NEXT();
            BACKTRACK();

        case ByteTerm::Type::CheckInput:
            DUMP_EXTRA("count:", currentTerm().checkInputCount);
            input.uncheckInput(currentTerm().checkInputCount);
            BACKTRACK();

        case ByteTerm::Type::UncheckInput:
            DUMP_EXTRA("count:", currentTerm().checkInputCount);
            input.checkInput(currentTerm().checkInputCount);
            BACKTRACK();

        case ByteTerm::Type::HaveCheckedInput:
            DUMP_EXTRA("count:", currentTerm().checkInputCount);
            BACKTRACK();

        case ByteTerm::Type::DotStarEnclosure:
            RELEASE_ASSERT_NOT_REACHED();
        }

        RELEASE_ASSERT_NOT_REACHED();
        return JSRegExpResult::ErrorNoMatch;
    }

    JSRegExpResult matchNonZeroDisjunction(ByteDisjunction* disjunction, DisjunctionContext* context, bool btrack)
    {
        JSRegExpResult result = matchDisjunction(disjunction, context, btrack);

        if (result == JSRegExpResult::Match) {
            while (context->matchBegin == context->matchEnd) {
                result = matchDisjunction(disjunction, context, /* btrack= */ true);
                if (result != JSRegExpResult::Match)
                    return result;
            }
            return JSRegExpResult::Match;
        }

        return result;
    }

    // WTF_IGNORES_THREAD_SAFETY_ANALYSIS because this function does conditional locking.
    unsigned interpret() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        // FIXME: https://bugs.webkit.org/show_bug.cgi?id=195970
        // [Yarr Interpreter] The interpreter doesn't have checks for stack overflow due to deep recursion
        if (!input.isAvailableInput(0))
            return offsetNoMatch;

        if (pattern->hasEndAnchoredFixedSize() && input.end() >= pattern->m_endAnchoredFixedSize)
            input.setPos(std::max(input.getPos(), input.end() - pattern->m_endAnchoredFixedSize));

        ConcurrentJSLocker locker(pattern->m_lock);

        for (unsigned i = 0; i < pattern->m_body->m_numSubpatterns + 1; ++i)
            output[i << 1] = offsetNoMatch;

        for (unsigned i = pattern->m_offsetVectorBaseForNamedCaptures; i < pattern->m_offsetsSize; ++i)
            output[i] = 0;

        allocatorPool = pattern->m_allocator->startAllocator(Options::maxRegExpStackSize());
        RELEASE_ASSERT(allocatorPool);

        DisjunctionContext* context = allocDisjunctionContext(pattern->m_body.get());
        if (!context) [[unlikely]]
            return offsetNoMatch;

        dataLogLnIf(verbose, "  Interpret input: ", input, "\n  Matching");

        JSRegExpResult result = matchDisjunction(pattern->m_body.get(), context, /* btrack= */ false);
        if (result == JSRegExpResult::Match) {
            output[0] = context->matchBegin;
            output[1] = context->matchEnd;
        }

        freeDisjunctionContext(context);

        // Keep a few overflow pools mapped between matches: patterns whose backtracking contexts
        // outgrow the head page would otherwise map and unmap them around every match.
        static constexpr size_t retainedAllocatorOverflowBytes = 64 * KB;
        pattern->m_allocator->stopAllocator(retainedAllocatorOverflowBytes);

        ASSERT((result == JSRegExpResult::Match) == (output[0] != offsetNoMatch));

        return output[0];
    }

    Interpreter(BytecodePattern* pattern, unsigned* output, std::span<const CharType> input, unsigned start)
        : pattern(pattern)
        , compileMode(pattern->compileMode())
        , output(output)
        , input(input, start, pattern->eitherUnicode())
        , startOffset(start)
        , noNewlineBefore(start)
        , remainingMatchCount(matchLimit)
    {
    }

private:
    inline bool LEGACY_NODELETE isLegacyCompilation() const { return compileMode == CompileMode::Legacy; }
    inline bool LEGACY_NODELETE isUnicodeCompilation() const { return compileMode == CompileMode::Unicode; }
    inline bool LEGACY_NODELETE isUnicodeSetsCompilation() const { return compileMode == CompileMode::UnicodeSets; }
    inline bool LEGACY_NODELETE isEitherUnicodeCompilation() const { return isUnicodeCompilation() || isUnicodeSetsCompilation(); }

    inline bool isSafeToRecurse() { return m_stackCheck.isSafeToRecurse(); }

    BytecodePattern* pattern;
    CompileMode compileMode;
    unsigned* output;
    InputStream input;
    StackCheck m_stackCheck;
    WTF::BumpPointerPool* allocatorPool { nullptr };
    unsigned startOffset;
    static constexpr unsigned bolUnsatisfiable = std::numeric_limits<unsigned>::max();
    unsigned noNewlineBefore; // see matchDotStarEnclosure
    unsigned remainingMatchCount;
};

class ByteCompiler {
    struct ParenthesesStackEntry {
        unsigned beginTerm;
        unsigned savedAlternativeIndex;
        ParenthesesStackEntry(unsigned beginTerm, unsigned savedAlternativeIndex/*, unsigned subpatternId, bool capture = false*/)
            : beginTerm(beginTerm)
            , savedAlternativeIndex(savedAlternativeIndex)
        {
        }
    };

public:
    ByteCompiler(YarrPattern& pattern)
        : m_pattern(pattern)
        , m_currentFlags(pattern.m_flags)
    {
    }

    std::unique_ptr<BytecodePattern> compile(BumpPointerAllocator* allocator, ConcurrentJSLock* lock, ErrorCode& errorCode)
    {
        if (!isSafeToRecurse()) [[unlikely]] {
            errorCode = ErrorCode::TooManyDisjunctions;
            return nullptr;
        }

        regexBegin(m_pattern.m_numSubpatterns, m_pattern.m_body->m_callFrameSize, m_pattern.m_body->m_alternatives[0]->onceThrough());
        if (auto error = emitDisjunction(m_pattern.m_body, 0, 0)) {
            errorCode = error.value();
            return nullptr;
        }
        regexEnd();

#ifdef ASSERT_ENABLED
        if (Options::dumpCompiledRegExpPatterns())
            ByteTermDumper(&m_pattern).dumpDisjunction(m_bodyDisjunction.get());
#endif

        return makeUnique<BytecodePattern>(WTF::move(m_bodyDisjunction), m_allParenthesesInfo, m_pattern, allocator, lock, m_pattern.offsetVectorBaseForNamedCaptures(), m_pattern.offsetsSize());
    }

    void checkInput(unsigned count)
    {
        m_bodyDisjunction->terms.append(ByteTerm::CheckInput(count, { }));
    }

    void uncheckInput(unsigned count)
    {
        m_bodyDisjunction->terms.append(ByteTerm::UncheckInput(count, { }));
    }

    void haveCheckedInput(unsigned count)
    {
        m_bodyDisjunction->terms.append(ByteTerm::HaveCheckedInput(count, { }));
    }

    void assertionBOL(unsigned inputPosition, OptionSet<Flags> flags)
    {
        m_bodyDisjunction->terms.append(ByteTerm::BOL(inputPosition, flags));
    }

    void assertionEOL(unsigned inputPosition, OptionSet<Flags> flags)
    {
        m_bodyDisjunction->terms.append(ByteTerm::EOL(inputPosition, flags));
    }

    void assertionWordBoundary(bool invert, MatchDirection matchDirection, unsigned inputPosition, OptionSet<Flags> flags)
    {
        m_bodyDisjunction->terms.append(ByteTerm::WordBoundary(invert, matchDirection, inputPosition, flags));
    }

    void atomPatternCharacter(char32_t ch, MatchDirection matchDirection, unsigned inputPosition, unsigned frameLocation, Checked<unsigned> quantityMaxCount, QuantifierType quantityType, OptionSet<Flags> flags)
    {
        // For case-insesitive compares, non-ascii characters that have different
        // upper & lower case representations are converted to a character class.
        ASSERT(!flags.contains(Flags::IgnoreCase) || isASCIIAlpha(ch) || isCanonicallyUnique(ch, m_pattern.eitherUnicode() ? CanonicalMode::Unicode : CanonicalMode::UCS2));
        if (flags.contains(Flags::IgnoreCase) && isASCIIAlpha(ch)) {
            auto lo = toASCIILower(static_cast<Latin1Character>(ch));
            auto hi = toASCIIUpper(static_cast<Latin1Character>(ch));
            if (lo != hi) {
                m_bodyDisjunction->terms.append(ByteTerm(lo, hi, inputPosition, frameLocation, quantityMaxCount, quantityType, flags));
                m_bodyDisjunction->terms.last().m_matchDirection = matchDirection;
                return;
            }
        }

        m_bodyDisjunction->terms.append(ByteTerm(ch, inputPosition, frameLocation, quantityMaxCount, quantityType, flags));
        m_bodyDisjunction->terms.last().m_matchDirection = matchDirection;
    }

    void atomCharacterClass(CharacterClass* characterClass, bool invert, MatchDirection matchDirection, unsigned inputPosition, unsigned frameLocation, Checked<unsigned> quantityMaxCount, QuantifierType quantityType, OptionSet<Flags> flags)
    {
        m_bodyDisjunction->terms.append(ByteTerm(characterClass, invert, inputPosition, flags));

        if (quantityType != QuantifierType::FixedCount)
            m_bodyDisjunction->terms.last().atom.quantityMinCount = 0;
        m_bodyDisjunction->terms.last().atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms.last().atom.quantityType = quantityType;
        m_bodyDisjunction->terms.last().frameLocation = frameLocation;
        m_bodyDisjunction->terms.last().m_matchDirection = matchDirection;
    }

    void atomBackReference(bool isNamed, unsigned subpatternId, MatchDirection matchDirection, unsigned inputPosition, unsigned frameLocation, Checked<unsigned> quantityMaxCount, QuantifierType quantityType, OptionSet<Flags> flags)
    {
        ASSERT(subpatternId);

        m_bodyDisjunction->terms.append(ByteTerm::BackReference(subpatternId, matchDirection, inputPosition, flags));

        if (isNamed && m_pattern.hasDuplicateNamedCaptureGroups()) {
            auto duplicateNamedGroupId = m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId];
            if (duplicateNamedGroupId)
                m_bodyDisjunction->terms.last().atom.parenIds.duplicateNamedGroupId = duplicateNamedGroupId;
        }

        m_bodyDisjunction->terms.last().atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms.last().atom.quantityType = quantityType;
        m_bodyDisjunction->terms.last().frameLocation = frameLocation;
    }

    void atomParenthesesOnceBegin(unsigned subpatternId, MatchDirection matchDirection, bool capture, unsigned inputPosition, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        unsigned beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::Type::ParenthesesSubpatternOnceBegin, subpatternId, capture, false, matchDirection, inputPosition, m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin(m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParenthesesTerminalBegin(unsigned subpatternId, MatchDirection matchDirection, bool capture, unsigned inputPosition, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        unsigned beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::Type::ParenthesesSubpatternTerminalBegin, subpatternId, capture, false, matchDirection, inputPosition, m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin(m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParenthesesSubpatternBegin(unsigned subpatternId, MatchDirection matchDirection, bool capture, unsigned inputPosition, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        // Errrk! - this is a little crazy, we initially generate as a Type::ParenthesesSubpatternOnceBegin,
        // then fix this up at the end! - simplifying this should make it much clearer.
        // https://bugs.webkit.org/show_bug.cgi?id=50136

        unsigned beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::Type::ParenthesesSubpatternOnceBegin, subpatternId, capture, false, matchDirection, inputPosition, m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin(m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParentheticalAssertionBegin(unsigned subpatternId, bool invert, MatchDirection matchDirection, unsigned frameLocation, unsigned alternativeFrameLocation)
    {
        unsigned beginTerm = m_bodyDisjunction->terms.size();

        m_bodyDisjunction->terms.append(ByteTerm::ParentheticalAssertionBegin(subpatternId, invert, matchDirection, m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = frameLocation;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeBegin(m_currentFlags));
        m_bodyDisjunction->terms.last().frameLocation = alternativeFrameLocation;

        m_parenthesesStack.append(ParenthesesStackEntry(beginTerm, m_currentAlternativeIndex));
        m_currentAlternativeIndex = beginTerm + 1;
    }

    void atomParentheticalAssertionEnd(unsigned lastSubpatternId, unsigned frameLocation, Checked<unsigned> quantityMaxCount, QuantifierType quantityType)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::ParentheticalAssertionBegin);

        bool invert = m_bodyDisjunction->terms[beginTerm].invert();
        MatchDirection matchDirection = m_bodyDisjunction->terms[beginTerm].matchDirection();
        unsigned subpatternId = m_bodyDisjunction->terms[beginTerm].subpatternId();

        m_bodyDisjunction->terms.append(ByteTerm::ParentheticalAssertionEnd(subpatternId, lastSubpatternId, invert, matchDirection, m_currentFlags));
        m_bodyDisjunction->terms[beginTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].frameLocation = frameLocation;

        m_bodyDisjunction->terms[beginTerm].atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[endTerm].atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityType = quantityType;
    }

    void assertionDotStarEnclosure(bool bolAnchored, bool eolAnchored)
    {
        m_bodyDisjunction->terms.append(ByteTerm::DotStarEnclosure(bolAnchored, eolAnchored, m_currentFlags));
    }

    unsigned LEGACY_NODELETE popParenthesesStack()
    {
        ASSERT(m_parenthesesStack.size());
        unsigned beginTerm = m_parenthesesStack.last().beginTerm;
        m_currentAlternativeIndex = m_parenthesesStack.last().savedAlternativeIndex;
        m_parenthesesStack.removeLast();

        ASSERT(beginTerm < m_bodyDisjunction->terms.size());
        ASSERT(m_currentAlternativeIndex < m_bodyDisjunction->terms.size());

        return beginTerm;
    }

    void closeAlternative(unsigned beginTerm)
    {
        unsigned origBeginTerm = beginTerm;
        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::AlternativeBegin);
        unsigned endIndex = m_bodyDisjunction->terms.size();

        unsigned frameLocation = m_bodyDisjunction->terms[beginTerm].frameLocation;

        if (!m_bodyDisjunction->terms[beginTerm].alternative.next)
            m_bodyDisjunction->terms.removeAt(beginTerm);
        else {
            while (m_bodyDisjunction->terms[beginTerm].alternative.next) {
                beginTerm += m_bodyDisjunction->terms[beginTerm].alternative.next;
                ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::AlternativeDisjunction);
                m_bodyDisjunction->terms[beginTerm].alternative.end = endIndex - beginTerm;
                m_bodyDisjunction->terms[beginTerm].frameLocation = frameLocation;
            }

            m_bodyDisjunction->terms[beginTerm].alternative.next = origBeginTerm - beginTerm;

            m_bodyDisjunction->terms.append(ByteTerm::AlternativeEnd(m_currentFlags));
            m_bodyDisjunction->terms[endIndex].frameLocation = frameLocation;
        }
    }

    void closeBodyAlternative()
    {
        unsigned beginTerm = 0;
        unsigned origBeginTerm = 0;
        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::BodyAlternativeBegin);
        unsigned endIndex = m_bodyDisjunction->terms.size();

        unsigned frameLocation = m_bodyDisjunction->terms[beginTerm].frameLocation;

        while (m_bodyDisjunction->terms[beginTerm].alternative.next) {
            beginTerm += m_bodyDisjunction->terms[beginTerm].alternative.next;
            ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::BodyAlternativeDisjunction);
            m_bodyDisjunction->terms[beginTerm].alternative.end = endIndex - beginTerm;
            m_bodyDisjunction->terms[beginTerm].frameLocation = frameLocation;
        }

        m_bodyDisjunction->terms[beginTerm].alternative.next = origBeginTerm - beginTerm;

        m_bodyDisjunction->terms.append(ByteTerm::BodyAlternativeEnd(m_currentFlags));
        m_bodyDisjunction->terms[endIndex].frameLocation = frameLocation;
    }

    void atomParenthesesSubpatternEnd(unsigned lastSubpatternId, unsigned inputPosition, unsigned frameLocation, Checked<unsigned> quantityMinCount, Checked<unsigned> quantityMaxCount, QuantifierType quantityType, unsigned callFrameSize = 0)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::ParenthesesSubpatternOnceBegin);

        ByteTerm& parenthesesBegin = m_bodyDisjunction->terms[beginTerm];

        auto parenthesesMatchDirection = parenthesesBegin.matchDirection();
        bool capture = parenthesesBegin.capture();
        unsigned subpatternId = parenthesesBegin.subpatternId();

        unsigned numSubpatterns = lastSubpatternId - subpatternId + 1;
        auto parenthesesDisjunction = makeUnique<ByteDisjunction>(numSubpatterns, callFrameSize);

        unsigned firstTermInParentheses = beginTerm + 1;
        parenthesesDisjunction->terms.reserveInitialCapacity(endTerm - firstTermInParentheses + 2);

        parenthesesDisjunction->terms.append(ByteTerm::SubpatternBegin(m_currentFlags));
        for (unsigned termInParentheses = firstTermInParentheses; termInParentheses < endTerm; ++termInParentheses)
            parenthesesDisjunction->terms.append(m_bodyDisjunction->terms[termInParentheses]);
        parenthesesDisjunction->terms.append(ByteTerm::SubpatternEnd(m_currentFlags));

        m_bodyDisjunction->terms.shrink(beginTerm);

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::Type::ParenthesesSubpattern, subpatternId, parenthesesDisjunction.get(), capture, inputPosition, m_currentFlags));
        m_bodyDisjunction->terms.last().m_matchDirection = parenthesesMatchDirection;
        m_allParenthesesInfo.append(WTF::move(parenthesesDisjunction));

        if (m_pattern.hasDuplicateNamedCaptureGroups() && capture) {
            auto duplicateNamedGroupId = m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId];
            if (duplicateNamedGroupId)
                m_bodyDisjunction->terms[beginTerm].atom.parenIds.duplicateNamedGroupId = duplicateNamedGroupId;
        }

        m_bodyDisjunction->terms[beginTerm].atom.quantityMinCount = quantityMinCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[beginTerm].frameLocation = frameLocation;
    }

    void atomParenthesesOnceEnd(unsigned inputPosition, unsigned frameLocation, Checked<unsigned> quantityMinCount, Checked<unsigned> quantityMaxCount, QuantifierType quantityType)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::ParenthesesSubpatternOnceBegin);

        bool capture = m_bodyDisjunction->terms[beginTerm].capture();
        unsigned subpatternId = m_bodyDisjunction->terms[beginTerm].subpatternId();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::Type::ParenthesesSubpatternOnceEnd, subpatternId, capture, false, inputPosition, m_currentFlags));
        if (m_bodyDisjunction->terms[beginTerm].matchDirection() == Backward) {
            // Swap input positions for backward captures.
            m_bodyDisjunction->terms[endTerm].inputPosition = m_bodyDisjunction->terms[beginTerm].inputPosition;
            m_bodyDisjunction->terms[beginTerm].inputPosition = inputPosition;
        }

        if (m_pattern.hasDuplicateNamedCaptureGroups() && m_bodyDisjunction->terms[beginTerm].capture()) {
            auto duplicateNamedGroupId = m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId];
            if (duplicateNamedGroupId) {
                m_bodyDisjunction->terms[endTerm].atom.parenIds.duplicateNamedGroupId = duplicateNamedGroupId;
                m_bodyDisjunction->terms[beginTerm].atom.parenIds.duplicateNamedGroupId = duplicateNamedGroupId;
            }
        }

        m_bodyDisjunction->terms[beginTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].frameLocation = frameLocation;
        m_bodyDisjunction->terms[endTerm].m_matchDirection = m_bodyDisjunction->terms[beginTerm].matchDirection();

        m_bodyDisjunction->terms[beginTerm].atom.quantityMinCount = quantityMinCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[endTerm].atom.quantityMinCount = quantityMinCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityType = quantityType;
    }

    void atomParenthesesTerminalEnd(unsigned inputPosition, unsigned frameLocation, Checked<unsigned> quantityMinCount, Checked<unsigned> quantityMaxCount, QuantifierType quantityType)
    {
        unsigned beginTerm = popParenthesesStack();
        closeAlternative(beginTerm + 1);
        unsigned endTerm = m_bodyDisjunction->terms.size();

        ASSERT(m_bodyDisjunction->terms[beginTerm].type == ByteTerm::Type::ParenthesesSubpatternTerminalBegin);

        if (m_bodyDisjunction->terms[beginTerm].matchDirection() == Backward)
            inputPosition = 0;
        bool capture = m_bodyDisjunction->terms[beginTerm].capture();
        unsigned subpatternId = m_bodyDisjunction->terms[beginTerm].subpatternId();

        m_bodyDisjunction->terms.append(ByteTerm(ByteTerm::Type::ParenthesesSubpatternTerminalEnd, subpatternId, capture, false, inputPosition, m_currentFlags));
        m_bodyDisjunction->terms[beginTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].atom.parenthesesWidth = endTerm - beginTerm;
        m_bodyDisjunction->terms[endTerm].frameLocation = frameLocation;

        if (m_pattern.hasDuplicateNamedCaptureGroups() && m_bodyDisjunction->terms[beginTerm].capture()) {
            auto duplicateNamedGroupId = m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId];
            if (duplicateNamedGroupId) {
                m_bodyDisjunction->terms[endTerm].atom.parenIds.duplicateNamedGroupId = duplicateNamedGroupId;
                m_bodyDisjunction->terms[beginTerm].atom.parenIds.duplicateNamedGroupId = duplicateNamedGroupId;
            }
        }

        m_bodyDisjunction->terms[beginTerm].atom.quantityMinCount = quantityMinCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms[beginTerm].atom.quantityType = quantityType;
        m_bodyDisjunction->terms[endTerm].atom.quantityMinCount = quantityMinCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityMaxCount = quantityMaxCount;
        m_bodyDisjunction->terms[endTerm].atom.quantityType = quantityType;
    }

    void regexBegin(unsigned numSubpatterns, unsigned callFrameSize, bool onceThrough)
    {
        m_bodyDisjunction = makeUnique<ByteDisjunction>(numSubpatterns, callFrameSize);
        m_bodyDisjunction->terms.append(ByteTerm::BodyAlternativeBegin(onceThrough, m_currentFlags));
        m_bodyDisjunction->terms[0].frameLocation = 0;
        m_currentAlternativeIndex = 0;
    }

    void regexEnd()
    {
        closeBodyAlternative();
    }

    void alternativeBodyDisjunction(bool onceThrough)
    {
        unsigned newAlternativeIndex = m_bodyDisjunction->terms.size();
        m_bodyDisjunction->terms[m_currentAlternativeIndex].alternative.next = newAlternativeIndex - m_currentAlternativeIndex;
        m_bodyDisjunction->terms.append(ByteTerm::BodyAlternativeDisjunction(onceThrough, m_currentFlags));

        m_currentAlternativeIndex = newAlternativeIndex;
    }

    void alternativeDisjunction()
    {
        unsigned newAlternativeIndex = m_bodyDisjunction->terms.size();
        m_bodyDisjunction->terms[m_currentAlternativeIndex].alternative.next = newAlternativeIndex - m_currentAlternativeIndex;
        m_bodyDisjunction->terms.append(ByteTerm::AlternativeDisjunction(m_currentFlags));

        m_currentAlternativeIndex = newAlternativeIndex;
    }

    [[nodiscard]] std::optional<ErrorCode> emitDisjunction(PatternDisjunction* disjunction, CheckedUint32 inputCountAlreadyChecked, unsigned parenthesesInputCountAlreadyChecked, MatchDirection matchDirection = Forward)
    {
        if (!isSafeToRecurse()) [[unlikely]]
            return ErrorCode::TooManyDisjunctions;

        for (unsigned alt = 0; alt < disjunction->m_alternatives.size(); ++alt) {
            auto currentCountAlreadyChecked = inputCountAlreadyChecked;

            PatternAlternative* alternative = disjunction->m_alternatives[alt].get();

            if (alt) {
                if (disjunction == m_pattern.m_body)
                    alternativeBodyDisjunction(alternative->onceThrough());
                else
                    alternativeDisjunction();
            }

            unsigned minimumSize = alternative->m_minimumSize;
            ASSERT(matchDirection == Backward || minimumSize >= parenthesesInputCountAlreadyChecked);

            unsigned countToCheck = 0;
            unsigned backwardUncheckAmount = 0;

            if (matchDirection == Forward)
                countToCheck = minimumSize - parenthesesInputCountAlreadyChecked;
            else {
                // Backward case
                unsigned minAlreadyChecked = std::min(disjunction->m_minimumSize, parenthesesInputCountAlreadyChecked);
                if (minimumSize > minAlreadyChecked) {
                    countToCheck = minimumSize - minAlreadyChecked;
                    auto checkedInput = countToCheck + currentCountAlreadyChecked;
                    if (checkedInput.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;
                    haveCheckedInput(checkedInput);

                    if (minimumSize > disjunction->m_minimumSize)
                        backwardUncheckAmount = countToCheck;
                    else
                        backwardUncheckAmount = minimumSize;
                }
            }

            if (countToCheck) {
                if (matchDirection == Forward)
                    checkInput(countToCheck);

                currentCountAlreadyChecked += countToCheck;
                if (currentCountAlreadyChecked.hasOverflowed())
                    return ErrorCode::OffsetTooLarge;
            }

            auto termCount = alternative->m_terms.size();
            for (unsigned i = 0; i < termCount; ++i) {
                unsigned termIndex = matchDirection == Forward ? i : termCount - 1 - i;
                auto& term = alternative->m_terms[termIndex];

                switch (term.type) {
                case PatternTerm::Type::AssertionBOL: {
                    auto currentInputPosition = currentCountAlreadyChecked - term.inputPosition;
                    if (currentInputPosition.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;
                    assertionBOL(currentInputPosition, term.m_currentFlags);
                    break;
                }

                case PatternTerm::Type::AssertionEOL: {
                    auto currentInputPosition = currentCountAlreadyChecked - term.inputPosition;
                    if (currentInputPosition.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;
                    assertionEOL(currentInputPosition, term.m_currentFlags);
                    break;
                }

                case PatternTerm::Type::AssertionWordBoundary: {
                    auto currentInputPosition = currentCountAlreadyChecked - term.inputPosition;
                    if (currentInputPosition.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;
                    assertionWordBoundary(term.invert(), matchDirection, currentInputPosition, term.m_currentFlags);
                    break;
                }

                case PatternTerm::Type::PatternCharacter: {
                    auto currentInputPosition = currentCountAlreadyChecked - term.inputPosition;
                    if (currentInputPosition.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;

                    atomPatternCharacter(term.patternCharacter, matchDirection, currentInputPosition, term.frameLocation, term.quantityMaxCount, term.quantityType, term.m_currentFlags);
                    break;
                }

                case PatternTerm::Type::CharacterClass: {
                    auto currentInputPosition = currentCountAlreadyChecked - term.inputPosition;
                    if (currentInputPosition.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;
                    atomCharacterClass(term.characterClass, term.invert(), matchDirection, currentInputPosition, term.frameLocation, term.quantityMaxCount, term.quantityType, term.m_currentFlags);
                    break;
                }

                case PatternTerm::Type::NumberedBackReference:
                case PatternTerm::Type::NamedBackReference: {
                    auto currentInputPosition = currentCountAlreadyChecked - term.inputPosition;
                    if (currentInputPosition.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;
                    atomBackReference(term.type == PatternTerm::Type::NamedBackReference, term.backReferenceSubpatternId, matchDirection, currentInputPosition, term.frameLocation, term.quantityMaxCount, term.quantityType, term.m_currentFlags);
                    break;
                }

                case PatternTerm::Type::NumberedForwardReference:
                case PatternTerm::Type::NamedForwardReference:
                    break;

                case PatternTerm::Type::ParenthesesSubpattern: {
                    unsigned disjunctionAlreadyCheckedCount = 0;
                    if (term.quantityMaxCount == 1 && !term.parentheses.isCopy) {
                        unsigned alternativeFrameLocation = term.frameLocation;
                        // For QuantifierType::FixedCount we pre-check the minimum size; for greedy/non-greedy we reserve a slot in the frame.
                        if (term.quantityType == QuantifierType::FixedCount)
                            disjunctionAlreadyCheckedCount = term.parentheses.disjunction->m_minimumSize;
                        else
                            alternativeFrameLocation += YarrStackSpaceForBackTrackInfoParenthesesOnce;
                        auto delegateEndInputOffset = currentCountAlreadyChecked - term.inputPosition;
                        if (delegateEndInputOffset.hasOverflowed())
                            return ErrorCode::OffsetTooLarge;
                        atomParenthesesOnceBegin(term.parentheses.subpatternId, matchDirection, term.capture(), disjunctionAlreadyCheckedCount + delegateEndInputOffset, term.frameLocation, alternativeFrameLocation);
                        if (auto error = emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, disjunctionAlreadyCheckedCount, matchDirection))
                            return error;
                        atomParenthesesOnceEnd(delegateEndInputOffset, term.frameLocation, term.quantityMinCount, term.quantityMaxCount, term.quantityType);
                    } else if (term.parentheses.isTerminal) {
                        auto delegateEndInputOffset = currentCountAlreadyChecked - term.inputPosition;
                        if (delegateEndInputOffset.hasOverflowed())
                            return ErrorCode::OffsetTooLarge;
                        atomParenthesesTerminalBegin(term.parentheses.subpatternId, matchDirection, term.capture(), disjunctionAlreadyCheckedCount + delegateEndInputOffset, term.frameLocation, term.frameLocation + YarrStackSpaceForBackTrackInfoParenthesesTerminal);
                        if (auto error = emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, disjunctionAlreadyCheckedCount, matchDirection))
                            return error;
                        atomParenthesesTerminalEnd(delegateEndInputOffset, term.frameLocation, term.quantityMinCount, term.quantityMaxCount, term.quantityType);
                    } else {
                        auto delegateEndInputOffset = currentCountAlreadyChecked - term.inputPosition;
                        if (delegateEndInputOffset.hasOverflowed())
                            return ErrorCode::OffsetTooLarge;
                        atomParenthesesSubpatternBegin(term.parentheses.subpatternId, matchDirection, term.capture(), disjunctionAlreadyCheckedCount + delegateEndInputOffset, term.frameLocation, 0);
                        unsigned inputOffset = 0;
                        if (auto error = emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, inputOffset, matchDirection))
                            return error;
                        atomParenthesesSubpatternEnd(term.parentheses.lastSubpatternId, delegateEndInputOffset, term.frameLocation, term.quantityMinCount, term.quantityMaxCount, term.quantityType, term.parentheses.disjunction->m_callFrameSize);
                    }
                    break;
                }

                case PatternTerm::Type::ParentheticalAssertion: {
                    unsigned alternativeFrameLocation = term.frameLocation + YarrStackSpaceForBackTrackInfoParentheticalAssertion;
                    auto positiveInputOffset = currentCountAlreadyChecked - term.inputPosition;
                    if (positiveInputOffset.hasOverflowed())
                        return ErrorCode::OffsetTooLarge;
                    if (term.matchDirection() == Forward) {
                        unsigned uncheckAmount = 0;
                        if (positiveInputOffset > term.parentheses.disjunction->m_minimumSize) {
                            uncheckAmount = positiveInputOffset - term.parentheses.disjunction->m_minimumSize;
                            uncheckInput(uncheckAmount);
                            currentCountAlreadyChecked -= uncheckAmount;
                            if (currentCountAlreadyChecked.hasOverflowed())
                                return ErrorCode::OffsetTooLarge;
                        }

                        atomParentheticalAssertionBegin(term.parentheses.subpatternId, term.invert(), term.matchDirection(), term.frameLocation, alternativeFrameLocation);
                        if (auto error = emitDisjunction(term.parentheses.disjunction, currentCountAlreadyChecked, positiveInputOffset - uncheckAmount, term.matchDirection()))
                            return error;
                        atomParentheticalAssertionEnd(term.parentheses.lastSubpatternId, term.frameLocation, term.quantityMaxCount, term.quantityType);
                        if (uncheckAmount) {
                            checkInput(uncheckAmount);
                            currentCountAlreadyChecked += uncheckAmount;
                            if (currentCountAlreadyChecked.hasOverflowed())
                                return ErrorCode::OffsetTooLarge;
                        }
                    } else { // Backward
                        CheckedUint32 checkedCountForLookbehind = currentCountAlreadyChecked;
                        ASSERT(checkedCountForLookbehind >= term.inputPosition);
                        checkedCountForLookbehind -= term.inputPosition;
                        if (checkedCountForLookbehind.hasOverflowed())
                            return ErrorCode::OffsetTooLarge;
                        auto minimumSize = term.parentheses.disjunction->m_minimumSize;
                        if (minimumSize) {
                            checkedCountForLookbehind += minimumSize;
                            if (checkedCountForLookbehind.hasOverflowed())
                                return ErrorCode::OffsetTooLarge;
                            if (checkedCountForLookbehind > currentCountAlreadyChecked && !term.invert()) {
                                // Do a quick check for what is required for the lookbehind.
                                // An inverted lookbehind can "match" without processing any input.
                                haveCheckedInput(checkedCountForLookbehind);
                            }
                        }
                        atomParentheticalAssertionBegin(term.parentheses.subpatternId, term.invert(), term.matchDirection(), term.frameLocation, alternativeFrameLocation);

                        if (auto error = emitDisjunction(term.parentheses.disjunction, checkedCountForLookbehind, positiveInputOffset + minimumSize, term.matchDirection()))
                            return error;
                        atomParentheticalAssertionEnd(term.parentheses.lastSubpatternId, term.frameLocation, term.quantityMaxCount, term.quantityType);
                    }
                    break;
                }

                case PatternTerm::Type::DotStarEnclosure:
                    assertionDotStarEnclosure(term.anchors.bolAnchor, term.anchors.eolAnchor);
                    break;
                }
            }

            if (matchDirection == Backward && backwardUncheckAmount)
                uncheckInput(backwardUncheckAmount);
        }
        return std::nullopt;
    }

private:
    inline bool isSafeToRecurse() { return m_stackCheck.isSafeToRecurse(); }

    YarrPattern& m_pattern;
    std::unique_ptr<ByteDisjunction> m_bodyDisjunction;
    StackCheck m_stackCheck;
    unsigned m_currentAlternativeIndex { 0 };
    Vector<ParenthesesStackEntry> m_parenthesesStack;
    Vector<std::unique_ptr<ByteDisjunction>> m_allParenthesesInfo;
    OptionSet<Flags> m_currentFlags;
};

void ByteTermDumper::dumpTerm(size_t idx, ByteTerm term)
{
    PrintStream& out = WTF::dataFile();

    auto outputTermIndexAndNest = [&](size_t index, unsigned termNesting) {
        if (!m_recursiveDump)
            termNesting = 1;

        for (unsigned lineIndent = 0; lineIndent < m_lineIndent; lineIndent++)
            out.print(" ");
        out.printf("%4zu", index);
        for (unsigned nestingDepth = 0; nestingDepth < termNesting; nestingDepth++)
            out.print("  ");
    };

    auto dumpQuantity = [&](ByteTerm& term) {
        if (term.atom.quantityType == QuantifierType::FixedCount) {
            if (term.atom.quantityMaxCount > 1)
                out.print(" {", term.atom.quantityMaxCount, "}");
            return;
        }

        out.print(" {", term.atom.quantityMinCount);
        if (term.atom.quantityMaxCount == UINT_MAX)
            out.print(",inf");
        else
            out.print(",", term.atom.quantityMaxCount);
        out.print("}");

        if (term.atom.quantityType == QuantifierType::Greedy)
            out.print(" greedy");
        else if (term.atom.quantityType == QuantifierType::NonGreedy)
            out.print(" non-greedy");
    };

    auto dumpCaptured = [&](ByteTerm& term) {
        if (term.capture())
            out.print(" captured (#", term.subpatternId(), ")");
    };

    auto dumpInverted = [&](ByteTerm& term) {
        if (term.invert())
            out.print(" inverted");
    };

    auto dumpMatchDirection = [&](ByteTerm& term) {
        if (term.matchDirection() == Backward) {
            if (term.type == ByteTerm::Type::ParentheticalAssertionBegin
                || term.type == ByteTerm::Type::ParentheticalAssertionEnd) {
                out.print(" lookbehind");
            } else
                out.print(" backward");
        }
    };

    auto dumpInputPosition = [&](ByteTerm& term) {
        out.printf(" inputPosition %u", term.inputPosition);
    };

    auto dumpFrameLocation = [&](ByteTerm& term) {
        out.printf(" frameLocation %u", term.frameLocation);
    };

    auto dumpCharacter = [&](ByteTerm& term) {
        out.print(" ");
        dumpChar32(out, term.atom.patternCharacter);
    };

    auto dumpCharClass = [&](ByteTerm& term) {
        out.print(" ");
        dumpCharacterClass(out, m_pattern, term.atom.characterClass);
    };

    switch (term.type) {
    case ByteTerm::Type::BodyAlternativeBegin:
        outputTermIndexAndNest(idx, m_nesting++);
        out.print("BodyAlternativeBegin");
        if (term.alternative.onceThrough)
            out.print(" onceThrough");
        break;
    case ByteTerm::Type::BodyAlternativeDisjunction:
        outputTermIndexAndNest(idx, m_nesting - 1);
        out.print("BodyAlternativeDisjunction");
        break;
    case ByteTerm::Type::BodyAlternativeEnd:
        outputTermIndexAndNest(idx, --m_nesting);
        out.print("BodyAlternativeEnd");
        break;
    case ByteTerm::Type::AlternativeBegin:
        outputTermIndexAndNest(idx, m_nesting++);
        out.print("AlternativeBegin");
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::AlternativeDisjunction:
        outputTermIndexAndNest(idx, m_nesting - 1);
        out.print("AlternativeDisjunction");
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::AlternativeEnd:
        outputTermIndexAndNest(idx, --m_nesting);
        out.print("AlternativeEnd");
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::SubpatternBegin:
        outputTermIndexAndNest(idx, m_nesting++);
        out.print("SubpatternBegin");
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::SubpatternEnd:
        outputTermIndexAndNest(idx, --m_nesting);
        out.print("SubpatternEnd");
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::AssertionBOL:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("AssertionBOL");
        break;
    case ByteTerm::Type::AssertionEOL:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("AssertionEOL");
        break;
    case ByteTerm::Type::AssertionWordBoundary:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("AssertionWordBoundary");
        dumpInverted(term);
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCharacterOnce:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCharacterOnce");
        dumpInverted(term);
        dumpInputPosition(term);
        dumpCharacter(term);
        dumpQuantity(term);
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCharacterFixed:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCharacterFixed");
        dumpInverted(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        dumpCharacter(term);
        out.print(" {", term.atom.quantityMinCount, "}");
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCharacterGreedy:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCharacterGreedy");
        dumpInverted(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        dumpCharacter(term);
        dumpQuantity(term);
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCharacterNonGreedy:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCharacterNonGreedy");
        dumpInverted(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        dumpCharacter(term);
        dumpQuantity(term);
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCasedCharacterOnce:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCasedCharacterOnce");
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCasedCharacterFixed:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCasedCharacterFixed");
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCasedCharacterGreedy:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCasedCharacterGreedy");
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::PatternCasedCharacterNonGreedy:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("PatternCasedCharacterNonGreedy");
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::CharacterClass:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("CharacterClass");
        dumpInverted(term);
        dumpInputPosition(term);
        if (term.atom.quantityType != QuantifierType::FixedCount || eitherUnicode())
            dumpFrameLocation(term);
        dumpCharClass(term);
        dumpQuantity(term);
        dumpMatchDirection(term);
        break;
    case ByteTerm::Type::BackReference:
        //  Need to update this for named capture group back references
        outputTermIndexAndNest(idx, m_nesting);
        out.print("BackReference #", term.subpatternId());
        dumpInputPosition(term);
        dumpQuantity(term);
        break;
    case ByteTerm::Type::ParenthesesSubpattern:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("ParenthesesSubpattern");
        dumpCaptured(term);
        dumpInverted(term);
        dumpMatchDirection(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        dumpQuantity(term);
        if (m_recursiveDump) {
            out.print("\n");
            dumpDisjunction(term.atom.parenthesesDisjunction, m_nesting);
        }
        break;
    case ByteTerm::Type::ParenthesesSubpatternOnceBegin:
        outputTermIndexAndNest(idx, m_nesting++);
        out.print("ParenthesesSubpatternOnceBegin");
        dumpCaptured(term);
        dumpInverted(term);
        dumpMatchDirection(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::ParenthesesSubpatternOnceEnd:
        outputTermIndexAndNest(idx, --m_nesting);
        out.print("ParenthesesSubpatternOnceEnd");
        dumpCaptured(term);
        dumpInverted(term);
        dumpMatchDirection(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::ParenthesesSubpatternTerminalBegin:
        outputTermIndexAndNest(idx, m_nesting++);
        out.print("ParenthesesSubpatternTerminalBegin");
        dumpInverted(term);
        dumpMatchDirection(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::ParenthesesSubpatternTerminalEnd:
        outputTermIndexAndNest(idx, --m_nesting);
        out.print("ParenthesesSubpatternTerminalEnd");
        dumpInverted(term);
        dumpMatchDirection(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::ParentheticalAssertionBegin:
        outputTermIndexAndNest(idx, m_nesting++);
        out.print("ParentheticalAssertionBegin");
        dumpInverted(term);
        dumpMatchDirection(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::ParentheticalAssertionEnd:
        outputTermIndexAndNest(idx, --m_nesting);
        out.print("ParentheticalAssertionEnd");
        dumpInverted(term);
        dumpMatchDirection(term);
        dumpInputPosition(term);
        dumpFrameLocation(term);
        break;
    case ByteTerm::Type::CheckInput:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("CheckInput ", term.checkInputCount);
        break;
    case ByteTerm::Type::UncheckInput:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("UncheckInput ", term.checkInputCount);
        break;
    case ByteTerm::Type::HaveCheckedInput:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("HaveCheckedInput ", term.checkInputCount);
        break;
    case ByteTerm::Type::DotStarEnclosure:
        outputTermIndexAndNest(idx, m_nesting);
        out.print("DotStarEnclosure");
        break;
    }
}

void ByteTermDumper::dumpDisjunction(ByteDisjunction* disjunction, unsigned nesting)
{
    PrintStream& out = WTF::dataFile();

    unsigned savedLineIndent = m_lineIndent;
    if (!nesting) {
        out.printf("ByteDisjunction(%p):\n", disjunction);
        m_recursiveDump = true;
        m_nesting = 1;
    } else
        m_lineIndent = nesting - 1;

    for (size_t idx = 0; idx < disjunction->terms.size(); ++idx) {
        ByteTerm term = disjunction->terms[idx];

        dumpTerm(idx, term);

        if (term.type != ByteTerm::Type::ParenthesesSubpattern)
            out.print("\n");
    }

    m_lineIndent = savedLineIndent;
}

static bool fastSetContainsSlow(const FastSet& set, char32_t character)
{
    const CharacterClass* characterClass = set.characterClass;
    if (!characterClass)
        return character == set.first || character == set.second;

    auto contains = [&] {
        if (characterClass->m_anyCharacter)
            return true;
        bool latin1 = isLatin1(character);
        auto& matches = latin1 ? characterClass->m_matches8 : characterClass->m_matches32;
        auto& ranges = latin1 ? characterClass->m_ranges8 : characterClass->m_ranges32;
        if (std::binary_search(matches.begin(), matches.end(), character))
            return true;
        auto range = std::upper_bound(ranges.begin(), ranges.end(), character, [](char32_t value, const CharacterRange& item) {
            return value < item.begin;
        });
        return range != ranges.begin() && character <= (range - 1)->end;
    };
    return contains() != set.invert;
}

class FastProgramBuilder {
public:
    explicit FastProgramBuilder(YarrPattern& pattern)
        : m_pattern(pattern)
        , m_logCaptures(pattern.m_containsBackreferences)
    {
    }

    std::unique_ptr<FastProgram> build()
    {
        if (m_pattern.eitherUnicode() || m_pattern.m_containsModifiers || m_pattern.m_containsLookbehinds
            || m_pattern.hasDuplicateNamedCaptureGroups() || m_pattern.containsUnsignedLengthPattern())
            return nullptr;

        auto program = makeUnique<FastProgram>();
        m_program = program.get();
        program->numSubpatterns = m_pattern.m_numSubpatterns;
        program->endAnchoredFixedSize = m_pattern.m_endAnchoredFixedSize;
        program->triesOnlyOnce = m_pattern.sticky();
        program->validate = Options::validateRegExpFastMatcher();

        auto& alternatives = m_pattern.m_body->m_alternatives;
        unsigned count = alternatives.size();
        if (alternatives[0]->onceThrough()) {
            count = 0;
            while (count < alternatives.size() && alternatives[count]->onceThrough())
                ++count;
            program->triesOnlyOnce |= count == alternatives.size();
        }

        if (!emitAlternatives(alternatives.span().first(count)))
            return nullptr;
        emit(FastOp::Kind::Match);
        if (m_nextRegister > std::numeric_limits<uint16_t>::max() || m_program->sets.size() > std::numeric_limits<uint16_t>::max())
            return nullptr;
        program->numRegisters = m_nextRegister;

        FirstCharacters first;
        bool canBeEmpty = false;
        m_termsLeftToVisit = termsVisitedForFirstCharacters;
        for (unsigned i = 0; i < count && m_termsLeftToVisit; ++i)
            canBeEmpty |= collectFirstCharacters(*alternatives[i], 0, first);
        canBeEmpty |= !m_termsLeftToVisit;
        if (!canBeEmpty && !first.atEnd && !program->triesOnlyOnce) {
            program->scansForFirstCharacter = true;
            program->firstCharacterMayBeWide = first.wide;
            unsigned count = 0;
            for (unsigned i = 0; i < 4; ++i) {
                program->firstCharacters[i] = first.bits[i];
                count += std::popcount(first.bits[i]);
                if (first.bits[i])
                    program->onlyFirstCharacter = i * 64 + std::countr_zero(first.bits[i]);
            }
            program->hasOnlyOneFirstCharacter = count == 1 && !first.wide;
        }

        if (!program->text.isEmpty()) {
            for (unsigned i = 0; i < sizeof(uint64_t); ++i)
                program->text.append(0);
        }
        program->ops.shrinkToFit();
        program->sets.shrinkToFit();
        program->text.shrinkToFit();
        program->wideText.shrinkToFit();
        return program;
    }

private:
    using Kind = FastOp::Kind;

    unsigned emit(Kind kind, unsigned set = 0, uint32_t argument = 0, uint32_t target = 0)
    {
        FastOp op { };
        op.kind = kind;
        op.set = static_cast<uint16_t>(set);
        op.argument = argument;
        op.target = target;
        m_program->ops.append(op);
        return m_program->ops.size() - 1;
    }

    unsigned here() const { return m_program->ops.size(); }
    void setTarget(unsigned op) { m_program->ops[op].target = here(); }

    unsigned emitChoice()
    {
        m_mayHaveBacktracked = true;
        return emit(Kind::Choice);
    }

    struct FirstCharacters {
        bool isFull() const { return !~(bits[0] & bits[1] & bits[2] & bits[3]); }
        void add(const FirstCharacters& other)
        {
            for (unsigned i = 0; i < 4; ++i)
                bits[i] |= other.bits[i];
            wide |= other.wide;
            atEnd |= other.atEnd;
        }
        void add(const FastSet& set)
        {
            for (unsigned i = 0; i < 4; ++i)
                bits[i] |= set.bits[i];
            wide |= mayContainWideCharacters(set);
        }
        void fill()
        {
            for (auto& word : bits)
                word = ~0ull;
            wide = true;
            atEnd = true;
        }
        bool overlaps(const FastSet& set) const
        {
            if (wide && mayContainWideCharacters(set))
                return true;
            return (bits[0] & set.bits[0]) | (bits[1] & set.bits[1]) | (bits[2] & set.bits[2]) | (bits[3] & set.bits[3]);
        }

        uint64_t bits[4] { };
        bool wide { false };
        bool atEnd { false };
    };

    static bool mayContainWideCharacters(const FastSet& set)
    {
        if (!set.characterClass)
            return set.first > 0xff || set.second > 0xff;
        return set.invert || set.characterClass->m_anyCharacter || !set.characterClass->m_matches32.isEmpty() || !set.characterClass->m_ranges32.isEmpty();
    }

    struct Continuation {
        const PatternAlternative* alternative;
        unsigned nextTermIndex;
        const PatternTerm* repeatedGroup;
        const Continuation* outer;
    };

    unsigned appendRawSet(const FirstCharacters& characters)
    {
        FastSet set { };
        for (unsigned i = 0; i < 4; ++i)
            set.bits[i] = characters.bits[i];
        set.first = 0xffff;
        set.second = characters.atEnd;
        set.invert = true;
        m_program->sets.append(set);
        return m_program->sets.size() - 1;
    }

    unsigned internSet(CharacterClass* characterClass, bool invert, char16_t first, char16_t second)
    {
        auto& sets = m_program->sets;
        for (unsigned i = 0; i < sets.size(); ++i) {
            if (sets[i].characterClass == characterClass && sets[i].invert == invert && sets[i].first == first && sets[i].second == second)
                return i;
        }
        FastSet set { };
        set.characterClass = characterClass;
        set.invert = invert;
        set.first = first;
        set.second = second;
        auto add = [&](char32_t character) {
            if (character < 256)
                set.bits[character >> 6] |= 1ull << (character & 63);
        };
        if (!characterClass) {
            add(first);
            add(second);
        } else if (characterClass->m_anyCharacter) {
            for (auto& word : set.bits)
                word = ~0ull;
        } else {
            for (char32_t character : characterClass->m_matches8)
                add(character);
            for (auto& range : characterClass->m_ranges8) {
                if (range.begin > 0xff)
                    continue;
                unsigned begin = range.begin;
                unsigned end = std::min<char32_t>(range.end, 0xff);
                for (unsigned word = begin >> 6; word <= end >> 6; ++word) {
                    unsigned low = word == begin >> 6 ? begin & 63 : 0;
                    unsigned high = word == end >> 6 ? end & 63 : 63;
                    set.bits[word] |= (~0ull >> (63 - high)) & (~0ull << low);
                }
            }
        }
        if (invert) {
            for (auto& word : set.bits)
                word = ~word;
        }
        sets.append(set);
        return sets.size() - 1;
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
        unsigned low { std::numeric_limits<unsigned>::max() };
        unsigned high { 0 };
    };

    static void addCaptures(const PatternTerm& term, CaptureRange& range)
    {
        if (term.type != PatternTerm::Type::ParenthesesSubpattern && term.type != PatternTerm::Type::ParentheticalAssertion)
            return;
        if (term.type == PatternTerm::Type::ParenthesesSubpattern && term.capture()) {
            range.low = std::min(range.low, term.parentheses.subpatternId);
            range.high = std::max(range.high, term.parentheses.subpatternId);
        }
        for (auto& alternative : term.parentheses.disjunction->m_alternatives) {
            for (auto& nested : alternative->m_terms)
                addCaptures(nested, range);
        }
    }

    void emitClear(const CaptureRange& range)
    {
        if (range.low <= range.high)
            emit(Kind::ClearCaptures, 0, range.low, range.high);
    }

    bool emitAlternatives(std::span<const std::unique_ptr<PatternAlternative>> alternatives)
    {
        if (m_recursionDepth >= 64)
            return false;
        ++m_recursionDepth;
        unsigned count = alternatives.size();
        if (count == 1) {
            bool result = emitAlternative(*alternatives[0]);
            --m_recursionDepth;
            return result;
        }

        if (!m_logCaptures && m_mayHaveBacktracked) {
            CaptureRange all;
            for (auto& alternative : alternatives) {
                for (auto& term : alternative->m_terms)
                    addCaptures(term, all);
            }
            emitClear(all);
        }

        Vector<FirstCharacters, 8> first(count);
        Vector<FirstCharacters, 8> later(count);
        Vector<bool, 8> canBeEmpty(count);
        for (unsigned i = 0; i < count; ++i) {
            m_termsLeftToVisit = termsVisitedForFirstCharacters;
            canBeEmpty[i] = collectFirstCharacters(*alternatives[i], 0, first[i]);
        }
        for (unsigned i = count - 1; i--;) {
            later[i] = later[i + 1];
            later[i].add(first[i + 1]);
            if (canBeEmpty[i + 1])
                later[i].fill();
        }

        CaptureRange earlier;
        Vector<unsigned, 8> jumpsToEnd;
        bool mayHaveBacktrackedAtEntry = m_mayHaveBacktracked;
        for (unsigned i = 0; i < count; ++i) {
            bool isLast = i + 1 == count;
            unsigned choice = 0;
            if (!isLast) {
                if (canBeEmpty[i] || first[i].isFull())
                    choice = emit(Kind::Choice);
                else {
                    unsigned own = appendRawSet(first[i]);
                    appendRawSet(later[i]);
                    choice = emit(Kind::ChoiceByFirst, own);
                }
            }
            m_mayHaveBacktracked = mayHaveBacktrackedAtEntry;
            if (!emitAlternative(*alternatives[i]))
                return false;
            if (isLast)
                break;
            jumpsToEnd.append(emit(Kind::Jump));
            setTarget(choice);
            if (!m_logCaptures) {
                for (auto& term : alternatives[i]->m_terms)
                    addCaptures(term, earlier);
                emitClear(earlier);
            }
            m_program->ops[choice].argument = here();
        }
        for (unsigned jump : jumpsToEnd)
            setTarget(jump);
        m_mayHaveBacktracked = true;
        --m_recursionDepth;
        return true;
    }

    bool emitGroupBody(const PatternTerm& term)
    {
        unsigned slot = term.parentheses.subpatternId << 1;
        auto& alternatives = term.parentheses.disjunction->m_alternatives;
        if (term.capture() && !m_logCaptures && alternatives.size() == 1 && alternatives[0]->matchDirection() == Forward) {
            auto& terms = alternatives[0]->m_terms;
            if (terms.isEmpty()) {
                emit(Kind::CaptureEmpty, 0, slot);
                return true;
            }
            if (auto run = runAt(*alternatives[0], 0); run && run->terms == terms.size() && run->kind == Kind::PossessiveRun && run->max == quantifyInfinite) {
                emit(Kind::CapturedRun, run->set, slot, run->min);
                return true;
            }
        }
        if (term.capture())
            emit(m_logCaptures ? Kind::LoggedCaptureStart : Kind::CaptureStart, 0, slot);
        if (!emitAlternatives(term.parentheses.disjunction->m_alternatives.span()))
            return false;
        if (term.capture())
            emit(m_logCaptures ? Kind::LoggedCaptureEnd : Kind::CaptureEnd, 0, slot + 1);
        return true;
    }

    bool emitOptionalGroup(const PatternTerm& term)
    {
        CaptureRange range;
        if (!m_logCaptures)
            addCaptures(term, range);
        bool canBeEmpty = !term.parentheses.disjunction->m_minimumSize;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;
        if (m_mayHaveBacktracked)
            emitClear(range);

        auto emitChecked = [&] {
            if (canBeEmpty)
                emit(m_logCaptures ? Kind::LoggedSavePosition : Kind::SavePosition, start);
            if (!emitGroupBody(term))
                return false;
            if (canBeEmpty)
                emit(Kind::FailIfEmpty, start);
            return true;
        };

        unsigned choice = emit(Kind::Choice);
        if (term.quantityType == QuantifierType::Greedy) {
            if (!emitChecked())
                return false;
            unsigned jump = emit(Kind::Jump);
            setTarget(choice);
            emitClear(range);
            setTarget(jump);
            m_mayHaveBacktracked = true;
            return true;
        }
        unsigned jump = emit(Kind::Jump);
        setTarget(choice);
        if (!emitChecked())
            return false;
        setTarget(jump);
        m_mayHaveBacktracked = true;
        return true;
    }

    template<typename Body>
    bool emitLoop(unsigned min, unsigned max, bool isLazy, bool canBeEmpty, const CaptureRange& range, const Body& emitBody)
    {
        bool needsCounter = min > 1 || max != quantifyInfinite;
        unsigned counter = needsCounter ? m_nextRegister++ : 0;
        unsigned start = canBeEmpty ? m_nextRegister++ : 0;

        SetForScope logCaptures(m_logCaptures, true);
        m_mayHaveBacktracked = true;

        std::optional<unsigned> jumpToBody;
        std::optional<unsigned> jumpToMandatory;
        Vector<unsigned, 3> jumpsToExit;
        if (needsCounter)
            emit(Kind::LoggedSetRegister, counter, 0);
        else if (min) {
            if (canBeEmpty)
                emit(Kind::LoggedSetRegister, start, offsetNoMatch);
            jumpToBody = emit(Kind::Jump);
        }

        unsigned head = here();
        if (needsCounter && min)
            jumpToMandatory = emit(Kind::JumpIfBelow, counter, min);
        if (min == max)
            jumpsToExit.append(emit(Kind::Jump));
        else {
            if (max != quantifyInfinite)
                jumpsToExit.append(emit(Kind::JumpIfNotBelow, counter, max));
            if (isLazy) {
                unsigned choice = emitChoice();
                jumpsToExit.append(emit(Kind::Jump));
                setTarget(choice);
            } else
                jumpsToExit.append(emitChoice());
            if (canBeEmpty)
                emit(Kind::LoggedSavePosition, start);
        }
        if (jumpToMandatory) {
            if (canBeEmpty) {
                std::optional<unsigned> skip;
                if (min != max)
                    skip = emit(Kind::Jump);
                setTarget(*jumpToMandatory);
                emit(Kind::LoggedSetRegister, start, offsetNoMatch);
                if (skip)
                    setTarget(*skip);
            } else
                setTarget(*jumpToMandatory);
        }
        if (jumpToBody)
            setTarget(*jumpToBody);
        if (needsCounter)
            emit(Kind::LoggedIncrement, counter);
        if (range.low <= range.high)
            emit(Kind::LoggedClearCaptures, 0, range.low, range.high);
        if (!emitBody())
            return false;
        if (canBeEmpty)
            emit(Kind::FailIfEmpty, start);
        emit(Kind::LoopBack, 0, 0, head);
        for (unsigned jump : jumpsToExit)
            setTarget(jump);
        return true;
    }

    bool matchesOnlyTheEmptyString(const PatternDisjunction& disjunction)
    {
        for (auto& alternative : disjunction.m_alternatives) {
            for (auto& term : alternative->m_terms) {
                if (!m_termsLeftToVisit)
                    return false;
                --m_termsLeftToVisit;
                switch (term.type) {
                case PatternTerm::Type::PatternCharacter:
                case PatternTerm::Type::CharacterClass:
                    if (term.quantityMaxCount)
                        return false;
                    break;
                case PatternTerm::Type::ParenthesesSubpattern:
                    if (term.quantityMaxCount && !matchesOnlyTheEmptyString(*term.parentheses.disjunction))
                        return false;
                    break;
                case PatternTerm::Type::NumberedBackReference:
                case PatternTerm::Type::NamedBackReference:
                case PatternTerm::Type::DotStarEnclosure:
                    return false;
                default:
                    break;
                }
            }
        }
        return true;
    }

    bool emitGroup(const PatternTerm& term)
    {
        if (term.parentheses.isCopy || term.matchDirection() != Forward)
            return false;
        if (!term.quantityMaxCount)
            return true;
        if (term.quantityMinCount != 1 || term.quantityMaxCount != 1) {
            m_termsLeftToVisit = termsVisitedForFirstCharacters;
            if (matchesOnlyTheEmptyString(*term.parentheses.disjunction))
                return !term.quantityMinCount || emitGroupBody(term);
        }
        if (term.quantityMaxCount == 1)
            return term.quantityMinCount ? emitGroupBody(term) : emitOptionalGroup(term);
        CaptureRange range;
        addCaptures(term, range);
        return emitLoop(term.quantityMinCount, term.quantityMaxCount, term.quantityType == QuantifierType::NonGreedy, !term.parentheses.disjunction->m_minimumSize, range, [&] {
            return emitGroupBody(term);
        });
    }

    bool emitLookahead(const PatternTerm& term)
    {
        if (term.matchDirection() != Forward)
            return false;
        CaptureRange range;
        addCaptures(term, range);
        unsigned registers = m_nextRegister;
        m_nextRegister += 2;
        auto alternatives = term.parentheses.disjunction->m_alternatives.span();

        if (!term.invert()) {
            emit(Kind::LookaheadBegin, registers);
            if (!emitAlternatives(alternatives))
                return false;
            emit(m_logCaptures && range.low <= range.high ? Kind::LookaheadEndKeepingCaptures : Kind::LookaheadEnd, registers);
            return true;
        }

        m_mayHaveBacktracked = true;
        unsigned begin = emit(Kind::NegativeLookaheadBegin, registers);
        if (!emitAlternatives(alternatives))
            return false;
        emit(Kind::NegativeLookaheadEnd, registers);
        setTarget(begin);
        if (!m_logCaptures)
            emitClear(range);
        return true;
    }

    struct Run {
        Kind kind;
        unsigned set;
        unsigned min;
        unsigned max;
        unsigned terms;
        std::optional<FirstCharacters> following;
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

    bool collectFollowingCharacters(const PatternAlternative& alternative, unsigned from, const Continuation* continuation, FirstCharacters& following)
    {
        if (!collectFirstCharacters(alternative, from, following))
            return false;
        if (!continuation || !continuation->alternative)
            return true;
        if (continuation->repeatedGroup) {
            for (auto& nested : continuation->repeatedGroup->parentheses.disjunction->m_alternatives) {
                collectFirstCharacters(*nested, 0, following);
                if (!m_termsLeftToVisit)
                    break;
            }
        }
        return collectFollowingCharacters(*continuation->alternative, continuation->nextTermIndex, continuation->outer, following);
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
        Kind kind = term.quantityType == QuantifierType::NonGreedy ? Kind::LazyRun : Kind::GreedyRun;
        FirstCharacters following;
        m_termsLeftToVisit = termsVisitedForFirstCharacters;
        bool mayBeFollowedByNothing = collectFollowingCharacters(alternative, counted + 1, m_continuation, following);
        if ((kind == Kind::GreedyRun && term.possessive()) || (!mayBeFollowedByNothing && !following.overlaps(m_program->sets[set])))
            return Run { Kind::PossessiveRun, set, min, max, counted + 1 - i, std::nullopt };
        if (mayBeFollowedByNothing || following.wide || following.isFull())
            return Run { kind, set, min, max, counted + 1 - i, std::nullopt };
        return Run { kind, set, min, max, counted + 1 - i, following };
    }

    void emitFixedAtom(const PatternTerm& term)
    {
        unsigned count = term.quantityMaxCount;
        if (!count)
            return;
        if (count > 1)
            emit(Kind::SetCount, setForTerm(term), count);
        else if (term.type == PatternTerm::Type::PatternCharacter && !isCased(term))
            emit(Kind::Character, 0, term.patternCharacter);
        else
            emit(Kind::Set, setForTerm(term));
    }

    bool isTextCharacter(const PatternAlternative& alternative, unsigned i, bool wide)
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

    bool emitAlternative(const PatternAlternative& alternative)
    {
        if (alternative.matchDirection() != Forward)
            return false;
        auto& terms = alternative.m_terms;
        for (unsigned i = 0; i < terms.size(); ++i) {
            const PatternTerm& term = terms[i];
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
                emit(term.multiline() ? Kind::LineStart : Kind::InputStart, term.multiline() ? newlineSet() : 0);
                break;
            case PatternTerm::Type::AssertionEOL:
                emit(term.multiline() ? Kind::LineEnd : Kind::InputEnd, term.multiline() ? newlineSet() : 0);
                break;
            case PatternTerm::Type::AssertionWordBoundary:
                emit(term.invert() ? Kind::NotWordBoundary : Kind::WordBoundary, internSet(m_pattern.wordcharCharacterClass(), false, 0, 0));
                break;
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass: {
                if (term.matchDirection() != Forward)
                    return false;
                if (auto run = runAt(alternative, i)) {
                    if (run->kind != Kind::PossessiveRun)
                        m_mayHaveBacktracked = true;
                    if (run->following) {
                        emit(run->kind == Kind::GreedyRun ? Kind::GreedyRunBeforeKnownCharacters : Kind::LazyRunBeforeKnownCharacters, run->set, run->max, run->min);
                        emit(Kind::KnownCharacters, appendRawSet(*run->following));
                    } else
                        emit(run->kind, run->set, run->max, run->min);
                    i += run->terms - 1;
                    break;
                }
                bool isWide = term.type == PatternTerm::Type::PatternCharacter && !isLatin1(term.patternCharacter);
                unsigned end = i;
                bool hasCase = false;
                while (isTextCharacter(alternative, end, isWide)) {
                    hasCase |= isCased(terms[end]);
                    ++end;
                }
                if (end - i >= 2) {
                    if (isWide) {
                        unsigned offset = m_program->wideText.size();
                        for (unsigned j = i; j < end; ++j)
                            m_program->wideText.append(static_cast<char16_t>(terms[j].patternCharacter));
                        emit(Kind::WideText, 0, offset, end - i);
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
                        emit(hasCase ? Kind::CasedText : Kind::Text, 0, offset, end - i);
                    }
                    i = end - 1;
                    break;
                }
                emitFixedAtom(term);
                break;
            }
            case PatternTerm::Type::ParenthesesSubpattern: {
                Continuation afterGroup { &alternative, i + 1, term.quantityMaxCount > 1 ? &term : nullptr, m_continuation };
                SetForScope continuation(m_continuation, &afterGroup);
                if (!emitGroup(term))
                    return false;
                break;
            }
            case PatternTerm::Type::ParentheticalAssertion: {
                Continuation unknown { nullptr, 0, nullptr, nullptr };
                SetForScope continuation(m_continuation, &unknown);
                if (!emitLookahead(term))
                    return false;
                break;
            }
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
            {
                if (term.matchDirection() != Forward)
                    return false;
                auto emitReference = [&] {
                    emit(term.ignoreCase() ? Kind::BackReferenceIgnoringCase : Kind::BackReference, 0, term.backReferenceSubpatternId << 1);
                    return true;
                };
                unsigned max = term.quantityMaxCount;
                if (!max)
                    break;
                if (term.quantityType == QuantifierType::FixedCount && max == 1) {
                    emitReference();
                    break;
                }
                emitLoop(term.quantityType == QuantifierType::FixedCount ? max : 0, max, term.quantityType == QuantifierType::NonGreedy, true, { }, emitReference);
                break;
            }
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                break;
            case PatternTerm::Type::DotStarEnclosure:
                return false;
            }
        }
        return true;
    }

    bool collectFirstCharacters(const PatternAlternative& alternative, unsigned from, FirstCharacters& first)
    {
        auto& terms = alternative.m_terms;
        for (unsigned i = from; i < terms.size(); ++i) {
            if (!m_termsLeftToVisit) {
                first.fill();
                return true;
            }
            --m_termsLeftToVisit;
            const PatternTerm& term = terms[i];
            switch (term.type) {
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass:
                if (!term.quantityMaxCount)
                    break;
                first.add(m_program->sets[setForTerm(term)]);
                if (term.quantityType == QuantifierType::FixedCount)
                    return false;
                break;
            case PatternTerm::Type::AssertionEOL:
                if (term.multiline())
                    break;
                first.atEnd = true;
                return false;
            case PatternTerm::Type::ParentheticalAssertion:
                if (term.invert() || term.matchDirection() != Forward)
                    break;
                [[fallthrough]];
            case PatternTerm::Type::ParenthesesSubpattern: {
                if (!term.quantityMaxCount)
                    break;
                bool groupCanBeEmpty = term.type == PatternTerm::Type::ParenthesesSubpattern && !term.quantityMinCount;
                for (auto& nested : term.parentheses.disjunction->m_alternatives) {
                    groupCanBeEmpty |= collectFirstCharacters(*nested, 0, first);
                    if (!m_termsLeftToVisit)
                        break;
                }
                if (!m_termsLeftToVisit) {
                    first.fill();
                    return true;
                }
                if (!groupCanBeEmpty)
                    return false;
                break;
            }
            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                first.fill();
                return true;
            default:
                break;
            }
        }
        return true;
    }

    YarrPattern& m_pattern;
    FastProgram* m_program { nullptr };
    static constexpr unsigned termsVisitedForFirstCharacters = 64;
    unsigned m_termsLeftToVisit { 0 };
    unsigned m_recursionDepth { 0 };
    unsigned m_nextRegister { 0 };
    const Continuation* m_continuation { nullptr };
    bool m_mayHaveBacktracked { false };
    bool m_logCaptures;
};

template<typename CharType>
static ALWAYS_INLINE bool fastSetContains(const FastSet& set, CharType character)
{
    if constexpr (sizeof(CharType) == 1)
        return set.bits[character >> 6] & (1ull << (character & 63));
    else {
        if (character < 256)
            return set.bits[character >> 6] & (1ull << (character & 63));
        return fastSetContainsSlow(set, character);
    }
}

template<typename CharType>
static ALWAYS_INLINE bool fastRawSetContains(const FastSet& set, CharType character)
{
    if (sizeof(CharType) > 1 && character > 255)
        return false;
    return set.bits[static_cast<uint8_t>(character) >> 6] & (1ull << (character & 63));
}

template<typename CharType>
static ALWAYS_INLINE bool fastTextMatches(const CharType* input, const Latin1Character* expected, unsigned count, unsigned available)
{
    if constexpr (sizeof(CharType) == 1) {
        constexpr unsigned wordSize = sizeof(uint64_t);
        if (count <= wordSize) {
            if (available >= wordSize) [[likely]]
                return !((WTF::unalignedLoad<uint64_t>(input) ^ WTF::unalignedLoad<uint64_t>(expected)) << ((wordSize - count) * 8));
        } else {
            unsigned i = 0;
            for (; i + wordSize < count; i += wordSize) {
                if (WTF::unalignedLoad<uint64_t>(input + i) != WTF::unalignedLoad<uint64_t>(expected + i))
                    return false;
            }
            return WTF::unalignedLoad<uint64_t>(input + count - wordSize) == WTF::unalignedLoad<uint64_t>(expected + count - wordSize);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (input[i] != expected[i])
            return false;
    }
    return true;
}

template<typename CharType>
static ALWAYS_INLINE bool fastCasedTextMatches(const CharType* input, const Latin1Character* expected, unsigned count, unsigned available)
{
    const Latin1Character* mask = expected + count;
    if constexpr (sizeof(CharType) == 1) {
        constexpr unsigned wordSize = sizeof(uint64_t);
        auto differs = [&](unsigned at) {
            return (WTF::unalignedLoad<uint64_t>(input + at) | WTF::unalignedLoad<uint64_t>(mask + at)) ^ WTF::unalignedLoad<uint64_t>(expected + at);
        };
        if (count <= wordSize) {
            if (available >= wordSize) [[likely]]
                return !(differs(0) << ((wordSize - count) * 8));
        } else {
            for (unsigned i = 0; i + wordSize < count; i += wordSize) {
                if (differs(i))
                    return false;
            }
            return !differs(count - wordSize);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if ((input[i] | mask[i]) != expected[i])
            return false;
    }
    return true;
}

struct FastMatchState {
    static constexpr unsigned inlineFrames = 128;
    static constexpr unsigned registersPerFrame = sizeof(FastFrame) / sizeof(uint32_t);

    FastFrame* storage;
    FastFrame* storageEnd;
    FastFrame* bottom;
    size_t framesOfRegisters;
    unsigned matchStart;
    unsigned remainingLoops { matchLimit };
    Vector<FastFrame> overflow;
    FastFrame inlineStorage[inlineFrames];
};

static NEVER_INLINE FastFrame* growFastFrames(FastMatchState& state, FastFrame* top, size_t needed)
{
    size_t used = top - state.storage;
    size_t newCapacity = (state.storageEnd - state.storage) * 2;
    while (newCapacity - used < needed)
        newCapacity *= 2;
    Vector<FastFrame> larger;
    if (newCapacity * sizeof(FastFrame) > Options::maxRegExpStackSize() || !larger.tryGrow(newCapacity))
        return nullptr;
    memcpySpan(larger.mutableSpan().first(used), std::span<const FastFrame> { state.storage, used });
    state.overflow = WTF::move(larger);
    auto span = state.overflow.mutableSpan();
    state.storage = span.data();
    state.storageEnd = span.data() + span.size();
    state.bottom = state.storage + state.framesOfRegisters;
    return state.storage + used;
}

template<typename CharType>
static unsigned runFastProgram(const FastProgram& program, std::span<const CharType> subject, unsigned start, unsigned* output)
{
    using Kind = FastOp::Kind;
    using Frame = FastFrame;

    const unsigned length = subject.size();
    if (start > length)
        return offsetNoMatch;

    const unsigned slots = (program.numSubpatterns + 1) * 2;
    for (unsigned i = 0; i < slots; i += 2) {
        output[i] = offsetNoMatch;
        output[i + 1] = offsetNoMatch;
        WTF::compilerFence();
    }

    if (program.endAnchoredFixedSize != YarrPattern::endAnchoredFixedSizeNotSet && length >= program.endAnchoredFixedSize)
        start = std::max(start, length - program.endAnchoredFixedSize);

    const CharType* input = subject.data();
    const FastOp* ops = program.ops.span().data();
    const FastSet* sets = program.sets.span().data();
    FastMatchState state;
    state.framesOfRegisters = (program.numRegisters + FastMatchState::registersPerFrame - 1) / FastMatchState::registersPerFrame;
    state.storage = state.inlineStorage;
    state.storageEnd = state.inlineStorage + FastMatchState::inlineFrames;
    state.bottom = state.storage + state.framesOfRegisters;
    state.matchStart = start;
    Frame* top = state.bottom;
    size_t framesNeeded = 1;
    unsigned position = start;
    const FastOp* op = ops;

    auto offsetOf = [&](const FastOp* at) ALWAYS_INLINE_LAMBDA -> uint32_t {
        return static_cast<uint32_t>(reinterpret_cast<const char*>(at) - reinterpret_cast<const char*>(ops));
    };
    auto opOf = [&](const Frame& frame) ALWAYS_INLINE_LAMBDA -> const FastOp* {
        return reinterpret_cast<const FastOp*>(reinterpret_cast<const char*>(ops) + frame.op);
    };
    auto undo = [&](const FastOp& from, const Frame& frame) ALWAYS_INLINE_LAMBDA {
        switch (from.kind) {
        case Kind::LoggedCaptureStart:
        case Kind::LoggedCaptureEnd:
            output[from.argument] = frame.position;
            break;
        case Kind::LoggedClearCaptures:
            output[frame.position << 1] = frame.limit;
            output[(frame.position << 1) + 1] = frame.extra;
            break;
        case Kind::LoggedSavePosition:
        case Kind::LoggedSetRegister:
        case Kind::LoggedIncrement:
            reinterpret_cast<uint32_t*>(state.storage)[from.set] = frame.position;
            break;
        default:
            break;
        }
    };

#define FAST_RESERVE(count) do { \
        if (static_cast<size_t>(state.storageEnd - top) < static_cast<size_t>(count)) [[unlikely]] { \
            framesNeeded = count; \
            goto growFrames; \
        } \
    } while (false)
#define FAST_PUSH(...) do { *top++ = Frame { offsetOf(op), __VA_ARGS__ }; } while (false)

#if HAVE(COMPUTED_GOTO)
#define YARR_FAST_OP_LABEL(name) &&op##name,
    static const void* const labels[] = { FOR_EACH_YARR_FAST_OP(YARR_FAST_OP_LABEL) };
#undef YARR_FAST_OP_LABEL
#define FAST_DISPATCH() goto *labels[static_cast<unsigned>(op->kind)]
#define FAST_OP(name) op##name:
#else
#define FAST_DISPATCH() goto dispatch
#define FAST_OP(name) case Kind::name:
#endif
#define FAST_NEXT() do { ++op; FAST_DISPATCH(); } while (false)
#define FAST_GOTO(index) do { op = ops + (index); FAST_DISPATCH(); } while (false)
#define FAST_FAIL() goto backtrack

    if (state.framesOfRegisters >= FastMatchState::inlineFrames) [[unlikely]] {
        framesNeeded = state.framesOfRegisters;
        top = state.storage;
        goto growFrames;
    }

tryAtMatchStart:
    if (program.scansForFirstCharacter) {
        if (program.hasOnlyOneFirstCharacter) {
            size_t found = WTF::find(subject, static_cast<CharType>(program.onlyFirstCharacter), state.matchStart);
            if (found == notFound)
                return offsetNoMatch;
            state.matchStart = found;
        } else {
            unsigned candidate = state.matchStart;
            while (candidate < length) {
                auto character = input[candidate];
                if (sizeof(CharType) == 1 || character < 256) {
                    if (program.firstCharacters[static_cast<uint8_t>(character) >> 6] & (1ull << (character & 63)))
                        break;
                } else if (program.firstCharacterMayBeWide)
                    break;
                ++candidate;
            }
            if (candidate >= length)
                return offsetNoMatch;
            state.matchStart = candidate;
        }
    }
    position = state.matchStart;
    op = ops;
    top = state.bottom;

#if HAVE(COMPUTED_GOTO)
    FAST_DISPATCH();
    {
#else
dispatch:
    switch (op->kind) {
#endif
    FAST_OP(Character) {
        if (position >= length || input[position] != op->argument)
            FAST_FAIL();
        ++position;
        FAST_NEXT();
    }
    FAST_OP(Text) {
        unsigned count = op->target;
        unsigned available = length - position;
        if (available < count || !fastTextMatches(input + position, program.text.span().data() + op->argument, count, available))
            FAST_FAIL();
        position += count;
        FAST_NEXT();
    }
    FAST_OP(CasedText) {
        unsigned count = op->target;
        unsigned available = length - position;
        if (available < count || !fastCasedTextMatches(input + position, program.text.span().data() + op->argument, count, available))
            FAST_FAIL();
        position += count;
        FAST_NEXT();
    }
    FAST_OP(WideText) {
        unsigned count = op->target;
        if (sizeof(CharType) == 1 || length - position < count)
            FAST_FAIL();
        const char16_t* expected = program.wideText.span().data() + op->argument;
        for (unsigned i = 0; i < count; ++i) {
            if (input[position + i] != expected[i])
                FAST_FAIL();
        }
        position += count;
        FAST_NEXT();
    }
    FAST_OP(Set) {
        if (position >= length || !fastSetContains(sets[op->set], input[position]))
            FAST_FAIL();
        ++position;
        FAST_NEXT();
    }
    FAST_OP(SetCount) {
        unsigned count = op->argument;
        if (length - position < count)
            FAST_FAIL();
        const FastSet& set = sets[op->set];
        for (unsigned i = 0; i < count; ++i) {
            if (!fastSetContains(set, input[position + i]))
                FAST_FAIL();
        }
        position += count;
        FAST_NEXT();
    }
    FAST_OP(GreedyRun) {
        FAST_RESERVE(1);
        const FastSet& set = sets[op->set];
        unsigned end = length - position > op->argument ? position + op->argument : length;
        unsigned begin = position;
        while (position < end && fastSetContains(set, input[position]))
            ++position;
        if (position - begin < op->target)
            FAST_FAIL();
        if (position - begin > op->target)
            FAST_PUSH(position, begin + op->target, 0);
        FAST_NEXT();
    }
    FAST_OP(GreedyRunBeforeKnownCharacters) {
        FAST_RESERVE(1);
        const FastSet& set = sets[op->set];
        const FastSet& following = sets[op[1].set];
        unsigned end = length - position > op->argument ? position + op->argument : length;
        unsigned begin = position;
        while (position < end && fastSetContains(set, input[position]))
            ++position;
        if (position - begin < op->target)
            FAST_FAIL();
        unsigned least = begin + op->target;
        while (position > least && !(position == length ? following.second : fastRawSetContains(following, input[position])))
            --position;
        if (position > least)
            FAST_PUSH(position, least, 0);
        ++op;
        FAST_NEXT();
    }
    FAST_OP(LazyRunBeforeKnownCharacters) {
        FAST_RESERVE(1);
        const FastSet& set = sets[op->set];
        const FastSet& following = sets[op[1].set];
        unsigned count = op->target;
        if (length - position < count)
            FAST_FAIL();
        for (unsigned i = 0; i < count; ++i) {
            if (!fastSetContains(set, input[position + i]))
                FAST_FAIL();
        }
        position += count;
        while (position < length && !fastRawSetContains(following, input[position])) {
            if (count == op->argument || !fastSetContains(set, input[position]))
                FAST_FAIL();
            ++count;
            ++position;
        }
        FAST_PUSH(position, count, 0);
        ++op;
        FAST_NEXT();
    }
    FAST_OP(KnownCharacters) {
        RELEASE_ASSERT_NOT_REACHED();
    }
    FAST_OP(PossessiveRun) {
        const FastSet& set = sets[op->set];
        unsigned end = length - position > op->argument ? position + op->argument : length;
        unsigned begin = position;
        while (position < end && fastSetContains(set, input[position]))
            ++position;
        if (position - begin < op->target)
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(LazyRun) {
        FAST_RESERVE(1);
        const FastSet& set = sets[op->set];
        unsigned count = op->target;
        if (length - position < count)
            FAST_FAIL();
        for (unsigned i = 0; i < count; ++i) {
            if (!fastSetContains(set, input[position + i]))
                FAST_FAIL();
        }
        position += count;
        FAST_PUSH(position, count, 0);
        FAST_NEXT();
    }
    FAST_OP(CapturedRun) {
        const FastSet& set = sets[op->set];
        unsigned begin = position;
        while (position < length && fastSetContains(set, input[position]))
            ++position;
        if (position - begin < op->target)
            FAST_FAIL();
        output[op->argument] = begin;
        output[op->argument + 1] = position;
        FAST_NEXT();
    }
    FAST_OP(Choice) {
        FAST_RESERVE(1);
        FAST_PUSH(position, 0, 0);
        FAST_NEXT();
    }
    FAST_OP(ChoiceByFirst) {
        FAST_RESERVE(1);
        if (position >= length) {
            if (!sets[op->set].second)
                FAST_GOTO(op->argument);
            if (!sets[op->set + 1].second)
                FAST_NEXT();
            FAST_PUSH(position, 0, 0);
            FAST_NEXT();
        }
        auto character = input[position];
        bool isNarrow = true;
        if constexpr (sizeof(CharType) > 1)
            isNarrow = character < 256;
        if (isNarrow) {
            uint64_t bit = 1ull << (character & 63);
            unsigned word = static_cast<unsigned>(character) >> 6;
            if (!(sets[op->set].bits[word] & bit))
                FAST_GOTO(op->argument);
            if (!(sets[op->set + 1].bits[word] & bit))
                FAST_NEXT();
        }
        FAST_PUSH(position, 0, 0);
        FAST_NEXT();
    }
    FAST_OP(Jump) {
        FAST_GOTO(op->target);
    }
    FAST_OP(CaptureStart) {
        output[op->argument] = position;
        FAST_NEXT();
    }
    FAST_OP(CaptureEnd) {
        output[op->argument] = position;
        FAST_NEXT();
    }
    FAST_OP(CaptureEmpty) {
        output[op->argument] = position;
        output[op->argument + 1] = position;
        FAST_NEXT();
    }
    FAST_OP(ClearCaptures) {
        for (unsigned i = op->argument << 1, end = (op->target << 1) + 2; i < end; ++i)
            output[i] = offsetNoMatch;
        FAST_NEXT();
    }
    FAST_OP(SavePosition) {
        reinterpret_cast<uint32_t*>(state.storage)[op->set] = position;
        FAST_NEXT();
    }
    FAST_OP(FailIfEmpty) {
        if (reinterpret_cast<uint32_t*>(state.storage)[op->set] == position)
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(InputStart) {
        if (position)
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(LineStart) {
        if (position && !fastSetContains(sets[op->set], input[position - 1]))
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(InputEnd) {
        if (position != length)
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(LineEnd) {
        if (position != length && !fastSetContains(sets[op->set], input[position]))
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(WordBoundary) {
        const FastSet& set = sets[op->set];
        bool before = position && fastSetContains(set, input[position - 1]);
        bool after = position < length && fastSetContains(set, input[position]);
        if (before == after)
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(NotWordBoundary) {
        const FastSet& set = sets[op->set];
        bool before = position && fastSetContains(set, input[position - 1]);
        bool after = position < length && fastSetContains(set, input[position]);
        if (before != after)
            FAST_FAIL();
        FAST_NEXT();
    }
    FAST_OP(Match) {
        output[0] = state.matchStart;
        output[1] = position;
        return state.matchStart;
    }
    FAST_OP(LoggedCaptureStart) {
        FAST_RESERVE(1);
        if (top != state.bottom)
            FAST_PUSH(output[op->argument], 0, 0);
        output[op->argument] = position;
        FAST_NEXT();
    }
    FAST_OP(LoggedCaptureEnd) {
        FAST_RESERVE(1);
        if (top != state.bottom)
            FAST_PUSH(output[op->argument], 0, 0);
        output[op->argument] = position;
        FAST_NEXT();
    }
    FAST_OP(LoggedClearCaptures) {
        FAST_RESERVE(op->target - op->argument + 1);
        for (unsigned i = op->argument; i <= op->target; ++i) {
            if (output[i << 1] == offsetNoMatch && output[(i << 1) + 1] == offsetNoMatch)
                continue;
            if (top != state.bottom)
                FAST_PUSH(i, output[i << 1], output[(i << 1) + 1]);
            output[i << 1] = offsetNoMatch;
            output[(i << 1) + 1] = offsetNoMatch;
        }
        FAST_NEXT();
    }
    FAST_OP(LoggedSavePosition) {
        FAST_RESERVE(1);
        if (top != state.bottom)
            FAST_PUSH(reinterpret_cast<uint32_t*>(state.storage)[op->set], 0, 0);
        reinterpret_cast<uint32_t*>(state.storage)[op->set] = position;
        FAST_NEXT();
    }
    FAST_OP(LoggedSetRegister) {
        FAST_RESERVE(1);
        if (top != state.bottom)
            FAST_PUSH(reinterpret_cast<uint32_t*>(state.storage)[op->set], 0, 0);
        reinterpret_cast<uint32_t*>(state.storage)[op->set] = op->argument;
        FAST_NEXT();
    }
    FAST_OP(LoggedIncrement) {
        FAST_RESERVE(1);
        if (top != state.bottom)
            FAST_PUSH(reinterpret_cast<uint32_t*>(state.storage)[op->set], 0, 0);
        ++reinterpret_cast<uint32_t*>(state.storage)[op->set];
        FAST_NEXT();
    }
    FAST_OP(JumpIfBelow) {
        if (reinterpret_cast<uint32_t*>(state.storage)[op->set] < op->argument)
            FAST_GOTO(op->target);
        FAST_NEXT();
    }
    FAST_OP(JumpIfNotBelow) {
        if (reinterpret_cast<uint32_t*>(state.storage)[op->set] >= op->argument)
            FAST_GOTO(op->target);
        FAST_NEXT();
    }
    FAST_OP(LoopBack) {
        if (!--state.remainingLoops) [[unlikely]]
            return offsetNoMatch;
        FAST_GOTO(op->target);
    }
    FAST_OP(LookaheadBegin) {
        reinterpret_cast<uint32_t*>(state.storage)[op->set] = position;
        reinterpret_cast<uint32_t*>(state.storage)[op->set + 1] = static_cast<uint32_t>(top - state.bottom);
        FAST_NEXT();
    }
    FAST_OP(LookaheadEnd) {
        position = reinterpret_cast<uint32_t*>(state.storage)[op->set];
        top = state.bottom + reinterpret_cast<uint32_t*>(state.storage)[op->set + 1];
        FAST_NEXT();
    }
    FAST_OP(LookaheadEndKeepingCaptures) {
        position = reinterpret_cast<uint32_t*>(state.storage)[op->set];
        Frame* kept = state.bottom + reinterpret_cast<uint32_t*>(state.storage)[op->set + 1];
        for (Frame* frame = kept; frame < top; ++frame) {
            Kind kind = opOf(*frame)->kind;
            if (kind == Kind::LoggedCaptureStart || kind == Kind::LoggedCaptureEnd || kind == Kind::LoggedClearCaptures)
                *kept++ = *frame;
        }
        top = kept;
        FAST_NEXT();
    }
    FAST_OP(NegativeLookaheadBegin) {
        FAST_RESERVE(1);
        reinterpret_cast<uint32_t*>(state.storage)[op->set + 1] = static_cast<uint32_t>(top - state.bottom);
        FAST_PUSH(position, 0, 0);
        FAST_NEXT();
    }
    FAST_OP(NegativeLookaheadEnd) {
        Frame* base = state.bottom + reinterpret_cast<uint32_t*>(state.storage)[op->set + 1];
        while (top > base) {
            --top;
            undo(*opOf(*top), *top);
        }
        FAST_FAIL();
    }
    FAST_OP(BackReference) {
        unsigned begin = output[op->argument];
        unsigned end = output[op->argument + 1];
        if (begin == offsetNoMatch || end == offsetNoMatch)
            FAST_NEXT();
        unsigned count = end - begin;
        if (length - position < count)
            FAST_FAIL();
        for (unsigned i = 0; i < count; ++i) {
            if (input[begin + i] != input[position + i])
                FAST_FAIL();
        }
        position += count;
        FAST_NEXT();
    }
    FAST_OP(BackReferenceIgnoringCase) {
        unsigned begin = output[op->argument];
        unsigned end = output[op->argument + 1];
        if (begin == offsetNoMatch || end == offsetNoMatch)
            FAST_NEXT();
        unsigned count = end - begin;
        if (length - position < count)
            FAST_FAIL();
        for (unsigned i = 0; i < count; ++i) {
            char32_t expected = input[begin + i];
            char32_t actual = input[position + i];
            if (expected == actual)
                continue;
            if (isASCII(expected) || isASCII(actual)) {
                if (toASCIIUpper(expected) == toASCIIUpper(actual))
                    continue;
            } else if (areCanonicallyEquivalent(expected, actual, CanonicalMode::UCS2))
                continue;
            FAST_FAIL();
        }
        position += count;
        FAST_NEXT();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();

backtrack:
    if (top == state.bottom) {
        if (program.triesOnlyOnce || state.matchStart >= length)
            return offsetNoMatch;
        ++state.matchStart;
        for (unsigned i = 2; i < slots; ++i)
            output[i] = offsetNoMatch;
        goto tryAtMatchStart;
    }
    {
#define FAST_FRAME() Frame* frame = top - 1; const FastOp* from = opOf(*frame)
#define FAST_RESUME(name) case Kind::name:
        switch (opOf(top[-1])->kind) {
        default:
            RELEASE_ASSERT_NOT_REACHED();
        FAST_RESUME(Choice)
        FAST_RESUME(ChoiceByFirst)
        FAST_RESUME(NegativeLookaheadBegin) {
            FAST_FRAME();
            position = frame->position;
            --top;
            FAST_GOTO(from->target);
        }
        FAST_RESUME(GreedyRun) {
            FAST_FRAME();
            position = --frame->position;
            if (position == frame->limit)
                --top;
            op = from;
            FAST_NEXT();
        }
        FAST_RESUME(GreedyRunBeforeKnownCharacters) {
            FAST_FRAME();
            position = frame->position - 1;
            while (position > frame->limit && !fastRawSetContains(sets[from[1].set], input[position]))
                --position;
            frame->position = position;
            if (position == frame->limit)
                --top;
            op = from + 1;
            FAST_NEXT();
        }
        FAST_RESUME(LazyRunBeforeKnownCharacters) {
            FAST_FRAME();
            position = frame->position;
            do {
                if (frame->limit == from->argument || position >= length || !fastSetContains(sets[from->set], input[position])) {
                    --top;
                    goto backtrack;
                }
                ++frame->limit;
                ++position;
            } while (position < length && !fastRawSetContains(sets[from[1].set], input[position]));
            frame->position = position;
            op = from + 1;
            FAST_NEXT();
        }
        FAST_RESUME(LazyRun) {
            FAST_FRAME();
            if (frame->limit < from->argument && frame->position < length && fastSetContains(sets[from->set], input[frame->position])) {
                ++frame->limit;
                position = ++frame->position;
                op = from;
                FAST_NEXT();
            }
            --top;
            goto backtrack;
        }
        FAST_RESUME(LoggedCaptureStart)
        FAST_RESUME(LoggedCaptureEnd) {
            FAST_FRAME();
            output[from->argument] = frame->position;
            --top;
            goto backtrack;
        }
        FAST_RESUME(LoggedClearCaptures) {
            FAST_FRAME();
            output[frame->position << 1] = frame->limit;
            output[(frame->position << 1) + 1] = frame->extra;
            --top;
            goto backtrack;
        }
        FAST_RESUME(LoggedSavePosition)
        FAST_RESUME(LoggedSetRegister)
        FAST_RESUME(LoggedIncrement) {
            FAST_FRAME();
            reinterpret_cast<uint32_t*>(state.storage)[from->set] = frame->position;
            --top;
            goto backtrack;
        }
        }
#undef FAST_RESUME
#undef FAST_FRAME
    }
    RELEASE_ASSERT_NOT_REACHED();

growFrames:
    {
        bool isBeforeTheFirstTry = top == state.storage;
        top = growFastFrames(state, top, framesNeeded);
        if (!top)
            return offsetNoMatch;
        if (isBeforeTheFirstTry)
            goto tryAtMatchStart;
        FAST_DISPATCH();
    }

#undef FAST_RESERVE
#undef FAST_PUSH
#undef FAST_DISPATCH
#undef FAST_OP
#undef FAST_NEXT
#undef FAST_GOTO
#undef FAST_FAIL
}

template<typename CharType>
static unsigned validatedFastProgram(BytecodePattern* bytecode, std::span<const CharType> subject, unsigned start, unsigned* output)
{
    const FastProgram& program = *bytecode->m_fastProgram;
    unsigned slots = (program.numSubpatterns + 1) * 2;
    Vector<unsigned, 32> fastOutput;
    fastOutput.grow(slots);
    for (auto& slot : fastOutput)
        slot = offsetNoMatch;
    unsigned fastResult = runFastProgram<CharType>(program, subject, start, fastOutput.mutableSpan().data());
    unsigned result = Interpreter<CharType>(bytecode, output, subject, start).interpret();
    bool same = fastResult == result;
    for (unsigned i = 0; same && result != offsetNoMatch && i <= program.numSubpatterns; ++i) {
        same = fastOutput[i << 1] == output[i << 1];
        if (same && output[i << 1] != offsetNoMatch)
            same = fastOutput[(i << 1) + 1] == output[(i << 1) + 1];
    }
    if (!same) {
        dataLogLn("validateRegExpFastMatcher: results differ at start ", start, ": fast ", static_cast<int>(fastResult), " interpreter ", static_cast<int>(result));
        for (unsigned i = 0; i <= program.numSubpatterns; ++i)
            dataLogLn("  group ", i, ": fast ", static_cast<int>(fastOutput[i << 1]), ",", static_cast<int>(fastOutput[(i << 1) + 1]), " interpreter ", static_cast<int>(output[i << 1]), ",", static_cast<int>(output[(i << 1) + 1]));
        dataLogLn("  subject: ", StringView(subject));
        CRASH();
    }
    return result;
}

std::unique_ptr<BytecodePattern> byteCompile(YarrPattern& pattern, BumpPointerAllocator* allocator, ErrorCode& errorCode, ConcurrentJSLock* lock)
{
    std::unique_ptr<FastProgram> fastProgram;
    if (!Options::useRegExpJIT())
        fastProgram = FastProgramBuilder(pattern).build();
    dataLogLnIf(Options::dumpCompiledRegExpPatterns(), fastProgram ? "Runs on a flat program" : "Runs in the bytecode interpreter");
    if (fastProgram && !fastProgram->validate) {
        Vector<std::unique_ptr<ByteDisjunction>> noParentheses;
        auto bytecode = makeUnique<BytecodePattern>(makeUnique<ByteDisjunction>(pattern.m_numSubpatterns, 0), noParentheses, pattern, allocator, lock, pattern.offsetVectorBaseForNamedCaptures(), pattern.offsetsSize());
        bytecode->m_fastProgram = WTF::move(fastProgram);
        return bytecode;
    }
    auto bytecode = ByteCompiler(pattern).compile(allocator, lock, errorCode);
    if (bytecode)
        bytecode->m_fastProgram = WTF::move(fastProgram);
    return bytecode;
}

unsigned interpret(BytecodePattern* bytecode, StringView input, unsigned start, unsigned* output)
{
    SuperSamplerScope superSamplerScope(false);
    if (auto* program = bytecode->m_fastProgram.get()) {
        if (program->validate) [[unlikely]] {
            if (input.is8Bit())
                return validatedFastProgram<Latin1Character>(bytecode, input.span8(), start, output);
            return validatedFastProgram<char16_t>(bytecode, input.span16(), start, output);
        }
        if (input.is8Bit())
            return runFastProgram<Latin1Character>(*program, input.span8(), start, output);
        return runFastProgram<char16_t>(*program, input.span16(), start, output);
    }
    if (input.is8Bit())
        return Interpreter<Latin1Character>(bytecode, output, input.span8(), start).interpret();
    return Interpreter<char16_t>(bytecode, output, input.span16(), start).interpret();
}

// These should be the same for both char16_t & Latin1Character.
static_assert(sizeof(BackTrackInfoPatternCharacter) == (YarrStackSpaceForBackTrackInfoPatternCharacter * sizeof(uintptr_t)));
static_assert(sizeof(BackTrackInfoCharacterClass) == (YarrStackSpaceForBackTrackInfoCharacterClass * sizeof(uintptr_t)));
static_assert(sizeof(BackTrackInfoBackReference) == (YarrStackSpaceForBackTrackInfoBackReference * sizeof(uintptr_t)));
static_assert(sizeof(BackTrackInfoAlternative) == (YarrStackSpaceForBackTrackInfoAlternative * sizeof(uintptr_t)));
static_assert(sizeof(BackTrackInfoParentheticalAssertion) == (YarrStackSpaceForBackTrackInfoParentheticalAssertion * sizeof(uintptr_t)));
static_assert(sizeof(BackTrackInfoParenthesesOnce) == (YarrStackSpaceForBackTrackInfoParenthesesOnce * sizeof(uintptr_t)));
static_assert(sizeof(BackTrackInfoParenthesesTerminal) == (YarrStackSpaceForBackTrackInfoParenthesesTerminal * sizeof(uintptr_t)));
static_assert(sizeof(Interpreter<char16_t>::BackTrackInfoParentheses) <= (YarrStackSpaceForBackTrackInfoParentheses * sizeof(uintptr_t)));


} }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
