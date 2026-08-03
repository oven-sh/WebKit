/*
 * Copyright (C) 2009-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2019-2026 the V8 project authors. All rights reserved.
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
#include "YarrJIT.h"

#include "AllowMacroScratchRegisterUsage.h"
#include "CCallHelpers.h"
#include "LinkBuffer.h"
#include "Options.h"
#include "ProbeContext.h"
#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
#include "JITThunks.h"
#endif
#include "VM.h"
#include "Yarr.h"
#include "YarrCanonicalize.h"
#include "YarrDisassembler.h"
#include "YarrJITRegisters.h"
#include "YarrMatchingContextHolder.h"
#include <wtf/ASCIICType.h>
#include <wtf/BitVector.h>
#include <wtf/ListDump.h>
#include <wtf/MathExtras.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if ENABLE(YARR_JIT)

namespace JSC { namespace Yarr {
namespace YarrJITInternal {
static constexpr bool verbose = false;
}

static constexpr int32_t errorCodePoint = -1;

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
enum class TryReadUnicodeCharGenFirstNonBMPOptimization { DontUseOptimization, UseOptimization };

static MacroAssemblerCodeRef<JITThunkPtrTag> tryReadUnicodeCharSlowThunkGenerator(VM&);

#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
static MacroAssemblerCodeRef<JITThunkPtrTag> tryReadUnicodeCharIncForNonBMPSlowThunkGenerator(VM&);
#endif
#endif

#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAreCanonicallyEquivalent, bool, (unsigned, unsigned, CanonicalMode));

static MacroAssemblerCodeRef<JITThunkPtrTag> areCanonicallyEquivalentThunkGenerator(VM&);

// Since the generator areCanonicallyEquivalentThunkGenerator() needs to be static,
// we set the incoming argument registers to the thunk here and ASSERT at runtime
// that they match.
#if CPU(ARM64)
static constexpr GPRReg areCanonicallyEquivalentCharArgReg = ARM64Registers::x6;
static constexpr GPRReg areCanonicallyEquivalentPattCharArgReg = ARM64Registers::x7;
static constexpr GPRReg areCanonicallyEquivalentCanonicalModeArgReg = ARM64Registers::x10;
#elif CPU(X86_64)
static constexpr GPRReg areCanonicallyEquivalentCharArgReg = X86Registers::eax;
static constexpr GPRReg areCanonicallyEquivalentPattCharArgReg = X86Registers::r9;
static constexpr GPRReg areCanonicallyEquivalentCanonicalModeArgReg = X86Registers::r13;

// The thunk code assumes that we return the result to areCanonicallyEquivalentCharArgReg.
static_assert(areCanonicallyEquivalentCharArgReg == GPRInfo::returnValueGPR);
#endif
#endif


WTF_MAKE_TZONE_ALLOCATED_IMPL(BoyerMooreBitmap);
WTF_MAKE_TZONE_ALLOCATED_IMPL(BoyerMooreFastCandidates);
WTF_MAKE_TZONE_ALLOCATED_IMPL(YarrBoyerMooreData);
WTF_MAKE_TZONE_ALLOCATED_IMPL(YarrCodeBlock);

#if CPU(ARM64E)
JSC_ANNOTATE_JIT_OPERATION_RETURN(vmEntryToYarrJITAfter);
#endif

// We should pick the less frequently appearing character as a BM search's anchor to make BM search more and more efficient.
// This class takes some samples from the passed subject string to put weight on characters so that we can pick an optimal one adaptively.
class SubjectSampler {
public:
    static constexpr unsigned sampleSize = 128;

    // Frequency, in units of the sample above, past which the vector search stops paying for
    // itself. Leaving the vector loop costs a vector to general purpose register transfer, so its
    // price is paid per candidate that fails to match. How often a candidate fails is not known
    // when compiling, leaving this frequency as the proxy. Measured on arm64: from here the scalar
    // loop is 2.2x faster on a keyword alternation whose candidates almost all fail, while below it
    // the vector loop reaches 8x on subjects holding no candidate at all. The vector loop leaves
    // more cheaply on x86_64, where the crossover sits higher and this gives up some of its reach.
    static constexpr int32_t maxCandidateFrequencyForSIMDSearch = 8;

    // Below this, one occurrence alone already scales past maxCandidateFrequencyForSIMDSearch, so
    // the sample cannot tell a rare candidate from a common one. Such a subject also says little
    // about the later subjects a compile is reused for, since RegExp caches keep the first one's.
    static constexpr unsigned minimumSampleSizeForFrequency = sampleSize / maxCandidateFrequencyForSIMDSearch;

    explicit SubjectSampler(CharSize charSize)
        : m_is8Bit(charSize == CharSize::Char8)
    {
    }

    bool canResolveCandidateFrequency() const { return m_size >= minimumSampleSizeForFrequency; }

    int32_t NODELETE frequency(char16_t character) const
    {
        if (!m_size)
            return 1;
        return static_cast<int32_t>(m_samples[character & BoyerMooreBitmap::mapMask]) * sampleSize / m_size;
    }

    void sample(StringView string)
    {
        unsigned half = string.length() > sampleSize ? (string.length() - sampleSize) / 2 : 0;
        unsigned end = std::min(string.length(), half + sampleSize);
        if (string.is8Bit()) {
            auto characters8 = string.span8();
            for (unsigned i = half; i < end; ++i)
                add(characters8[i]);
        } else {
            auto characters16 = string.span16();
            for (unsigned i = half; i < end; ++i)
                add(characters16[i]);
        }
    }

    void dump() const
    {
        dataLogLn("Sampling Results size:(", m_size, ")");
        for (unsigned i = 0; i < BoyerMooreBitmap::mapSize; ++i)
            dataLogLn("    [", makeString(pad(' ', 3, i)), "] ", m_samples[i]);
    }

    bool NODELETE is8Bit() const { return m_is8Bit; }

private:
    inline void add(char16_t character)
    {
        ++m_size;
        ++m_samples[character & BoyerMooreBitmap::mapMask];
    }

    std::array<uint8_t, BoyerMooreBitmap::mapSize> m_samples { };
    uint8_t m_size { };
    bool m_is8Bit { true };
};

void BoyerMooreFastCandidates::dump(PrintStream& out) const
{
    if (!isValid()) {
        out.print("isValid:(false)");
        return;
    }
    out.print("isValid:(true),characters:(", listDump(m_characters), ")");
}

class BoyerMooreInfo {
    WTF_MAKE_NONCOPYABLE(BoyerMooreInfo);
    WTF_MAKE_TZONE_ALLOCATED(BoyerMooreInfo);
public:
    static constexpr unsigned maxLength = 32;

    explicit BoyerMooreInfo(CharSize charSize, unsigned length)
        : m_characters(length)
        , m_charSize(charSize)
    {
        ASSERT(this->length() <= maxLength);
    }

    unsigned NODELETE length() const { return m_characters.size(); }
    void NODELETE shortenLength(unsigned length)
    {
        if (length <= this->length())
            m_characters.shrink(length);
    }

    void set(unsigned index, char32_t character)
    {
        m_characters[index].add(m_charSize, character);
    }

    void setAll(unsigned index)
    {
        m_characters[index].setAll();
    }

    void addCharacters(unsigned index, const Vector<char32_t>& characters)
    {
        m_characters[index].addCharacters(m_charSize, characters);
    }

    void addRanges(unsigned index, const Vector<CharacterRange>& range)
    {
        m_characters[index].addRanges(m_charSize, range);
    }

    static UniqueRef<BoyerMooreInfo> NODELETE create(CharSize charSize, unsigned length)
    {
        return makeUniqueRef<BoyerMooreInfo>(charSize, length);
    }

    std::optional<std::tuple<unsigned, unsigned>> findWorthwhileCharacterSequenceForLookahead(const SubjectSampler&) const;
    std::tuple<BoyerMooreBitmap::Map, BoyerMooreFastCandidates> createCandidateBitmap(unsigned begin, unsigned end) const;

    void dump(PrintStream&) const;

private:
    std::tuple<int32_t, unsigned, unsigned> findBestCharacterSequence(const SubjectSampler&) const;

    Vector<BoyerMooreBitmap> m_characters;
    CharSize m_charSize;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(BoyerMooreInfo);

std::tuple<int32_t, unsigned, unsigned> BoyerMooreInfo::findBestCharacterSequence(const SubjectSampler& sampler) const
{
    // The search loop tests one character against the union of the candidate sets in [begin, end)
    // and shifts by the length of that range, so a longer range shifts further while its union
    // matches more often. Score every sub-range: restricting the search to maximal runs would miss
    // a short selective range whose neighbours pollute the union, e.g. the distinct leading
    // characters of /zalpha|qbravo|xcharlie/, whose union with the following positions matches
    // almost everything.
    int32_t biggestPoint = INT32_MIN;
    unsigned beginResult = 0;
    unsigned endResult = 0;
    for (unsigned begin = 0; begin < length(); ++begin) {
        BoyerMooreBitmap::Map map { };
        int32_t frequency = 0;
        for (unsigned end = begin + 1; end <= length(); ++end) {
            auto& candidates = m_characters[end - 1];
            // A position matching every character records that in its count alone, leaving its map
            // empty, so merging one would drop every character it matches out of the union and the
            // search would skip positions that can start a match. It could only widen the union to
            // everything in any case, which no search can profit from.
            if (candidates.isAllSet())
                break;
            candidates.map().forEachSetBit([&](unsigned character) {
                if (!map.testAndSet(character))
                    frequency += sampler.canResolveCandidateFrequency() ? sampler.frequency(character) : 1;
            });

            // An empty union is not searchable -- the caller needs at least one candidate character to
            // test against. Its frequency of zero would otherwise score higher than any real range and
            // suppress the search entirely. This happens in 8-bit compiles of a class holding only
            // characters above Latin1, where no alternative can match at all.
            if (map.isEmpty())
                continue;

            // Cutoff at 50%. If we could encounter the character more than 50%, then BM search would be useless probably.
            int32_t matchingProbability = (BoyerMooreBitmap::mapSize / 2) - frequency;
            int32_t point = static_cast<int32_t>(end - begin) * matchingProbability;
            if (point > biggestPoint) {
                biggestPoint = point;
                beginResult = begin;
                endResult = end;
            }
            // Extending the range only adds characters, so the probability never recovers and a
            // longer stride only scales an already-negative score further down.
            if (matchingProbability < 0)
                break;
        }
    }
    return std::tuple { biggestPoint, beginResult, endResult };
}

std::optional<std::tuple<unsigned, unsigned>> BoyerMooreInfo::findWorthwhileCharacterSequenceForLookahead(const SubjectSampler& sampler) const
{
    auto [biggestPoint, begin, end] = findBestCharacterSequence(sampler);
    if (biggestPoint < 0)
        return std::nullopt;
    return std::tuple { begin, end };
}

std::tuple<BoyerMooreBitmap::Map, BoyerMooreFastCandidates> BoyerMooreInfo::createCandidateBitmap(unsigned begin, unsigned end) const
{
    BoyerMooreBitmap::Map map { };
    BoyerMooreFastCandidates charactersFastPath;
    for (unsigned index = begin; index < end; ++index) {
        auto& bmBitmap = m_characters[index];
        map.merge(bmBitmap.map());
        charactersFastPath.merge(bmBitmap.charactersFastPath());
    }
    return std::tuple { WTF::move(map), WTF::move(charactersFastPath) };
}

void BoyerMooreInfo::dump(PrintStream& out) const
{
    out.println("BoyerMooreInfo size:(", m_characters.size(), ")");
    unsigned index = 0;
    for (auto& map : m_characters)
        out.println("    [", makeString(pad(' ', 3, index++)), "] ", map);
}

void BoyerMooreBitmap::dump(PrintStream& out) const
{
    out.print(m_map);
}

// SIMD multi-pattern search info for alternation patterns.
// For patterns like /agggtaaa|tttaccct/i, extracts the first 4 bytes of each alternative
// and creates masks for parallel SIMD comparison across multiple starting positions.
//
// Load input at 4 offsets (0,1,2,3), apply mask, compare as 4-byte words.
// This checks 4 consecutive starting positions per 16-byte SIMD register.
//
// FIXME: Currently we are only supporting 2 alternative cases at first, aligned to V8's optimization.
// We will extend it to 1 and 3 later.
// First-character set of an alternative: an over-approximation of the set of
// characters a match can start with. Latin-1 characters are tracked precisely;
// `wide` means a first character >= 0x100 is possible; `any` is the top
// element (every character possible). An over-approximation is always sound
// for dispatch: membership merely means the alternative is tried there.
struct FirstCharacterSet {
    WTF::BitSet<256> latin1;
    bool wide { false };
    bool any { false };
    // Whether analysis reached a term that must consume a character. If false
    // the alternative can match the empty string, which for dispatch is `any`.
    bool consumed { false };

    void add(char32_t ch)
    {
        if (ch < 256)
            latin1.set(ch);
        else
            wide = true;
    }
    void addRange(char32_t begin, char32_t end)
    {
        for (char32_t ch = begin; ch <= end && ch < 256; ++ch)
            latin1.set(ch);
        if (end >= 256)
            wide = true;
    }
    void merge(const FirstCharacterSet& other)
    {
        latin1.merge(other.latin1);
        wide |= other.wide;
        any |= other.any;
    }
    bool matches(unsigned latin1Char) const { return any || latin1.get(latin1Char); }
    bool matchesWide() const { return any || wide; }
};

// First-character dispatch for a nested disjunction with many alternatives.
//
// Trying N alternatives in sequence costs an entry attempt each. When every
// alternative must consume a first character, that character is already inside
// input the enclosing alternative claimed, so it can be read once (no bounds
// check) and a decision tree over its value can jump straight to the ordered
// chain of alternatives whose first-character set contains it. Alternatives
// are still tried in source order within a chain, so leftmost-first semantics
// hold; an alternative whose set is `any` appears in every chain. A failing
// alternative continues its chain through a code address the chain stored in
// the group's frame (BackTrackInfoParenthesesOnce::chainResume) before entry.
struct DispatchInfo {
private:
    WTF_MAKE_TZONE_ALLOCATED(DispatchInfo);
public:
    struct Chain {
        Vector<unsigned, 4> alternativeIndices; // ascending; tried in this order
        MacroAssembler::Label head; // start of the chain's stub sequence
    };

    Vector<Chain, 8> chains; // distinct Latin-1 chains
    std::array<unsigned, 256> chainForCharacter { }; // Latin-1 char -> chain index, or noChain
    Chain wideChain; // characters >= 0x100 (16-bit strings only)
    unsigned frameLocation { 0 }; // the group's frame; chainResume lives here
    Checked<unsigned> firstCharacterOffset; // read offset of the group's first character
    PatternDisjunction* disjunction { nullptr }; // the dispatched alternatives
    Checked<unsigned> enclosingCheckedOffset; // checkedOffset at the group term
    size_t endOpIndex { 0 }; // the group's NestedAlternativeEnd op (success join point)

    // Per alternative: jumps from chain stubs awaiting that alternative's
    // entry point (always a forward reference; see emitJumpToDispatchEntry).
    Vector<MacroAssembler::JumpList, 8> pendingEntryJumps;

    // Failures that mean "no alternative can match here" (empty chains, chain
    // exhaustion). Routed as the group's failure with the index unmoved.
    MacroAssembler::JumpList groupFailJumps;

    static constexpr unsigned noChain = std::numeric_limits<unsigned>::max();
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(DispatchInfo);

// First-character dispatch across the body's repeated alternatives, with no
// group and no frame. It applies only when the alternatives' first-character
// sets are pairwise disjoint, so the character at the match start names at most
// one candidate. A binary decision tree over that character jumps straight to
// its sole candidate; when that candidate fails, no other alternative can
// match at this position, so the failure advances the search instead of
// walking the remaining alternatives. Every decision is static -- no per-match
// state -- so the pattern gains no Yarr frame and stays eligible for
// RegExpTestInline.
struct BodyDispatchInfo {
private:
    WTF_MAKE_TZONE_ALLOCATED(BodyDispatchInfo);
public:
    // Latin-1 char -> index (into the repeated alternatives, 0-based from the
    // Begin) of the sole alternative that can start with it, or noAlternative.
    std::array<unsigned, 256> firstAlternativeForCharacter { };
    unsigned wideFirstAlternative { noAlternative }; // sole candidate for chars >= 0x100, or noAlternative
    Checked<unsigned> firstCharacterOffset; // read offset back from the claimed frontier
    unsigned alternativeCount { 0 };

    // Per repeated alternative: dispatch-tree jumps awaiting that alternative's
    // entry (bound right after its Begin/Next op's input-claim delta), and the
    // bound entry label itself once known, for the backtrack-pass tail dispatch
    // (which is emitted after every entry has been bound).
    Vector<MacroAssembler::JumpList, 8> pendingEntryJumps;
    Vector<MacroAssembler::Label, 8> pendingEntryLabels;

    // Search-advance stubs that ran out of input for the next position join
    // the body loop's own first-input-check-failed flow (its input-exhaustion
    // protocol). That code is emitted in the backtrack pass, after the forward
    // pass's no-candidate leaf, so those jumps wait here until it is bound.
    MacroAssembler::JumpList inputExhaustedJumps;
    MacroAssembler::Label firstInputCheckFailed;
    bool haveFirstInputCheckFailed { false };

    static constexpr unsigned noAlternative = std::numeric_limits<unsigned>::max();
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(BodyDispatchInfo);

struct MaskedAlternativeInfo {
private:
    WTF_MAKE_TZONE_ALLOCATED(MaskedAlternativeInfo);
public:

    static constexpr unsigned maxAlternatives = 2; // Support 2 alternatives initially
    static constexpr unsigned patternBytes = 4; // Match first 4 bytes
    static constexpr unsigned thresholdForDistinctCharacters = 16;

    // Per-alternative pattern data
    struct Alternative {
        uint32_t chars { 0 }; // First 4 bytes as little-endian uint32
        uint32_t mask { 0 }; // Mask for case-insensitive matching (0xFF = exact, 0xDF = case-insensitive)
    };

    std::array<Alternative, maxAlternatives> alternatives { };
    unsigned numAlternatives { 0 };
    uint32_t minPatternLength { 0 }; // Minimum length across alternatives

    // Compute mask and char value for a character class that makes all members equivalent
    // Returns false if the class is too complex (e.g., ranges spanning many bits, inverted class)
    static bool computeMaskForCharacterClass(const CharacterClass& charClass, bool ignoreCase, uint8_t& outChar, uint8_t& outMask)
    {
        // Don't handle inverted classes or classes with ranges for now
        // Only handle simple character sets like [acg] or [cgt]
        if (charClass.m_matches8.isEmpty())
            return false;
        if (!charClass.m_ranges8.isEmpty())
            return false;
        if (!charClass.m_matches32.isEmpty() || !charClass.m_ranges32.isEmpty())
            return false;

        // Collect all characters (and their case variants if ignoreCase)
        Vector<uint8_t, 32> chars;
        for (char32_t ch : charClass.m_matches8) {
            if (!isASCII(ch))
                return false; // ASCII only

            if (ignoreCase && isASCIIAlpha(ch)) {
                chars.append(toASCIILower(static_cast<uint8_t>(ch)));
                chars.append(toASCIIUpper(static_cast<uint8_t>(ch)));
            } else
                chars.append(static_cast<uint8_t>(ch));

            if (chars.size() > thresholdForDistinctCharacters)
                return false;
        }

        if (chars.isEmpty() || chars.size() > thresholdForDistinctCharacters)
            return false;

        // Compute XOR of all differing bits
        uint8_t differingBits = 0;
        for (unsigned i = 1; i < chars.size(); ++i)
            differingBits |= (chars[0] ^ chars[i]);

        // Mask clears all differing bits
        outMask = ~differingBits;

        // The character value is any character ANDed with the mask (they all produce the same result)
        outChar = chars[0] & outMask;

        return true;
    }

    // Create from a disjunction with exactly 2 fixed alternatives
    // Returns invalid info if the pattern doesn't qualify for this optimization
    static std::optional<MaskedAlternativeInfo> create(const PatternDisjunction& disjunction, bool ignoreCase, CharSize charSize)
    {
        MaskedAlternativeInfo info;

        // Only support Latin1 (8-bit) for now
        if (charSize != CharSize::Char8)
            return std::nullopt;

        // Need exactly 2 alternatives
        auto& alternatives = disjunction.m_alternatives;
        if (alternatives.size() != maxAlternatives)
            return std::nullopt;

        // Both alternatives must have at least 4 characters and be fixed-size
        info.minPatternLength = UINT32_MAX;
        for (unsigned i = 0; i < maxAlternatives; ++i) {
            const PatternAlternative* alt = alternatives[i].get();
            if (!alt->m_hasFixedSize)
                return std::nullopt;
            if (alt->m_minimumSize < patternBytes)
                return std::nullopt;
            info.minPatternLength = std::min(info.minPatternLength, alt->m_minimumSize);

            // Extract first 4 characters - can be simple characters or character classes
            uint32_t chars = 0;
            uint32_t mask = 0;
            unsigned charIndex = 0;
            for (const PatternTerm& term : alt->m_terms) {
                if (charIndex >= patternBytes)
                    break;
                if (term.quantityType != QuantifierType::FixedCount || term.quantityMinCount != 1)
                    return std::nullopt; // No quantifiers

                uint8_t byteChar = 0;
                uint8_t byteMask = 0xFF;

                if (term.type == PatternTerm::Type::PatternCharacter) {
                    char32_t ch = term.patternCharacter;
                    if (!isASCII(ch))
                        return std::nullopt; // ASCII only for now

                    byteChar = static_cast<uint8_t>(ch);
                    // Mask: 0xDF for case-insensitive letters, 0xFF for exact match
                    if (ignoreCase && isASCIIAlpha(ch))
                        byteMask = 0xDF; // Clear bit 5 to normalize case
                } else if (term.type == PatternTerm::Type::CharacterClass) {
                    // Handle character class by computing a mask that makes all members equivalent
                    if (term.m_invert)
                        return std::nullopt; // Don't handle inverted classes
                    if (!computeMaskForCharacterClass(*term.characterClass, ignoreCase, byteChar, byteMask))
                        return std::nullopt; // Class too complex
                } else
                    return std::nullopt; // Unsupported term type

                // Build the 4-byte pattern (little-endian)
                chars |= static_cast<uint32_t>(byteChar) << (charIndex * 8);
                mask |= static_cast<uint32_t>(byteMask) << (charIndex * 8);

                ++charIndex;
            }

            if (charIndex < patternBytes)
                return std::nullopt; // Not enough characters

            info.alternatives[i].chars = chars;
            info.alternatives[i].mask = mask;
        }

        info.numAlternatives = 2;
        return info;
    }
};
WTF_MAKE_TZONE_ALLOCATED_IMPL(MaskedAlternativeInfo);

static constexpr MacroAssembler::TrustedImm32 surrogateTagMask = MacroAssembler::TrustedImm32(0xfc00fc00);
static constexpr MacroAssembler::TrustedImm32 surrogatePairTags = MacroAssembler::TrustedImm32(0xdc00d800);
static constexpr MacroAssembler::TrustedImm32 surrogateTagShiftedRight11 = MacroAssembler::TrustedImm32(0xd800 >> 11);

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
template<TryReadUnicodeCharGenFirstNonBMPOptimization useNonBMPOptimization>
void tryReadUnicodeCharImpl(VM& vm, CCallHelpers& jit, MacroAssembler::RegisterID resultReg)
{
    MacroAssembler::JumpList slowCases;
    MacroAssembler::JumpList done;

    YarrJITDefaultRegisters regs;

    if (resultReg != regs.regT0)
        jit.swap(regs.regT0, resultReg);

    // A code unit that is not a surrogate is a BMP code point on its own.
    jit.load16Unaligned(MacroAssembler::Address(regs.regUnicodeInputAndTrail), resultReg);
    jit.urshift32(resultReg, MacroAssembler::TrustedImm32(11), regs.unicodeAndSubpatternIdTemp);
    done.append(jit.branch32(MacroAssembler::NotEqual, regs.unicodeAndSubpatternIdTemp, surrogateTagShiftedRight11));

    // The first character is a surrogate. Check if we can read the trailing code unit as well.
    jit.add64(MacroAssembler::TrustedImm32(4), regs.regUnicodeInputAndTrail, regs.unicodeAndSubpatternIdTemp);
    slowCases.append(jit.branchPtr(MacroAssembler::Above, regs.unicodeAndSubpatternIdTemp, regs.endOfStringAddress));

    // Load the pair. If it is a proper surrogate pair, compute the non-BMP codepoint.
    jit.load32(MacroAssembler::Address(regs.regUnicodeInputAndTrail), resultReg);
    jit.and32(surrogateTagMask, resultReg, regs.unicodeAndSubpatternIdTemp);
    slowCases.append(jit.branch32(MacroAssembler::NotEqual, regs.unicodeAndSubpatternIdTemp, surrogatePairTags));

    // Create the UTF32 character from the surrogate pair.
#if CPU(ARM64)
    jit.urshift32(resultReg, MacroAssembler::TrustedImm32(16), regs.unicodeAndSubpatternIdTemp);
    jit.insertBitField32(resultReg, MacroAssembler::TrustedImm32(10), MacroAssembler::TrustedImm32(10), regs.unicodeAndSubpatternIdTemp);
    jit.add32(MacroAssembler::TrustedImm32(0x10000), regs.unicodeAndSubpatternIdTemp, resultReg);
#else
    jit.and32(MacroAssembler::TrustedImm32(0xffff), resultReg, regs.unicodeAndSubpatternIdTemp);
    jit.lshift32(MacroAssembler::TrustedImm32(10), regs.unicodeAndSubpatternIdTemp);
    jit.urshift32(resultReg, MacroAssembler::TrustedImm32(16), resultReg);
    jit.getEffectiveAddress(MacroAssembler::BaseIndex(regs.unicodeAndSubpatternIdTemp, resultReg, MacroAssembler::TimesOne, -U16_SURROGATE_OFFSET), resultReg);
#endif

#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
    if (useNonBMPOptimization == TryReadUnicodeCharGenFirstNonBMPOptimization::UseOptimization)
        jit.move(MacroAssembler::TrustedImm32(1), regs.firstCharacterAdditionalReadSize);
#endif
    done.append(jit.jump());

    slowCases.link(&jit);
#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
    if constexpr (useNonBMPOptimization == TryReadUnicodeCharGenFirstNonBMPOptimization::UseOptimization)
        jit.nearCallThunk(CodeLocationLabel { vm.getCTIStub(tryReadUnicodeCharIncForNonBMPSlowThunkGenerator).template retaggedCode<NoPtrTag>() });
    else
#endif
        jit.nearCallThunk(CodeLocationLabel { vm.getCTIStub(tryReadUnicodeCharSlowThunkGenerator).template retaggedCode<NoPtrTag>() });
    done.link(&jit);

    if (resultReg != regs.regT0)
        jit.swap(regs.regT0, resultReg);
}

template<TryReadUnicodeCharGenFirstNonBMPOptimization useNonBMPOptimization>
void tryReadUnicodeCharSlowImpl(CCallHelpers& jit)
{
    MacroAssembler::JumpList bmpOnly;
    MacroAssembler::JumpList isBMP;
    MacroAssembler::JumpList notSurrogatePair;
    MacroAssembler::JumpList checkForDanglingSurrogates;
    MacroAssembler::JumpList bmpDone;
    MacroAssembler::JumpList haveResult;

    YarrJITDefaultRegisters regs;

    // This code generator is used to build two variations of a character reader that handles Unicode non-BMP surrogate pairs.
    // This code generator is used to build thunks or as inline code. Its "calling convention" is unconventional.
    // It assumes the following registers are already populated with these values:
    // regs.regUnicodeInputAndTrail is the string address to start reading from.
    // regs.input contains the pointer of the beginning of the string.
    // regs.endOfStringAddress contains the address one past the end of the string.
    // For architectures that put the surrogate masks and tags in registers,
    // regs.surrogateTagMask contains 0xfc00fc00 and regs.surrogatePairTags contains 0xdc00d800.
    // When the YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP optimization is enabled and used,
    // regs.firstCharacterAdditionalReadSize is used to advance 2 characters when we read a non-BMP codepoint.
    // regs.unicodeAndSubpatternIdTemp is used as a temporary.
    // The result is returned via regs.regT0.

    auto resultReg = regs.regT0;

    // Check if we can read two UTF-16 characters at once.
    jit.add64(MacroAssembler::TrustedImm32(4), regs.regUnicodeInputAndTrail, regs.unicodeAndSubpatternIdTemp);
    bmpOnly.append(jit.branchPtr(MacroAssembler::Above, regs.unicodeAndSubpatternIdTemp, regs.endOfStringAddress));

    // Load and try to process two UTF-16 characters.
    // If they are a proper surrogate pair, compute the non-BMP codepoint.
    jit.load32(MacroAssembler::Address(regs.regUnicodeInputAndTrail), resultReg);
#if CPU(ARM64)
    jit.and32AndSetFlags(surrogateTagMask, resultReg, regs.unicodeAndSubpatternIdTemp);
    isBMP.append(jit.branch(MacroAssembler::Zero));
#else
    jit.and32(surrogateTagMask, resultReg, regs.unicodeAndSubpatternIdTemp);
    isBMP.append(jit.branch32(MacroAssembler::Equal, regs.unicodeAndSubpatternIdTemp, MacroAssembler::TrustedImm32(0)));
#endif

    // if it is surrogate pair, we already handled it in the inlined code.

    // Check if we can return the dangling surrogate or if it is part of a valid pair where the leading surrogate
    // that is offset one character before the load pointer.
    jit.and32(MacroAssembler::TrustedImm32(0xffff), regs.unicodeAndSubpatternIdTemp);
    // If it is a leading surrogate, the check above proved that it wasn't followed by a trailing surrogate.
    // If so fall through, otherwise perform other dangling checks.
    checkForDanglingSurrogates.append(jit.branch32(MacroAssembler::Equal, regs.unicodeAndSubpatternIdTemp, MacroAssembler::TrustedImm32(0xdc00)));

    isBMP.link(jit);
    jit.and32(MacroAssembler::TrustedImm32(0xffff), resultReg);

    jit.ret();

    checkForDanglingSurrogates.link(&jit);
    // Remove the second character that we loaded.
    jit.and32(MacroAssembler::TrustedImm32(0xffff), resultReg);
    MacroAssembler::Label checkForDanglingSurrogatesLabel(&jit);

    // Can ew read the prior character?
    jit.subPtr(MacroAssembler::TrustedImm32(2), regs.regUnicodeInputAndTrail);
    // If not, we branch to return the dangling surrogate.
    bmpDone.append(jit.branchPtr(MacroAssembler::Below, regs.regUnicodeInputAndTrail, regs.input));

    // Load the prior character and check if it is a leading surrogate.
    jit.load16Unaligned(MacroAssembler::Address(regs.regUnicodeInputAndTrail), regs.regUnicodeInputAndTrail);
    jit.and32(surrogateTagMask, regs.regUnicodeInputAndTrail, regs.unicodeAndSubpatternIdTemp);
    // It wasn't a leading surrogate, so return the original dangling surrogate.
    bmpDone.append(jit.branch32(MacroAssembler::NotEqual, regs.unicodeAndSubpatternIdTemp, MacroAssembler::TrustedImm32(0xd800)));

    // The prior characters was a leading surrogate, Ecma262 says that this is an error, so return the error code point.
    jit.move(MacroAssembler::TrustedImm32(errorCodePoint), resultReg);
    bmpDone.append(jit.jump());

    bmpOnly.link(&jit);
    // Can't read two characters, then just read one.
    jit.load16Unaligned(MacroAssembler::Address(regs.regUnicodeInputAndTrail), resultReg);

    // Is the character a trailing surrogate?
    jit.and32(surrogateTagMask, resultReg, regs.unicodeAndSubpatternIdTemp);
    // If so, branch back to handle the possibility that we loaded the second surrogate of a proper pair.
    jit.branch32(MacroAssembler::Equal, regs.unicodeAndSubpatternIdTemp, MacroAssembler::TrustedImm32(0xdc00)).linkTo(checkForDanglingSurrogatesLabel, jit);

    bmpDone.link(&jit);

    haveResult.link(&jit);
}


#endif // ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)

template<class YarrJITRegs = YarrJITDefaultRegisters>
class YarrGenerator final : public YarrJITInfo {
    class MatchTargets {
    public:
        enum class PreferredTarget : uint8_t {
            NoPreference = 0,
            PreferMatchSucceeded = 1,
            MatchFailFallThrough = PreferMatchSucceeded,
            PreferMatchFailed = 2,
            MatchSuccessFallThrough = PreferMatchFailed
        };

        MatchTargets(PreferredTarget preferredTarget = PreferredTarget::NoPreference)
            : m_preferredTarget(preferredTarget)
        { }

        MatchTargets(MacroAssembler::JumpList& matchDest)
            : m_matchSucceededTargets(&matchDest)
            , m_preferredTarget(PreferredTarget::PreferMatchSucceeded)
        { }

        MatchTargets(MacroAssembler::JumpList& compareDest, PreferredTarget preferredTarget)
            : m_preferredTarget(preferredTarget)
        {
            if (preferredTarget == PreferredTarget::PreferMatchFailed)
                m_matchFailedTargets = &compareDest;
            else
                m_matchSucceededTargets  = &compareDest;
        }

        MatchTargets(MacroAssembler::JumpList& matchDest, MacroAssembler::JumpList& failDest, PreferredTarget preferredTarget = PreferredTarget::NoPreference)
            : m_matchSucceededTargets(&matchDest)
            , m_matchFailedTargets(&failDest)
            , m_preferredTarget(preferredTarget)
        { }

        PreferredTarget NODELETE preferredTarget()
        {
            return m_preferredTarget;
        }

        bool NODELETE hasSucceedTarget()
        {
            return m_matchSucceededTargets != nullptr;
        }

        bool NODELETE hasFailedTarget()
        {
            return m_matchFailedTargets != nullptr;
        }

        MacroAssembler::JumpList& NODELETE matchSucceeded() { return *m_matchSucceededTargets; }
        MacroAssembler::JumpList& NODELETE matchFailed() { return *m_matchFailedTargets; }

        void appendSucceeded(MacroAssembler::Jump jump)
        {
            ASSERT(m_matchSucceededTargets != nullptr);
            m_matchSucceededTargets->append(jump);
        }

        void appendFailed(MacroAssembler::Jump jump)
        {
            ASSERT(m_matchFailedTargets != nullptr);
            m_matchFailedTargets->append(jump);
        }

    private:
        MacroAssembler::JumpList* m_matchSucceededTargets { nullptr };
        MacroAssembler::JumpList* m_matchFailedTargets { nullptr };
        PreferredTarget m_preferredTarget;
    };

#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
    struct ParenContextSizes {
        size_t m_numSubpatterns;
        size_t m_numDuplicateNamedCaptures;
        size_t m_frameSlots;

        ParenContextSizes(size_t numSubpatterns, size_t numDuplicateNamedCaptures, size_t frameSlots)
            : m_numSubpatterns(numSubpatterns)
            , m_numDuplicateNamedCaptures(numDuplicateNamedCaptures)
            , m_frameSlots(frameSlots)
        {
        }

        size_t NODELETE numSubpatterns() { return m_numSubpatterns; }

        size_t NODELETE numDuplicateNamedCaptures() { return m_numDuplicateNamedCaptures; }

        size_t NODELETE frameSlots() { return m_frameSlots; }
    };

    struct ParenContext {
        struct ParenContext* next;
        struct BeginAndMatchAmount {
            uint32_t begin;
            uint32_t matchAmount;
        } beginAndMatchAmount;
        uintptr_t returnAddress;
        struct Subpatterns {
            unsigned start;
            unsigned end;
        } subpatterns[0];
        unsigned duplicateNamedCaptures[0];
        uintptr_t frameSlots[0];

        static size_t NODELETE sizeFor(ParenContextSizes& parenContextSizes)
        {
            return sizeof(ParenContext) + sizeof(Subpatterns) * parenContextSizes.numSubpatterns() + sizeof(unsigned) * (parenContextSizes.numDuplicateNamedCaptures()) + sizeof(uintptr_t) * parenContextSizes.frameSlots();
        }

        static constexpr ptrdiff_t NODELETE nextOffset()
        {
            return offsetof(ParenContext, next);
        }

        static constexpr ptrdiff_t NODELETE beginOffset()
        {
            return offsetof(ParenContext, beginAndMatchAmount) + offsetof(BeginAndMatchAmount, begin);
        }

        static constexpr ptrdiff_t NODELETE matchAmountOffset()
        {
            return offsetof(ParenContext, beginAndMatchAmount) + offsetof(BeginAndMatchAmount, matchAmount);
        }

        static constexpr ptrdiff_t NODELETE returnAddressOffset()
        {
            return offsetof(ParenContext, returnAddress);
        }

        static constexpr ptrdiff_t NODELETE subpatternOffset(size_t subpattern)
        {
            return offsetof(ParenContext, subpatterns) + (subpattern - 1) * sizeof(Subpatterns);
        }

        static constexpr ptrdiff_t NODELETE duplicateNamedCaptureOffset(ParenContextSizes& parenContextSizes, size_t namedCapture)
        {
            return offsetof(ParenContext, subpatterns) + (parenContextSizes.numSubpatterns()) * sizeof(Subpatterns) + (namedCapture - 1) * sizeof(unsigned);
        }

        static ptrdiff_t NODELETE savedFrameOffset(ParenContextSizes& parenContextSizes)
        {
            return offsetof(ParenContext, subpatterns) + (parenContextSizes.numSubpatterns()) * sizeof(Subpatterns) + (parenContextSizes.numDuplicateNamedCaptures()) * sizeof(unsigned);
        }
    };

    void allocateParenContext(MacroAssembler::RegisterID result)
    {
        m_hitMatchLimit.append(m_jit.branchSub32(MacroAssembler::Zero, MacroAssembler::TrustedImm32(1), m_regs.remainingMatchCount));

        // Try to allocate from freelist first.
        MacroAssembler::Jump allocateFromStack;
        if (m_regs.freelistRegister != InvalidGPRReg) {
            allocateFromStack = m_jit.branchTestPtr(MacroAssembler::Zero, m_regs.freelistRegister);
            m_jit.move(m_regs.freelistRegister, result);
            m_jit.loadPtr(MacroAssembler::Address(m_regs.freelistRegister, ParenContext::nextOffset()), m_regs.freelistRegister);
        } else {
            m_jit.loadPtr(MacroAssembler::Address(m_regs.matchingContext, MatchingContextHolder::offsetOfFreeList()), result);
            allocateFromStack = m_jit.branchTestPtr(MacroAssembler::Zero, result);
            m_jit.transferPtr(MacroAssembler::Address(result, ParenContext::nextOffset()), MacroAssembler::Address(m_regs.matchingContext, MatchingContextHolder::offsetOfFreeList()));
        }
        auto done = m_jit.jump();

        // Freelist is null, allocate from stack.
        allocateFromStack.link(&m_jit);
        size_t parenContextSize = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(ParenContext::sizeFor(m_parenContextSizes));
        m_jit.subPtr(MacroAssembler::stackPointerRegister, MacroAssembler::TrustedImm32(static_cast<int32_t>(parenContextSize)), result);

        m_abortExecution.append(m_jit.branchPtr(MacroAssembler::Above, MacroAssembler::Address(m_regs.matchingContext, MatchingContextHolder::offsetOfStackLimit()), result));
        m_jit.move(result, MacroAssembler::stackPointerRegister);

        done.link(&m_jit);
    }

    void freeParenContext(MacroAssembler::RegisterID headPtrRegister)
    {
        if (m_regs.freelistRegister != InvalidGPRReg) {
            m_jit.storePtr(m_regs.freelistRegister, MacroAssembler::Address(headPtrRegister, ParenContext::nextOffset()));
            m_jit.move(headPtrRegister, m_regs.freelistRegister);
        } else {
            m_jit.transferPtr(MacroAssembler::Address(m_regs.matchingContext, MatchingContextHolder::offsetOfFreeList()), MacroAssembler::Address(headPtrRegister, ParenContext::nextOffset()));
            m_jit.storePtr(headPtrRegister, MacroAssembler::Address(m_regs.matchingContext, MatchingContextHolder::offsetOfFreeList()));
        }
    }

    void storeBeginAndMatchAmountToParenContext(MacroAssembler::RegisterID beginGPR, MacroAssembler::RegisterID matchAmountGPR, MacroAssembler::RegisterID parenContextGPR)
    {
        static_assert(ParenContext::beginOffset() + 4 == ParenContext::matchAmountOffset());
        m_jit.storePair32(beginGPR, matchAmountGPR, parenContextGPR, MacroAssembler::TrustedImm32(ParenContext::beginOffset()));
    }

    void loadBeginAndMatchAmountFromParenContext(MacroAssembler::RegisterID parenContextGPR, MacroAssembler::RegisterID beginGPR, MacroAssembler::RegisterID matchAmountGPR)
    {
        static_assert(ParenContext::beginOffset() + 4 == ParenContext::matchAmountOffset());
        m_jit.loadPair32(parenContextGPR, MacroAssembler::TrustedImm32(ParenContext::beginOffset()), beginGPR, matchAmountGPR);
    }

    // Frame slots are copied to/from the ParenContext save area by unrolling (small
    // counts) or by walking a pointer (large counts, to bound generated code size).
    static constexpr unsigned directCopySlotThreshold = 5;

#if CPU(ARM64)
    // Walking copies 16-byte ldp/stp pairs, emitting a runtime loop only once there are
    // enough pairs to amortize its setup; smaller counts are unrolled.
    static constexpr unsigned pairsPerLoopIteration = 4;
    static constexpr unsigned minLoopIterations = 2;
#endif

    enum class FrameSlotTransfer : bool { Save, Restore };

    // Copy frame slots [firstSlot, lastSlot) between the frame and the ParenContext save area.
    // contextGPR is preserved. framePointerGPR and scratchGPR are clobbered in the walking path.
    void emitFrameSlotsTransfer(FrameSlotTransfer direction, MacroAssembler::RegisterID contextGPR, MacroAssembler::RegisterID framePointerGPR, MacroAssembler::RegisterID scratchGPR, unsigned firstSlot, unsigned lastSlot)
    {
        ASSERT(noOverlap(contextGPR, framePointerGPR, scratchGPR, GPRInfo::callFrameRegister));

        if (firstSlot >= lastSlot)
            return;

        bool isSave = direction == FrameSlotTransfer::Save;
        unsigned count = lastSlot - firstSlot;
        const ptrdiff_t contextSaveAreaOffset = ParenContext::savedFrameOffset(m_parenContextSizes);

        // Copy each slot directly if count is less than the threshold.
        if (count < directCopySlotThreshold) {
            for (unsigned slot = firstSlot; slot < lastSlot; ++slot) {
                MacroAssembler::Address frameSlot = frameAddress().withOffset(slot * sizeof(uintptr_t));
                MacroAssembler::Address contextSlot(contextGPR, contextSaveAreaOffset + slot * sizeof(uintptr_t));
                if (isSave)
                    m_jit.transferPtr(frameSlot, contextSlot);
                else
                    m_jit.transferPtr(contextSlot, frameSlot);
            }
            return;
        }

        const intptr_t framePointerBias = frameAddress().offset + static_cast<intptr_t>(firstSlot) * static_cast<intptr_t>(sizeof(uintptr_t));
        const intptr_t contextPointerBias = contextSaveAreaOffset + static_cast<intptr_t>(firstSlot) * static_cast<intptr_t>(sizeof(uintptr_t));

        m_jit.addPtr(MacroAssembler::TrustedImmPtr(framePointerBias), GPRInfo::callFrameRegister, framePointerGPR);
        m_jit.addPtr(MacroAssembler::TrustedImmPtr(contextPointerBias), contextGPR);

#if CPU(ARM64)
        auto emitCopy = [&](unsigned slots) {
            MacroAssembler::PostIndexAddress frame(framePointerGPR, slots * sizeof(uintptr_t));
            MacroAssembler::PostIndexAddress context(contextGPR, slots * sizeof(uintptr_t));
            auto src = isSave ? frame : context;
            auto dst = isSave ? context : frame;
            if (slots == 2)
                m_jit.transferPair64(src, dst);
            else
                m_jit.transfer64(src, dst);
        };

        unsigned pairCount = count / 2;
        bool hasOddSlot = count & 1;

        unsigned loopIterations = 0;
        unsigned tailPairs = pairCount;
        if (pairCount >= pairsPerLoopIteration * minLoopIterations) {
            loopIterations = pairCount / pairsPerLoopIteration;
            tailPairs = pairCount % pairsPerLoopIteration;
        }

        if (loopIterations) {
            m_jit.move(MacroAssembler::TrustedImm32(loopIterations), scratchGPR);
            MacroAssembler::Label loopHead(&m_jit);
            for (unsigned i = 0; i < pairsPerLoopIteration; ++i)
                emitCopy(2);
            m_jit.branchSub32(MacroAssembler::NonZero, MacroAssembler::TrustedImm32(1), scratchGPR).linkTo(loopHead, &m_jit);
        }
        for (unsigned i = 0; i < tailPairs; ++i)
            emitCopy(2);
        if (hasOddSlot)
            emitCopy(1);
#else
        m_jit.move(MacroAssembler::TrustedImm32(count), scratchGPR);
        MacroAssembler::Label loopHead(&m_jit);
        if (isSave)
            m_jit.transferPtr(MacroAssembler::Address(framePointerGPR), MacroAssembler::Address(contextGPR));
        else
            m_jit.transferPtr(MacroAssembler::Address(contextGPR), MacroAssembler::Address(framePointerGPR));
        m_jit.addPtr(MacroAssembler::TrustedImm32(sizeof(uintptr_t)), framePointerGPR);
        m_jit.addPtr(MacroAssembler::TrustedImm32(sizeof(uintptr_t)), contextGPR);
        m_jit.branchSub32(MacroAssembler::NonZero, MacroAssembler::TrustedImm32(1), scratchGPR).linkTo(loopHead, &m_jit);
#endif

        // Wind contextGPR back to its incoming value.
        m_jit.subPtr(MacroAssembler::TrustedImmPtr(contextPointerBias + static_cast<intptr_t>(count) * static_cast<intptr_t>(sizeof(uintptr_t))), contextGPR);
    }

    // On return matchAmountGPR holds the pre-iteration matchAmount for the caller to increment.
    void saveParenContext(MacroAssembler::RegisterID parenContextReg, MacroAssembler::RegisterID matchAmountGPR, MacroAssembler::RegisterID scratchGPR, unsigned firstSubpattern, unsigned lastSubpattern, unsigned subpatternBaseFrameLocation, bool savesReturnAddress)
    {
        ASSERT(noOverlap(parenContextReg, matchAmountGPR, scratchGPR, m_regs.index, GPRInfo::callFrameRegister));

        BitVector duplicateNamedCaptureGroups;
        bool hasNamedCaptures = m_pattern.hasDuplicateNamedCaptureGroups();

        emitFrameSlotsTransfer(FrameSlotTransfer::Save, parenContextReg, matchAmountGPR, scratchGPR, subpatternBaseFrameLocation + YarrStackSpaceForBackTrackInfoParentheses, m_parenContextSizes.frameSlots());

        // Save-at-BEGIN: snapshot the pre-iteration index and matchAmount.
        loadFromFrame(subpatternBaseFrameLocation + BackTrackInfoParentheses::matchAmountIndex(), matchAmountGPR);
        storeBeginAndMatchAmountToParenContext(m_regs.index, matchAmountGPR, parenContextReg);
        // returnAddress is only written by multi-alt parens; skip the round-trip otherwise.
        if (savesReturnAddress) {
            m_jit.transferPtr(
                frameAddress().withOffset((subpatternBaseFrameLocation + BackTrackInfoParentheses::returnAddressIndex()) * sizeof(uintptr_t)),
                MacroAssembler::Address(parenContextReg, ParenContext::returnAddressOffset()));
        }
        if (shouldRecordSubpatterns()) {
            for (unsigned subpattern = firstSubpattern; subpattern <= lastSubpattern; subpattern++) {
                static_assert(is64Bit());
                m_jit.transfer64(subpatternStartAddress(subpattern), MacroAssembler::Address(parenContextReg, ParenContext::subpatternOffset(subpattern)));
                if (hasNamedCaptures) {
                    unsigned duplicateNamedGroup = m_pattern.m_duplicateNamedGroupForSubpatternId[subpattern];
                    if (duplicateNamedGroup)
                        duplicateNamedCaptureGroups.set(duplicateNamedGroup);
                }
                // Reset nested captures per ECMAScript spec.
                clearSubpattern(subpattern);
            }
            for (unsigned duplicateNamedGroupId : duplicateNamedCaptureGroups) {
                m_jit.transfer32(
                    duplicateNamedGroupAddress(duplicateNamedGroupId),
                    MacroAssembler::Address(parenContextReg, ParenContext::duplicateNamedCaptureOffset(m_parenContextSizes, duplicateNamedGroupId)));
                storeDuplicateNamedGroupSubpatternId(duplicateNamedGroupId, 0);
            }
        }
    }

    // On return matchAmountGPR holds the restored matchAmount, so the caller can use it without reloading from the frame.
    void restoreParenContext(MacroAssembler::RegisterID parenContextReg, MacroAssembler::RegisterID matchAmountGPR, MacroAssembler::RegisterID scratchGPR, unsigned firstSubpattern, unsigned lastSubpattern, unsigned subpatternBaseFrameLocation, bool savesReturnAddress)
    {
        ASSERT(noOverlap(parenContextReg, matchAmountGPR, scratchGPR, m_regs.index, GPRInfo::callFrameRegister));

        BitVector duplicateNamedCaptureGroups;
        bool hasNamedCaptures = m_pattern.hasDuplicateNamedCaptureGroups();

        // Copy the saved frame slots back first (this clobbers matchAmountGPR), then load
        // matchAmount into matchAmountGPR so it stays live for the caller.
        emitFrameSlotsTransfer(FrameSlotTransfer::Restore, parenContextReg, matchAmountGPR, scratchGPR, subpatternBaseFrameLocation + YarrStackSpaceForBackTrackInfoParentheses, m_parenContextSizes.frameSlots());

        loadBeginAndMatchAmountFromParenContext(parenContextReg, m_regs.index, matchAmountGPR);
        storeToFrame(m_regs.index, subpatternBaseFrameLocation + BackTrackInfoParentheses::beginIndex());
        storeToFrame(matchAmountGPR, subpatternBaseFrameLocation + BackTrackInfoParentheses::matchAmountIndex());
        if (savesReturnAddress) {
            m_jit.transferPtr(
                MacroAssembler::Address(parenContextReg, ParenContext::returnAddressOffset()),
                frameAddress().withOffset((subpatternBaseFrameLocation + BackTrackInfoParentheses::returnAddressIndex()) * sizeof(void*)));
        }
        if (shouldRecordSubpatterns()) {
            for (unsigned subpattern = firstSubpattern; subpattern <= lastSubpattern; subpattern++) {
                static_assert(is64Bit());
                m_jit.transfer64(MacroAssembler::Address(parenContextReg, ParenContext::subpatternOffset(subpattern)), subpatternStartAddress(subpattern));
                if (hasNamedCaptures) {
                    unsigned duplicateNamedGroup = m_pattern.m_duplicateNamedGroupForSubpatternId[subpattern];
                    if (duplicateNamedGroup)
                        duplicateNamedCaptureGroups.set(duplicateNamedGroup);
                }
            }
            for (unsigned duplicateNamedGroupId : duplicateNamedCaptureGroups) {
                m_jit.transfer32(
                    MacroAssembler::Address(parenContextReg, ParenContext::duplicateNamedCaptureOffset(m_parenContextSizes, duplicateNamedGroupId)),
                    duplicateNamedGroupAddress(duplicateNamedGroupId));
            }
        }
    }
#endif

    size_t alternativeEndOpIndex(size_t opIndex)
    {
        if (m_ops[opIndex].m_endOp != notFound)
            return m_ops[opIndex].m_endOp;
        size_t endIndex = opIndex;
        while (m_ops[endIndex].m_nextOp != notFound)
            endIndex = m_ops[endIndex].m_nextOp;
        ASSERT(m_ops[endIndex].m_op == YarrOpCode::SimpleNestedAlternativeEnd || m_ops[endIndex].m_op == YarrOpCode::StringListAlternativeEnd || m_ops[endIndex].m_op == YarrOpCode::NestedAlternativeEnd);
        for (size_t i = opIndex; i != notFound; i = m_ops[i].m_nextOp)
            m_ops[i].m_endOp = endIndex;
        return endIndex;
    }

    void NODELETE optimizeAlternative(PatternAlternative* alternative)
    {
        if (!alternative->m_terms.size())
            return;

        for (unsigned i = 0; i < alternative->m_terms.size() - 1; ++i) {
            PatternTerm& term = alternative->m_terms[i];
            PatternTerm& nextTerm = alternative->m_terms[i + 1];

            // We can move BMP only character classes after fixed character terms.
            // When not decoding surrogate pairs (Char8 mode), only swap if the character class
            // has no non-BMP characters and is not inverted. Otherwise the swapped pattern could
            // be passed to byteCodeCompilePattern (on JIT allocation failure) and then executed
            // against a Char16 string where the changed term order would cause misreads of
            // surrogate pairs. An inverted class like [^a] has BMP-only class data but can match
            // non-BMP characters (variable width in UTF-16), so it must not be swapped either.
            if ((term.type == PatternTerm::Type::CharacterClass)
                && (term.quantityType == QuantifierType::FixedCount)
                && !term.m_invert
                && ((!m_decodeSurrogatePairs && !term.characterClass->hasNonBMPCharacters()) || term.characterClass->hasOneCharacterSize())
                && (nextTerm.type == PatternTerm::Type::PatternCharacter)
                && (nextTerm.quantityType == QuantifierType::FixedCount)) {
                PatternTerm termCopy = term;
                alternative->m_terms[i] = nextTerm;
                alternative->m_terms[i + 1] = termCopy;
            }
        }
    }

    constexpr static unsigned MaximumCharacterClassSizeForBitTest = 8 * sizeof(UCPURegister);
    using CharacterBitSet = WTF::BitSet<MaximumCharacterClassSizeForBitTest>;

    void matchCharacterClassByBitTest(MacroAssembler::RegisterID character, MacroAssembler::RegisterID scratch, MacroAssembler::JumpList& matchDest, char32_t min, char32_t max, CharacterBitSet mask)
    {
        switch (mask.count()) {
        case 0:
            return;
        case 1:
        case 2:
        case 3:
        case 4:
            // If the set is small enough, still defer to a series of branches.
            mask.forEachSetBit([&](size_t value) {
                matchDest.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::TrustedImm32(min + value)));
                return IterationStatus::Continue;
            });
            break;
        default: {
            // Otherwise, actually perform the bit test.
#if CPU(REGISTER64)
            m_jit.sub32(character, MacroAssembler::Imm32(static_cast<unsigned>(min)), scratch);
            MacroAssembler::Jump notInVector = m_jit.branch32(MacroAssembler::Above, scratch, MacroAssembler::TrustedImm32(max - min));
            m_jit.lshift64(MacroAssembler::TrustedImm32(1), scratch, scratch);
            matchDest.append(m_jit.branchTest64(MacroAssembler::NonZero, scratch, MacroAssembler::TrustedImm64(mask.storage()[0])));
#else
            m_jit.sub32(character, MacroAssembler::Imm32(static_cast<unsigned>(min)), scratch);
            MacroAssembler::Jump notInVector = m_jit.branch32(MacroAssembler::Above, scratch, MacroAssembler::TrustedImm32(max - min));
            m_jit.lshift32(MacroAssembler::TrustedImm32(1), scratch, scratch);
            matchDest.append(m_jit.branchTest32(MacroAssembler::NonZero, scratch, MacroAssembler::TrustedImm32(mask.storage()[0])));
#endif
            notInVector.link(&m_jit);
        }
        }
    }

    void matchCharacterClassSet(MacroAssembler::RegisterID character, MacroAssembler::RegisterID scratch, MacroAssembler::JumpList& matchDest, std::span<const char32_t> matches)
    {
        if (matches.empty())
            return;

        if (matches.size() == 1) {
            matchDest.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(static_cast<unsigned>(matches.front()))));
            return;
        }

        // If we have multiple matches close together (not necessarily contiguous), we
        // can try a biased bitmask - subtract the minimum match from the character,
        // then see if it's present in a precomputed mask. We keep the bitset size quite
        // small in order to keep it easy to materialize - this approach lets us avoid
        // a load or lookup table in favor of just masking against an immediate.

        char32_t min = matches.front();
        char32_t max = matches.back();
        ASSERT(max > min);
        if ((max - min) < MaximumCharacterClassSizeForBitTest) {
            CharacterBitSet mask;
            for (char32_t character : matches)
                mask.set(character - min);
            matchCharacterClassByBitTest(character, scratch, matchDest, min, max, mask);
            return;
        }

        // We have too many matches to handle in a single set, but we may be able to
        // recursively group some of our matches together. Worst case, we just do a
        // character-by-character match. Greedily matching is potentially suboptimal,
        // but I doubt worth spending time doing better.

        size_t lastStart = 0;
        for (size_t index = 1; index < matches.size(); ++index) {
            if ((matches[index] - matches[lastStart]) >= MaximumCharacterClassSizeForBitTest) {
                matchCharacterClassSet(character, scratch, matchDest, matches.subspan(lastStart, index - lastStart));
                lastStart = index;
            }
        }
        if (lastStart < matches.size())
            matchCharacterClassSet(character, scratch, matchDest, matches.subspan(lastStart));
    }

    void matchCharacterClassRange(MacroAssembler::RegisterID character, MacroAssembler::RegisterID scratch, MacroAssembler::JumpList& failures, MacroAssembler::JumpList& matchDest, std::span<const CharacterRange> ranges, std::span<const char32_t> matches, bool& shouldGenerateFailureJump, bool isTopLevel)
    {
        if (ranges.size() == 1 && !matches.size()) {
            matchCharacterClassOnlyOneRange(character, scratch, failures, ranges.front());
            matchDest.append(m_jit.jump());
            shouldGenerateFailureJump = false;
            return;
        }
        ASSERT(ranges.size()); // We could handle this case, but we shouldn't expect to reach here without any ranges.

        // Let's first see if all our ranges and matches neatly fit into a bitvector...
        uint32_t min = ranges.front().begin;
        uint32_t max = ranges.back().end;
        if (matches.size()) {
            min = std::min<uint32_t>(min, matches.front());
            max = std::max<uint32_t>(max, matches.back());
        }
        if ((max - min) < MaximumCharacterClassSizeForBitTest) {
            CharacterBitSet mask;
            for (auto range : ranges) {
                for (char32_t ch = range.begin; ch <= range.end; ++ch)
                    mask.set(ch - min);
            }
            for (auto character : matches)
                mask.set(character - min);
            matchCharacterClassByBitTest(character, scratch, matchDest, min, max, mask);
            return;
        }

        // Otherwise, binary search the ranges and matches. We still want to take advantage of a bitvector test
        // if possible, so we greedily add ranges to the median as long as we fit within the bit test size.
        unsigned whichFirst = ranges.size() >> 1;
        unsigned whichLast = whichFirst;
        char32_t lo = ranges[whichFirst].begin;
        char32_t hi = ranges[whichLast].end;
        while (whichLast < ranges.size() - 1) {
            char32_t nextHi = ranges[whichLast + 1].end;
            if (nextHi - lo < MaximumCharacterClassSizeForBitTest) {
                whichLast++;
                hi = nextHi;
            } else
                break;
        }

        // First, explore any matches below the minimum of the current range.
        unsigned smallerMatchCount = 0;
        while (smallerMatchCount < matches.size() && matches[smallerMatchCount] < lo)
            smallerMatchCount++;

        // Otherwise, explore any matches beyond the maximum of the current range.
        unsigned higherMatchStart = smallerMatchCount;
        while (higherMatchStart < matches.size() && matches[higherMatchStart] <= hi)
            higherMatchStart++;

        if (whichFirst) {
            MacroAssembler::Jump loOrAbove = m_jit.branch32(MacroAssembler::GreaterThanOrEqual, character, MacroAssembler::Imm32(static_cast<unsigned>(lo)));
            bool shouldGenerateFailureJump = true;
            matchCharacterClassRange(character, scratch, failures, matchDest, ranges.first(whichFirst), matches.first(smallerMatchCount), shouldGenerateFailureJump, false);
            if (shouldGenerateFailureJump)
                failures.append(m_jit.jump());
            loOrAbove.link(&m_jit);
        } else if (smallerMatchCount) {
            MacroAssembler::Jump loOrAbove = m_jit.branch32(MacroAssembler::GreaterThanOrEqual, character, MacroAssembler::Imm32(static_cast<unsigned>(lo)));
            matchCharacterClassSet(character, scratch, matchDest, matches.first(smallerMatchCount));
            failures.append(m_jit.jump());
            loOrAbove.link(&m_jit);
        } else
            failures.append(m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::Imm32(static_cast<unsigned>(lo))));

        // At this point we will have either matched, failed, or character is >= lo. Next, let's check if we're actually in the current range.

        if (whichFirst != whichLast) {
            CharacterBitSet mask;
            for (auto range : ranges.subspan(whichFirst, whichLast - whichFirst + 1)) {
                for (char32_t ch = range.begin; ch <= range.end; ++ch)
                    mask.set(ch - lo);
            }
            for (auto character : matches.subspan(smallerMatchCount, higherMatchStart - smallerMatchCount))
                mask.set(character - lo);
            matchCharacterClassByBitTest(character, scratch, matchDest, lo, hi, mask);
        } else
            matchDest.append(m_jit.branch32(MacroAssembler::LessThanOrEqual, character, MacroAssembler::Imm32(static_cast<unsigned>(hi))));

        if (whichLast + 1 < ranges.size()) {
            bool shouldGenerateFailureJump = true;
            matchCharacterClassRange(character, scratch, failures, matchDest, ranges.subspan(whichLast + 1), matches.subspan(higherMatchStart), shouldGenerateFailureJump, false);
            if (shouldGenerateFailureJump)
                failures.append(m_jit.jump());
        } else if (higherMatchStart < matches.size()) {
            matchCharacterClassSet(character, scratch, matchDest, matches.subspan(higherMatchStart));
            if (!isTopLevel)
                failures.append(m_jit.jump());
        }
    }

    void matchCharacterClassOnlyOneRange(const MacroAssembler::RegisterID character, const MacroAssembler::RegisterID scratch, MacroAssembler::JumpList& failMatches, const CharacterRange& range)
    {
        // Instead of doing two branches, we rely on unsigned underflow - any values below ranges.begin
        // will wrap around to the top of the 32-bit unsigned integer range, meaning all values outside
        // the range will be strictly above (end - begin).
        unsigned biasedEnd = range.end - range.begin;
        m_jit.sub32(character, MacroAssembler::Imm32(static_cast<unsigned>(range.begin)), scratch);
        failMatches.append(m_jit.branch32(MacroAssembler::Above, scratch, MacroAssembler::TrustedImm32(biasedEnd)));
    }

    void matchCharacterClassOnlyOneRange(const MacroAssembler::RegisterID character, const MacroAssembler::RegisterID scratch, MacroAssembler::JumpList& failMatches, const Vector<CharacterRange>& ranges)
    {
        ASSERT(ranges.size() == 1);
        matchCharacterClassOnlyOneRange(character, scratch, failMatches, ranges[0]);
    }

    void matchCharacterClassTable(MacroAssembler::RegisterID character, MacroAssembler::JumpList& failMatches, const char* table, bool tableInverted = false)
    {
        ASSERT(!m_decodeSurrogatePairs);
        MacroAssembler::ExtendedAddress tableEntry(character, reinterpret_cast<intptr_t>(table));
        failMatches.append(m_jit.branchTest8(tableInverted ? MacroAssembler::NonZero : MacroAssembler::Zero, tableEntry));
    }

    void matchCharacterClass(MacroAssembler::RegisterID character, MacroAssembler::RegisterID scratch, MatchTargets matchTargets, const CharacterClass* charClass)
    {
        if (charClass->m_table && !m_decodeSurrogatePairs) {
            if (matchTargets.hasFailedTarget()) {
                MacroAssembler::ExtendedAddress tableEntry(character, reinterpret_cast<intptr_t>(charClass->m_table));
                matchTargets.appendFailed(m_jit.branchTest8(charClass->m_tableInverted ? MacroAssembler::NonZero : MacroAssembler::Zero, tableEntry));
                return;
            }
            MacroAssembler::ExtendedAddress tableEntry(character, reinterpret_cast<intptr_t>(charClass->m_table));
            matchTargets.appendSucceeded(m_jit.branchTest8(charClass->m_tableInverted ? MacroAssembler::Zero : MacroAssembler::NonZero, tableEntry));
            return;
        }

        if (charClass->m_latin1Table) {
            bool pureLatin1 = charClass->m_matches32.isEmpty() && charClass->m_ranges32.isEmpty();
            if (m_charSize == CharSize::Char8 || pureLatin1) {
                const auto* table = m_boyerMooreData->addLatin1Table(*charClass->m_latin1Table);
                bool needsHighGuard = m_charSize != CharSize::Char8;
                if (matchTargets.hasFailedTarget()) {
                    if (needsHighGuard)
                        matchTargets.appendFailed(m_jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(0x100)));
                    MacroAssembler::ExtendedAddress tableEntry(character, reinterpret_cast<intptr_t>(table));
                    matchTargets.appendFailed(m_jit.branchTest8(MacroAssembler::Zero, tableEntry));
                    return;
                }

                MacroAssembler::Jump isHigh;
                if (needsHighGuard)
                    isHigh = m_jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(0x100));
                MacroAssembler::ExtendedAddress tableEntry(character, reinterpret_cast<intptr_t>(table));
                matchTargets.appendSucceeded(m_jit.branchTest8(MacroAssembler::NonZero, tableEntry));
                if (isHigh.isSet())
                    isHigh.link(&m_jit);
                return;
            }
        }

        Vector<char32_t, 32> unifiedMatches;
        Vector<CharacterRange, 32> unifiedRanges;
        unifiedMatches.appendVector(charClass->m_matches8);
        unifiedRanges.appendVector(charClass->m_ranges8);
        // m_matches32 / m_ranges32 hold non-Latin-1 entries (>= 0x100). For Char8
        // input the character register can only ever hold a value <= 0xff, so those entries
        // are unreachable — skip them to avoid emitting dead compares.
        if (m_charSize != CharSize::Char8) {
            unifiedMatches.appendVector(charClass->m_matches32);
            unifiedRanges.appendVector(charClass->m_ranges32);
        }

        ASSERT(std::is_sorted(unifiedMatches.begin(), unifiedMatches.end(), [](auto& lhs, auto& rhs) {
            return lhs < rhs;
        }));
        ASSERT(std::is_sorted(unifiedRanges.begin(), unifiedRanges.end(), [](auto& lhs, auto& rhs) {
            return lhs.begin < rhs.begin;
        }));

        std::ranges::sort(unifiedMatches, [](auto& lhs, auto& rhs) {
            return lhs < rhs;
        });
        std::ranges::sort(unifiedRanges, [](auto& lhs, auto& rhs) {
            return lhs.begin < rhs.begin;
        });

        if (!unifiedRanges.size() && !unifiedMatches.size() && matchTargets.hasFailedTarget()) {
            matchTargets.appendFailed(m_jit.jump());
            return;
        }

        if (unifiedRanges.size()) {
            MacroAssembler::JumpList failures;
            bool shouldGenerateFailureJump = false;
            matchCharacterClassRange(character, scratch, failures, matchTargets.matchSucceeded(), unifiedRanges.span(), unifiedMatches.span(), shouldGenerateFailureJump, true);
            failures.link(&m_jit);
        } else if (unifiedMatches.size())
            matchCharacterClassSet(character, scratch, matchTargets.matchSucceeded(), unifiedMatches.span());
    }

    void matchCharacterClassTermInner(PatternTerm* term, MacroAssembler::JumpList& failures, const MacroAssembler::RegisterID character, const MacroAssembler::RegisterID scratch)
    {
        ASSERT(term->type == PatternTerm::Type::CharacterClass);

        auto processCharacterClass = [&] (CharacterClass* characterClassToProcess) {
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
            if (m_decodeSurrogatePairs && term->invert())
                failures.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::TrustedImm32(errorCodePoint)));
#endif
            if (term->invert())
                matchCharacterClass(character, scratch, failures, characterClassToProcess);
            else if (characterClassToProcess->m_matches8.isEmpty() && characterClassToProcess->m_matches32.isEmpty()
                && (characterClassToProcess->m_ranges8.size() + characterClassToProcess->m_ranges32.size()) == 1) {
                if (m_charSize == CharSize::Char8 && characterClassToProcess->m_ranges8.isEmpty()) {
                    // The sole range is in m_ranges32 (>= 0x100), unreachable for Char8 input.
                    failures.append(m_jit.jump());
                } else
                    matchCharacterClassOnlyOneRange(character, scratch, failures, characterClassToProcess->m_ranges8.size() ? characterClassToProcess->m_ranges8 : characterClassToProcess->m_ranges32);
            } else {
                MacroAssembler::JumpList matchDest;
                // If we are matching the "any character" builtin class we only need to read the
                // character and don't need to match, as it will always succeed -- except that in a
                // unicode pattern a dangling surrogate reads as errorCodePoint, which is not a
                // character and must fail (the inverted-class path above rejects it likewise).
                if (!characterClassToProcess->m_anyCharacter) {
                    matchCharacterClass(character, scratch, MatchTargets(matchDest, failures, MatchTargets::PreferredTarget::MatchSuccessFallThrough), characterClassToProcess);
                    if (!matchDest.empty())
                        failures.append(m_jit.jump());
                }
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
                else if (m_decodeSurrogatePairs)
                    failures.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::TrustedImm32(errorCodePoint)));
#endif
                matchDest.link(&m_jit);
            }
        };

        processCharacterClass(term->characterClass);

        // Note that this falls through on a successful characterClass match.
    }

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
    void advanceIndexAfterCharacterClassTermMatch(const PatternTerm* term, MacroAssembler::JumpList& failuresAfterIncrementingIndex, const MacroAssembler::RegisterID character)
    {
        ASSERT(term->type == PatternTerm::Type::CharacterClass);

        m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
        if (term->isFixedWidthCharacterClass()) {
            if (term->characterClass->hasNonBMPCharacters()) {
                failuresAfterIncrementingIndex.append(atEndOfInput());
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
            }
        } else {
            MacroAssembler::Jump isBMPChar = m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::TrustedImm32(0x10000));
            failuresAfterIncrementingIndex.append(atEndOfInput());
            m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
            isBMPChar.link(&m_jit);
        }
    }
#endif

    // Input availability primitives.
    //
    // Forward: `index` is the frontier of claimed input; claiming N characters is
    // `index += N` (fails if that passes `length`).
    // Backward (lookbehind body): `index` is the leftward-moving cursor / left
    // frontier; claiming N characters is `index -= N` (fails if it goes below
    // zero). Both `index` and any claimable N are < 2^31: `index <= length` and
    // string lengths are bounded by INT32_MAX, and a claim count larger than that
    // marks the pattern containsUnsignedLengthPattern(), which never reaches the
    // JIT. So a borrowing subtraction sets the sign bit, letting us branch on the
    // Signed/PositiveOrZero result of the same instruction, exactly mirroring the
    // forward add-then-compare.

    // Jumps if input not available; will have (incorrectly) incremented already!
    MacroAssembler::Jump jumpIfNoAvailableInput(unsigned countToCheck = 0)
    {
        if (m_direction == Backward) {
            // Backward: the frontier is exhausted once index would go below zero.
            // With a count, claim it and branch on the borrow; with none, just test
            // the current frontier's sign (the leftward mirror of index > length).
            if (countToCheck)
                return m_jit.branchSub32(MacroAssembler::Signed, MacroAssembler::TrustedImm32(countToCheck), m_regs.index);
            return m_jit.branch32(MacroAssembler::LessThan, m_regs.index, MacroAssembler::TrustedImm32(0));
        }
        if (countToCheck)
            m_jit.add32(MacroAssembler::Imm32(countToCheck), m_regs.index);
        return m_jit.branch32(MacroAssembler::Above, m_regs.index, m_regs.length);
    }

    // Extend the leftward claim by one unit (a decoded pair's lead). On the
    // failure path the borrowed unit is restored before joining `failures`, so
    // a negative frontier is never left live where another term (e.g. a sibling
    // op's non-greedy re-entry) could address input through it before this term's
    // own backtrack restores its beginIndex.
    void claimBackwardPairLead(MacroAssembler::JumpList& failures)
    {
        ASSERT(m_direction == Backward);
        MacroAssembler::Jump claimed = m_jit.branchSub32(MacroAssembler::PositiveOrZero, MacroAssembler::TrustedImm32(1), m_regs.index);
        m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index); // undo the failed borrow
        failures.append(m_jit.jump());
        claimed.link(&m_jit);
    }

    MacroAssembler::Jump jumpIfAvailableInput(unsigned countToCheck)
    {
        if (m_direction == Backward)
            return m_jit.branchSub32(MacroAssembler::PositiveOrZero, MacroAssembler::TrustedImm32(countToCheck), m_regs.index);
        m_jit.add32(MacroAssembler::Imm32(countToCheck), m_regs.index);
        return m_jit.branch32(MacroAssembler::BelowOrEqual, m_regs.index, m_regs.length);
    }

    MacroAssembler::Jump checkNotEnoughInput(MacroAssembler::RegisterID additionalAmount)
    {
        ASSERT(m_direction == Forward);
        m_jit.add32(m_regs.index, additionalAmount);
        return m_jit.branch32(MacroAssembler::Above, additionalAmount, m_regs.length);
    }

    // Backward (lookbehind) form: a dynamic span of `amount` units must lie
    // wholly left of the cursor. Computes the span's left edge (index - amount)
    // into `edge` and jumps if it would be negative; the caller then walks the
    // span rightward from `edge` and finally moves the frontier down to it.
    MacroAssembler::Jump checkNotEnoughInputBackward(MacroAssembler::RegisterID amount, MacroAssembler::RegisterID edge)
    {
        ASSERT(m_direction == Backward);
        m_jit.move(m_regs.index, edge);
        m_jit.sub32(amount, edge);
        return m_jit.branch32(MacroAssembler::LessThan, edge, MacroAssembler::TrustedImm32(0));
    }

    // A backreference's span of `size` units: forward, it lies right of the
    // frontier (availability check only; the walk advances the frontier through
    // it); in a mirrored frame it lies left of the frontier, so the frontier is
    // first lowered to the span's left edge -- recorded in the frame so the walk,
    // which raises index back up as it compares, can be undone by
    // emitBackReferenceSpanFrontier -- and the walk proceeds rightward from there.
    void emitBackReferenceSpanClaim(MacroAssembler::JumpList& notAvailable, MacroAssembler::RegisterID size, MacroAssembler::RegisterID edge, unsigned frameLocation)
    {
        if (m_direction != Backward) {
            notAvailable.append(checkNotEnoughInput(size));
            return;
        }
        notAvailable.append(checkNotEnoughInputBackward(size, edge));
        storeToFrame(edge, frameLocation + BackTrackInfoBackReference::backwardSpanEdgeIndex());
        m_jit.move(edge, m_regs.index);
    }

    // After a mirrored backreference walk, the span has been consumed leftward:
    // the frontier rests at the span's left edge, not where the walk left it.
    void emitBackReferenceSpanFrontier(unsigned frameLocation)
    {
        if (m_direction == Backward)
            loadFromFrame(frameLocation + BackTrackInfoBackReference::backwardSpanEdgeIndex(), m_regs.index);
    }

    MacroAssembler::Jump checkInput()
    {
        if (m_direction == Backward)
            return m_jit.branch32(MacroAssembler::GreaterThanOrEqual, m_regs.index, MacroAssembler::TrustedImm32(0));
        return m_jit.branch32(MacroAssembler::BelowOrEqual, m_regs.index, m_regs.length);
    }

    MacroAssembler::Jump atEndOfInput()
    {
        if (m_direction == Backward)
            return m_jit.branchTest32(MacroAssembler::Zero, m_regs.index);
        return m_jit.branch32(MacroAssembler::Equal, m_regs.index, m_regs.length);
    }

    MacroAssembler::Jump notAtEndOfInput()
    {
        if (m_direction == Backward)
            return m_jit.branchTest32(MacroAssembler::NonZero, m_regs.index);
        return m_jit.branch32(MacroAssembler::NotEqual, m_regs.index, m_regs.length);
    }

    MacroAssembler::BaseIndex negativeOffsetIndexedAddress(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID tempReg)
    {
        return negativeOffsetIndexedAddress(negativeCharacterOffset, tempReg, m_regs.index);
    }

    MacroAssembler::BaseIndex negativeOffsetIndexedAddress(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID tempReg, MacroAssembler::RegisterID indexReg)
    {
        MacroAssembler::RegisterID base = m_regs.input;

        // MacroAssembler::BaseIndex() addressing can take a int32_t offset. Given that we can have a regular
        // expression that has unsigned character offsets, MacroAssembler::BaseIndex's signed offset is insufficient
        // for addressing in extreme cases where we might underflow. Therefore we check to see if
        // negativeCharacterOffset will underflow directly or after converting for 16 bit characters.
        // If so, we do our own address calculating by adjusting the base, using the result register
        // as a temp address register.
        unsigned maximumNegativeOffsetForCharacterSize = m_charSize == CharSize::Char8 ? 0x7fffffff : 0x3fffffff;
        unsigned offsetAdjustAmount = 0x40000000;

        if (m_direction == Backward) {
            // In a lookbehind body the index register is the left frontier of the
            // (leftward) claimed input. A forward frame offset k (1..checkedTotal)
            // names the k'th character behind the forward frontier; mirrored, that
            // character lives at input[index + (k - 1)]. k == 0 (a peek at the
            // frontier itself, used by anchors) maps to input[index - 1].
            Checked<int32_t> characterOffset = static_cast<int32_t>(negativeCharacterOffset.value()) - 1;
            unsigned positiveCharacterOffset = negativeCharacterOffset.value() ? negativeCharacterOffset.value() - 1 : 0;
            if (positiveCharacterOffset > maximumNegativeOffsetForCharacterSize) {
                base = tempReg;
                m_jit.move(m_regs.input, base);
                while (positiveCharacterOffset > maximumNegativeOffsetForCharacterSize) {
                    m_jit.addPtr(MacroAssembler::TrustedImm32(offsetAdjustAmount), base);
                    if (m_charSize != CharSize::Char8)
                        m_jit.addPtr(MacroAssembler::TrustedImm32(offsetAdjustAmount), base);
                    positiveCharacterOffset -= offsetAdjustAmount;
                }
                characterOffset = static_cast<int32_t>(positiveCharacterOffset);
            }
            if (m_charSize == CharSize::Char8)
                return MacroAssembler::BaseIndex(base, indexReg, MacroAssembler::TimesOne, characterOffset.value() * static_cast<int32_t>(sizeof(char)));
            return MacroAssembler::BaseIndex(base, indexReg, MacroAssembler::TimesTwo, characterOffset.value() * static_cast<int32_t>(sizeof(char16_t)));
        }

        if (negativeCharacterOffset > maximumNegativeOffsetForCharacterSize) {
            base = tempReg;
            m_jit.move(m_regs.input, base);
            while (negativeCharacterOffset > maximumNegativeOffsetForCharacterSize) {
                m_jit.subPtr(MacroAssembler::TrustedImm32(offsetAdjustAmount), base);
                if (m_charSize != CharSize::Char8)
                    m_jit.subPtr(MacroAssembler::TrustedImm32(offsetAdjustAmount), base);
                negativeCharacterOffset -= offsetAdjustAmount;
            }
        }

        Checked<int32_t> characterOffset(-static_cast<int32_t>(negativeCharacterOffset));

        if (m_charSize == CharSize::Char8)
            return MacroAssembler::BaseIndex(base, indexReg, MacroAssembler::TimesOne, characterOffset * static_cast<int32_t>(sizeof(char)));

        return MacroAssembler::BaseIndex(base, indexReg, MacroAssembler::TimesTwo, characterOffset * static_cast<int32_t>(sizeof(char16_t)));
    }

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
    void tryReadUnicodeChar(MacroAssembler::BaseIndex address, MacroAssembler::RegisterID resultReg)
    {
        ASSERT(m_charSize == CharSize::Char16);

        m_jit.getEffectiveAddress(address, m_regs.regUnicodeInputAndTrail);

#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
        if (m_useFirstNonBMPCharacterOptimization) {
            tryReadUnicodeCharImpl<TryReadUnicodeCharGenFirstNonBMPOptimization::UseOptimization>(*m_vm, m_jit, resultReg);
            return;
        }
#endif
        tryReadUnicodeCharImpl<TryReadUnicodeCharGenFirstNonBMPOptimization::DontUseOptimization>(*m_vm, m_jit, resultReg);
    }

    void tryReadNonBMPUnicodeChar(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID resultReg, MacroAssembler::RegisterID indexReg)
    {
        ASSERT(m_charSize == CharSize::Char16);

        MacroAssembler::BaseIndex address = negativeOffsetIndexedAddress(negativeCharacterOffset, resultReg, indexReg);

        m_jit.getEffectiveAddress(address, m_regs.regUnicodeInputAndTrail);
        tryReadUnicodeCharImpl<TryReadUnicodeCharGenFirstNonBMPOptimization::DontUseOptimization>(*m_vm, m_jit, resultReg);
    }

    // Backward unicode read: decode the code point whose LAST code unit lives at
    // `address` (the mirror of tryReadUnicodeChar, which decodes from a first
    // unit). Matches the bytecode interpreter's tryReadBackward exactly:
    //   - load the unit at `address`;
    //   - if it is a trail surrogate and the unit before it (within the string)
    //     is a lead surrogate, the result is the supplementary code point;
    //   - otherwise the result is the unit itself. A lone lead or lone trail is
    //     returned as-is, never as an error (deliberately unlike the forward
    //     reader), because a backward match position can legitimately sit inside
    //     a surrogate pair the pattern splits.
    // Callers recover the consumed width from the result: >= 0x10000 means a
    // decoded pair (2 units), anything lower is a single unit -- exactly as the
    // forward paths branch on `character < 0x10000`. `temp2` is a scratch.
    void readUnicodeCharBackward(MacroAssembler::BaseIndex address, MacroAssembler::RegisterID resultReg, MacroAssembler::RegisterID temp2)
    {
        ASSERT(m_charSize == CharSize::Char16);
        ASSERT(resultReg != temp2);
        const MacroAssembler::RegisterID unitAddress = m_regs.regUnicodeInputAndTrail;
        const MacroAssembler::RegisterID temp = m_regs.unicodeAndSubpatternIdTemp;

        m_jit.getEffectiveAddress(address, unitAddress);
        m_jit.load16(MacroAssembler::Address(unitAddress), resultReg);

        MacroAssembler::JumpList notAPair;
        // resultReg is a trail surrogate iff (resultReg & 0xfc00) == 0xdc00.
        m_jit.move(resultReg, temp);
        m_jit.and32(MacroAssembler::TrustedImm32(0xfc00), temp);
        notAPair.append(m_jit.branch32(MacroAssembler::NotEqual, temp, MacroAssembler::TrustedImm32(0xdc00)));
        // The unit before must exist (unitAddress > input) ...
        notAPair.append(m_jit.branchPtr(MacroAssembler::BelowOrEqual, unitAddress, m_regs.input));
        // ... and be a lead surrogate: (unit & 0xfc00) == 0xd800.
        m_jit.load16(MacroAssembler::Address(unitAddress, -static_cast<int32_t>(sizeof(char16_t))), temp2);
        m_jit.move(temp2, temp);
        m_jit.and32(MacroAssembler::TrustedImm32(0xfc00), temp);
        notAPair.append(m_jit.branch32(MacroAssembler::NotEqual, temp, MacroAssembler::TrustedImm32(0xd800)));

        // A proper pair: U16_GET_SUPPLEMENTARY(lead, trail)
        //   = (lead << 10) + trail - U16_SURROGATE_OFFSET
        m_jit.lshift32(MacroAssembler::TrustedImm32(10), temp2);
        m_jit.add32(temp2, resultReg);
        m_jit.sub32(MacroAssembler::TrustedImm32(U16_SURROGATE_OFFSET), resultReg);

        notAPair.link(&m_jit);
    }

    // Specialized read+match for character classes whose codepoints all share a single UTF-16
    // lead surrogate. Loads the (lead, trail) pair as a single 32-bit value, branches to
    // |failures| if the lead doesn't match |sharedLead|, otherwise leaves the trail surrogate
    // in |character| and matches it against |trailsOnlyClass| (a transient class containing
    // only the trail surrogate values from the source class). Skips the full surrogate-pair
    // decode that tryReadUnicodeCharImpl would emit (~16 instructions).
    void readSurrogatePairAndMatchTrailForSharedLead(Checked<unsigned> negativeCharacterOffset,
        MacroAssembler::RegisterID character,
        MacroAssembler::RegisterID scratch,
        MacroAssembler::RegisterID indexReg,
        char16_t sharedLead,
        const CharacterClass* trailsOnlyClass,
        MacroAssembler::JumpList& failures)
    {
        ASSERT(m_charSize == CharSize::Char16);
        ASSERT(trailsOnlyClass->m_matches8.isEmpty());
        ASSERT(trailsOnlyClass->m_ranges8.isEmpty());

        MacroAssembler::BaseIndex address = negativeOffsetIndexedAddress(negativeCharacterOffset, character, indexReg);
        m_jit.getEffectiveAddress(address, m_regs.regUnicodeInputAndTrail);
        m_jit.load32(MacroAssembler::Address(m_regs.regUnicodeInputAndTrail), character);

        // Fast path for a small matches-only trail set (e.g., OfAKindRegExp's
        // {0xa1,0xb1,0xc1,0xd1}): compare the loaded 32-bit pair directly against
        // ((trail << 16) | sharedLead) for each match. This folds the lead check
        // into each cmp — no separate and+cmp+lsr needed. ARM64's cached-temp
        // tracker emits movk-only updates between consecutive compares since only
        // the upper 16 bits change.
        if (trailsOnlyClass->m_ranges32.isEmpty() && !trailsOnlyClass->m_matches32.isEmpty()) {
            const auto& matches = trailsOnlyClass->m_matches32;
#if CPU(ARM64)
            // For 2+ matches, fold the chain into cmp + ccmp(NE, Z=1) ... + b.ne.
            // ccmp keeps the EQ flag sticky once any compare matches, so the
            // entire alternation collapses to a single conditional branch. Saves
            // (matches.size() - 1) branches and avoids creating a matchDest label
            // (which would invalidate the cached temp register holding sharedLead).
            if (matches.size() >= 2) {
                constexpr int32_t nzcvEqual = 0x4; // Z=1 → flag is Equal.
                unsigned expected0 = (static_cast<unsigned>(matches[0]) << 16) | static_cast<unsigned>(sharedLead);
                m_jit.compareOnFlags32(character, MacroAssembler::TrustedImm32(expected0));
                for (size_t i = 1; i < matches.size(); ++i) {
                    unsigned expected = (static_cast<unsigned>(matches[i]) << 16) | static_cast<unsigned>(sharedLead);
                    m_jit.compareConditionallyOnFlags32(character, MacroAssembler::TrustedImm32(expected), MacroAssembler::TrustedImm32(nzcvEqual), MacroAssembler::NotEqual);
                }
                failures.append(m_jit.branchOnFlags(MacroAssembler::NotEqual));
                return;
            }
#endif
            MacroAssembler::JumpList matchDest;
            for (size_t i = 0; i + 1 < matches.size(); ++i) {
                unsigned expected = (static_cast<unsigned>(matches[i]) << 16) | static_cast<unsigned>(sharedLead);
                matchDest.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(expected)));
            }
            unsigned expected = (static_cast<unsigned>(matches[matches.size() - 1]) << 16) | static_cast<unsigned>(sharedLead);
            failures.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(expected)));
            matchDest.link(&m_jit);
            return;
        }

        // Fast path for a single trail-only range (e.g., FlushRegExp's [0xa1-0xae]):
        // emit a single sub+cmp+b.hi straight to outer failures with no trampolines.
        if (trailsOnlyClass->m_matches32.isEmpty() && trailsOnlyClass->m_ranges32.size() == 1) {
            const auto& range = trailsOnlyClass->m_ranges32[0];
#if CPU(ARM64)
            // Fold the lead-check and range-check into a single conditional branch
            // via ccmp. Saves one instruction and one branch per inner iteration.
            //   uxth   scratch, character           ; scratch = lead (low 16, zero-extended)
            //   cmp    scratch, sharedLead          ; Z=1 iff lead matches
            //   lsr    character, character, 16     ; trail (no flags)
            //   sub    scratch, character, begin    ; biased trail (no flags)
            //   ccmp   scratch, biasedEnd, #2, eq   ; if lead matched: real cmp; else NZCV→hi
            //   b.hi   failures
            unsigned biasedEnd = range.end - range.begin;
            constexpr int32_t nzcvHi = 0x2; // C=1, Z=0 → b.hi takes when lead mismatched
            m_jit.zeroExtend16To32(character, scratch);
            m_jit.compareOnFlags32(scratch, MacroAssembler::TrustedImm32(sharedLead));
            m_jit.urshift32(character, MacroAssembler::TrustedImm32(16), character);
            m_jit.sub32(character, MacroAssembler::Imm32(static_cast<unsigned>(range.begin)), scratch);
            m_jit.compareConditionallyOnFlags32(scratch, MacroAssembler::TrustedImm32(biasedEnd), MacroAssembler::TrustedImm32(nzcvHi), MacroAssembler::Equal);
            failures.append(m_jit.branchOnFlags(MacroAssembler::Above));
            return;
#else
            m_jit.zeroExtend16To32(character, scratch);
            failures.append(m_jit.branch32(MacroAssembler::NotEqual, scratch, MacroAssembler::TrustedImm32(sharedLead)));
            m_jit.urshift32(character, MacroAssembler::TrustedImm32(16), character);
            matchCharacterClassOnlyOneRange(character, scratch, failures, range);
            return;
#endif
        }

        m_jit.zeroExtend16To32(character, scratch);
        failures.append(m_jit.branch32(MacroAssembler::NotEqual, scratch, MacroAssembler::TrustedImm32(sharedLead)));

        m_jit.urshift32(character, MacroAssembler::TrustedImm32(16), character);

        MacroAssembler::JumpList matchDest;
        matchCharacterClass(character, scratch, MatchTargets(matchDest, failures, MatchTargets::PreferredTarget::MatchSuccessFallThrough), trailsOnlyClass);
        if (!matchDest.empty())
            failures.append(m_jit.jump());
        matchDest.link(&m_jit);
    }

    static void buildTrailsOnlyClass(const CharacterClass& source, CharacterClass& dest)
    {
        for (auto cp : source.m_matches32) {
            ASSERT(!U_IS_BMP(cp));
            dest.m_matches32.append(U16_TRAIL(cp));
        }

        for (auto& range : source.m_ranges32) {
            ASSERT(!U_IS_BMP(range.begin));
            ASSERT(!U_IS_BMP(range.end));
            char32_t trailBegin = U16_TRAIL(range.begin);
            char32_t trailEnd = U16_TRAIL(range.end);
            dest.m_ranges32.append(CharacterRange(trailBegin, trailEnd));
        }
        std::ranges::sort(dest.m_matches32);
        std::ranges::sort(dest.m_ranges32, [](auto& a, auto& b) { return a.begin < b.begin; });
    }
#endif

    void readCharacter(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID resultReg)
    {
        readCharacter(negativeCharacterOffset, resultReg, m_regs.index);
    }

    void readCharacter(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID resultReg, MacroAssembler::RegisterID indexReg)
    {
        MacroAssembler::BaseIndex address = negativeOffsetIndexedAddress(negativeCharacterOffset, resultReg, indexReg);

        if (m_charSize == CharSize::Char8)
            m_jit.load8(address, resultReg);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        else if (m_decodeSurrogatePairs)
            tryReadUnicodeChar(address, resultReg);
#endif
        else
            m_jit.load16Unaligned(address, resultReg);
    }

    // Read a raw code unit without surrogate pair decoding. This is used for
    // Boyer-Moore lookahead which is a hash-based prefilter where false positives
    // are safe, so exact codepoint decoding is unnecessary.
    void readCharacterRaw(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID resultReg)
    {
        MacroAssembler::BaseIndex address = negativeOffsetIndexedAddress(negativeCharacterOffset, resultReg, m_regs.index);

        if (m_charSize == CharSize::Char8)
            m_jit.load8(address, resultReg);
        else
            m_jit.load16Unaligned(address, resultReg);
    }

    // Read the code point relative to the term's direction. A forward term is
    // addressed by its first unit (decode rightward, tryReadUnicodeChar); a
    // backward (lookbehind) term is addressed by its last unit (decode leftward,
    // the interpreter's tryReadBackward). Reads that are inherently forward -- a
    // capture's own units, the Boyer-Moore scans -- keep using readCharacter().
    void readCharacterForDirection(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID resultReg)
    {
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_direction == Backward && m_decodeSurrogatePairs) {
            readUnicodeCharBackward(negativeOffsetIndexedAddress(negativeCharacterOffset, resultReg), resultReg, m_regs.regT2);
            m_usesT2 = true;
            return;
        }
#endif
        readCharacter(negativeCharacterOffset, resultReg, m_regs.index);
    }

    // Load the single code unit at the offset, with no surrogate-pair decoding.
    // Used where a term compares exact UTF-16 units (a fixed astral literal is
    // matched as its two known units), so pair decoding would be wrong.
    void readCodeUnit(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID resultReg)
    {
        readCodeUnit(negativeCharacterOffset, resultReg, m_regs.index);
    }

    void readCodeUnit(Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID resultReg, MacroAssembler::RegisterID indexReg)
    {
        MacroAssembler::BaseIndex address = negativeOffsetIndexedAddress(negativeCharacterOffset, resultReg, indexReg);
        if (m_charSize == CharSize::Char8)
            m_jit.load8(address, resultReg);
        else
            m_jit.load16Unaligned(address, resultReg);
    }

    template<MacroAssembler::RelationalCondition cond>
    MacroAssembler::Jump jumpIfCharCond(char32_t ch, Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID character, bool ignoreCase)
    {
        // A term compares the code point that begins (forward) or ends (backward)
        // at its position; the direction picks the decode. Astral literals never
        // reach here in a backward frame (they compare units directly).
        readCharacterForDirection(negativeCharacterOffset, character);

        // For case-insesitive compares, non-ascii characters that have different
        // upper & lower case representations are converted to a character class.
        ASSERT(!ignoreCase || isASCIIAlpha(ch) || isCanonicallyUnique(ch, m_canonicalMode));
        if (ignoreCase && isASCIIAlpha(ch)) {
            m_jit.or32(MacroAssembler::TrustedImm32(0x20), character);
            ch |= 0x20;
        }

        return m_jit.branch32(cond, character, MacroAssembler::Imm32(ch));
    }

    MacroAssembler::Jump jumpIfCharNotEquals(char32_t ch, Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID character, bool ignoreCase)
    {
        return jumpIfCharCond<MacroAssembler::NotEqual>(ch, negativeCharacterOffset, character, ignoreCase);
    }

    MacroAssembler::Jump jumpIfCharEquals(char32_t ch, Checked<unsigned> negativeCharacterOffset, MacroAssembler::RegisterID character, bool ignoreCase)
    {
        return jumpIfCharCond<MacroAssembler::Equal>(ch, negativeCharacterOffset, character, ignoreCase);
    }

    void storeToFrame(MacroAssembler::RegisterID reg, unsigned frameLocation)
    {
        m_jit.storePtr(reg, frameAddress().withOffset(frameLocation * sizeof(void*)));
    }

    void storeToFrame(MacroAssembler::TrustedImm32 imm, unsigned frameLocation)
    {
        m_jit.storePtr(imm, frameAddress().withOffset(frameLocation * sizeof(void*)));
    }

#if CPU(ARM64) || CPU(X86_64) || CPU(RISCV64)
    void storeToFrame(MacroAssembler::TrustedImmPtr imm, unsigned frameLocation)
    {
        m_jit.storePtr(imm, frameAddress().withOffset(frameLocation * sizeof(void*)));
    }
#endif

    MacroAssembler::DataLabelPtr storeToFrameWithPatch(unsigned frameLocation)
    {
        return m_jit.storePtrWithPatch(MacroAssembler::TrustedImmPtr(nullptr), frameAddress().withOffset(frameLocation * sizeof(void*)));
    }

    void loadFromFrame(unsigned frameLocation, MacroAssembler::RegisterID reg)
    {
        m_jit.loadPtr(frameAddress().withOffset(frameLocation * sizeof(void*)), reg);
    }

    void loadFromFrameAndJump(unsigned frameLocation)
    {
        m_jit.farJump(frameAddress().withOffset(frameLocation * sizeof(void*)), YarrBacktrackPtrTag);
    }

    CCallHelpers::Address NODELETE frameAddress()
    {
        size_t stackSizeForCalleeSaves = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(m_calleeSaves.registerCount() * sizeof(UCPURegister));
        return CCallHelpers::Address(GPRInfo::callFrameRegister, -(stackSizeForCalleeSaves + m_callFrameSizeInBytes));
    }

    CCallHelpers::Address NODELETE internalSubpatternOutputAddress(unsigned byteOffset)
    {
        ASSERT(m_needsInternalSubpatternOutput);
        return frameAddress().withOffset(m_internalSubpatternOutputOffsetInFrame * sizeof(void*) + byteOffset);
    }

    MacroAssembler::Address NODELETE subpatternStartAddress(unsigned subpatternId)
    {
        if (m_needsInternalSubpatternOutput)
            return internalSubpatternOutputAddress((subpatternId << 1) * sizeof(int));
        return MacroAssembler::Address(m_regs.output, (subpatternId << 1) * sizeof(int));
    }

    MacroAssembler::Address NODELETE subpatternEndAddress(unsigned subpatternId)
    {
        return subpatternStartAddress(subpatternId).withOffset(sizeof(int));
    }

    MacroAssembler::Address NODELETE duplicateNamedGroupAddress(unsigned duplicateNamedGroupId)
    {
        if (m_needsInternalSubpatternOutput)
            return internalSubpatternOutputAddress(offsetForDuplicateNamedGroupId(duplicateNamedGroupId) * sizeof(int));
        return MacroAssembler::Address(m_regs.output, offsetForDuplicateNamedGroupId(duplicateNamedGroupId) * sizeof(int));
    }

    static bool NODELETE needsSubpatternRecording(ExecutionMode executionMode, const YarrPattern& pattern)
    {
        return executionMode == ExecutionMode::IncludeSubpatterns || (executionMode == ExecutionMode::MatchOnly && pattern.m_containsBackreferences);
    }

    static unsigned NODELETE alignCallFrameSizeInBytes(unsigned originalCallFrameSize)
    {
        unsigned callFrameSize = originalCallFrameSize;
        if (!callFrameSize)
            return 0;

        callFrameSize *= sizeof(void*);
        RELEASE_ASSERT(callFrameSize / sizeof(void*) == originalCallFrameSize);
        return WTF::roundUpToMultipleOf<stackAlignmentBytes()>(callFrameSize);
    }

    void generateFailReturn()
    {
        m_jit.move(MacroAssembler::TrustedImmPtr((void*)WTF::notFound), m_regs.returnRegister);
        m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.returnRegister2);

#if ENABLE(YARR_JIT_REGEXP_TEST_INLINE)
        if (m_executionMode == ExecutionMode::InlineTest) {
            m_inlinedFailedMatch.append(m_jit.jump());
            return;
        }
#endif

        generateReturn();
    }

    void generateJITFailReturn()
    {
        if (m_abortExecution.empty() && m_hitMatchLimit.empty())
            return;

        MacroAssembler::JumpList finishExiting;
        if (!m_abortExecution.empty()) {
            m_abortExecution.link(&m_jit);
            m_jit.move(MacroAssembler::TrustedImmPtr((void*)static_cast<size_t>(JSRegExpResult::JITCodeFailure)), m_regs.returnRegister);
            finishExiting.append(m_jit.jump());
        }

        if (!m_hitMatchLimit.empty()) {
            m_hitMatchLimit.link(&m_jit);
            m_jit.move(MacroAssembler::TrustedImmPtr((void*)static_cast<size_t>(JSRegExpResult::ErrorNoMatch)), m_regs.returnRegister);
        }

        finishExiting.link(&m_jit);
        m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.returnRegister2);
        generateReturn();
    }

    // Used to record subpatterns, should be called in ExecutionMode::IncludeSubpatterns　or when m_needsInternalSubpatternOutput is true (MatchOnly with backreferences).
    void setSubpatternStart(MacroAssembler::RegisterID reg, unsigned subpattern)
    {
        ASSERT(subpattern);
        m_jit.store32(reg, subpatternStartAddress(subpattern));
    }
    void setSubpatternEnd(MacroAssembler::RegisterID reg, unsigned subpattern)
    {
        ASSERT(subpattern);
        m_jit.store32(reg, subpatternEndAddress(subpattern));
    }
    void clearSubpattern(unsigned subpattern)
    {
#if USE(JSVALUE64)
        m_jit.store64(MacroAssembler::TrustedImm64(static_cast<uint64_t>(-1)), subpatternStartAddress(subpattern));
#else
        m_jit.store32(MacroAssembler::TrustedImm32(static_cast<uint32_t>(-1)), subpatternStartAddress(subpattern));
        m_jit.store32(MacroAssembler::TrustedImm32(static_cast<uint32_t>(-1)), subpatternEndAddress(subpattern));
#endif
    }

    void emitClearCapturesForTerm(PatternTerm* term)
    {
        if (!shouldRecordSubpatterns() || !term->containsAnyCaptures())
            return;
        for (unsigned subpattern = term->parentheses.subpatternId; subpattern <= term->parentheses.lastSubpatternId; ++subpattern)
            clearSubpattern(subpattern);
    }

    void emitClearAllCaptures()
    {
        if (!shouldRecordSubpatterns())
            return;
        for (unsigned subpatternId = 0; subpatternId < m_pattern.m_numSubpatterns + 1; ++subpatternId)
            clearSubpattern(subpatternId);
        for (unsigned i = 1; i <= m_pattern.m_numDuplicateNamedCaptureGroups; ++i)
            m_jit.store32(MacroAssembler::TrustedImm32(0), duplicateNamedGroupAddress(i));
    }

    // Compute the real input position of a point that is `inputOffset` frame
    // slots behind the current frame position, into `reg`. Forward: behind the
    // frontier means a lower address (subtract); in a backward (mirrored) frame
    // it is ahead of the leftward cursor (add).
    void emitFramePosition(unsigned inputOffset, MacroAssembler::RegisterID reg)
    {
        if (m_direction == Backward)
            m_jit.add32(MacroAssembler::Imm32(inputOffset), m_regs.index, reg);
        else
            m_jit.sub32(m_regs.index, MacroAssembler::Imm32(inputOffset), reg);
    }

    // Consume `count` characters at the frame position: advance the forward
    // frontier, or move the backward (mirrored) leftward cursor down.
    void consumeIndex(unsigned count)
    {
        if (m_direction == Backward)
            m_jit.sub32(MacroAssembler::Imm32(count), m_regs.index);
        else
            m_jit.add32(MacroAssembler::Imm32(count), m_regs.index);
    }

    // Give `count` characters back (undo a consume).
    void rewindIndex(unsigned count)
    {
        if (m_direction == Backward)
            m_jit.add32(MacroAssembler::Imm32(count), m_regs.index);
        else
            m_jit.sub32(MacroAssembler::Imm32(count), m_regs.index);
    }

    // Give back the number of characters held in `countRegister`.
    void rewindIndex(MacroAssembler::RegisterID countRegister)
    {
        if (m_direction == Backward)
            m_jit.add32(countRegister, m_regs.index);
        else
            m_jit.sub32(countRegister, m_regs.index);
    }

    // In a backward (mirrored) frame a capture group's virtual start is its
    // real end and vice versa, so the start/end slots are written swapped -
    // matching the interpreter, which records backward captures end-first.

    void emitCaptureStart(PatternTerm* term, Checked<unsigned> checkedOffset, MacroAssembler::RegisterID indexTemporary, unsigned extraOffset = 0)
    {
        // FIXME: could avoid offsetting this value in JIT code, apply offsets only afterwards, at the point the results array is being accessed.
        ASSERT(term->capture() && shouldRecordSubpatterns());
        unsigned inputOffset = checkedOffset - term->inputPosition + extraOffset;
        MacroAssembler::RegisterID positionReg = m_regs.index;
        if (inputOffset) {
            emitFramePosition(inputOffset, indexTemporary);
            positionReg = indexTemporary;
        }
        if (m_direction == Backward)
            setSubpatternEnd(positionReg, term->parentheses.subpatternId);
        else
            setSubpatternStart(positionReg, term->parentheses.subpatternId);
    }

    void emitCaptureEnd(PatternTerm* term, Checked<unsigned> checkedOffset, MacroAssembler::RegisterID indexTemporary)
    {
        // FIXME: could avoid offsetting this value in JIT code, apply offsets only afterwards, at the point the results array is being accessed.
        ASSERT(term->capture() && shouldRecordSubpatterns());
        auto subpatternId = term->parentheses.subpatternId;
        unsigned inputOffset = checkedOffset - term->inputPosition;
        MacroAssembler::RegisterID positionReg = m_regs.index;
        if (inputOffset) {
            emitFramePosition(inputOffset, indexTemporary);
            positionReg = indexTemporary;
        }
        if (m_direction == Backward)
            setSubpatternStart(positionReg, subpatternId);
        else
            setSubpatternEnd(positionReg, subpatternId);
        if (m_pattern.m_numDuplicateNamedCaptureGroups) {
            if (auto duplicateNamedGroupId = m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId])
                storeDuplicateNamedGroupSubpatternId(duplicateNamedGroupId, subpatternId);
        }
    }

    void storeDuplicateNamedGroupSubpatternId(unsigned duplicateNamedGroupId, unsigned subpatternId)
    {
        m_jit.store32(MacroAssembler::TrustedImm32(subpatternId), duplicateNamedGroupAddress(duplicateNamedGroupId));
    }

    void storeDuplicateNamedGroupSubpatternIdFromReg(unsigned duplicateNamedGroupId, MacroAssembler::RegisterID reg)
    {
        m_jit.store32(reg, duplicateNamedGroupAddress(duplicateNamedGroupId));
    }

    void loadDuplicateNamedGroupSubpatternId(unsigned duplicateNamedGroupId, MacroAssembler::RegisterID reg)
    {
        m_jit.load32(duplicateNamedGroupAddress(duplicateNamedGroupId), reg);
    }

    bool NODELETE shouldRecordSubpatterns() const
    {
        return m_executionMode == ExecutionMode::IncludeSubpatterns || m_needsInternalSubpatternOutput;
    }

    // We use one of three different strategies to track the start of the current match,
    // while matching.
    // 1) If the pattern has a fixed size, do nothing! - we calculate the value lazily
    //    at the end of matching. This is irrespective of m_executionMode, and in this case
    //    these methods should never be called.
    // 2) If we're compiling ExecutionMode::IncludeSubpatterns, 'm_regs.output' contains a pointer to an output
    //    vector, store the match start in the output vector.
    // 3) If we're compiling MatchOnly or InlinedTest, 'm_regs.output' is unused, store the match start directly
    //    in this register.
    void setMatchStart(MacroAssembler::RegisterID reg)
    {
        ASSERT(!m_pattern.m_body->m_hasFixedSize);
        if (shouldRecordSubpatterns())
            m_jit.store32(reg, subpatternStartAddress(0));
        else
            m_jit.move(reg, m_regs.output);
    }
    void getMatchStart(MacroAssembler::RegisterID reg)
    {
        ASSERT(!m_pattern.m_body->m_hasFixedSize);
        if (shouldRecordSubpatterns())
            m_jit.load32(subpatternStartAddress(0), reg);
        else
            m_jit.move(m_regs.output, reg);
    }

    enum class YarrOpCode : uint8_t {
        // These nodes wrap body alternatives - those in the main disjunction,
        // rather than subpatterns or assertions. These are chained together in
        // a doubly linked list, with a 'begin' node for the first alternative,
        // a 'next' node for each subsequent alternative, and an 'end' node at
        // the end. In the case of repeating alternatives, the 'end' node also
        // has a reference back to 'begin'.
        BodyAlternativeBegin,
        BodyAlternativeNext,
        BodyAlternativeEnd,
        // Similar to the body alternatives, but used for subpatterns with two
        // or more alternatives.
        NestedAlternativeBegin,
        NestedAlternativeNext,
        NestedAlternativeEnd,
        // Used for alternatives in subpatterns where there is only a single
        // alternative (backtracking is easier in these cases), or for alternatives
        // which never need to be backtracked (those in parenthetical assertions,
        // terminal subpatterns).
        SimpleNestedAlternativeBegin,
        SimpleNestedAlternativeNext,
        SimpleNestedAlternativeEnd,
        // Used for alternatives in subpatterns where there is a list of BOL anchored
        // string alternatives. Such a string list doesn't need backtracking. If the
        // pattern is also EOL anchored, e.g. /^(?:cat|dog|doggy)$/, the list of strings
        // needs to be sorted such that all longer strings with a prefix prior in the
        // list appear first. In the example, we'd sort the alternatives to something
        // like: /^(?:cat|doggy|dog)$/. This elimnates the need to backktrack.
        StringListAlternativeBegin,
        StringListAlternativeNext,
        StringListAlternativeEnd,
        // Used to wrap 'Once' subpattern matches (quantityMaxCount == 1).
        ParenthesesSubpatternOnceBegin,
        ParenthesesSubpatternOnceEnd,
        // Used to wrap 'Terminal' subpattern matches (at the end of the regexp).
        ParenthesesSubpatternTerminalBegin,
        ParenthesesSubpatternTerminalEnd,
        // Used to wrap non-capturing FixedCount parentheses (e.g., (?:x){3,3}).
        // These are only used for non-capturing groups; capturing FixedCount uses ParenthesesSubpatternBegin/End.
        ParenthesesSubpatternFixedCountBegin,
        ParenthesesSubpatternFixedCountEnd,
        // Used to wrap generic captured matches
        ParenthesesSubpatternBegin,
        ParenthesesSubpatternEnd,
        // Used to wrap parenthetical assertions.
        ParentheticalAssertionBegin,
        ParentheticalAssertionEnd,
        // Wraps all simple terms (pattern characters, character classes).
        Term,
        // Where an expression contains only 'once through' body alternatives
        // and no repeating ones, this op is used to return match failure.
        MatchFailed
    };

    // This structure is used to hold the compiled opcode information,
    // including reference back to the original PatternTerm/PatternAlternatives,
    // and JIT compilation data structures.
    struct YarrOp {
        explicit YarrOp(PatternTerm* term)
            : m_term(term)
            , m_op(YarrOpCode::Term)
        {
        }

        explicit YarrOp(YarrOpCode op)
            : m_op(op)
        {
        }

        // For alternatives, this holds the PatternAlternative and doubly linked
        // references to this alternative's siblings. In the case of the
        // YarrOpCode::BodyAlternativeEnd node at the end of a section of repeating nodes,
        // m_nextOp will reference the YarrOpCode::BodyAlternativeBegin node of the first
        // repeating alternative.
        PatternAlternative* m_alternative;
        size_t m_index { 0 };
        size_t m_previousOp;
        size_t m_nextOp;
        size_t m_endOp { notFound };

        // The operation, as a YarrOpCode, and also a reference to the PatternTerm.
        PatternTerm* m_term;
        YarrOpCode m_op;

        // The matching direction in effect for the code this op generates. Ops
        // belonging to a lookbehind body (compiled from a mirrored copy of the
        // body, see mirrorDisjunctionForLookbehind) are Backward: the index register
        // is a leftward-moving cursor rather than the usual rightward frontier.
        MatchDirection m_direction { Forward };

        // Used to record a set of Jumps out of the generated code, typically
        // used for jumps out to backtracking code, and a single reentry back
        // into the code for a node (likely where a backtrack will trigger
        // rematching).
        MacroAssembler::Label m_reentry;
        MacroAssembler::JumpList m_jumps;

        // Used for backtracking when the prior alternative did not consume any
        // characters but matched.
        MacroAssembler::Jump m_zeroLengthMatch;

        // This flag is used to null out the subsequent pattern characters, when
        // multiple are fused to match as a group.
        bool m_isDeadCode { false };

        // True for the one term whose first read is guaranteed to be at the match
        // start: the alternative's first term, provided no earlier term can move
        // the frontier at runtime. Only such a read may establish
        // firstCharacterAdditionalReadSize ("the start character is a non-BMP
        // pair"); a term after a zero-or-more (min-0) sibling reads past the
        // start whenever that sibling consumed input.
        bool m_readsStartCharacter { false };

        // Currently used in the case of some of the more complex management of
        // 'm_checkedOffset', to cache the offset used in this alternative, to avoid
        // recalculating it.
        Checked<unsigned> m_checkAdjust;

        // This records the current input offset being applied due to the current
        // set of alternatives we are nested within. E.g. when matching the
        // character 'b' within the regular expression /abc/, we will know that
        // the minimum size for the alternative is 3, checked upon entry to the
        // alternative, and that 'b' is at offset 1 from the start, and as such
        // when matching 'b' we need to apply an offset of -2 to the load.
        Checked<unsigned> m_checkedOffset { };

        // Used by YarrOpCode::NestedAlternativeNext/End to hold the pointer to the
        // value that will be pushed into the pattern's frame to return to,
        // upon backtracking back into the disjunction.
        MacroAssembler::DataLabelPtr m_returnAddress;

        BoyerMooreInfo* m_bmInfo { nullptr };
        MaskedAlternativeInfo* m_maskedAltInfo { nullptr };

        // Set on the alternative Begin/Next/End ops of a nested disjunction whose
        // alternatives are dispatched on their first character (see DispatchInfo).
        struct DispatchInfo* m_dispatch { nullptr };
        // Position of this Begin/Next op's alternative within the dispatched
        // disjunction.
        unsigned m_dispatchAlternativeIndex { 0 };

        // Set on the repeated body's Begin/Next/End ops when the body alternation
        // is dispatched on its first character (see BodyDispatchInfo).
        struct BodyDispatchInfo* m_bodyDispatch { nullptr };
        // Position of this Begin/Next op's alternative among the repeated body
        // alternatives.
        unsigned m_bodyDispatchAlternativeIndex { 0 };

        // Shared UTF-16 lead surrogate across all repeated alternatives' first character.
        std::optional<char16_t> m_bodyAltSharedLead;

        // For quantified parentheses (FixedCount/Greedy/NonGreedy): label for the
        // content's backtrack entry. BEGIN.bt jumps here to re-drive an iteration's
        // content (within-iteration / inter-iteration backtracking).
        MacroAssembler::Label m_contentBacktrackEntryLabel;
    };

    // BacktrackingState
    // This class encapsulates information about the state of code generation
    // whilst generating the code for backtracking, when a term fails to match.
    // Upon entry to code generation of the backtracking code for a given node,
    // the Backtracking state will hold references to all control flow sources
    // that are outputs in need of further backtracking from the prior node
    // generated (which is the subsequent operation in the regular expression,
    // and in the m_ops Vector, since we generated backtracking backwards).
    // These references to control flow take the form of:
    //  - A jump list of jumps, to be linked to code that will backtrack them
    //    further.
    //  - A set of DataLabelPtr values, to be populated with values to be
    //    treated effectively as return addresses backtracking into complex
    //    subpatterns.
    //  - A flag indicating that the current sequence of generated code up to
    //    this point requires backtracking.
    class BacktrackingState {
    private:
        struct ReturnAddressRecord {
            ReturnAddressRecord(MacroAssembler::DataLabelPtr dataLabel, MacroAssembler::Label backtrackLocation)
                : m_dataLabel(dataLabel)
                , m_backtrackLocation(backtrackLocation)
            {
            }

            MacroAssembler::DataLabelPtr m_dataLabel;
            MacroAssembler::Label m_backtrackLocation;
        };

    public:
        typedef Vector<ReturnAddressRecord, 4> BacktrackRecords;

        BacktrackingState()
            : m_pendingFallthrough(false)
        {
        }

        // Add a jump or jumps, a return address, or set the flag indicating
        // that the current 'fallthrough' control flow requires backtracking.
        void append(const MacroAssembler::Jump& jump)
        {
            m_laterFailures.append(jump);
        }
        void append(MacroAssembler::JumpList& jumpList)
        {
            m_laterFailures.append(jumpList);
        }
        void append(const MacroAssembler::DataLabelPtr& returnAddress)
        {
            m_pendingReturns.append(returnAddress);
        }
        // Bind a patchable frame store directly to a known code location (used by
        // first-character dispatch chains, whose targets are forward-generated
        // stubs rather than backtracking sites).
        void addReturnAddressRecord(const MacroAssembler::DataLabelPtr& dataLabel, MacroAssembler::Label target)
        {
            m_backtrackRecords.append(ReturnAddressRecord(dataLabel, target));
        }
        void NODELETE fallthrough()
        {
            ASSERT(!m_pendingFallthrough);
            m_pendingFallthrough = true;
        }

        // These methods clear the backtracking state, either linking to the
        // current location, a provided label, or copying the backtracking out
        // to a JumpList. All actions may require code generation to take place,
        // and as such are passed a pointer to the assembler.
        void link(YarrGenerator& generator, const YarrOp& op)
        {
            if (m_pendingReturns.size()) {
                MacroAssembler::Label here(generator.m_jit);
                for (unsigned i = 0; i < m_pendingReturns.size(); ++i)
                    m_backtrackRecords.append(ReturnAddressRecord(m_pendingReturns[i], here));
                m_pendingReturns.clear();
            }
            m_laterFailures.link(generator.m_jit);
            m_laterFailures.clear();
            m_pendingFallthrough = false;

            if (Options::traceRegExpJITExecution()) [[unlikely]] {
                GPRReg indexReg = generator.m_regs.index;
                auto index = op.m_index;
                auto opcode = op.m_op;
                generator.m_jit.probeDebug([=](Probe::Context& ctx) {
                    int32_t indexValue = static_cast<int32_t>(ctx.gpr(indexReg));
                    dataLogLn("RegExpJIT [", index, "] ", opcode, ".bt index=", indexValue);
                });
            }
        }

        void linkTo(MacroAssembler::Label label, MacroAssembler* assembler)
        {
            if (m_pendingReturns.size()) {
                for (unsigned i = 0; i < m_pendingReturns.size(); ++i)
                    m_backtrackRecords.append(ReturnAddressRecord(m_pendingReturns[i], label));
                m_pendingReturns.clear();
            }
            if (m_pendingFallthrough)
                assembler->jump(label);
            m_laterFailures.linkTo(label, assembler);
            m_laterFailures.clear();
            m_pendingFallthrough = false;
        }
        void takeBacktracksToJumpList(MacroAssembler::JumpList& jumpList, MacroAssembler* assembler)
        {
            if (m_pendingReturns.size()) {
                MacroAssembler::Label here(assembler);
                for (unsigned i = 0; i < m_pendingReturns.size(); ++i)
                    m_backtrackRecords.append(ReturnAddressRecord(m_pendingReturns[i], here));
                m_pendingReturns.clear();
                m_pendingFallthrough = true;
            }
            if (m_pendingFallthrough)
                jumpList.append(assembler->jump());
            jumpList.append(m_laterFailures);
            m_laterFailures.clear();
            m_pendingFallthrough = false;
        }

        bool isEmpty()
        {
            return m_laterFailures.empty() && m_pendingReturns.isEmpty() && !m_pendingFallthrough;
        }

        BacktrackRecords& NODELETE backtrackRecords()
        {
            return m_backtrackRecords;
        }

        static void linkBacktrackRecords(LinkBuffer& linkBuffer, const BacktrackRecords& backtrackRecords)
        {
            for (unsigned i = 0; i < backtrackRecords.size(); ++i)
                linkBuffer.patch(backtrackRecords[i].m_dataLabel, linkBuffer.locationOf<YarrBacktrackPtrTag>(backtrackRecords[i].m_backtrackLocation));
        }

        // Called at the end of code generation to link all return addresses.
        void linkDataLabels(LinkBuffer& linkBuffer)
        {
            ASSERT(isEmpty());
            for (unsigned i = 0; i < m_backtrackRecords.size(); ++i)
                linkBuffer.patch(m_backtrackRecords[i].m_dataLabel, linkBuffer.locationOf<YarrBacktrackPtrTag>(m_backtrackRecords[i].m_backtrackLocation));
        }

    private:
        MacroAssembler::JumpList m_laterFailures;
        bool m_pendingFallthrough;
        Vector<MacroAssembler::DataLabelPtr, 4> m_pendingReturns;
        Vector<ReturnAddressRecord, 4> m_backtrackRecords;
    };

    unsigned NODELETE offsetForDuplicateNamedGroupId(unsigned duplicateNamedGroupId)
    {
        ASSERT(duplicateNamedGroupId);
        return ((m_pattern.m_numSubpatterns + 1) << 1) + duplicateNamedGroupId - 1;
    }

    // Generation methods:
    // ===================

    // This method provides a default implementation of backtracking common
    // to many terms; terms commonly jump out of the forwards  matching path
    // on any failed conditions, and add these jumps to the m_jumps list. If
    // no special handling is required we can often just backtrack to m_jumps.
    void backtrackTermDefault(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        m_backtrackingState.append(op.m_jumps);
    }

    void generateAssertionBOL(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        if (m_direction == Backward) {
            generateAssertionBOLBackward(opIndex);
            return;
        }

        // The anchor's real string position is index - (checkedOffset - position), so
        // it is the start of input iff index == checkedOffset - position. A top-level
        // ^ at position 0 gives the classic index == checkedOffset; the general form
        // also covers a ^ in the body of an assertion nested inside a lookbehind, whose
        // numbering continues from a nonzero origin (and makes a ^ that can never be
        // at position 0 -- e.g. /a^b/ -- simply never satisfied, as before).
        MacroAssembler::Imm32 startFrontier(static_cast<int32_t>((op.m_checkedOffset - term->inputPosition).value()));

        if (term->multiline()) {
            const MacroAssembler::RegisterID character = m_regs.regT0;
            const MacroAssembler::RegisterID scratch = m_regs.regT1;

            MacroAssembler::JumpList matchDest;
            matchDest.append(m_jit.branch32(MacroAssembler::Equal, m_regs.index, startFrontier));

            readCharacter(op.m_checkedOffset - term->inputPosition + 1, character);
            matchCharacterClass(character, scratch, matchDest, m_pattern.newlineCharacterClass());
            op.m_jumps.append(m_jit.jump());

            matchDest.link(&m_jit);
        } else
            op.m_jumps.append(m_jit.branch32(MacroAssembler::NotEqual, m_regs.index, startFrontier));
    }

    // BOL inside a lookbehind body (mirrored frame). The anchor's real position
    // is index + (checkedOffset - inputPosition). It can be the real start of
    // input only when it sits at the frame's right edge (inputPosition ==
    // checkedOffset) and the cursor has reached position 0; the real character
    // preceding the anchor is at frame offset (checkedOffset - inputPosition),
    // one less than in the forward case.
    void generateAssertionBOLBackward(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;
        bool atFrameEdge = term->inputPosition == op.m_checkedOffset;

        if (term->multiline()) {
            const MacroAssembler::RegisterID character = m_regs.regT0;
            const MacroAssembler::RegisterID scratch = m_regs.regT1;

            MacroAssembler::JumpList matchDest;
            if (atFrameEdge)
                matchDest.append(m_jit.branchTest32(MacroAssembler::Zero, m_regs.index));

            // The character just left of the anchor ends at the anchor: a leftward
            // decode in a unicode pattern.
            readCharacterForDirection(op.m_checkedOffset - term->inputPosition, character);
            matchCharacterClass(character, scratch, matchDest, m_pattern.newlineCharacterClass());
            op.m_jumps.append(m_jit.jump());

            matchDest.link(&m_jit);
        } else {
            if (atFrameEdge)
                op.m_jumps.append(m_jit.branchTest32(MacroAssembler::NonZero, m_regs.index));
            else
                op.m_jumps.append(m_jit.jump());
        }
    }
    void backtrackAssertionBOL(size_t opIndex)
    {
        backtrackTermDefault(opIndex);
    }

    void generateAssertionEOL(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        if (m_direction == Backward) {
            generateAssertionEOLBackward(opIndex);
            return;
        }

        if (term->multiline()) {
            const MacroAssembler::RegisterID character = m_regs.regT0;
            const MacroAssembler::RegisterID scratch = m_regs.regT1;

            MacroAssembler::JumpList matchDest;
            if (term->inputPosition == op.m_checkedOffset)
                matchDest.append(atEndOfInput());

            readCharacter(op.m_checkedOffset - term->inputPosition, character);
            matchCharacterClass(character, scratch, matchDest, m_pattern.newlineCharacterClass());
            op.m_jumps.append(m_jit.jump());

            matchDest.link(&m_jit);
        } else {
            if (term->inputPosition == op.m_checkedOffset)
                op.m_jumps.append(notAtEndOfInput());
            // Erk, really should poison out these alternatives early. :-/
            else
                op.m_jumps.append(m_jit.jump());
        }
    }

    // EOL inside a lookbehind body (mirrored frame). The anchor's real position
    // is index + (checkedOffset - inputPosition). It can be the real end of input
    // only when it sits at the frame's left edge (inputPosition == 0, i.e. the
    // assertion point itself) and index + checkedOffset == length; the real
    // character following the anchor is at frame offset (checkedOffset -
    // inputPosition + 1), one more than in the forward case.
    void generateAssertionEOLBackward(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;
        bool atFrameEdge = !term->inputPosition;
        const MacroAssembler::RegisterID scratch = m_regs.regT1;

        auto atRealEnd = [&](MacroAssembler::RelationalCondition condition) {
            m_jit.add32(MacroAssembler::Imm32(op.m_checkedOffset), m_regs.index, scratch);
            return m_jit.branch32(condition, scratch, m_regs.length);
        };

        if (term->multiline()) {
            const MacroAssembler::RegisterID character = m_regs.regT0;

            MacroAssembler::JumpList matchDest;
            if (atFrameEdge)
                matchDest.append(atRealEnd(MacroAssembler::Equal));

            readCharacter(op.m_checkedOffset - term->inputPosition + 1, character);
            matchCharacterClass(character, scratch, matchDest, m_pattern.newlineCharacterClass());
            op.m_jumps.append(m_jit.jump());

            matchDest.link(&m_jit);
        } else {
            if (atFrameEdge)
                op.m_jumps.append(atRealEnd(MacroAssembler::NotEqual));
            else
                op.m_jumps.append(m_jit.jump());
        }
    }
    void backtrackAssertionEOL(size_t opIndex)
    {
        backtrackTermDefault(opIndex);
    }

    // Also falls though on nextIsNotWordChar.
    void matchAssertionWordchar(size_t opIndex, MacroAssembler::JumpList& nextIsWordChar, MacroAssembler::JumpList& nextIsNotWordChar)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID scratch = m_regs.regT1;

        if (m_direction == Backward) {
            // Mirrored frame: the boundary's real position is index + (checked -
            // pos). It is the real end of input only at the frame's left edge
            // (pos == 0) with index + checked == length; the following real
            // character is at frame offset (checked - pos + 1). The character AFTER
            // a boundary starts at it, so it is a forward decode from that unit --
            // readCharacter, not the direction-relative reader.
            if (!term->inputPosition) {
                m_jit.add32(MacroAssembler::Imm32(op.m_checkedOffset), m_regs.index, scratch);
                nextIsNotWordChar.append(m_jit.branch32(MacroAssembler::Equal, scratch, m_regs.length));
            }
            readCharacter(op.m_checkedOffset - term->inputPosition + 1, character);
        } else {
            if (term->inputPosition == op.m_checkedOffset)
                nextIsNotWordChar.append(atEndOfInput());

            readCharacter(op.m_checkedOffset - term->inputPosition, character);
        }

        CharacterClass* wordcharCharacterClass;

        if (m_pattern.eitherUnicode() && term->ignoreCase())
            wordcharCharacterClass = m_pattern.wordUnicodeIgnoreCaseCharCharacterClass();
        else
            wordcharCharacterClass = m_pattern.wordcharCharacterClass();

        matchCharacterClass(character, scratch, nextIsWordChar, wordcharCharacterClass);
    }

    void generateAssertionWordBoundary(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID scratch = m_regs.regT1;


        MacroAssembler::Jump atBegin;
        MacroAssembler::JumpList matchDest;
        // Forward: the boundary's real position is index - (checked - pos), so it
        // is the real start of input iff index == checked - pos (the classic
        // index == checked when the ^-like term is at pos 0; the general form also
        // covers a \b in the body of an assertion nested inside a lookbehind, whose
        // numbering continues from a nonzero origin). The preceding real character
        // is at frame offset (checked - pos + 1). Backward (mirrored): the real
        // start test is at the frame's right edge (pos == checked) with index == 0,
        // and the preceding real character is at (checked - pos).
        bool atStartEdge = m_direction == Backward ? term->inputPosition == op.m_checkedOffset : true;
        if (atStartEdge) {
            if (m_direction == Backward)
                atBegin = m_jit.branchTest32(MacroAssembler::Zero, m_regs.index);
            else
                atBegin = m_jit.branch32(MacroAssembler::Equal, m_regs.index, MacroAssembler::Imm32(static_cast<int32_t>((op.m_checkedOffset - term->inputPosition).value())));
        }
        // The character BEFORE the boundary ends at the unit just left of it. In
        // a mirrored frame that unit is at frame offset (checked - pos), and in a
        // unicode pattern it is a leftward decode (an astral pair ending at the
        // boundary is classified as its code point). Forward: (checked - pos + 1).
        if (m_direction == Backward)
            readCharacterForDirection(op.m_checkedOffset - term->inputPosition, character);
        else
            readCharacter(op.m_checkedOffset - term->inputPosition + 1, character);

        CharacterClass* wordcharCharacterClass;

        if (m_pattern.eitherUnicode() && term->ignoreCase())
            wordcharCharacterClass = m_pattern.wordUnicodeIgnoreCaseCharCharacterClass();
        else
            wordcharCharacterClass = m_pattern.wordcharCharacterClass();

        matchCharacterClass(character, scratch, matchDest, wordcharCharacterClass);
        if (atStartEdge)
            atBegin.link(&m_jit);

        // We fall through to here if the last character was not a wordchar.
        MacroAssembler::JumpList nonWordCharThenWordChar;
        MacroAssembler::JumpList nonWordCharThenNonWordChar;
        if (term->invert()) {
            matchAssertionWordchar(opIndex, nonWordCharThenNonWordChar, nonWordCharThenWordChar);
            nonWordCharThenWordChar.append(m_jit.jump());
        } else {
            matchAssertionWordchar(opIndex, nonWordCharThenWordChar, nonWordCharThenNonWordChar);
            nonWordCharThenNonWordChar.append(m_jit.jump());
        }
        op.m_jumps.append(nonWordCharThenNonWordChar);

        // We jump here if the last character was a wordchar.
        matchDest.link(&m_jit);
        MacroAssembler::JumpList wordCharThenWordChar;
        MacroAssembler::JumpList wordCharThenNonWordChar;
        if (term->invert()) {
            matchAssertionWordchar(opIndex, wordCharThenNonWordChar, wordCharThenWordChar);
            wordCharThenWordChar.append(m_jit.jump());
        } else {
            matchAssertionWordchar(opIndex, wordCharThenWordChar, wordCharThenNonWordChar);
            // This can fall-though!
        }

        op.m_jumps.append(wordCharThenWordChar);

        nonWordCharThenWordChar.link(&m_jit);
        wordCharThenNonWordChar.link(&m_jit);
    }

    void backtrackAssertionWordBoundary(size_t opIndex)
    {
        backtrackTermDefault(opIndex);
    }

#if ENABLE(YARR_JIT_BACKREFERENCES)
    void matchBackreference(size_t opIndex, MacroAssembler::JumpList& characterMatchFails, MacroAssembler::RegisterID character, MacroAssembler::RegisterID patternIndex, MacroAssembler::RegisterID patternCharacter, MacroAssembler::RegisterID subpatternIdReg)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;
        unsigned subpatternId = term->backReferenceSubpatternId;
        unsigned duplicateNamedGroupId = m_pattern.hasDuplicateNamedCaptureGroups() ? m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId] : 0;


        MacroAssembler::Label loop(&m_jit);

        // The referenced capture is an ordinary substring: its units are read
        // forward from patternIndex, and the input span is compared ascending in
        // lockstep. Both operand offsets are expressed in the frame's addressing:
        // forward reads input[reg - k], a mirrored (lookbehind) frame reads
        // input[reg + k - 1] -- so the capture unit itself is k = 1 there, and
        // the input span (walked upward from its left edge, which the caller put
        // in index) begins one unit past the term's own offset.
        unsigned captureUnitOffset = m_direction == Backward ? 1 : 0;
        Checked<unsigned> inputSpanOffset = op.m_checkedOffset - term->inputPosition;
        if (m_direction == Backward)
            inputSpanOffset += 1;

#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
        if (!m_decodeSurrogatePairs)
            readCharacter(captureUnitOffset, patternCharacter, patternIndex);
        else {
            // For reading Unicode characters, use the standard resultReg so we can call the standard tryReadUnicodeChar()
            // helper instead of emitting an inlined version.
            readCharacter(captureUnitOffset, character, patternIndex);
            m_jit.move(character, patternCharacter);
        }
#else
        readCharacter(captureUnitOffset, patternCharacter, patternIndex);
#endif
        readCharacter(inputSpanOffset, character);

        if (!term->ignoreCase()) {
            // When !m_decodeSurrogatePairs, readCharacter() emits load8/load16
            // (zero-extended), so the result is always in [0, 0xFFFF] and can
            // never equal errorCodePoint (-1). Only tryReadUnicodeChar() —
            // reached when m_decodeSurrogatePairs — can produce errorCodePoint.
            if (m_decodeSurrogatePairs)
                characterMatchFails.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::TrustedImm32(errorCodePoint)));
            characterMatchFails.append(m_jit.branch32(MacroAssembler::NotEqual, character, patternCharacter));
        } else if (m_charSize == CharSize::Char8) {
            MacroAssembler::Jump charactersMatch = m_jit.branch32(MacroAssembler::Equal, character, patternCharacter);
            MacroAssembler::ExtendedAddress characterTableEntry(character, reinterpret_cast<intptr_t>(&latin1CanonicalizationTable));
            m_jit.load16(characterTableEntry, character);
            MacroAssembler::ExtendedAddress patternTableEntry(patternCharacter, reinterpret_cast<intptr_t>(&latin1CanonicalizationTable));
            m_jit.load16(patternTableEntry, patternCharacter);
            characterMatchFails.append(m_jit.branch32(MacroAssembler::NotEqual, character, patternCharacter));
            charactersMatch.link(&m_jit);
        }
#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
        else {
            // 16 Bit ignore case matching.
            RELEASE_ASSERT(character == areCanonicallyEquivalentCharArgReg);
            RELEASE_ASSERT(patternCharacter == areCanonicallyEquivalentPattCharArgReg);
            RELEASE_ASSERT(m_regs.regUnicodeInputAndTrail == areCanonicallyEquivalentCanonicalModeArgReg);
            ASSERT(m_decode16BitForBackreferencesWithCalls);

            // When !m_decodeSurrogatePairs, readCharacter() emits load8/load16
            // (zero-extended), so the result is always in [0, 0xFFFF] and can
            // never equal errorCodePoint (-1). Only tryReadUnicodeChar() —
            // reached when m_decodeSurrogatePairs — can produce errorCodePoint.
            if (m_decodeSurrogatePairs) {
                characterMatchFails.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::TrustedImm32(errorCodePoint)));
                characterMatchFails.append(m_jit.branch32(MacroAssembler::Equal, patternCharacter, MacroAssembler::TrustedImm32(errorCodePoint)));
            }

            MacroAssembler::JumpList charactersMatch;
            charactersMatch.append(m_jit.branch32(MacroAssembler::Equal, character, patternCharacter));
            // Both character and patternCharacter must be ASCII to use the latin1CanonicalizationTable
            // (which has only 256 entries). If either is non-ASCII, fall through to the slow path.
            MacroAssembler::Jump characterNotASCII = m_jit.branch32(MacroAssembler::GreaterThan, character, MacroAssembler::TrustedImm32(127));
            MacroAssembler::Jump patternCharNotASCII = m_jit.branch32(MacroAssembler::GreaterThan, patternCharacter, MacroAssembler::TrustedImm32(127));
            // The ASCII part of latin1CanonicalizationTable works for UCS2 and Unicode patterns.
            MacroAssembler::ExtendedAddress characterTableEntry(character, reinterpret_cast<intptr_t>(&latin1CanonicalizationTable));
            m_jit.load16(characterTableEntry, character);
            MacroAssembler::ExtendedAddress patternTableEntry(patternCharacter, reinterpret_cast<intptr_t>(&latin1CanonicalizationTable));
            m_jit.load16(patternTableEntry, patternCharacter);
            characterMatchFails.append(m_jit.branch32(MacroAssembler::NotEqual, character, patternCharacter));
            charactersMatch.append(m_jit.jump());

            characterNotASCII.link(&m_jit);
            patternCharNotASCII.link(&m_jit);
            // We are safe to use the regUnicodeInputAndTrail register as an argument since it
            // is only used when reading unicode characters.
            int32_t canonicalMode = static_cast<int32_t>(m_decodeSurrogatePairs ? CanonicalMode::Unicode : CanonicalMode::UCS2);
            m_jit.move(MacroAssembler::TrustedImm32(canonicalMode), areCanonicallyEquivalentCanonicalModeArgReg);

            m_jit.nearCallThunk(CodeLocationLabel { m_vm->getCTIStub(areCanonicallyEquivalentThunkGenerator).retaggedCode<NoPtrTag>() });

            // Match return as a bool in character reg.
            characterMatchFails.append(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(0)));
            // Add code to compare non-ASCII Unicode codepoints.
            charactersMatch.link(&m_jit);
        }
#endif

        m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
        m_jit.add32(MacroAssembler::TrustedImm32(1), patternIndex);

        if (m_decodeSurrogatePairs) {
            auto isBMPChar = m_jit.branch32(MacroAssembler::LessThan, patternCharacter, MacroAssembler::TrustedImm32(0x10000));
            m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
            m_jit.add32(MacroAssembler::TrustedImm32(1), patternIndex);
            isBMPChar.link(&m_jit);
        }

        if (!!duplicateNamedGroupId) {
            MacroAssembler::RegisterID endIndex = character; // We can reuse the character register here as we already matched.

            if (subpatternIdReg == InvalidGPRReg) {
                subpatternIdReg = m_regs.unicodeAndSubpatternIdTemp;
                loadDuplicateNamedGroupSubpatternId(duplicateNamedGroupId, subpatternIdReg);
            }
            loadSubPatternEnd(subpatternIdReg, endIndex);
            if (m_decodeSurrogatePairs)
                characterMatchFails.append(m_jit.branch32(MacroAssembler::Above, patternIndex, endIndex));
            m_jit.branch32(MacroAssembler::NotEqual, patternIndex, endIndex).linkTo(loop, &m_jit);
        } else {
            if (m_decodeSurrogatePairs)
                characterMatchFails.append(m_jit.branch32(MacroAssembler::Above, patternIndex, subpatternEndAddress(subpatternId)));
            m_jit.branch32(MacroAssembler::NotEqual, patternIndex, subpatternEndAddress(subpatternId)).linkTo(loop, &m_jit);
        }
    }

    void generateBackReference(bool isNamed, size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

#if !ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
        if (term->ignoreCase() && m_charSize != CharSize::Char8) {
            m_failureReason = JITFailureReason::BackReference;
            return;
        }
#endif

        unsigned subpatternId = term->backReferenceSubpatternId;
        unsigned duplicateNamedGroupId = (isNamed && m_pattern.hasDuplicateNamedCaptureGroups()) ? m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId] : 0;
        unsigned parenthesesFrameLocation = term->frameLocation;

        const MacroAssembler::RegisterID characterOrTemp = m_regs.regT0;
        const MacroAssembler::RegisterID patternTemp = m_regs.regT1;
        const MacroAssembler::RegisterID patternIndex = m_regs.regT2;
        m_usesT2 = true;

        MacroAssembler::RegisterID subpatternIdReg = InvalidGPRReg;

        storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoBackReference::beginIndex());
        if (term->quantityType != QuantifierType::FixedCount || term->quantityMaxCount != 1)
            storeToFrame(MacroAssembler::TrustedImm32(0), parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());

        MacroAssembler::JumpList matches;

        if (term->quantityType != QuantifierType::NonGreedy) {
            MacroAssembler::JumpList zeroLengthMatches;

            if (duplicateNamedGroupId) {
                if (!m_decodeSurrogatePairs)
                    subpatternIdReg  = m_regs.unicodeAndSubpatternIdTemp;
                else
                    subpatternIdReg  = patternTemp;

                loadSubPatternIdForDuplicateNamedGroup(duplicateNamedGroupId, subpatternIdReg);
                MacroAssembler::Jump emptySubpattern = m_jit.branch32(MacroAssembler::Equal, MacroAssembler::TrustedImm32(0), subpatternIdReg);
                if (term->quantityType != QuantifierType::FixedCount || term->quantityMaxCount != 1) {
                    // This is an empty match, which is successful.
                    matches.append(emptySubpattern);
                } else
                    zeroLengthMatches.append(emptySubpattern);

                loadSubPattern(subpatternIdReg, patternIndex, patternTemp);
            } else
                loadSubPattern(subpatternId, patternIndex, patternTemp);

            // An empty match is successful without consuming characters
            if (term->quantityType != QuantifierType::FixedCount || term->quantityMaxCount != 1) {
                matches.append(m_jit.branch32(MacroAssembler::Equal, MacroAssembler::TrustedImm32(-1), patternTemp));
                matches.append(m_jit.branch32(MacroAssembler::Equal, patternIndex, patternTemp));
            } else {
                zeroLengthMatches.append(m_jit.branch32(MacroAssembler::Equal, MacroAssembler::TrustedImm32(-1), patternTemp));
                MacroAssembler::Jump tryNonZeroMatch = m_jit.branch32(MacroAssembler::NotEqual, patternIndex, patternTemp);
                zeroLengthMatches.link(&m_jit);
                storeToFrame(MacroAssembler::TrustedImm32(1), parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());
                if (term->quantityType == QuantifierType::Greedy)
                    storeToFrame(MacroAssembler::TrustedImm32(0), parenthesesFrameLocation + BackTrackInfoBackReference::backReferenceSizeIndex());
                matches.append(m_jit.jump());
                tryNonZeroMatch.link(&m_jit);
            }
        }

        switch (term->quantityType) {
        case QuantifierType::FixedCount: {
            MacroAssembler::Label outerLoop(&m_jit);

            // PatternTemp should contain pattern end index at this point. Compute pattern size.
            m_jit.sub32(patternIndex, patternTemp);
            emitBackReferenceSpanClaim(op.m_jumps, patternTemp, characterOrTemp, parenthesesFrameLocation);

            matchBackreference(opIndex, op.m_jumps, characterOrTemp, patternIndex, patternTemp, subpatternIdReg == m_regs.unicodeAndSubpatternIdTemp ? subpatternIdReg : InvalidGPRReg);
            emitBackReferenceSpanFrontier(parenthesesFrameLocation);

            if (term->quantityMaxCount != 1) {
                loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex(), characterOrTemp);
                m_jit.add32(MacroAssembler::TrustedImm32(1), characterOrTemp);
                storeToFrame(characterOrTemp, parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());
                matches.append(m_jit.branch32(MacroAssembler::Equal, MacroAssembler::Imm32(term->quantityMaxCount), characterOrTemp));
                if (duplicateNamedGroupId) {
                    if (m_decodeSurrogatePairs)
                        loadSubPatternIdForDuplicateNamedGroup(duplicateNamedGroupId, subpatternIdReg);
                    // At this point, we have already checked that subpatternIdReg has a valid subpatternId.
                    loadSubPattern(subpatternIdReg, patternIndex, patternTemp);
                } else
                    loadSubPattern(subpatternId, patternIndex, patternTemp);
                m_jit.jump(outerLoop);
            }
            matches.link(&m_jit);

            storeToFrame(MacroAssembler::TrustedImm32(1), parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());
            break;
        }

        case QuantifierType::Greedy: {
            MacroAssembler::JumpList incompleteMatches;

            MacroAssembler::Label outerLoop(&m_jit);

            // PatternTemp should contain pattern end index at this point. Compute pattern size.
            m_jit.sub32(patternIndex, patternTemp);
            storeToFrame(patternTemp, parenthesesFrameLocation + BackTrackInfoBackReference::backReferenceSizeIndex());

            emitBackReferenceSpanClaim(matches, patternTemp, characterOrTemp, parenthesesFrameLocation);

            matchBackreference(opIndex, incompleteMatches, characterOrTemp, patternIndex, patternTemp, subpatternIdReg == m_regs.unicodeAndSubpatternIdTemp ? subpatternIdReg : InvalidGPRReg);
            emitBackReferenceSpanFrontier(parenthesesFrameLocation);

            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex(), characterOrTemp);
            m_jit.add32(MacroAssembler::TrustedImm32(1), characterOrTemp);
            storeToFrame(characterOrTemp, parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());
            if (term->quantityMaxCount != quantifyInfinite)
                matches.append(m_jit.branch32(MacroAssembler::Equal, MacroAssembler::Imm32(term->quantityMaxCount), characterOrTemp));
            if (duplicateNamedGroupId) {
                if (m_decodeSurrogatePairs)
                    loadSubPatternIdForDuplicateNamedGroup(duplicateNamedGroupId, subpatternIdReg);
                // At this point, we have already checked that subpatternIdReg has a valid subpatternId.
                loadSubPattern(subpatternIdReg, patternIndex, patternTemp);
            } else
                loadSubPattern(subpatternId, patternIndex, patternTemp);

            // Store current index in frame for restoring after a partial match
            storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoBackReference::beginIndex());
            m_jit.jump(outerLoop);

            incompleteMatches.link(&m_jit);
            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::beginIndex(), m_regs.index);

            matches.link(&m_jit);
            defineReentryLabel(op);
            break;
        }

        case QuantifierType::NonGreedy: {
            MacroAssembler::JumpList incompleteMatches;
            MacroAssembler::JumpList zeroLengthMatches;

            // Save the initial index before matching so we can restore it
            // if backtracking fails completely. We reuse the backReferenceSize
            // frame slot, which is only used by Greedy for storing match size.
            // beginIndex cannot be used here because it is overwritten on
            // each reentry iteration for partial match recovery.
            storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoBackReference::backReferenceSizeIndex());
            matches.append(m_jit.jump());

            defineReentryLabel(op);

            if (duplicateNamedGroupId) {
                if (!m_decodeSurrogatePairs)
                    subpatternIdReg  = m_regs.unicodeAndSubpatternIdTemp;
                else
                    subpatternIdReg  = patternTemp;

                loadSubPatternIdForDuplicateNamedGroup(duplicateNamedGroupId, subpatternIdReg);
                zeroLengthMatches.append(m_jit.branch32(MacroAssembler::Equal, MacroAssembler::TrustedImm32(0), subpatternIdReg));

                loadSubPattern(subpatternIdReg, patternIndex, patternTemp);
            } else
                loadSubPattern(subpatternId, patternIndex, patternTemp);

            // An empty match is successful without consuming characters
            zeroLengthMatches.append(m_jit.branch32(MacroAssembler::Equal, MacroAssembler::TrustedImm32(-1), patternTemp));
            MacroAssembler::Jump tryNonZeroMatch = m_jit.branch32(MacroAssembler::NotEqual, patternIndex, patternTemp);
            zeroLengthMatches.link(&m_jit);
            storeToFrame(MacroAssembler::TrustedImm32(1), parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());
            matches.append(m_jit.jump());
            tryNonZeroMatch.link(&m_jit);

            // Check if we have input remaining to match.
            // Update beginIndex before the check so the backtrack code's
            // zero-width progress guard sees the current position even
            // when checkNotEnoughInput bails out early.
            storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoBackReference::beginIndex());
            m_jit.sub32(patternIndex, patternTemp);
            emitBackReferenceSpanClaim(matches, patternTemp, characterOrTemp, parenthesesFrameLocation);

            matchBackreference(opIndex, incompleteMatches, characterOrTemp, patternIndex, patternTemp, subpatternIdReg == m_regs.unicodeAndSubpatternIdTemp ? subpatternIdReg : InvalidGPRReg);
            emitBackReferenceSpanFrontier(parenthesesFrameLocation);

            matches.append(m_jit.jump());

            incompleteMatches.link(&m_jit);
            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::beginIndex(), m_regs.index);

            matches.link(&m_jit);
            break;
        }
        }
    }
    void backtrackBackReference(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        op.m_jumps.link(&m_jit);
        m_backtrackingState.link(*this, op);

        MacroAssembler::JumpList failures;

        unsigned parenthesesFrameLocation = term->frameLocation;
        switch (term->quantityType) {
        case QuantifierType::FixedCount:
            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::beginIndex(), m_regs.index);
            break;

        case QuantifierType::Greedy: {
            const MacroAssembler::RegisterID matchAmount = m_regs.regT0;
            const MacroAssembler::RegisterID matchSize = m_regs.regT1;

            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex(), matchAmount);
            failures.append(m_jit.branchTest32(MacroAssembler::Zero, matchAmount));

            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::backReferenceSizeIndex(), matchSize);
            // Give one span back: retreat the forward frontier, or raise the
            // leftward-consuming frontier of a mirrored frame, by the span size.
            if (m_direction == Backward)
                m_jit.add32(matchSize, m_regs.index);
            else
                m_jit.sub32(matchSize, m_regs.index);

            m_jit.sub32(MacroAssembler::TrustedImm32(1), matchAmount);
            storeToFrame(matchAmount, parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());
            m_jit.jump(op.m_reentry);
            break;
        }

        case QuantifierType::NonGreedy: {
            const MacroAssembler::RegisterID matchAmount = m_regs.regT0;
            const MacroAssembler::RegisterID beginIndex = m_regs.regT1;

            MacroAssembler::JumpList nonGreedyFailures;
            nonGreedyFailures.append(atEndOfInput());
            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex(), matchAmount);
            if (term->quantityMaxCount != quantifyInfinite)
                nonGreedyFailures.append(m_jit.branch32(MacroAssembler::AboveOrEqual, matchAmount, MacroAssembler::Imm32(term->quantityMaxCount)));

            // If the index hasn't advanced past beginIndex and matchAmount > 0,
            // the backreference matched zero-width (undefined or empty capture).
            // Repeating zero-width matches cannot make progress, so fail.
            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::beginIndex(), beginIndex);
            MacroAssembler::Jump indexAdvanced = m_jit.branch32(MacroAssembler::NotEqual, m_regs.index, beginIndex);
            nonGreedyFailures.append(m_jit.branchTest32(MacroAssembler::NonZero, matchAmount));
            indexAdvanced.link(&m_jit);

            m_jit.add32(MacroAssembler::TrustedImm32(1), matchAmount);
            storeToFrame(matchAmount, parenthesesFrameLocation + BackTrackInfoBackReference::matchAmountIndex());
            m_jit.jump(op.m_reentry);

            nonGreedyFailures.link(&m_jit);
            // Restore the initial index saved before any NonGreedy matching.
            loadFromFrame(parenthesesFrameLocation + BackTrackInfoBackReference::backReferenceSizeIndex(), m_regs.index);
            break;
        }
        }
        failures.link(&m_jit);
        m_backtrackingState.fallthrough();
    }
#endif

    void generatePatternCharacterOnce(size_t opIndex, MatchTargets& matchTargets)
    {
        YarrOp& op = m_ops[opIndex];

        if (op.m_isDeadCode)
            return;

        MatchTargets defaultMatchTargets(matchTargets.hasFailedTarget() ? matchTargets.matchFailed() : op.m_jumps, MatchTargets::PreferredTarget::MatchSuccessFallThrough);
        MatchTargets lastMatchTargets(matchTargets.matchSucceeded(), matchTargets.hasFailedTarget() ? matchTargets.matchFailed() : op.m_jumps, matchTargets.preferredTarget());

        // m_ops always ends with a YarrOpCode::BodyAlternativeEnd or YarrOpCode::MatchFailed
        // node, so there must always be at least one more node.
        ASSERT(opIndex + 1 < m_ops.size());

        const MacroAssembler::RegisterID character = m_regs.regT0;
#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
        unsigned maxCharactersAtOnce = m_charSize == CharSize::Char8 ? 8 : 4;
#else
        unsigned maxCharactersAtOnce = m_charSize == CharSize::Char8 ? 4 : 2;
#endif

        uint64_t charMask = m_charSize == CharSize::Char8 ? 0xff : 0xffff;
        Vector<YarrOp*, 16> opList;
        unsigned firstPosition = op.m_term->inputPosition;
        unsigned lastPosition = firstPosition;
        auto firstChar = op.m_term->patternCharacter;
        bool have16BitCharacter = !isLatin1(firstChar);

        // For case-insesitive compares, non-ascii characters that have different
        // upper & lower case representations are converted to a character class.
        ASSERT(!op.m_term->ignoreCase() || isASCIIAlpha(firstChar) || isCanonicallyUnique(firstChar, m_canonicalMode));

        if (m_direction == Backward || (m_decodeSurrogatePairs && (!U_IS_BMP(firstChar) || U16_IS_SURROGATE(firstChar)))) {
            // Match a single character on its own, without fusing neighbours into a
            // wide load. In a backward (lookbehind) frame ascending memory holds
            // *descending* pattern positions, so the forward multi-character packing
            // does not apply (a run could be fused by comparing against the reversed
            // packed constant loaded from the run's lowest address; not done yet);
            // and a non-BMP / dangling surrogate char in a unicode pattern is likewise
            // matched individually.
            uint64_t charToMatch = firstChar;

            auto offset = op.m_checkedOffset - op.m_term->inputPosition;

            if (m_direction == Backward && m_decodeSurrogatePairs && !U_IS_BMP(firstChar)) {
                // Backward astral literal: the term is a fixed two-unit code point.
                // Its higher frame offset addresses the later (trail) unit and the
                // next-lower offset the lead unit, so comparing the two units at
                // their fixed positions equals comparing the code point (a lead+trail
                // pair encodes exactly one code point). No decode, no dynamic width.
                // Such a literal is canonically unique or ASCII by construction
                // (atomPatternCharacter), so no case folding applies.
                ASSERT(!op.m_term->ignoreCase() || isCanonicallyUnique(firstChar, m_canonicalMode));
                readCodeUnit(offset, character);
                defaultMatchTargets.appendFailed(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_TRAIL(firstChar))));
                readCodeUnit(offset - 1, character);
                if (!matchTargets.hasSucceedTarget() || m_ops[opIndex + 1].m_op == YarrOpCode::Term)
                    defaultMatchTargets.appendFailed(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_LEAD(firstChar))));
                else
                    matchTargets.appendSucceeded(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(U16_LEAD(firstChar))));
                return;
            }

            if (!matchTargets.hasSucceedTarget() || m_ops[opIndex + 1].m_op == YarrOpCode::Term)
                defaultMatchTargets.appendFailed(jumpIfCharNotEquals(charToMatch, offset, character, op.m_term->ignoreCase()));
            else
                matchTargets.appendSucceeded(jumpIfCharEquals(charToMatch, offset, character, op.m_term->ignoreCase()));

            return;
        }

        opList.append(&m_ops[opIndex]);

        for (size_t i = opIndex + 1; i < m_ops.size(); ++i) {
            YarrOp* currOp = &m_ops[i];
            if (currOp->m_op != YarrOpCode::Term)
                break;

            PatternTerm* currTerm = currOp->m_term;

            // YarrJIT handles decoded surrogate pair as one character if unicode flag is enabled.
            // Note that the numberCharacters become 1 while the width of the pattern character becomes 32bit in this case.
            if (currTerm->quantityType != QuantifierType::FixedCount
                || currTerm->quantityMaxCount != 1
                || (currTerm->type != PatternTerm::Type::PatternCharacter
                    && currTerm->type != PatternTerm::Type::CharacterClass)
                || (m_decodeSurrogatePairs
                    && ((currTerm->type == PatternTerm::Type::PatternCharacter && (!U_IS_BMP(currTerm->patternCharacter) || U16_IS_SURROGATE(currTerm->patternCharacter)))
                        || (currTerm->type == PatternTerm::Type::CharacterClass && (currTerm->characterClass->hasNonBMPCharacters()
                            || currTerm->invert())))))
                break;

            unsigned currPosition = currTerm->inputPosition;

            constexpr unsigned maxGroupingDistance = 16;

            if (currPosition > lastPosition) {
                // If the next term is too far away, we'll handle it by itself
                if (currPosition > lastPosition + maxGroupingDistance)
                    break;
                if (currPosition > lastPosition + 1)
                    opList.insertFill(lastPosition - firstPosition + 1, nullptr, currPosition - lastPosition - 1);
                opList.append(currOp);
                lastPosition = currPosition;
            } else if (currPosition < firstPosition) {
                // If the next term is too far away, we'll handle it by itself
                if (currPosition < firstPosition - maxGroupingDistance)
                    break;
                opList.insertFill(0, nullptr, firstPosition - currPosition);
                opList.first() = currOp;
                firstPosition = currPosition;
            } else {
                ASSERT(opList[currPosition - firstPosition] == nullptr);
                opList[currPosition - firstPosition] = currOp;
            }
        }

        // Prune list after first hole and check for 16 bit characters. Also mark "dead" terms that will be checked as part of this term's processing.
        bool foundFirstCharTerm = opList[0]->m_term->type == PatternTerm::Type::PatternCharacter;
        size_t firstCharTermIndex = 0;
        for (size_t i = 1; i < opList.size(); ++i) {
            YarrOp* currOp = opList[i];

            if (!currOp) {
                // If we have characters, break out
                if (foundFirstCharTerm) {
                    opList.shrink(i);
                    break;
                }
                // Otherwise, we're still in the non-character prefix
                continue;
            }

            if (currOp->m_term->type == PatternTerm::Type::PatternCharacter) {
                // For case-insesitive compares, non-ascii characters that have different
                // upper & lower case representations are converted to a character class.
                ASSERT(!currOp->m_term->ignoreCase() || isASCIIAlpha(currOp->m_term->patternCharacter) || isCanonicallyUnique(currOp->m_term->patternCharacter, m_canonicalMode));
                if (foundFirstCharTerm)
                    currOp->m_isDeadCode = true;
                else {
                    foundFirstCharTerm = true;
                    firstCharTermIndex = i;
                }
                have16BitCharacter |= !isLatin1(currOp->m_term->patternCharacter);
            }
        }

        // We definitely should have a PatternCharacter, otherwise we shouldn't have gotten here
        // This assertion also checks that firstCharTermIndex is correct
        ASSERT(foundFirstCharTerm);
        if (firstCharTermIndex)
            opList.removeAt(0, firstCharTermIndex);

        if (have16BitCharacter && (m_charSize == CharSize::Char8)) {
            // Have a 16 bit pattern character and an 8 bit string - short circuit
            defaultMatchTargets.appendFailed(m_jit.jump());
            return;
        }

        // Remove all trailing character class terms.
        while (!opList.isEmpty() && opList.last()->m_term->type == PatternTerm::Type::CharacterClass)
            opList.removeLast();

        RELEASE_ASSERT(!opList.isEmpty());

        YarrOp* lastFusedOp = opList[0];
        for (YarrOp* fusedOp : opList)
            lastFusedOp = std::max(lastFusedOp, fusedOp);
        bool fusedRunEndsAlternative = lastFusedOp[1].m_op != YarrOpCode::Term;

        auto checkedOffset = opList[0]->m_checkedOffset;

        unsigned startPosition = opList[0]->m_term->inputPosition;
        unsigned numCharsToCheck { 0 };
        unsigned charsCheckedLastIter { 0 };

        for (size_t opListIdx = 0; opListIdx < opList.size(); opListIdx += numCharsToCheck, startPosition += numCharsToCheck, charsCheckedLastIter = numCharsToCheck) {
            // Skip past leading non-Character terms.
            for (; opListIdx < opList.size(); ++opListIdx, ++startPosition) {
                YarrOp* currOp = opList[opListIdx];
                ASSERT(currOp);
                if (currOp->m_term->type == PatternTerm::Type::PatternCharacter)
                    break;
            }

            if (opListIdx == opList.size()) {
                // The remaining term(s) are all character classes. Our work here is done.
                return;
            }

            unsigned numCharsRemaining = opList.size() - opListIdx;
            unsigned negativeOffset = 0;
            numCharsToCheck = std::min(numCharsRemaining, maxCharactersAtOnce);

            // We want to do the minimul number of load, compare and branch blocks.
            // This means that we want to do overlapping loads and masking if that is profitable.
            // For example, if we have 7 adjacent characters, we want to do two load32, compare and branch
            // groups with the second group offset by 1 byte. If that group of 7 adjacent characters
            // occurs after a group of 8, we want to do one load64, compare and branch offset by one byte.
            // The goal is to do as many larger loads first, followed by one smaller one.
            // After this adjustment, numCharsToCheck should be 1, 2, 4 or 8.
            switch (numCharsToCheck) {
            case 3:
                if (charsCheckedLastIter >= 4) {
                    numCharsToCheck = 4;
                    negativeOffset = 1;
                } else
                    numCharsToCheck = 2;
                break;
            case 5:
                if (charsCheckedLastIter == 8) {
                    numCharsToCheck = 8;
                    negativeOffset = 3;
                } else
                    numCharsToCheck = 4;
                break;
            case 6:
                if (charsCheckedLastIter == 8) {
                    numCharsToCheck = 8;
                    negativeOffset = 2;
                } else
                    numCharsToCheck = 4;
                break;
            case 7:
                if (charsCheckedLastIter == 8) {
                    numCharsToCheck = 8;
                    negativeOffset = 1;
                } else if (charsCheckedLastIter == 4) {
                    numCharsToCheck = 4;
                    negativeOffset = 1;
                } else
                    numCharsToCheck = 4;
                break;
            default:
                break;
            }

            if (negativeOffset) {
                opListIdx -= negativeOffset;
                startPosition -= negativeOffset;
            }

#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
            if (startPosition)
                m_useFirstNonBMPCharacterOptimization = false;
#endif

            ASSERT(numCharsToCheck == 1 || numCharsToCheck == 2 || numCharsToCheck == 4 || numCharsToCheck == 8);

            auto calcShiftAmount = [&] (unsigned positionInLoad) {
                return (m_charSize == CharSize::Char8 ? 8 : 16) * positionInLoad;
            };

            char32_t currentCharacter { 0 };
            uint64_t allCharacters { 0 };
            uint64_t caseMask { 0 };
            uint64_t ignoredCharsMask { 0 };
            unsigned positionInLoad = 0;
            unsigned firstCharInLoad = opListIdx + negativeOffset;
            unsigned lastCharInLoad { 0 };
            for (unsigned i = 0; i < negativeOffset; ++i, ++positionInLoad)
                ignoredCharsMask |= charMask << calcShiftAmount(positionInLoad);

            for (auto i = opListIdx + negativeOffset; i < opListIdx + numCharsToCheck; ++i, ++positionInLoad) {
                YarrOp* currOp = opList[i];

                ASSERT(currOp);

                PatternTerm* currTerm = currOp->m_term;

                unsigned shiftAmount = calcShiftAmount(positionInLoad);

                if (currTerm->type == PatternTerm::Type::PatternCharacter) {
                    currentCharacter = currTerm->patternCharacter;

                    lastCharInLoad = i;
                    allCharacters |= (static_cast<uint64_t>(currentCharacter) << shiftAmount);
                    if (currTerm->ignoreCase() && isASCIIAlpha(currentCharacter))
                        caseMask |= 32ULL << shiftAmount;
                } else if (currTerm->type == PatternTerm::Type::CharacterClass)
                    ignoredCharsMask |= charMask << shiftAmount;
            }

            auto numRealCharsToCheck = roundUpToPowerOfTwo(lastCharInLoad - firstCharInLoad + 1);

#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
            if (m_useFirstNonBMPCharacterOptimization && numRealCharsToCheck > 1) {
                // We are going to try matching more than one character at a time,
                // so we should only advance one character at a time as normal.
                m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.firstCharacterAdditionalReadSize);
            }
#endif

            MatchTargets* matchTargetForFinalComparison = (fusedRunEndsAlternative && opListIdx + numCharsToCheck >= opList.size()) ? &lastMatchTargets : &defaultMatchTargets;

            if (m_charSize == CharSize::Char8) {
                auto check1 = [&] (Checked<unsigned> offset, char32_t characters, uint16_t caseMask, MatchTargets& matchTargets) {
                    readCharacter(offset, character);
                    if (caseMask)
                        m_jit.or32(MacroAssembler::Imm32(caseMask), character);
                    if (!matchTargets.hasSucceedTarget())
                        defaultMatchTargets.appendFailed(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(characters | caseMask)));
                    else
                        matchTargets.appendSucceeded(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(characters | caseMask)));
                };

                auto check2 = [&] (Checked<unsigned> offset, uint16_t characters, uint16_t caseMask, MatchTargets& matchTargets) {
                    m_jit.load16Unaligned(negativeOffsetIndexedAddress(offset, character), character);
                    if (caseMask)
                        m_jit.or32(MacroAssembler::Imm32(caseMask), character);
                    if (!matchTargets.hasSucceedTarget())
                        defaultMatchTargets.appendFailed(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(characters | caseMask)));
                    else
                        matchTargets.appendSucceeded(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(characters | caseMask)));
                };

                auto check4 = [&] (Checked<unsigned> offset, unsigned characters, unsigned caseMask, uint64_t ignoredCharsMask, MatchTargets& matchTargets) {
                    m_jit.load32WithUnalignedHalfWords(negativeOffsetIndexedAddress(offset, character), character);
                    if (ignoredCharsMask)
                        m_jit.and32(MacroAssembler::Imm32(~ignoredCharsMask), character);
                    if (caseMask)
                        m_jit.or32(MacroAssembler::Imm32(caseMask), character);
                    if (!matchTargets.hasSucceedTarget())
                        defaultMatchTargets.appendFailed(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(characters | caseMask)));
                    else
                        matchTargets.appendSucceeded(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(characters | caseMask)));
                };

#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
                auto check8 = [&] (Checked<unsigned> offset, uint64_t characters, uint64_t caseMask, uint64_t ignoredCharsMask, MatchTargets& matchTargets) {
                    m_jit.load64(negativeOffsetIndexedAddress(offset, character), character);
                    if (ignoredCharsMask)
                        m_jit.and64(MacroAssembler::TrustedImm64(~ignoredCharsMask), character);
                    if (caseMask)
                        m_jit.or64(MacroAssembler::TrustedImm64(caseMask), character);
                    if (!matchTargets.hasSucceedTarget())
                        defaultMatchTargets.appendFailed(m_jit.branch64(MacroAssembler::NotEqual, character, MacroAssembler::TrustedImm64(characters | caseMask)));
                    else
                        matchTargets.appendSucceeded(m_jit.branch64(MacroAssembler::Equal, character, MacroAssembler::TrustedImm64(characters | caseMask)));
                };
#endif

                switch (numRealCharsToCheck) {
                case 1:
                    ASSERT(~ignoredCharsMask);
                    check1(checkedOffset - startPosition, allCharacters & 0xff, caseMask & 0xff, *matchTargetForFinalComparison);
                    break;
                case 2: {
                    ASSERT(~ignoredCharsMask);
                    check2(checkedOffset - startPosition, allCharacters & 0xffff, caseMask & 0xffff, *matchTargetForFinalComparison);
                    break;
                }
                case 4: {
                    check4(checkedOffset - startPosition, allCharacters & 0xffffffff, caseMask & 0xffffffff, ignoredCharsMask, *matchTargetForFinalComparison);
                    break;
                }
#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
                case 8: {
                    check8(checkedOffset - startPosition, allCharacters, caseMask, ignoredCharsMask, *matchTargetForFinalComparison);
                    break;
                }
#endif
                default:
                    ASSERT_NOT_REACHED();
                    break;
                }
            } else {
                // m_charSize == CharSize::Char16
                auto check1 = [&] (Checked<unsigned> offset, char32_t characters, uint16_t caseMask, MatchTargets& matchTargets) {
                    if (!matchTargets.hasSucceedTarget())
                        defaultMatchTargets.appendFailed(jumpIfCharNotEquals(characters, offset, character, caseMask));
                    else
                        matchTargets.appendSucceeded(jumpIfCharEquals(characters, offset, character, caseMask));
                };

                auto check2 = [&] (Checked<unsigned> offset, unsigned characters, unsigned caseMask, MatchTargets& matchTargets) {
                    m_jit.load32WithUnalignedHalfWords(negativeOffsetIndexedAddress(offset, character), character);
                    if (caseMask)
                        m_jit.or32(MacroAssembler::Imm32(caseMask), character);
                    if (!matchTargets.hasSucceedTarget())
                        defaultMatchTargets.appendFailed(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(characters | caseMask)));
                    else
                        matchTargets.appendSucceeded(m_jit.branch32(MacroAssembler::Equal, character, MacroAssembler::Imm32(characters | caseMask)));
                };

#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
                auto check4 = [&] (Checked<unsigned> offset, uint64_t characters, uint64_t caseMask, uint64_t ignoredCharsMask, MatchTargets& matchTargets) {
                    m_jit.load64(negativeOffsetIndexedAddress(offset, character), character);
                    if (ignoredCharsMask)
                        m_jit.and64(MacroAssembler::TrustedImm64(~ignoredCharsMask), character);
                    if (caseMask)
                        m_jit.or64(MacroAssembler::TrustedImm64(caseMask), character);
                    if (!matchTargets.hasSucceedTarget())
                        defaultMatchTargets.appendFailed(m_jit.branch64(MacroAssembler::NotEqual, character, MacroAssembler::TrustedImm64(characters | caseMask)));
                    else
                        matchTargets.appendSucceeded(m_jit.branch64(MacroAssembler::Equal, character, MacroAssembler::TrustedImm64(characters | caseMask)));
                };
#endif

                switch (numRealCharsToCheck) {
                case 1:
                    ASSERT(~ignoredCharsMask);
                    check1(checkedOffset - startPosition, allCharacters & 0xffffffff, caseMask & 0xffffffff, *matchTargetForFinalComparison);
                    break;
                case 2: {
                    ASSERT(~ignoredCharsMask);
                    check2(checkedOffset - startPosition, allCharacters & 0xffffffff, caseMask & 0xffffffff, *matchTargetForFinalComparison);
                    break;
                }
#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
                case 4: {
                    check4(checkedOffset - startPosition, allCharacters, caseMask, ignoredCharsMask, *matchTargetForFinalComparison);
                    break;
                }
#endif
                default:
                    ASSERT_NOT_REACHED();
                    break;
                }
            }
        }
    }

    void backtrackPatternCharacterOnce(size_t opIndex)
    {
        backtrackTermDefault(opIndex);
    }

    void generatePatternCharacterFixed(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;
        char32_t ch = term->patternCharacter;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID countRegister = m_regs.regT1;

        if (m_decodeSurrogatePairs)
            op.m_jumps.append(jumpIfNoAvailableInput());

        Checked<unsigned> scaledMaxCount = term->quantityMaxCount;
        scaledMaxCount *= U_IS_BMP(ch) ? 1 : 2;
        // The count register walks toward index over the run of characters:
        // upward from index - count in a forward frame, downward from index + count
        // in a backward (mirrored) frame where index is the left frontier.
        emitFramePosition(scaledMaxCount, countRegister);

        Checked<unsigned> baseOffset = op.m_checkedOffset - term->inputPosition - scaledMaxCount;

        // For case-insesitive compares, non-ascii characters that have different
        // upper & lower case representations are converted to a character class.
        ASSERT(!term->ignoreCase() || isASCIIAlpha(ch) || isCanonicallyUnique(ch, m_canonicalMode));

        MacroAssembler::Label loop(&m_jit);
        if (m_direction == Backward && m_decodeSurrogatePairs && !U_IS_BMP(ch)) {
            // Backward run of a fixed astral literal: each occurrence is a known
            // (lead, trail) unit pair. At loop entry countRegister addresses the last
            // occurrence's trail unit; compare it, step the count register down one
            // unit to the lead, compare that, and step down once more. The lead is
            // reached by moving the index register rather than the (unsigned) frame
            // offset, which is 0 at the lowest occurrence. Unit-exact, no decoding.
            readCodeUnit(baseOffset, character, countRegister);
            op.m_jumps.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_TRAIL(ch))));
            m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);
            readCodeUnit(baseOffset, character, countRegister);
            op.m_jumps.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_LEAD(ch))));
            m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);
        } else {
            readCharacter(baseOffset, character, countRegister);
            if (term->ignoreCase() && isASCIIAlpha(ch)) {
                m_jit.or32(MacroAssembler::TrustedImm32(0x20), character);
                ch |= 0x20;
            }

            op.m_jumps.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(ch)));
            if (m_direction == Backward)
                m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
            else if (m_decodeSurrogatePairs && !U_IS_BMP(ch))
                m_jit.add32(MacroAssembler::TrustedImm32(2), countRegister);
#endif
            else
                m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);
        }
        m_jit.branch32(MacroAssembler::NotEqual, countRegister, m_regs.index).linkTo(loop, &m_jit);
    }
    void backtrackPatternCharacterFixed(size_t opIndex)
    {
        backtrackTermDefault(opIndex);
    }

    void generatePatternCharacterGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;
        char32_t ch = term->patternCharacter;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID countRegister = m_regs.regT1;

        m_jit.move(MacroAssembler::TrustedImm32(0), countRegister);

        // Unless have a 16 bit pattern character and an 8 bit string - short circuit
        if (!(!isLatin1(ch) && (m_charSize == CharSize::Char8))) {
            MacroAssembler::JumpList failures;
            MacroAssembler::Label loop(&m_jit);
            failures.append(atEndOfInput());
            if (m_direction == Backward && m_decodeSurrogatePairs && !U_IS_BMP(ch)) {
                // Backward astral literal: the frontier-adjacent unit must be the
                // trail; take it, then require and compare the lead below it, taking
                // that too. Unit-exact (a lead+trail pair is one code point). If the
                // lead is absent or wrong the trail unit is given back before failing.
                Checked<unsigned> offset = op.m_checkedOffset - term->inputPosition;
                readCodeUnit(offset, character);
                failures.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_TRAIL(ch))));
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the trail unit
                MacroAssembler::JumpList giveBackTrail;
                giveBackTrail.append(m_jit.branchTest32(MacroAssembler::Zero, m_regs.index)); // no lead below
                readCodeUnit(offset, character); // now addresses the unit below the trail
                giveBackTrail.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_LEAD(ch))));
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the lead unit
                MacroAssembler::Jump matched = m_jit.jump();
                giveBackTrail.link(&m_jit);
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
                failures.append(m_jit.jump());
                matched.link(&m_jit);
            } else {
                failures.append(jumpIfCharNotEquals(ch, op.m_checkedOffset - term->inputPosition, character, term->ignoreCase()));

                // Consume the character: advance the frontier forward, or move the
                // leftward cursor down in a backward (mirrored) frame.
                if (m_direction == Backward)
                    m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
                else
                    m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
                if (m_decodeSurrogatePairs && !U_IS_BMP(ch)) {
                    MacroAssembler::Jump surrogatePairOk = notAtEndOfInput();
                    m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
                    failures.append(m_jit.jump());
                    surrogatePairOk.link(&m_jit);
                    m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
                }
#endif
            }
            m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);

            if (term->quantityMaxCount == quantifyInfinite)
                m_jit.jump(loop);
            else
                m_jit.branch32(MacroAssembler::NotEqual, countRegister, MacroAssembler::Imm32(term->quantityMaxCount)).linkTo(loop, &m_jit);

            failures.link(&m_jit);
        }
        defineReentryLabel(op);

        storeToFrame(countRegister, term->frameLocation + BackTrackInfoPatternCharacter::matchAmountIndex());
    }
    void backtrackPatternCharacterGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID countRegister = m_regs.regT1;

        m_backtrackingState.link(*this, op);

        if (term->possessive()) {
            // When term is possessive, we do not need to do backtracking.
            // Just rewind index completely and falling through to the next backtracking.
            loadFromFrame(term->frameLocation + BackTrackInfoPatternCharacter::matchAmountIndex(), countRegister);
            if (m_decodeSurrogatePairs && !U_IS_BMP(term->patternCharacter))
                m_jit.lshift32(MacroAssembler::TrustedImm32(1), countRegister);
            rewindIndex(countRegister);
            m_backtrackingState.fallthrough();
            return;
        }

        loadFromFrame(term->frameLocation + BackTrackInfoPatternCharacter::matchAmountIndex(), countRegister);
        m_backtrackingState.append(m_jit.branchTest32(MacroAssembler::Zero, countRegister));
        m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);
        // Give one character back: retreat the forward frontier, or move the
        // leftward cursor back up in a backward (mirrored) frame. The character
        // is a single unit unless it is an astral literal in a unicode pattern.
        unsigned characterWidth = (m_decodeSurrogatePairs && !U_IS_BMP(term->patternCharacter)) ? 2 : 1;
        if (m_direction == Backward)
            m_jit.add32(MacroAssembler::TrustedImm32(characterWidth), m_regs.index);
        else
            m_jit.sub32(MacroAssembler::TrustedImm32(characterWidth), m_regs.index);
        m_jit.jump(op.m_reentry);
    }

    void generatePatternCharacterNonGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID countRegister = m_regs.regT1;

        m_jit.move(MacroAssembler::TrustedImm32(0), countRegister);
        defineReentryLabel(op);
        storeToFrame(countRegister, term->frameLocation + BackTrackInfoPatternCharacter::matchAmountIndex());
    }
    void backtrackPatternCharacterNonGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;
        char32_t ch = term->patternCharacter;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID countRegister = m_regs.regT1;

        m_backtrackingState.link(*this, op);

        loadFromFrame(term->frameLocation + BackTrackInfoPatternCharacter::matchAmountIndex(), countRegister);

        // Unless have a 16 bit pattern character and an 8 bit string - short circuit
        if (!(!isLatin1(ch) && (m_charSize == CharSize::Char8))) {
            MacroAssembler::JumpList nonGreedyFailures;
            nonGreedyFailures.append(atEndOfInput());
            if (term->quantityMaxCount != quantifyInfinite)
                nonGreedyFailures.append(m_jit.branch32(MacroAssembler::Equal, countRegister, MacroAssembler::Imm32(term->quantityMaxCount)));
            if (m_direction == Backward && m_decodeSurrogatePairs && !U_IS_BMP(ch)) {
                // Backward astral literal: trail at the frontier, then the lead below
                // it (see generatePatternCharacterGreedy); a partial take is undone.
                Checked<unsigned> offset = op.m_checkedOffset - term->inputPosition;
                readCodeUnit(offset, character);
                nonGreedyFailures.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_TRAIL(ch))));
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the trail unit
                MacroAssembler::JumpList giveBackTrail;
                giveBackTrail.append(m_jit.branchTest32(MacroAssembler::Zero, m_regs.index)); // no lead below
                readCodeUnit(offset, character);
                giveBackTrail.append(m_jit.branch32(MacroAssembler::NotEqual, character, MacroAssembler::Imm32(U16_LEAD(ch))));
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the lead unit
                MacroAssembler::Jump matched = m_jit.jump();
                giveBackTrail.link(&m_jit);
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
                nonGreedyFailures.append(m_jit.jump());
                matched.link(&m_jit);
            } else {
                nonGreedyFailures.append(jumpIfCharNotEquals(ch, op.m_checkedOffset - term->inputPosition, character, term->ignoreCase()));

                if (m_direction == Backward)
                    m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
                else
                    m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
                if (m_decodeSurrogatePairs && !U_IS_BMP(ch)) {
                    MacroAssembler::Jump surrogatePairOk = notAtEndOfInput();
                    m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
                    nonGreedyFailures.append(m_jit.jump());
                    surrogatePairOk.link(&m_jit);
                    m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
                }
#endif
            }
            m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);

            m_jit.jump(op.m_reentry);
            nonGreedyFailures.link(&m_jit);
        }

        if (m_decodeSurrogatePairs && !U_IS_BMP(ch)) {
            // subtract countRegister*2 for non-BMP characters
            m_jit.lshift32(MacroAssembler::TrustedImm32(1), countRegister);
        }

        // Rewind everything this term consumed.
        rewindIndex(countRegister);
        m_backtrackingState.fallthrough();
    }

    void generateCharacterClassOnce(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID scratch = m_regs.regT1;

        if (m_decodeSurrogatePairs) {
            op.m_jumps.append(jumpIfNoAvailableInput());
            storeToFrame(m_regs.index, term->frameLocation + BackTrackInfoCharacterClass::beginIndex());
        }

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        // The shared-lead fast path loads the (lead, trail) pair forward from the
        // term's first unit; a backward frame addresses the term by its last unit
        // and uses the general path below.
        if (m_decodeSurrogatePairs && !term->invert() && m_direction == Forward) {
            if (auto sharedLead = term->characterClass->hasSharedLeadSurrogate()) {
                CharacterClass trailsOnlyClass;
                buildTrailsOnlyClass(*term->characterClass, trailsOnlyClass);
                readSurrogatePairAndMatchTrailForSharedLead(op.m_checkedOffset - term->inputPosition, character, scratch, m_regs.index, sharedLead.value(), &trailsOnlyClass, op.m_jumps);
                return;
            }
        }
#endif

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_direction == Backward && m_decodeSurrogatePairs) {
            // Backward + unicode: read the code point that ends at the term's
            // position (its last unit is at frame offset checkedOffset - position).
            // The class match runs first and leaves the frontier alone, preserving
            // `character` for the width test just as the forward path does;
            // beginIndex (stored above) restores the frontier on backtrack.
            readUnicodeCharBackward(negativeOffsetIndexedAddress(op.m_checkedOffset - term->inputPosition, character), character, scratch);
            matchCharacterClassTermInner(term, op.m_jumps, character, scratch);
            // A static-width class was laid out at its true width (2 units when its
            // members are all astral) and its claim already covers the whole pair.
            // Only a variable-width class was budgeted a single unit, so only it
            // grows the leftward claim by one when the code point was a pair
            // (failing when the frontier is exhausted).
            if (!term->isFixedWidthCharacterClass()) {
                MacroAssembler::Jump isBMPChar = m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::TrustedImm32(0x10000));
                claimBackwardPairLead(op.m_jumps); // claim the pair's lead unit
                isBMPChar.link(&m_jit);
            }
            return;
        }
#endif

        readCharacter(op.m_checkedOffset - term->inputPosition, character);

        matchCharacterClassTermInner(term, op.m_jumps, character, scratch);

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs && (!term->characterClass->hasOneCharacterSize() || term->invert())) {
            MacroAssembler::Jump isBMPChar = m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::TrustedImm32(0x10000));
            op.m_jumps.append(atEndOfInput());
            m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
            isBMPChar.link(&m_jit);
        }
#endif
    }

    void backtrackCharacterClassOnce(size_t opIndex, bool fallThroughToCharacterClassFixedCount)
    {
        UNUSED_PARAM(fallThroughToCharacterClassFixedCount);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs) {
            YarrOp& op = m_ops[opIndex];
            PatternTerm* term = op.m_term;

            m_backtrackingState.link(*this, op);
            // If we fallthough to the same CharacterClassOnce, we will override this index register, so we do not need to load here.
            if (!fallThroughToCharacterClassFixedCount)
                loadFromFrame(term->frameLocation + BackTrackInfoCharacterClass::beginIndex(), m_regs.index);
            m_backtrackingState.fallthrough();
        }
#endif
        backtrackTermDefault(opIndex);
    }

    void generateCharacterClassFixed(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID countRegister = m_regs.regT1;
        const MacroAssembler::RegisterID scratch = m_regs.regT2;
        m_usesT2 = true;

        MacroAssembler::JumpList done;

        if (m_decodeSurrogatePairs) {
            if (!term->isFixedWidthCharacterClass())
                storeToFrame(m_regs.index, term->frameLocation + BackTrackInfoCharacterClass::beginIndex());
            op.m_jumps.append(jumpIfNoAvailableInput());
        }

        Checked<unsigned> scaledMaxCount = term->quantityMaxCount;

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        bool nonBMPOnly = false;
        if (m_decodeSurrogatePairs && !term->invert() && term->characterClass->hasOnlyNonBMPCharacters()) {
            scaledMaxCount *= 2;
            nonBMPOnly = true;

            // The shared-lead pair fast path reads (lead, trail) forward from the
            // count position; a backward frame uses the general loop below.
            if (auto sharedLead = m_direction == Forward ? term->characterClass->hasSharedLeadSurrogate() : std::nullopt) {
                // Replace tryReadUnicodeCharImpl + full-codepoint class match
                // with a single 32-bit pair load + lead check + trail-only class match.
                ASSERT(term->isFixedWidthCharacterClass());
                CharacterClass trailsOnlyClass;
                buildTrailsOnlyClass(*term->characterClass, trailsOnlyClass);

                m_jit.sub32(m_regs.index, MacroAssembler::Imm32(scaledMaxCount), countRegister);

                MacroAssembler::Label sharedLoop(&m_jit);
                readSurrogatePairAndMatchTrailForSharedLead(op.m_checkedOffset - term->inputPosition - scaledMaxCount, character, scratch, countRegister, sharedLead.value(), &trailsOnlyClass, op.m_jumps);
                m_jit.add32(MacroAssembler::TrustedImm32(2), countRegister);
                m_jit.branch32(MacroAssembler::NotEqual, countRegister, m_regs.index).linkTo(sharedLoop, &m_jit);
                return;
            }
        }
#endif

        // See generatePatternCharacterFixed for the forward/backward count register walk.
        emitFramePosition(scaledMaxCount, countRegister);

        Checked<unsigned> baseOffset = op.m_checkedOffset - term->inputPosition - scaledMaxCount;

        MacroAssembler::Label loop(&m_jit);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_direction == Backward && m_decodeSurrogatePairs) {
            // Backward + unicode. At loop entry countRegister addresses the last
            // unit of the next-lower occurrence; decode the code point ending
            // there. Width is recovered from the code point itself (>= 0x10000 is
            // a pair), after the class match, which preserves `character`.
            readUnicodeCharBackward(negativeOffsetIndexedAddress(baseOffset, character, countRegister), character, scratch);
            matchCharacterClassTermInner(term, op.m_jumps, character, scratch);
            if (nonBMPOnly) {
                // Static width 2: budgeted at two units per match.
                m_jit.sub32(MacroAssembler::TrustedImm32(2), countRegister);
            } else if (term->isFixedWidthCharacterClass()) {
                // Static width 1 (all-BMP class). The read still decodes so that a
                // unit trailing a lead is presented as its astral code point, exactly
                // as the interpreter's tryReadBackward presents it; such a code point
                // is never in this class, so no width consequence arises.
                m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);
            } else {
                // Variable width: a pair consumed one more unit than budgeted, so
                // grow the leftward claim by one (fail if exhausted) and step over
                // both units. beginIndex, stored above, restores the frontier when
                // this term backtracks.
                m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);
                MacroAssembler::Jump isBMPChar = m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::TrustedImm32(0x10000));
                claimBackwardPairLead(op.m_jumps); // claim the pair's lead unit
                m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister); // extra unit of the pair
                isBMPChar.link(&m_jit);
            }
            m_jit.branch32(MacroAssembler::NotEqual, countRegister, m_regs.index).linkTo(loop, &m_jit);
            done.link(&m_jit);
            return;
        }
#endif

        readCharacter(baseOffset, character, countRegister);

        MacroAssembler::Label nonBMPLoop(&m_jit);

        matchCharacterClassTermInner(term, op.m_jumps, character, scratch);

        if (m_direction == Backward)
            m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        else if (m_decodeSurrogatePairs) {
            if (term->isFixedWidthCharacterClass())
                m_jit.add32(MacroAssembler::TrustedImm32(term->characterClass->hasNonBMPCharacters() ? 2 : 1), countRegister);
            else {
                m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);
                MacroAssembler::Jump isBMPChar = m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::TrustedImm32(0x10000));
                op.m_jumps.append(atEndOfInput());
                m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
                isBMPChar.link(&m_jit);
            }
        }
#endif
        else
            m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (nonBMPOnly) {
            done.append(m_jit.branch32(MacroAssembler::Equal, countRegister, m_regs.index));
            tryReadNonBMPUnicodeChar(baseOffset, character, countRegister);
            m_jit.jump().linkTo(nonBMPLoop, &m_jit);
        } else
#endif
        m_jit.branch32(MacroAssembler::NotEqual, countRegister, m_regs.index).linkTo(loop, &m_jit);

        done.link(&m_jit);
    }

    void backtrackCharacterClassFixed(size_t opIndex)
    {
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs) {
            YarrOp& op = m_ops[opIndex];
            PatternTerm* term = op.m_term;
            if (!term->isFixedWidthCharacterClass()) {
                m_backtrackingState.link(*this, op);
                op.m_jumps.link(&m_jit);
                loadFromFrame(term->frameLocation + BackTrackInfoCharacterClass::beginIndex(), m_regs.index);
                m_backtrackingState.fallthrough();
                return;
            }
        }
#endif
        backtrackTermDefault(opIndex);
    }

    void generateCharacterClassGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID countRegister = m_regs.regT1;
        const MacroAssembler::RegisterID scratch = m_regs.regT2;
        m_usesT2 = true;

        if (m_decodeSurrogatePairs && (!term->characterClass->hasOneCharacterSize() || term->invert()))
            storeToFrame(m_regs.index, term->frameLocation + BackTrackInfoCharacterClass::beginIndex());
        m_jit.move(MacroAssembler::TrustedImm32(0), countRegister);

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        // Forward-only: the shared-lead fast path reads the pair from its first unit.
        if (m_decodeSurrogatePairs && !term->invert() && m_direction == Forward) {
            if (auto sharedLead = term->characterClass->hasSharedLeadSurrogate()) {
                ASSERT(term->isFixedWidthCharacterClass());
                CharacterClass trailsOnlyClass;
                buildTrailsOnlyClass(*term->characterClass, trailsOnlyClass);

                MacroAssembler::JumpList sharedFailures;
                MacroAssembler::Label sharedLoop(&m_jit);

                // Need at least 2 code units remaining: index + 2 <= length.
                m_jit.add32(MacroAssembler::TrustedImm32(2), m_regs.index, scratch);
                sharedFailures.append(m_jit.branch32(MacroAssembler::Above, scratch, m_regs.length));

                readSurrogatePairAndMatchTrailForSharedLead(op.m_checkedOffset - term->inputPosition, character, scratch, m_regs.index, sharedLead.value(), &trailsOnlyClass, sharedFailures);

                m_jit.add32(MacroAssembler::TrustedImm32(2), m_regs.index);
                m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);

                if (term->quantityMaxCount == quantifyInfinite)
                    m_jit.jump(sharedLoop);
                else
                    m_jit.branch32(MacroAssembler::NotEqual, countRegister, MacroAssembler::Imm32(term->quantityMaxCount)).linkTo(sharedLoop, &m_jit);

                sharedFailures.link(&m_jit);
                defineReentryLabel(op);
                storeToFrame(countRegister, term->frameLocation + BackTrackInfoCharacterClass::matchAmountIndex());
                return;
            }
        }
#endif

        MacroAssembler::JumpList failures;
        MacroAssembler::JumpList failuresDecrementIndex;
        MacroAssembler::Label loop(&m_jit);
        failures.append(atEndOfInput());

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_direction == Backward && m_decodeSurrogatePairs) {
            // Backward + unicode: decode the code point ending just left of the
            // frontier (frame offset 0), class-match it (frontier untouched, so a
            // mismatch simply ends the greedy run), then consume it by its width,
            // read off the code point exactly as the forward path does. A pair's
            // lead is the unit below its trail: once the trail unit is taken and the
            // frontier is 0, there is no lead to take, so that path rejoins through
            // failuresDecrementIndex, which gives back the trail unit.
            readUnicodeCharBackward(negativeOffsetIndexedAddress(op.m_checkedOffset - term->inputPosition, character), character, scratch);
            matchCharacterClassTermInner(term, failures, character, scratch);
            m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the code point's last unit
            MacroAssembler::Jump isBMPChar = m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::TrustedImm32(0x10000));
            // The pair's lead is one further unit; mirroring the forward path, test
            // for it BEFORE taking it, so a failure here has consumed exactly one unit
            // (the trail) -- which failuresDecrementIndex gives back.
            failuresDecrementIndex.append(atEndOfInput());
            m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the pair's lead unit
            isBMPChar.link(&m_jit);
        } else
#endif
        {
            readCharacter(op.m_checkedOffset - term->inputPosition, character);

            matchCharacterClassTermInner(term, failures, character, scratch);

            if (m_direction == Backward)
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
            else if (m_decodeSurrogatePairs)
                advanceIndexAfterCharacterClassTermMatch(term, failuresDecrementIndex, character);
#endif
            else
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
        }
        m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);

        if (term->quantityMaxCount == quantifyInfinite)
            m_jit.jump(loop);
        else {
            m_jit.branch32(MacroAssembler::NotEqual, countRegister, MacroAssembler::Imm32(term->quantityMaxCount)).linkTo(loop, &m_jit);
            if (!failuresDecrementIndex.empty()) {
                // Don't emit the superfluous jump to the next instruction if we don't have any failuresDecrementIndex jumps to link.
                failures.append(m_jit.jump());
            }
        }

        if (!failuresDecrementIndex.empty()) {
            failuresDecrementIndex.link(&m_jit);
            // Give back the unit that was already consumed before the failure: the
            // frontier moved up in a forward frame, down in a backward frame.
            if (m_direction == Backward)
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
            else
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
        }

        failures.link(&m_jit);
        defineReentryLabel(op);

        storeToFrame(countRegister, term->frameLocation + BackTrackInfoCharacterClass::matchAmountIndex());
    }
    void backtrackCharacterClassGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID countRegister = m_regs.regT1;

        m_backtrackingState.link(*this, op);

        // Leveraging possessive flag to optimize this backtracking by rewinding entire matching so far.
        // Important thing is possessive in YarrJIT is optional optimization flag: we can leverage this only when we can use it easily.
        // We can completely ignore this flag and still this is OK.
        if (term->possessive() && (!m_decodeSurrogatePairs || term->isFixedWidthCharacterClass())) {
            loadFromFrame(term->frameLocation + BackTrackInfoCharacterClass::matchAmountIndex(), countRegister);
            if (m_decodeSurrogatePairs && term->characterClass->hasNonBMPCharacters())
                m_jit.lshift32(MacroAssembler::TrustedImm32(1), countRegister); // 2 code units per match.
            rewindIndex(countRegister);
            m_backtrackingState.fallthrough();
            return;
        }

        loadFromFrame(term->frameLocation + BackTrackInfoCharacterClass::matchAmountIndex(), countRegister);
        m_backtrackingState.append(m_jit.branchTest32(MacroAssembler::Zero, countRegister));
        m_jit.sub32(MacroAssembler::TrustedImm32(1), countRegister);

        if (m_direction == Backward && !m_decodeSurrogatePairs)
            m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
        else if (m_direction == Backward && term->isFixedWidthCharacterClass())
            m_jit.add32(MacroAssembler::TrustedImm32(term->characterClass->hasNonBMPCharacters() ? 2 : 1), m_regs.index);
        else if (m_direction != Backward && !m_decodeSurrogatePairs)
            m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
        else if (m_direction != Backward && term->isFixedWidthCharacterClass())
            m_jit.sub32(MacroAssembler::TrustedImm32(term->characterClass->hasNonBMPCharacters() ? 2 : 1), m_regs.index);
        else {
            const MacroAssembler::RegisterID character = m_regs.regT0;

            MacroAssembler::Jump backtrackToBegin = m_jit.branchTest32(MacroAssembler::Zero, countRegister);

            if (m_direction == Backward)
                m_jit.load32WithUnalignedHalfWords(negativeOffsetIndexedAddress(op.m_checkedOffset - term->inputPosition + 1, character), character);
            else
                m_jit.load32WithUnalignedHalfWords(negativeOffsetIndexedAddress(op.m_checkedOffset - term->inputPosition + 2, character), character);
            m_jit.and32(surrogateTagMask, character);
            m_jit.compare32(MacroAssembler::Equal, character, surrogatePairTags, character);
            m_jit.add32(MacroAssembler::TrustedImm32(1), character);
            if (m_direction == Backward)
                m_jit.add32(character, m_regs.index);
            else
                m_jit.sub32(character, m_regs.index);
            m_jit.jump(op.m_reentry);

            backtrackToBegin.link(&m_jit);
            loadFromFrame(term->frameLocation + BackTrackInfoCharacterClass::beginIndex(), m_regs.index);
        }
        m_jit.jump(op.m_reentry);
    }

    void generateCharacterClassNonGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID countRegister = m_regs.regT1;

        m_jit.move(MacroAssembler::TrustedImm32(0), countRegister);

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs)
            storeToFrame(m_regs.index, term->frameLocation + BackTrackInfoCharacterClass::beginIndex());
#endif

        defineReentryLabel(op);

        storeToFrame(countRegister, term->frameLocation + BackTrackInfoCharacterClass::matchAmountIndex());
    }

    void backtrackCharacterClassNonGreedy(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID countRegister = m_regs.regT1;
        const MacroAssembler::RegisterID scratch = m_regs.regT2;
        m_usesT2 = true;

        MacroAssembler::JumpList nonGreedyFailures;
        MacroAssembler::JumpList nonGreedyFailuresDecrementIndex;

        m_backtrackingState.link(*this, op);

        loadFromFrame(term->frameLocation + BackTrackInfoCharacterClass::matchAmountIndex(), countRegister);

        nonGreedyFailures.append(atEndOfInput());
        nonGreedyFailures.append(m_jit.branch32(MacroAssembler::Equal, countRegister, MacroAssembler::Imm32(term->quantityMaxCount)));

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_direction == Backward && m_decodeSurrogatePairs) {
            // Backward + unicode: as in the greedy loop, decode the code point
            // ending just left of the frontier, class-match it, then consume it by
            // the width read off the code point; the pair's lead needs a further
            // unit below the frontier or the take is undone via
            // nonGreedyFailuresDecrementIndex.
            readUnicodeCharBackward(negativeOffsetIndexedAddress(op.m_checkedOffset - term->inputPosition, character), character, scratch);
            matchCharacterClassTermInner(term, nonGreedyFailures, character, scratch);
            m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the code point's last unit
            MacroAssembler::Jump isBMPChar = m_jit.branch32(MacroAssembler::LessThan, character, MacroAssembler::TrustedImm32(0x10000));
            // As in the greedy loop: test for the lead before taking it, so the failure
            // path (nonGreedyFailuresDecrementIndex) gives back exactly the trail.
            nonGreedyFailuresDecrementIndex.append(atEndOfInput());
            m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index); // the pair's lead unit
            isBMPChar.link(&m_jit);
        } else
#endif
        {
            readCharacter(op.m_checkedOffset - term->inputPosition, character);

            matchCharacterClassTermInner(term, nonGreedyFailures, character, scratch);

            if (m_direction == Backward)
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
            else if (m_decodeSurrogatePairs)
                advanceIndexAfterCharacterClassTermMatch(term, nonGreedyFailuresDecrementIndex, character);
#endif
            else
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
        }
        m_jit.add32(MacroAssembler::TrustedImm32(1), countRegister);

        m_jit.jump(op.m_reentry);

        if (!nonGreedyFailuresDecrementIndex.empty()) {
            nonGreedyFailuresDecrementIndex.link(&m_jit);
            // Give back the unit consumed before the failure (frontier moved up in
            // a forward frame, down in a backward frame).
            if (m_direction == Backward)
                m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
            else
                m_jit.sub32(MacroAssembler::TrustedImm32(1), m_regs.index);
        }
        nonGreedyFailures.link(&m_jit);

        // Restore the frontier to where this term began. With per-match widths
        // (unicode) the distance is not derivable from the match count, so it is
        // reloaded from beginIndex; otherwise each match was exactly one unit.
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs)
            loadFromFrame(term->frameLocation + BackTrackInfoCharacterClass::beginIndex(), m_regs.index);
        else
#endif
        if (m_direction == Backward)
            m_jit.add32(countRegister, m_regs.index);
        else
            m_jit.sub32(countRegister, m_regs.index);
        m_backtrackingState.fallthrough();
    }

    void generateDotStarEnclosure(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        const MacroAssembler::RegisterID character = m_regs.regT0;
        const MacroAssembler::RegisterID matchPos = m_regs.regT1;
        const MacroAssembler::RegisterID scratch = m_regs.regT2;
        const MacroAssembler::RegisterID initialStart = m_regs.regT3;
        m_usesT2 = true;

        MacroAssembler::JumpList foundBeginningNewLine;
        MacroAssembler::JumpList saveStartIndex;
        MacroAssembler::JumpList foundEndingNewLine;

        if (term->dotAll()) {
            m_jit.move(MacroAssembler::TrustedImm32(0), matchPos);
            setMatchStart(matchPos);
            m_jit.move(m_regs.length, m_regs.index);
            return;
        }

        ASSERT(!m_pattern.m_body->m_hasFixedSize);
        getMatchStart(matchPos);
        loadFromFrame(m_pattern.m_initialStartValueFrameLocation, initialStart);

        saveStartIndex.append(m_jit.branch32(MacroAssembler::BelowOrEqual, matchPos, initialStart));
        MacroAssembler::Label findBOLLoop(&m_jit);
        m_jit.sub32(MacroAssembler::TrustedImm32(1), matchPos);
        if (m_charSize == CharSize::Char8)
            m_jit.load8(MacroAssembler::BaseIndex(m_regs.input, matchPos, MacroAssembler::TimesOne, 0), character);
        else
            m_jit.load16(MacroAssembler::BaseIndex(m_regs.input, matchPos, MacroAssembler::TimesTwo, 0), character);
        matchCharacterClass(character, scratch, foundBeginningNewLine, m_pattern.newlineCharacterClass());

        m_jit.branch32(MacroAssembler::Above, matchPos, initialStart).linkTo(findBOLLoop, &m_jit);
        saveStartIndex.append(m_jit.jump());

        foundBeginningNewLine.link(&m_jit);
        m_jit.add32(MacroAssembler::TrustedImm32(1), matchPos); // Advance past newline
        saveStartIndex.link(&m_jit);

        if (!term->multiline() && term->anchors.bolAnchor)
            op.m_jumps.append(m_jit.branchTest32(MacroAssembler::NonZero, matchPos));

        ASSERT(!m_pattern.m_body->m_hasFixedSize);
        setMatchStart(matchPos);

        m_jit.move(m_regs.index, matchPos);

        MacroAssembler::Label findEOLLoop(&m_jit);
        foundEndingNewLine.append(m_jit.branch32(MacroAssembler::Equal, matchPos, m_regs.length));
        if (m_charSize == CharSize::Char8)
            m_jit.load8(MacroAssembler::BaseIndex(m_regs.input, matchPos, MacroAssembler::TimesOne, 0), character);
        else
            m_jit.load16(MacroAssembler::BaseIndex(m_regs.input, matchPos, MacroAssembler::TimesTwo, 0), character);
        matchCharacterClass(character, scratch, foundEndingNewLine, m_pattern.newlineCharacterClass());
        m_jit.add32(MacroAssembler::TrustedImm32(1), matchPos);
        m_jit.jump(findEOLLoop);

        foundEndingNewLine.link(&m_jit);

        if (!term->multiline() && term->anchors.eolAnchor)
            op.m_jumps.append(m_jit.branch32(MacroAssembler::NotEqual, matchPos, m_regs.length));

        m_jit.move(matchPos, m_regs.index);
    }

    void backtrackDotStarEnclosure(size_t opIndex)
    {
        backtrackTermDefault(opIndex);
    }
    
    // Code generation/backtracking for simple terms
    // (pattern characters, character classes, and assertions).
    // These methods farm out work to the set of functions above.
    void generateTerm(size_t opIndex, MatchTargets& matchTargets)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
        // firstCharacterAdditionalReadSize means "the character at the match START
        // was a non-BMP pair, so advancing the start by one code point takes an
        // extra unit". Only the term whose first read is at the match start may
        // establish that (see YarrOp::m_readsStartCharacter); a non-BMP read
        // anywhere else says nothing about the start character.
        SetForScope firstCharacterOnly(m_useFirstNonBMPCharacterOptimization, m_canUseFirstNonBMPCharacterOptimization && op.m_readsStartCharacter);
#endif

        switch (term->type) {
        case PatternTerm::Type::PatternCharacter:
            switch (term->quantityType) {
            case QuantifierType::FixedCount:
                if (term->quantityMaxCount == 1)
                    generatePatternCharacterOnce(opIndex, matchTargets);
                else
                    generatePatternCharacterFixed(opIndex);
                break;
            case QuantifierType::Greedy:
                generatePatternCharacterGreedy(opIndex);
                break;
            case QuantifierType::NonGreedy:
                generatePatternCharacterNonGreedy(opIndex);
                break;
            }
            break;

        case PatternTerm::Type::CharacterClass:
            switch (term->quantityType) {
            case QuantifierType::FixedCount:
                if (term->quantityMaxCount == 1)
                    generateCharacterClassOnce(opIndex);
                else
                    generateCharacterClassFixed(opIndex);
                break;
            case QuantifierType::Greedy:
                generateCharacterClassGreedy(opIndex);
                break;
            case QuantifierType::NonGreedy:
                generateCharacterClassNonGreedy(opIndex);
                break;
            }
            break;

        case PatternTerm::Type::AssertionBOL:
            generateAssertionBOL(opIndex);
            break;

        case PatternTerm::Type::AssertionEOL:
            generateAssertionEOL(opIndex);
            break;

        case PatternTerm::Type::AssertionWordBoundary:
            generateAssertionWordBoundary(opIndex);
            break;

        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
            break;

        case PatternTerm::Type::ParenthesesSubpattern:
        case PatternTerm::Type::ParentheticalAssertion:
            RELEASE_ASSERT_NOT_REACHED();

        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
#if ENABLE(YARR_JIT_BACKREFERENCES)
            generateBackReference(term->type == PatternTerm::Type::NamedBackReference, opIndex);
#else
            m_failureReason = JITFailureReason::BackReference;
#endif
            break;
        case PatternTerm::Type::DotStarEnclosure:
            generateDotStarEnclosure(opIndex);
            break;
        }
    }
    void backtrackTerm(size_t opIndex)
    {
        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;

        switch (term->type) {
        case PatternTerm::Type::PatternCharacter:
            switch (term->quantityType) {
            case QuantifierType::FixedCount:
                if (term->quantityMaxCount == 1)
                    backtrackPatternCharacterOnce(opIndex);
                else
                    backtrackPatternCharacterFixed(opIndex);
                break;
            case QuantifierType::Greedy:
                backtrackPatternCharacterGreedy(opIndex);
                break;
            case QuantifierType::NonGreedy:
                backtrackPatternCharacterNonGreedy(opIndex);
                break;
            }
            break;

        case PatternTerm::Type::CharacterClass:
            switch (term->quantityType) {
            case QuantifierType::FixedCount:
                if (term->quantityMaxCount == 1) {
                    bool fallThroughToCharacterClassFixedCount = false;
                    if (opIndex != 0) {
                        auto& previousOp = m_ops[opIndex - 1];
                        if (previousOp.m_op == YarrOpCode::Term) {
                            auto* term = previousOp.m_term;
                            if (term->type == PatternTerm::Type::CharacterClass && term->quantityType == QuantifierType::FixedCount)
                                fallThroughToCharacterClassFixedCount = true;
                        }
                    }
                    backtrackCharacterClassOnce(opIndex, fallThroughToCharacterClassFixedCount);
                } else
                    backtrackCharacterClassFixed(opIndex);
                break;
            case QuantifierType::Greedy:
                backtrackCharacterClassGreedy(opIndex);
                break;
            case QuantifierType::NonGreedy:
                backtrackCharacterClassNonGreedy(opIndex);
                break;
            }
            break;

        case PatternTerm::Type::AssertionBOL:
            backtrackAssertionBOL(opIndex);
            break;

        case PatternTerm::Type::AssertionEOL:
            backtrackAssertionEOL(opIndex);
            break;

        case PatternTerm::Type::AssertionWordBoundary:
            backtrackAssertionWordBoundary(opIndex);
            break;

        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
            break;

        case PatternTerm::Type::ParenthesesSubpattern:
        case PatternTerm::Type::ParentheticalAssertion:
            RELEASE_ASSERT_NOT_REACHED();

        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
#if ENABLE(YARR_JIT_BACKREFERENCES)
            backtrackBackReference(opIndex);
#else
            m_failureReason = JITFailureReason::BackReference;
#endif
            break;

        case PatternTerm::Type::DotStarEnclosure:
            backtrackDotStarEnclosure(opIndex);
            break;
        }
    }

    void generate()
    {
        // Forwards generate the matching code.
        ASSERT(m_ops.size());
        size_t opIndex = 0;
        Vector<MatchTargets, 8> termMatchTargets;

        termMatchTargets.append(MatchTargets());

        do {
            if (m_disassembler)
                m_disassembler->setForGenerate(opIndex, m_jit.label());

            YarrOp& op = m_ops[opIndex];
            m_direction = op.m_direction;
            switch (op.m_op) {

            case YarrOpCode::Term:
                generateTerm(opIndex, termMatchTargets.last());
                break;

            // YarrOpCode::BodyAlternativeBegin/Next/End
            //
            // These nodes wrap the set of alternatives in the body of the regular expression.
            // There may be either one or two chains of OpBodyAlternative nodes, one representing
            // the 'once through' sequence of alternatives (if any exist), and one representing
            // the repeating alternatives (again, if any exist).
            //
            // Upon normal entry to the Begin alternative, we will check that input is available.
            // Reentry to the Begin alternative will take place after the check has taken place,
            // and will assume that the input position has already been progressed as appropriate.
            //
            // Entry to subsequent Next/End alternatives occurs when the prior alternative has
            // successfully completed a match - return a success state from JIT code.
            //
            // Next alternatives allow for reentry optimized to suit backtracking from its
            // preceding alternative. It expects the input position to still be set to a position
            // appropriate to its predecessor, and it will only perform an input check if the
            // predecessor had a minimum size less than its own.
            //
            // In the case 'once through' expressions, the End node will also have a reentry
            // point to jump to when the last alternative fails. Again, this expects the input
            // position to still reflect that expected by the prior alternative.
            case YarrOpCode::BodyAlternativeBegin: {
                PatternAlternative* alternative = op.m_alternative;

                termMatchTargets.append(MatchTargets());

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                // Initialize before input check to prevent uninitialized data in arithmetic.
                if (m_canUseFirstNonBMPCharacterOptimization)
                    m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.firstCharacterAdditionalReadSize);
#endif

                // Upon entry at the head of the set of alternatives, check if input is available
                // to run the first alternative. (This progresses the input position).
                op.m_jumps.append(jumpIfNoAvailableInput(alternative->m_minimumSize));

                // We will reenter after the check, and assume the input position to have been
                // set as appropriate to this alternative.
                defineReentryLabel(op);

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                // Initialize after reentry to ensure fresh register state on backtrack retries,
                // since character reading operations modify this register during execution.
                if (m_canUseFirstNonBMPCharacterOptimization)
                    m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.firstCharacterAdditionalReadSize);
#endif

#if CPU(ARM64) || CPU(X86_64)
                // Try multi-pattern SIMD search first if available (more selective for alternation patterns)
                if (op.m_maskedAltInfo) {
                    MacroAssembler::JumpList matched;
                    dataLogLnIf(Options::verboseRegExpCompilation(), "Using multi-pattern SIMD search");
                    auto simdResult = generateMultiPatternSIMDSearch(*op.m_maskedAltInfo, op.m_checkedOffset, matched);

                    // If SIMD search was generated, use backtrack entry as reentry point.
                    // The backtrack entry re-computes the SIMD threshold which gets clobbered
                    // by the scalar loop's use of regT1 as a scratch register.
                    if (simdResult) {
                        m_usesSIMD = true;
                        op.m_reentry = simdResult->backtrackTarget;

                        // Scalar loop fell through (out of input). Do not enter the
                        // body; index would be past length and the body would OOB.
                        if (!m_pattern.m_body->m_hasFixedSize) {
                            if (alternative->m_minimumSize) {
                                m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                                setMatchStart(m_regs.regT0);
                            } else
                                setMatchStart(m_regs.index);
                        }
                        op.m_jumps.append(m_jit.jump());

                        matched.link(&m_jit);
                        if (!m_pattern.m_body->m_hasFixedSize) {
                            if (alternative->m_minimumSize) {
                                m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                                setMatchStart(m_regs.regT0);
                            } else
                                setMatchStart(m_regs.index);
                        }
                        break;
                    }

                    ASSERT(matched.empty());
                    break;
                }
#endif

                // Emit fast skip path with stride if we have BoyerMooreInfo.
                if (op.m_bmInfo) {
                    auto range = op.m_bmInfo->findWorthwhileCharacterSequenceForLookahead(m_sampler);
                    if (range) {
                        auto [beginIndex, endIndex] = *range;
                        ASSERT(endIndex <= alternative->m_minimumSize);

                        auto [map, charactersFastPath] = op.m_bmInfo->createCandidateBitmap(beginIndex, endIndex);
                        unsigned mapCount = map.count();
                        // If candiate characters are <= 2, checking each is better than using vector.
                        MacroAssembler::JumpList matched;
                        dataLogLnIf(YarrJITInternal::verbose, "BM Bitmap is ", map);
                        // Patterns like /[]/ have zero candidates. Since it is rare, we do not do nothing for now.
                        if (!mapCount)
                            break;

                        unsigned strideLength = endIndex - beginIndex;
#if CPU(ARM64) || CPU(X86_64)
                        // Try SIMD nibble table optimization first for any number of candidates.
                        // This includes the scalar loop internally for tail processing,
                        // following the same pattern as generateMultiPatternSIMDSearch.
                        auto simdResult = generateBitInTableSIMDSearch(map, charactersFastPath, strideLength, endIndex, op.m_checkedOffset, matched);
                        if (simdResult) {
                            m_usesSIMD = true;
                            op.m_reentry = simdResult->backtrackTarget;
                            dataLogLnIf(Options::verboseRegExpCompilation(), "Using SIMD nibble table lookahead count:(", mapCount, "),range:[", beginIndex, ", ", endIndex, ")");

                            // Handle failure path - out of characters
                            // After scalar loop exhausts, we fall through here
                            if (!m_pattern.m_body->m_hasFixedSize) {
                                if (alternative->m_minimumSize) {
                                    m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                                    setMatchStart(m_regs.regT0);
                                } else
                                    setMatchStart(m_regs.index);
                                op.m_jumps.append(m_jit.jump());
                            } else
                                op.m_jumps.append(m_jit.jump());

                            // Handle match path
                            matched.link(&m_jit);
                            if (!m_pattern.m_body->m_hasFixedSize) {
                                if (alternative->m_minimumSize) {
                                    m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                                    setMatchStart(m_regs.regT0);
                                } else
                                    setMatchStart(m_regs.index);
                            }
                            break;
                        }
#endif

                        // SIMD not available or returned nullopt - use scalar-only paths
                        generateBoyerMooreScalarLoop(map, charactersFastPath, strideLength, endIndex, op.m_checkedOffset, matched);

                        // Fallthrough if out-of-length failure happens.

                        // If the pattern size is not fixed, then store the start index for use if we match.
                        // This is used for adjusting match-start when we failed to find the start with BoyerMoore search.
                        if (!m_pattern.m_body->m_hasFixedSize) {
                            if (alternative->m_minimumSize) {
                                m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                                setMatchStart(m_regs.regT0);
                            } else
                                setMatchStart(m_regs.index);
                        }
                        op.m_jumps.append(m_jit.jump());

                        matched.link(&m_jit);
                        // If the pattern size is not fixed, then store the start index for use if we match.
                        // This is used for adjusting match-start when we start pattern matching with the updated index
                        // by BoyerMoore search.
                        if (!m_pattern.m_body->m_hasFixedSize) {
                            if (alternative->m_minimumSize) {
                                m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                                setMatchStart(m_regs.regT0);
                            } else
                                setMatchStart(m_regs.index);
                        }
                    } else
                        dataLogLnIf(Options::verboseRegExpCompilation(), "BM search candidates were not efficient enough. Not using BM search");
                    break;
                }

                if (op.m_bodyAltSharedLead) {
                    // Shared-lead per-position prefilter for /u patterns whose alternatives all
                    // start with non-BMP character classes sharing the same UTF-16 lead surrogate.
                    // We scan input one UTF-16 unit at a time, falling through to per-alt dispatch
                    // only when the lead matches. Mismatch advances index by 1 and retries.
                    ASSERT(m_decodeSurrogatePairs);
                    char16_t sharedLeadSurrogate = op.m_bodyAltSharedLead.value();

                    // We are intentionally not using tryReadUnicodeChar here as we decomposed a non-BMP character into lead and trail surrogates
                    // and searching for a shared lead surrogate.
                    JIT_COMMENT(m_jit, "Shared-lead-surrogate body-alt filter");
                    auto loopHead = m_jit.label();
                    MacroAssembler::BaseIndex address = negativeOffsetIndexedAddress(op.m_checkedOffset, m_regs.regT0);
                    m_jit.load16Unaligned(address, m_regs.regT0);
                    auto matched = m_jit.branch32(MacroAssembler::Equal, m_regs.regT0, MacroAssembler::TrustedImm32(sharedLeadSurrogate));
                    jumpIfAvailableInput(1).linkTo(loopHead, &m_jit);

                    // Out of input: hand off to alt failure path.
                    if (!m_pattern.m_body->m_hasFixedSize) {
                        if (alternative->m_minimumSize) {
                            m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                            setMatchStart(m_regs.regT0);
                        } else
                            setMatchStart(m_regs.index);
                    }
                    op.m_jumps.append(m_jit.jump());

                    matched.link(&m_jit);
                    if (!m_pattern.m_body->m_hasFixedSize) {
                        if (alternative->m_minimumSize) {
                            m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize), m_regs.regT0);
                            setMatchStart(m_regs.regT0);
                        } else
                            setMatchStart(m_regs.index);
                    }
                }
                break;
            }
            case YarrOpCode::BodyAlternativeNext:
            case YarrOpCode::BodyAlternativeEnd: {
                PatternAlternative* priorAlternative = m_ops[op.m_previousOp].m_alternative;
                PatternAlternative* alternative = op.m_alternative;

                if (op.m_op == YarrOpCode::BodyAlternativeEnd)
                    termMatchTargets.takeLast();

                // If we get here, the prior alternative matched - return success.

                // Load appropriate values into the return register and the first output
                // slot, and return. In the case of pattern with a fixed size, we will
                // not have yet set the value in the first 
                ASSERT(m_regs.index != m_regs.returnRegister);
                ASSERT(m_regs.output != m_regs.returnRegister);
                if (m_pattern.m_body->m_hasFixedSize) {
                    if (priorAlternative->m_minimumSize)
                        m_jit.sub32(m_regs.index, MacroAssembler::Imm32(priorAlternative->m_minimumSize), m_regs.returnRegister);
                    else
                        m_jit.move(m_regs.index, m_regs.returnRegister);
                    if (shouldRecordSubpatterns())
                        m_jit.storePair32(m_regs.returnRegister, m_regs.index, subpatternStartAddress(0));
                } else {
                    getMatchStart(m_regs.returnRegister);
                    if (shouldRecordSubpatterns())
                        m_jit.store32(m_regs.index, subpatternEndAddress(0));
                }
                m_jit.move(m_regs.index, m_regs.returnRegister2);
                generateReturn();

                // This is the divide between the tail of the prior alternative, above, and
                // the head of the subsequent alternative, below.

                if (op.m_op == YarrOpCode::BodyAlternativeNext) {
                    // This is the reentry point for the Next alternative. We expect any code
                    // that jumps here to do so with the input position matching that of the
                    // PRIOR alteranative, and we will only check input availability if we
                    // need to progress it forwards.
                    defineReentryLabel(op);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                    // Reset on reentry: a prior alternative may have read a non-BMP codepoint
                    // and left this register set to 1. The next alternative starts a fresh
                    // first-character read.
                    if (m_canUseFirstNonBMPCharacterOptimization)
                        m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.firstCharacterAdditionalReadSize);
#endif
                    if (shouldRecordSubpatterns() && priorAlternative->needToCleanupCaptures()) {
                        for (unsigned subpattern = priorAlternative->firstCleanupSubpatternId(); subpattern <= priorAlternative->m_lastSubpatternId; subpattern++)
                            clearSubpattern(subpattern);
                    }
                    if (alternative->m_minimumSize > priorAlternative->m_minimumSize) {
                        m_jit.add32(MacroAssembler::Imm32(alternative->m_minimumSize - priorAlternative->m_minimumSize), m_regs.index);
                        op.m_jumps.append(jumpIfNoAvailableInput());
                    } else if (priorAlternative->m_minimumSize > alternative->m_minimumSize)
                        m_jit.sub32(MacroAssembler::Imm32(priorAlternative->m_minimumSize - alternative->m_minimumSize), m_regs.index);
                    // The claim is now this alternative's; a dispatched entry lands here.
                    if (op.m_bodyDispatch)
                        bindBodyDispatchEntry(*op.m_bodyDispatch, op.m_bodyDispatchAlternativeIndex);
                } else if (op.m_nextOp == notFound) {
                    // This is the reentry point for the End of 'once through' alternatives,
                    // jumped to when the last alternative fails to match.
                    defineReentryLabel(op);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                    // Reset on reentry: see BodyAlternativeNext above.
                    if (m_canUseFirstNonBMPCharacterOptimization)
                        m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.firstCharacterAdditionalReadSize);
#endif
                    m_jit.sub32(MacroAssembler::Imm32(priorAlternative->m_minimumSize), m_regs.index);
                }
                break;
            }

            // YarrOpCode::SimpleNestedAlternativeBegin/Next/End
            // YarrOpCode::StringListAlternativeBegin/Next/End
            // YarrOpCode::NestedAlternativeBegin/Next/End
            //
            // These nodes are used to handle sets of alternatives that are nested within
            // subpatterns and parenthetical assertions. The 'simple' forms are used where
            // we do not need to be able to backtrack back into any alternative other than
            // the last, the normal forms allow backtracking into any alternative.
            //
            // Each Begin/Next node is responsible for planting an input check to ensure
            // sufficient input is available on entry. Next nodes additionally need to
            // jump to the end - Next nodes use the End node's m_jumps list to hold this
            // set of jumps.
            //
            // In the non-simple forms, successful alternative matches must store a
            // 'return address' using a DataLabelPtr, used to store the address to jump
            // to when backtracking, to get to the code for the appropriate alternative.
            case YarrOpCode::SimpleNestedAlternativeBegin:
            case YarrOpCode::StringListAlternativeBegin:
            case YarrOpCode::NestedAlternativeBegin: {
                PatternTerm* term = op.m_term;
                PatternAlternative* alternative = op.m_alternative;
                PatternDisjunction* disjunction = term->parentheses.disjunction;

                if (op.m_op == YarrOpCode::StringListAlternativeBegin) {
                    YarrOp* endOp = &m_ops[alternativeEndOpIndex(opIndex)];

                    termMatchTargets.last() = alternative->m_isLastAlternative ? MatchTargets(MatchTargets::PreferredTarget::MatchSuccessFallThrough) : MatchTargets(endOp->m_jumps, op.m_jumps, MatchTargets::PreferredTarget::MatchFailFallThrough);
                }

                // First-character dispatch: read the character once and route to
                // the applicable chain, then fall into alternative 0's entry (which
                // is now a labelled chain target rather than a fallthrough).
                if (op.m_dispatch) {
                    generateDispatch(op);
                    bindDispatchEntry(*op.m_dispatch, 0);
                }

                // Calculate how much input we need to check for, and if non-zero check.
                op.m_checkAdjust = Checked<unsigned>(alternative->m_minimumSize);
                if ((term->quantityType == QuantifierType::FixedCount) && (term->quantityMaxCount == 1) && (term->type != PatternTerm::Type::ParentheticalAssertion))
                    op.m_checkAdjust -= disjunction->m_minimumSize;
                if (op.m_checkAdjust)
                    op.m_jumps.append(jumpIfNoAvailableInput(op.m_checkAdjust));
                break;
            }
            case YarrOpCode::SimpleNestedAlternativeNext:
            case YarrOpCode::StringListAlternativeNext:
            case YarrOpCode::NestedAlternativeNext: {
                PatternTerm* term = op.m_term;
                PatternAlternative* alternative = op.m_alternative;
                PatternDisjunction* disjunction = term->parentheses.disjunction;

                YarrOp* endOp = &m_ops[alternativeEndOpIndex(opIndex)];

                if (op.m_op == YarrOpCode::StringListAlternativeNext)
                    termMatchTargets.last() = alternative->m_isLastAlternative ? MatchTargets(MatchTargets::PreferredTarget::MatchSuccessFallThrough) : MatchTargets(endOp->m_jumps, op.m_jumps, MatchTargets::PreferredTarget::MatchFailFallThrough);

                // In the non-simple case, store a 'return address' so we can backtrack correctly.
                if (op.m_op == YarrOpCode::NestedAlternativeNext) {
                    unsigned parenthesesFrameLocation = term->frameLocation;
                    op.m_returnAddress = storeToFrameWithPatch(parenthesesFrameLocation + BackTrackInfoParentheses::returnAddressIndex());
                }

                if (term->quantityType != QuantifierType::FixedCount && !term->quantityMinCount && !m_ops[op.m_previousOp].m_alternative->m_minimumSize) {
                    // If the previous alternative matched without consuming characters then
                    // backtrack to try to match while consumming some input.
                    // For VariableMin (min > 0) we skip this check; the abort-to-interpreter
                    // branch at ParenthesesSubpatternEnd handles forced empty iterations.
                    op.m_zeroLengthMatch = m_jit.branch32(MacroAssembler::Equal, m_regs.index, frameAddress().withOffset(term->frameLocation * sizeof(void*)));
                }

                if (op.m_op != YarrOpCode::StringListAlternativeNext) {
                    // If we reach here then the last alternative has matched - jump to the
                    // End node, to skip over any further alternatives.
                    //
                    // FIXME: this is logically O(N^2) (though N can be expected to be very
                    // small). We could avoid this either by adding an extra jump to the JIT
                    // data structures, or by making backtracking code that jumps to Next
                    // alternatives are responsible for checking that input is available (if
                    // we didn't need to plant the input checks, then m_jumps would be free).
                    endOp->m_jumps.append(m_jit.jump());
                }

                // This is the entry point for the next alternative.
                defineReentryLabel(op);
                if (op.m_dispatch)
                    bindDispatchEntry(*op.m_dispatch, op.m_dispatchAlternativeIndex);

                // Calculate how much input we need to check for, and if non-zero check.
                op.m_checkAdjust = alternative->m_minimumSize;
                if ((term->quantityType == QuantifierType::FixedCount) && (term->quantityMaxCount == 1) && (term->type != PatternTerm::Type::ParentheticalAssertion))
                    op.m_checkAdjust -= disjunction->m_minimumSize;
                if (op.m_op == YarrOpCode::StringListAlternativeNext) {
                    YarrOp* prevOp = &m_ops[op.m_previousOp];

                    prevOp->m_jumps.link(&m_jit);
                    prevOp->m_jumps.clear();
                    op.m_jumps.link(&m_jit);
                    op.m_jumps.clear();
                    auto lastCheckAdjust = prevOp->m_checkAdjust;
                    if (lastCheckAdjust > op.m_checkAdjust)
                        m_jit.sub32(MacroAssembler::Imm32(lastCheckAdjust - op.m_checkAdjust), m_regs.index);
                    else if (op.m_checkAdjust > lastCheckAdjust)
                        m_jit.add32(MacroAssembler::Imm32(op.m_checkAdjust - lastCheckAdjust), m_regs.index);
                    op.m_jumps.append(jumpIfNoAvailableInput());
                } else if (op.m_checkAdjust)
                    op.m_jumps.append(jumpIfNoAvailableInput(op.m_checkAdjust));
                break;
            }
            case YarrOpCode::SimpleNestedAlternativeEnd:
            case YarrOpCode::StringListAlternativeEnd:
            case YarrOpCode::NestedAlternativeEnd: {
                PatternTerm* term = op.m_term;

                // In the non-simple case, store a 'return address' so we can backtrack correctly.
                if (op.m_op == YarrOpCode::NestedAlternativeEnd) {
                    unsigned parenthesesFrameLocation = term->frameLocation;
                    op.m_returnAddress = storeToFrameWithPatch(parenthesesFrameLocation + BackTrackInfoParentheses::returnAddressIndex());
                }

                // Zero-length match check: only for non-FixedCount with min == 0.
                // FixedCount has its own backtracking handling. For VariableMin
                // (Greedy/NonGreedy with min > 0) we want forced empty iterations
                // to satisfy quantityMinCount; that's handled by the abort-to-interpreter
                // branch in ParenthesesSubpatternEnd's forward emission.
                if (term->quantityType != QuantifierType::FixedCount && !term->quantityMinCount && !m_ops[op.m_previousOp].m_alternative->m_minimumSize) {
                    // If the previous alternative matched without consuming characters then
                    // backtrack to try to match while consumming some input.
                    op.m_zeroLengthMatch = m_jit.branch32(MacroAssembler::Equal, m_regs.index, frameAddress().withOffset(term->frameLocation * sizeof(void*)));
                }

                // If this set of alternatives contains more than one alternative,
                // then the Next nodes will have planted jumps to the End, and added
                // them to this node's m_jumps list.
                op.m_jumps.link(&m_jit);
                op.m_jumps.clear();
                break;
            }

            // YarrOpCode::ParenthesesSubpatternOnceBegin/End
            //
            // These nodes support (optionally) capturing subpatterns, that have a
            // quantity count of 1 (this covers fixed once, and ?/?? quantifiers).
            case YarrOpCode::ParenthesesSubpatternOnceBegin: {
                PatternTerm* term = op.m_term;

                termMatchTargets.append(MatchTargets());

                unsigned parenthesesFrameLocation = term->frameLocation;
                ASSERT(term->quantityMaxCount == 1);

                // Upon entry to a Greedy quantified set of parenthese store the index.
                // We'll use this for two purposes:
                //  - To indicate which iteration we are on of matching the remainder of
                //    the expression after the parentheses - the first, including the
                //    match within the parentheses, or the second having skipped over them.
                //  - To check for empty matches, which must be rejected.
                //
                // At the head of a NonGreedy set of parentheses we'll immediately set the
                // value on the stack to -1 (indicating a match skipping the subpattern),
                // and plant a jump to the end. We'll also plant a label to backtrack to
                // to reenter the subpattern later, with a store to set up index on the
                // second iteration.
                //
                // FIXME: for capturing parens, could use the index in the capture array?
                if (term->quantityType == QuantifierType::Greedy)
                    storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoParenthesesOnce::beginIndex());
                else if (term->quantityType == QuantifierType::NonGreedy) {
                    storeToFrame(MacroAssembler::TrustedImm32(-1), parenthesesFrameLocation + BackTrackInfoParenthesesOnce::beginIndex());
                    op.m_jumps.append(m_jit.jump());
                    defineReentryLabel(op);
                    storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoParenthesesOnce::beginIndex());
                }

                // If the parenthese are capturing, store the starting index value to the captures array.
                if (term->capture() && shouldRecordSubpatterns()) {
                    unsigned extraOffset = term->quantityType == QuantifierType::FixedCount ? term->parentheses.disjunction->m_minimumSize : 0;
                    emitCaptureStart(term, op.m_checkedOffset, m_regs.regT0, extraOffset);
                }
                break;
            }
            case YarrOpCode::ParenthesesSubpatternOnceEnd: {
                PatternTerm* term = op.m_term;
                ASSERT(term->quantityMaxCount == 1);

                termMatchTargets.takeLast();

                // If the nested alternative matched without consuming any characters, punt this back to the interpreter.
                // FIXME: <https://bugs.webkit.org/show_bug.cgi?id=200786> Add ability for the YARR JIT to properly
                // handle nested expressions that can match without consuming characters
                if (term->quantityType != QuantifierType::FixedCount && !term->parentheses.disjunction->m_minimumSize)
                    m_abortExecution.append(m_jit.branch32(MacroAssembler::Equal, m_regs.index, frameAddress().withOffset(term->frameLocation * sizeof(void*))));

                // If the parenthese are capturing, store the ending index value to the captures array.
                if (term->capture() && shouldRecordSubpatterns())
                    emitCaptureEnd(term, op.m_checkedOffset, m_regs.regT0);

                // If the parentheses are quantified Greedy then add a label to jump back
                // to if we get a failed match from after the parentheses. For NonGreedy
                // parentheses, link the jump from before the subpattern to here.
                if (term->quantityType == QuantifierType::Greedy)
                    defineReentryLabel(op);
                else if (term->quantityType == QuantifierType::NonGreedy) {
                    YarrOp& beginOp = m_ops[op.m_previousOp];
                    beginOp.m_jumps.link(&m_jit);
                }
                break;
            }

            // YarrOpCode::ParenthesesSubpatternTerminalBegin/End
            case YarrOpCode::ParenthesesSubpatternTerminalBegin: {
                PatternTerm* term = op.m_term;
                ASSERT(!term->capture());
                if (term->quantityType == QuantifierType::Greedy)
                    ASSERT(term->quantityMaxCount == quantifyInfinite);
                if (term->quantityType == QuantifierType::FixedCount)
                    ASSERT(term->quantityMaxCount == 1);

                termMatchTargets.append(MatchTargets());

                // Upon entry set a label to loop back to.
                defineReentryLabel(op);

                // Store the start index of the current match; we need to reject zero
                // length matches.
                storeToFrame(m_regs.index, term->frameLocation + BackTrackInfoParenthesesTerminal::beginIndex());
                break;
            }
            case YarrOpCode::ParenthesesSubpatternTerminalEnd: {
                YarrOp& beginOp = m_ops[op.m_previousOp];
                PatternTerm* term = op.m_term;

                termMatchTargets.takeLast();

                // If the nested alternative matched without consuming any characters, punt this back to the interpreter.
                // FIXME: <https://bugs.webkit.org/show_bug.cgi?id=200786> Add ability for the YARR JIT to properly
                // handle nested expressions that can match without consuming characters
                if (term->quantityType != QuantifierType::FixedCount && !term->parentheses.disjunction->m_minimumSize)
                    m_abortExecution.append(m_jit.branch32(MacroAssembler::Equal, m_regs.index, frameAddress().withOffset(term->frameLocation * sizeof(void*))));

                // We know that the match is non-zero, we can accept it and
                // loop back up to the head of the subpattern.
                m_jit.jump(beginOp.m_reentry);

                // This is the entry point to jump to when we stop matching - we will
                // do so once the subpattern cannot match any more.
                defineReentryLabel(op);
                break;
            }

            // YarrOpCode::ParenthesesSubpatternFixedCountBegin/End
            //
            // Used to wrap FixedCount parentheses (capturing or non-capturing) whose
            // content has a single alternative and contains nothing backtrackable
            // (no backreferences, no variable quantifiers, no multi-alt). Examples:
            //   (?:abc){3,3}, (?:x){5,5}, ([0-9a-fA-F]){12}, ([0-9]{4}\.){2}
            //
            // The semantics are: match exactly N times. Any failure = total failure.
            // For capturing parens, the body cannot observe intermediate capture
            // values (the safety check above rejects any inner backref), so we only
            // need to set the capture to the last iteration's positions; per-iteration
            // bookkeeping in a ParenContext is not required.
            //
            // Note: We reuse BackTrackInfoParentheses offsets for frame layout
            // compatibility with the interpreter fallback path.
            case YarrOpCode::ParenthesesSubpatternFixedCountBegin: {
                termMatchTargets.append(MatchTargets());

                PatternTerm* term = op.m_term;
                unsigned parenthesesFrameLocation = term->frameLocation;

                // Save the initial index so we can restore it on backtrack.
                // The beginIndex slot is reused per-iteration for empty match detection,
                // so we use returnAddressIndex (unused in this single-alt, non-ParenContext path).
                storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoParentheses::returnAddressIndex());

                // Initialize the match count to 0.
                storeToFrame(MacroAssembler::TrustedImm32(0), parenthesesFrameLocation + BackTrackInfoParentheses::matchAmountIndex());

                // Set the reentry label for looping.
                defineReentryLabel(op);

                // Clear nested captures at the start of each iteration. ECMAScript
                // requires capture groups to be reset to undefined at the start of
                // each iteration of a quantified group. For a capturing outer, we
                // skip its own id here because BEGIN's setSubpatternStart and END's
                // setSubpatternEnd unconditionally overwrite both halves.
                unsigned firstClear = term->parentheses.subpatternId + (term->capture() ? 1 : 0);
                if (shouldRecordSubpatterns() && firstClear <= term->parentheses.lastSubpatternId) {
                    for (unsigned subpattern = firstClear; subpattern <= term->parentheses.lastSubpatternId; subpattern++)
                        clearSubpattern(subpattern);
                }

                // For capturing parens, record this iteration's starting index.
                if (term->capture() && shouldRecordSubpatterns())
                    emitCaptureStart(term, op.m_checkedOffset, m_regs.regT0);

                // Store the current index for empty match detection.
                storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex());
                break;
            }
            case YarrOpCode::ParenthesesSubpatternFixedCountEnd: {
                YarrOp& beginOp = m_ops[op.m_previousOp];
                PatternTerm* term = op.m_term;
                unsigned parenthesesFrameLocation = term->frameLocation;

                termMatchTargets.takeLast();

                // If the nested alternative matched without consuming any characters, punt to interpreter.
                // FIXME: <https://bugs.webkit.org/show_bug.cgi?id=200786>
                if (!term->parentheses.disjunction->m_minimumSize)
                    m_abortExecution.append(m_jit.branch32(MacroAssembler::Equal, m_regs.index, frameAddress().withOffset(parenthesesFrameLocation * sizeof(void*))));

                // For capturing parens, record this iteration's ending index. After
                // the loop exits with success the surviving values are exactly the
                // last iteration's positions, which is what ECMA requires.
                if (term->capture() && shouldRecordSubpatterns())
                    emitCaptureEnd(term, op.m_checkedOffset, m_regs.regT0);

                // Increment the match count.
                const MacroAssembler::RegisterID countTemporary = m_regs.regT0;
                loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheses::matchAmountIndex(), countTemporary);
                m_jit.add32(MacroAssembler::TrustedImm32(1), countTemporary);
                storeToFrame(countTemporary, parenthesesFrameLocation + BackTrackInfoParentheses::matchAmountIndex());

                // If we haven't matched enough times yet, loop back.
                m_jit.branch32(MacroAssembler::Below, countTemporary, MacroAssembler::Imm32(term->quantityMaxCount)).linkTo(beginOp.m_reentry, &m_jit);

                // We've matched the required number of times, continue to next opcode.
                // Set the reentry point for backtracking to propagate failure upward.
                defineReentryLabel(op);
                break;
            }

            // YarrOpCode::ParenthesesSubpatternBegin/End
            //
            // These nodes handle capturing subpatterns and non-capturing subpatterns
            // that need ParenContext for inter-iteration state management. All three
            // quantifier types (FixedCount, Greedy, NonGreedy) share one model:
            //
            //   - matchAmount counts the ParenContexts currently on the stack, exactly
            //     like the interpreter (++ on push, -- on pop; see
            //     append/popParenthesesDisjunctionContext in YarrInterpreter.cpp).
            //   - BEGIN.forward pushes a ParenContext snapshotting the pre-iteration
            //     state (save-at-BEGIN), increments matchAmount, and clears nested
            //     captures for the new iteration.
            //   - END.forward decides whether to loop back for another iteration. It
            //     does NOT touch matchAmount (the push already counted this iteration).
            //   - On backtrack, BEGIN.bt pops the most recent iteration's context; the
            //     pop's decrement is performed by restoreParenContext (which restores
            //     the pre-push count saved in the popped context). It then either accepts
            //     fewer iterations (Greedy) or re-drives an earlier iteration's content
            //     (FixedCount / below-min). Re-driving passes through no increment, so
            //     the count stays correct without any compensating arithmetic.
            //
            // FixedCount is just the case where min == max == N: it loops to exactly N
            // and never accepts fewer, so it always re-drives earlier content on backtrack.
            case YarrOpCode::ParenthesesSubpatternBegin: {
                termMatchTargets.append(MatchTargets());

#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
                PatternTerm* term = op.m_term;
                unsigned parenthesesFrameLocation = term->frameLocation;

                storeToFrame(MacroAssembler::TrustedImm32(0), parenthesesFrameLocation + BackTrackInfoParentheses::matchAmountIndex());
                storeToFrame(MacroAssembler::TrustedImmPtr(nullptr), parenthesesFrameLocation + BackTrackInfoParentheses::parenContextHeadIndex());

                // NonGreedy with min == 0: initially skip the subpattern (set beginIndex = -1
                // and jump to end); on backtrack we re-enter here to try matching it. With
                // min > 0 we fall through to run the mandatory iterations first.
                if (term->quantityType == QuantifierType::NonGreedy && !term->quantityMinCount) {
                    storeToFrame(MacroAssembler::TrustedImm32(-1), parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex());
                    op.m_jumps.append(m_jit.jump());
                }

                defineReentryLabel(op);
                MacroAssembler::RegisterID currParenContextReg = m_regs.regT0;
                MacroAssembler::RegisterID newParenContextReg = m_regs.regT1;
                MacroAssembler::RegisterID countTemporary = m_regs.regT2;

                loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheses::parenContextHeadIndex(), currParenContextReg);
                allocateParenContext(newParenContextReg);
                m_jit.storePtr(currParenContextReg, MacroAssembler::Address(newParenContextReg, ParenContext::nextOffset()));
                storeToFrame(newParenContextReg, parenthesesFrameLocation + BackTrackInfoParentheses::parenContextHeadIndex());

                // Save-at-BEGIN. saveParenContext leaves the pre-iteration matchAmount
                // in regT2 (countTemporary) for the increment below.
                bool savesReturnAddress = term->parentheses.disjunction->m_alternatives.size() > 1;
                saveParenContext(newParenContextReg, /* matchAmountGPR */ countTemporary, /* scratchGPR */ m_regs.regT3, term->parentheses.subpatternId, term->parentheses.lastSubpatternId, parenthesesFrameLocation, savesReturnAddress);

                // Bump matchAmount: the just-pushed context captured the pre-increment value.
                m_jit.add32(MacroAssembler::TrustedImm32(1), countTemporary);
                storeToFrame(countTemporary, parenthesesFrameLocation + BackTrackInfoParentheses::matchAmountIndex());
                storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex());

                // If the parenthese are capturing, store the starting index value to the captures array.
                if (term->capture() && shouldRecordSubpatterns())
                    emitCaptureStart(term, op.m_checkedOffset, m_regs.regT0);
#else // !YARR_JIT_ALL_PARENS_EXPRESSIONS
                RELEASE_ASSERT_NOT_REACHED();
#endif
                break;
            }
            case YarrOpCode::ParenthesesSubpatternEnd: {
                termMatchTargets.takeLast();

#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
                PatternTerm* term = op.m_term;
                YarrOp& beginOp = m_ops[op.m_previousOp];
                unsigned parenthesesFrameLocation = term->frameLocation;

                // If the nested alternative matched without consuming any characters, punt this back to the interpreter.
                // FIXME: <https://bugs.webkit.org/show_bug.cgi?id=200786> Add ability for the YARR JIT to properly
                // handle nested expressions that can match without consuming characters
                if (!term->parentheses.disjunction->m_minimumSize)
                    m_abortExecution.append(m_jit.branch32(MacroAssembler::Equal, m_regs.index, frameAddress().withOffset(parenthesesFrameLocation * sizeof(void*))));

                // matchAmount was already incremented at this iteration's BEGIN (on
                // push), so after iteration k it equals k. We only need to read it here
                // for the loop-back comparison below.
                const MacroAssembler::RegisterID countTemporary = m_regs.regT1;
                loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheses::matchAmountIndex(), countTemporary);

                // If the parenthese are capturing, store the ending index value to the captures array.
                if (term->capture() && shouldRecordSubpatterns())
                    emitCaptureEnd(term, op.m_checkedOffset, m_regs.regT0);

                switch (term->quantityType) {
                case QuantifierType::FixedCount:
                case QuantifierType::Greedy: {
                    // FixedCount and Greedy both loop back while we are still below the
                    // maximum iteration count (FixedCount has max == N, so it stops at
                    // exactly N; Greedy with an infinite max always loops back). If we get
                    // a failed match from after the parentheses we re-enter at this label.
                    if (term->quantityMaxCount != quantifyInfinite)
                        m_jit.branch32(MacroAssembler::Below, countTemporary, MacroAssembler::Imm32(term->quantityMaxCount)).linkTo(beginOp.m_reentry, &m_jit);
                    else
                        m_jit.jump(beginOp.m_reentry);

                    defineReentryLabel(op);
                    break;
                }
                case QuantifierType::NonGreedy: {
                    // For NonGreedy parentheses, link the jump from before the subpattern to here.
                    // For min > 0, run min mandatory iterations before lazily falling through:
                    // if matchAmount < min, loop back to BEGIN.reentry to run another iteration.
                    if (term->quantityMinCount)
                        m_jit.branch32(MacroAssembler::Below, countTemporary, MacroAssembler::Imm32(term->quantityMinCount)).linkTo(beginOp.m_reentry, &m_jit);
                    beginOp.m_jumps.link(&m_jit);
                    defineReentryLabel(op);
                    break;
                }
                }
#else // !YARR_JIT_ALL_PARENS_EXPRESSIONS
                RELEASE_ASSERT_NOT_REACHED();
#endif
                break;
            }

            // YarrOpCode::ParentheticalAssertionBegin/End
            case YarrOpCode::ParentheticalAssertionBegin: {
                PatternTerm* term = op.m_term;

                termMatchTargets.append(MatchTargets());

                // Store the current index - assertions should not update index, so
                // we will need to restore it upon a successful match.
                unsigned parenthesesFrameLocation = term->frameLocation;
                storeToFrame(m_regs.index, parenthesesFrameLocation + BackTrackInfoParentheticalAssertion::beginIndex());

                // Realign the index register to the assertion point. In a forward
                // frame that point is checkAdjust characters behind the frontier;
                // in a backward (mirrored) frame it is checkAdjust characters ahead
                // of the leftward cursor.
                if (op.m_checkAdjust) {
                    if (op.m_direction == Backward)
                        m_jit.add32(MacroAssembler::Imm32(op.m_checkAdjust), m_regs.index);
                    else
                        m_jit.sub32(MacroAssembler::Imm32(op.m_checkAdjust), m_regs.index);
                }
                break;
            }
            case YarrOpCode::ParentheticalAssertionEnd: {
                PatternTerm* term = op.m_term;

                termMatchTargets.takeLast();

                // Restore the input index value.
                unsigned parenthesesFrameLocation = term->frameLocation;
                loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheticalAssertion::beginIndex(), m_regs.index);

                // If inverted, a successful match of the assertion must be treated
                // as a failure, clear any nested captures and jump to backtracking.
                if (term->invert()) {
                    emitClearCapturesForTerm(term);
                    op.m_jumps.append(m_jit.jump());
                    defineReentryLabel(op);
                }
                break;
            }

            case YarrOpCode::MatchFailed:
                generateFailReturn();
                break;
            }

            // A body Begin op's search prefilters `break` out of its case at
            // several points; all of them converge here, index at the first
            // alternative's claimed frontier, before alternative 0's terms. Route
            // on the match-start character now so every path is dispatched.
            if (op.m_op == YarrOpCode::BodyAlternativeBegin && op.m_bodyDispatch) {
                generateBodyDispatch(op);
                bindBodyDispatchEntry(*op.m_bodyDispatch, 0);
            }

            ++opIndex;
        } while (opIndex < m_ops.size() && !hasExceededCodeSizeLimit());

        termMatchTargets.takeLast();
    }

    void backtrack()
    {
        // Backwards generate the backtracking code.
        size_t opIndex = m_ops.size();
        ASSERT(opIndex);

        do {
            --opIndex;

            if (m_disassembler)
                m_disassembler->setForBacktrack(opIndex, m_jit.label());

            YarrOp& op = m_ops[opIndex];
            m_direction = op.m_direction;
            switch (op.m_op) {

            case YarrOpCode::Term:
                backtrackTerm(opIndex);
                break;

            // YarrOpCode::BodyAlternativeBegin/Next/End
            //
            // For each Begin/Next node representing an alternative, we need to decide what to do
            // in two circumstances:
            //  - If we backtrack back into this node, from within the alternative.
            //  - If the input check at the head of the alternative fails (if this exists).
            //
            // We treat these two cases differently since in the former case we have slightly
            // more information - since we are backtracking out of a prior alternative we know
            // that at least enough input was available to run it. For example, given the regular
            // expression /a|b/, if we backtrack out of the first alternative (a failed pattern
            // character match of 'a'), then we need not perform an additional input availability
            // check before running the second alternative.
            //
            // Backtracking required differs for the last alternative, which in the case of the
            // repeating set of alternatives must loop. The code generated for the last alternative
            // will also be used to handle all input check failures from any prior alternatives -
            // these require similar functionality, in seeking the next available alternative for
            // which there is sufficient input.
            //
            // Since backtracking of all other alternatives simply requires us to link backtracks
            // to the reentry point for the subsequent alternative, we will only be generating any
            // code when backtracking the last alternative.
            case YarrOpCode::BodyAlternativeBegin:
            case YarrOpCode::BodyAlternativeNext: {
                PatternAlternative* alternative = op.m_alternative;

                // Is this the last alternative? If not, then if we backtrack to this point we just
                // need to jump to try to match the next alternative.
                if (m_ops[op.m_nextOp].m_op != YarrOpCode::BodyAlternativeEnd) {
                    if (op.m_bodyDispatch) {
                        // Under body dispatch this alternative was the sole candidate for
                        // the character at the match start (first-character sets are
                        // pairwise disjoint), so no later alternative can match here:
                        // its failure advances the search rather than trying the next
                        // alternative. Materialise the failure flow, then advance from
                        // this alternative's claim.
                        m_backtrackingState.link(*this, op);
                        YarrOp* beginOp = &op;
                        while (beginOp->m_op != YarrOpCode::BodyAlternativeBegin)
                            beginOp = &m_ops[beginOp->m_previousOp];
                        emitBodyDispatchAdvance(*op.m_bodyDispatch, *beginOp, alternative->m_minimumSize);
                        break;
                    }
                    m_backtrackingState.linkTo(m_ops[op.m_nextOp].m_reentry, &m_jit);
                    break;
                }
                YarrOp& endOp = m_ops[op.m_nextOp];
                ASSERT(endOp.m_op == YarrOpCode::BodyAlternativeEnd);

                YarrOp* beginOp = &op;
                while (beginOp->m_op != YarrOpCode::BodyAlternativeBegin) {
                    ASSERT(beginOp->m_op == YarrOpCode::BodyAlternativeNext);
                    beginOp = &m_ops[beginOp->m_previousOp];
                }

                bool onceThrough = endOp.m_nextOp == notFound;
                
                MacroAssembler::JumpList lastStickyAlternativeFailures;

                // First, generate code to handle cases where we backtrack out of an attempted match
                // of the last alternative. If this is a 'once through' set of alternatives then we
                // have nothing to do - link this straight through to the End.
                if (onceThrough)
                    m_backtrackingState.linkTo(endOp.m_reentry, &m_jit);
                else {
                    if (m_pattern.sticky()) {
                        // It is a sticky pattern and the last alternative failed, jump to the end.
                        m_backtrackingState.takeBacktracksToJumpList(lastStickyAlternativeFailures, &m_jit);
                    } else if (m_pattern.m_body->m_hasFixedSize
                        && (alternative->m_minimumSize > beginOp->m_alternative->m_minimumSize)
                        && (alternative->m_minimumSize - beginOp->m_alternative->m_minimumSize == 1)) {
                        // If we don't need to move the input position, and the pattern has a fixed size
                        // (in which case we omit the store of the start index until the pattern has matched)
                        // then we can just link the backtrack out of the last alternative straight to the
                        // head of the first alternative.
                        m_backtrackingState.linkTo(beginOp->m_reentry, &m_jit);
                    } else {
                        // We need to generate a trampoline of code to execute before looping back
                        // around to the first alternative.
                        m_backtrackingState.link(*this, op);

                        // No need to advance and retry for a sticky pattern. And it is already handled before this branch.
                        ASSERT(!m_pattern.sticky());

                        // If the pattern size is not fixed, then store the start index for use if we match.
                        if (!m_pattern.m_body->m_hasFixedSize) {
                            if (alternative->m_minimumSize == 1)
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                                if (m_canUseFirstNonBMPCharacterOptimization) {
                                    m_jit.add32(m_regs.firstCharacterAdditionalReadSize, m_regs.index, m_regs.regT0);
                                    setMatchStart(m_regs.regT0);
                                } else
#endif
                                setMatchStart(m_regs.index);
                            else {
                                if (alternative->m_minimumSize)
                                    m_jit.sub32(m_regs.index, MacroAssembler::Imm32(alternative->m_minimumSize - 1), m_regs.regT0);
                                else
                                    m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index, m_regs.regT0);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                                if (m_canUseFirstNonBMPCharacterOptimization)
                                    m_jit.add32(m_regs.firstCharacterAdditionalReadSize, m_regs.regT0);
#endif
                                setMatchStart(m_regs.regT0);
                            }
                        }

                        // Generate code to loop. Check whether the last alternative is longer than the
                        // first (e.g. /a|xy/ or /a|xyz/).
                        if (alternative->m_minimumSize > beginOp->m_alternative->m_minimumSize) {
                            // We want to loop, and increment input position. If the delta is 1, it is
                            // already correctly incremented, if more than one then decrement as appropriate.
                            unsigned delta = alternative->m_minimumSize - beginOp->m_alternative->m_minimumSize;
                            ASSERT(delta);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                            if (m_canUseFirstNonBMPCharacterOptimization) {
                                m_jit.add32(m_regs.firstCharacterAdditionalReadSize, m_regs.index);
                                if (delta != 1)
                                    m_jit.sub32(MacroAssembler::Imm32(delta - 1), m_regs.index);
                                checkInput().linkTo(beginOp->m_reentry, &m_jit);
                            } else {
#endif
                                if (delta != 1)
                                    m_jit.sub32(MacroAssembler::Imm32(delta - 1), m_regs.index);
                                m_jit.jump(beginOp->m_reentry);
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                            }
#endif
                        } else {
                            // If the first alternative has minimum size 0xFFFFFFFFu, then there cannot
                            // be sufficent input available to handle this, so just fall through.
                            unsigned delta = beginOp->m_alternative->m_minimumSize - alternative->m_minimumSize;
                            if (delta != 0xFFFFFFFFu) {
                                // We need to check input because we are incrementing the input.
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                                if (m_canUseFirstNonBMPCharacterOptimization)
                                    m_jit.add32(m_regs.firstCharacterAdditionalReadSize, m_regs.index);
#endif
                                m_jit.add32(MacroAssembler::Imm32(delta + 1), m_regs.index);
                                checkInput().linkTo(beginOp->m_reentry, &m_jit);
                            }
                        }
                    }
                }

                // We can reach this point in the code in two ways:
                //  - Fallthrough from the code above (a repeating alternative backtracked out of its
                //    last alternative, and did not have sufficent input to run the first).
                //  - We will loop back up to the following label when a repeating alternative loops,
                //    following a failed input check.
                //
                // Either way, we have just failed the input check for the first alternative.
                MacroAssembler::Label firstInputCheckFailed(&m_jit);

                if (BodyDispatchInfo* bodyDispatch = beginOp->m_bodyDispatch) {
                    // Dispatched body: the Begin's input-check failures and the
                    // search-advance stubs that lack input for the next position all
                    // arrive here with the index at the first alternative's claim
                    // (min[0] characters unavailable). Route the tail on the
                    // match-start character instead of walking the alternatives in
                    // order, so that every entry into an alternative is a sole
                    // candidate dispatch entry; the tail falls out below with the
                    // index at the last alternative's claim.
                    beginOp->m_jumps.link(&m_jit);
                    bodyDispatch->inputExhaustedJumps.link(&m_jit);
                    bodyDispatch->firstInputCheckFailed = firstInputCheckFailed;
                    bodyDispatch->haveFirstInputCheckFailed = true;
                    generateBodyDispatchTail(*bodyDispatch, *beginOp, op);
                } else {
                    // Generate code to handle input check failures from alternatives except the last.
                    // prevOp is the alternative we're handling a bail out from (initially Begin), and
                    // nextOp is the alternative we will be attempting to reenter into.
                    //
                    // We will link input check failures from the forwards matching path back to the code
                    // that can handle them.
                    YarrOp* prevOp = beginOp;
                    YarrOp* nextOp = &m_ops[beginOp->m_nextOp];
                    while (nextOp->m_op != YarrOpCode::BodyAlternativeEnd) {
                        prevOp->m_jumps.link(&m_jit);

                        // We only get here if an input check fails, it is only worth checking again
                        // if the next alternative has a minimum size less than the last.
                        if (prevOp->m_alternative->m_minimumSize > nextOp->m_alternative->m_minimumSize) {
                            // FIXME: if we added an extra label to YarrOp, we could avoid needing to
                            // subtract delta back out, and reduce this code. Should performance test
                            // the benefit of this.
                            unsigned delta = prevOp->m_alternative->m_minimumSize - nextOp->m_alternative->m_minimumSize;
                            m_jit.sub32(MacroAssembler::Imm32(delta), m_regs.index);
                            MacroAssembler::Jump fail = jumpIfNoAvailableInput();
                            m_jit.add32(MacroAssembler::Imm32(delta), m_regs.index);
                            m_jit.jump(nextOp->m_reentry);
                            fail.link(&m_jit);
                        } else if (prevOp->m_alternative->m_minimumSize < nextOp->m_alternative->m_minimumSize)
                            m_jit.add32(MacroAssembler::Imm32(nextOp->m_alternative->m_minimumSize - prevOp->m_alternative->m_minimumSize), m_regs.index);
                        prevOp = nextOp;
                        nextOp = &m_ops[nextOp->m_nextOp];
                    }
                }

                // We fall through to here if there is insufficient input to run the last alternative.

                // If there is insufficient input to run the last alternative, then for 'once through'
                // alternatives we are done - just jump back up into the forwards matching path at the End.
                if (onceThrough) {
                    op.m_jumps.linkTo(endOp.m_reentry, &m_jit);
                    m_jit.jump(endOp.m_reentry);
                    break;
                }

                // For repeating alternatives, link any input check failure from the last alternative to
                // this point.
                op.m_jumps.link(&m_jit);

                bool needsToUpdateMatchStart = !m_pattern.m_body->m_hasFixedSize;

                // Check for cases where input position is already incremented by 1 for the last
                // alternative (this is particularly useful where the minimum size of the body
                // disjunction is 0, e.g. /a*|b/).
                if (needsToUpdateMatchStart && alternative->m_minimumSize == 1) {
                    // index is already incremented by 1, so just store it now!
                    setMatchStart(m_regs.index);
                    needsToUpdateMatchStart = false;
                }

                if (!m_pattern.sticky()) {
                    // Check whether there is sufficient input to loop. Increment the input position by
                    // one, and check. Also add in the minimum disjunction size before checking - there
                    // is no point in looping if we're just going to fail all the input checks around
                    // the next iteration.
                    ASSERT(alternative->m_minimumSize >= m_pattern.m_body->m_minimumSize);
                    if (alternative->m_minimumSize == m_pattern.m_body->m_minimumSize) {
                        // If the last alternative had the same minimum size as the disjunction,
                        // just simply increment input pos by 1, no adjustment based on minimum size.
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
                        if (m_canUseFirstNonBMPCharacterOptimization)
                            m_jit.add32(m_regs.firstCharacterAdditionalReadSize, m_regs.index);
#endif
                        m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
                    } else {
                        // If the minumum for the last alternative was one greater than than that
                        // for the disjunction, we're already progressed by 1, nothing to do!
                        unsigned delta = (alternative->m_minimumSize - m_pattern.m_body->m_minimumSize) - 1;
                        if (delta)
                            m_jit.sub32(MacroAssembler::Imm32(delta), m_regs.index);
                    }
                    MacroAssembler::Jump matchFailed = jumpIfNoAvailableInput();

                    if (needsToUpdateMatchStart) {
                        if (!m_pattern.m_body->m_minimumSize)
                            setMatchStart(m_regs.index);
                        else {
                            m_jit.sub32(m_regs.index, MacroAssembler::Imm32(m_pattern.m_body->m_minimumSize), m_regs.regT0);
                            setMatchStart(m_regs.regT0);
                        }
                    }

                    // Calculate how much more input the first alternative requires than the minimum
                    // for the body as a whole. If no more is needed then we dont need an additional
                    // input check here - jump straight back up to the start of the first alternative.
                    if (beginOp->m_alternative->m_minimumSize == m_pattern.m_body->m_minimumSize)
                        m_jit.jump(beginOp->m_reentry);
                    else {
                        if (beginOp->m_alternative->m_minimumSize > m_pattern.m_body->m_minimumSize)
                            m_jit.add32(MacroAssembler::Imm32(beginOp->m_alternative->m_minimumSize - m_pattern.m_body->m_minimumSize), m_regs.index);
                        else
                            m_jit.sub32(MacroAssembler::Imm32(m_pattern.m_body->m_minimumSize - beginOp->m_alternative->m_minimumSize), m_regs.index);
                        checkInput().linkTo(beginOp->m_reentry, &m_jit);
                        m_jit.jump(firstInputCheckFailed);
                    }

                    // We jump to here if we iterate to the point that there is insufficient input to
                    // run any matches, and need to return a failure state from JIT code.
                    matchFailed.link(&m_jit);
                }

                lastStickyAlternativeFailures.link(&m_jit);
                generateFailReturn();
                break;
            }
            case YarrOpCode::BodyAlternativeEnd: {
                // We should never backtrack back into a body disjunction.
                ASSERT(m_backtrackingState.isEmpty());
                break;
            }

            // YarrOpCode::SimpleNestedAlternativeBegin/Next/End
            // YarrOpCode::StringListAlternativeBegin/Next/End
            // YarrOpCode::NestedAlternativeBegin/Next/End
            //
            // Generate code for when we backtrack back out of an alternative into
            // a Begin or Next node, or when the entry input count check fails. If
            // there are more alternatives we need to jump to the next alternative,
            // if not we backtrack back out of the current set of parentheses.
            //
            // In the case of non-simple nested assertions we need to also link the
            // 'return address' appropriately to backtrack back out into the correct
            // alternative.
            case YarrOpCode::SimpleNestedAlternativeBegin:
            case YarrOpCode::SimpleNestedAlternativeNext:
            case YarrOpCode::StringListAlternativeBegin:
            case YarrOpCode::StringListAlternativeNext:
            case YarrOpCode::NestedAlternativeBegin:
            case YarrOpCode::NestedAlternativeNext: {
                YarrOp& nextOp = m_ops[op.m_nextOp];
                bool isBegin = op.m_previousOp == notFound;
                bool isLastAlternative = nextOp.m_nextOp == notFound;
                ASSERT(isBegin == (op.m_op == YarrOpCode::SimpleNestedAlternativeBegin || op.m_op == YarrOpCode::StringListAlternativeBegin || op.m_op == YarrOpCode::NestedAlternativeBegin));
                ASSERT(isLastAlternative == (nextOp.m_op == YarrOpCode::SimpleNestedAlternativeEnd || nextOp.m_op == YarrOpCode::StringListAlternativeEnd || nextOp.m_op == YarrOpCode::NestedAlternativeEnd));

                // Treat an input check failure the same as a failed match.
                m_backtrackingState.append(op.m_jumps);

                // Dispatched alternatives (first-character dispatch): a failing
                // alternative releases its claim and continues its own chain through
                // the resume address the chain stored in the frame, rather than
                // falling to a fixed successor. Failures that mean "no alternative can
                // match here" (empty routes, exhausted chains) join the group's
                // failure flow at the Begin op, where the index is at the group frame.
                if (op.m_dispatch) {
                    if (!m_backtrackingState.isEmpty()) {
                        m_backtrackingState.link(*this, op);
                        // Give back the input this alternative claimed on entry.
                        if (op.m_checkAdjust)
                            m_jit.sub32(MacroAssembler::Imm32(op.m_checkAdjust), m_regs.index);
                        loadFromFrameAndJump(op.m_dispatch->frameLocation + BackTrackInfoParenthesesOnce::chainResumeIndex());
                    }
                    if (isBegin) {
                        // Group-level failures gathered by the dispatch (empty routes,
                        // exhausted chains) resume at the Begin, where the index is at
                        // the group frame. (Under dispatch nothing plants jumps on the
                        // End: Next backtracks jump through chainResume, and dispatch
                        // requires a FixedCount group, so no zero-length-match check.)
                        m_backtrackingState.append(op.m_dispatch->groupFailJumps);
                    }
                    // For non-simple alternatives, link the alternative's 'return
                    // address' so we backtrack back out into it correctly.
                    if (op.m_op == YarrOpCode::NestedAlternativeNext)
                        m_backtrackingState.append(op.m_returnAddress);
                    op.m_contentBacktrackEntryLabel = m_jit.label();
                    break;
                }

                // Set the backtracks to jump to the appropriate place. We may need
                // to link the backtracks in one of three different way depending on
                // the type of alternative we are dealing with:
                //  - A single alternative, with no siblings.
                //  - The last alternative of a set of two or more.
                //  - An alternative other than the last of a set of two or more.
                //
                // In the case of a single alternative on its own, we don't need to
                // jump anywhere - if the alternative fails to match we can just
                // continue to backtrack out of the parentheses without jumping.
                //
                // In the case of the last alternative in a set of more than one, we
                // need to jump to return back out to the beginning. We'll do so by
                // adding a jump to the End node's m_jumps list, and linking this
                // when we come to generate the Begin node. For alternatives other
                // than the last, we need to jump to the next alternative.
                //
                // If the alternative had adjusted the input position we must link
                // backtracking to here, correct, and then jump on. If not we can
                // link the backtracks directly to their destination.
                if (op.m_checkAdjust) {
                    if (!m_backtrackingState.isEmpty()) {
                        // Handle the cases where we need to link the backtracks here.
                        m_backtrackingState.link(*this, op);
                        // Give back the input this alternative claimed on entry.
                        if (op.m_direction == Backward)
                            m_jit.add32(MacroAssembler::Imm32(op.m_checkAdjust), m_regs.index);
                        else
                            m_jit.sub32(MacroAssembler::Imm32(op.m_checkAdjust), m_regs.index);
                        if (!isLastAlternative) {
                            // An alternative that is not the last should jump to its successor.
                            m_jit.jump(nextOp.m_reentry);
                        } else if (!isBegin) {
                            // The last of more than one alternatives must jump back to the beginning.
                            nextOp.m_jumps.append(m_jit.jump());
                        } else {
                            // A single alternative on its own can fall through.
                            m_backtrackingState.fallthrough();
                        }
                    }
                } else {
                    // Handle the cases where we can link the backtracks directly to their destinations.
                    if (!isLastAlternative) {
                        // An alternative that is not the last should jump to its successor.
                        m_backtrackingState.linkTo(nextOp.m_reentry, &m_jit);
                    } else if (!isBegin) {
                        // The last of more than one alternatives must jump back to the beginning.
                        m_backtrackingState.takeBacktracksToJumpList(nextOp.m_jumps, &m_jit);
                    }
                    // In the case of a single alternative on its own do nothing - it can fall through.
                }

                // If there is a backtrack jump from a zero length match link it here.
                if (op.m_zeroLengthMatch.isSet())
                    m_backtrackingState.append(op.m_zeroLengthMatch);

                // At this point we've handled the backtracking back into this node.
                // Now link any backtracks that need to jump to here.

                // For non-simple alternatives, link the alternative's 'return address'
                // so that we backtrack back out into the previous alternative.
                if (op.m_op == YarrOpCode::NestedAlternativeNext)
                    m_backtrackingState.append(op.m_returnAddress);

                // If there is more than one alternative, then the last alternative will
                // have planted a jump to be linked to the end. This jump was added to the
                // End node's m_jumps list. If we are back at the beginning, link it here.
                if (isBegin) {
                    YarrOp* endOp = &m_ops[alternativeEndOpIndex(opIndex)];
                    m_backtrackingState.append(endOp->m_jumps);
                }
                op.m_contentBacktrackEntryLabel = m_jit.label();
                break;
            }
            case YarrOpCode::SimpleNestedAlternativeEnd:
            case YarrOpCode::StringListAlternativeEnd:
            case YarrOpCode::NestedAlternativeEnd: {
                PatternTerm* term = op.m_term;

                // If there is a backtrack jump from a zero length match link it here.
                if (op.m_zeroLengthMatch.isSet())
                    m_backtrackingState.append(op.m_zeroLengthMatch);

                // If we backtrack into the end of a simple subpattern do nothing;
                // just continue through into the last alternative. If we backtrack
                // into the end of a non-simple set of alterntives we need to jump
                // to the backtracking return address set up during generation.
                if (op.m_op == YarrOpCode::NestedAlternativeEnd) {
                    m_backtrackingState.link(*this, op);

                    // Jump to the return address stored by whichever alternative was taken
                    // (stored during forward generation by NestedAlternativeNext/End, and
                    // carried across iterations via the ParenContext save/restore).
                    unsigned parenthesesFrameLocation = term->frameLocation;
                    loadFromFrameAndJump(parenthesesFrameLocation + BackTrackInfoParentheses::returnAddressIndex());

                    // Link the DataLabelPtr associated with the end of the last alternative to this point.
                    m_backtrackingState.append(op.m_returnAddress);
                }
                op.m_contentBacktrackEntryLabel = m_jit.label();
                break;
            }

            // YarrOpCode::ParenthesesSubpatternOnceBegin/End
            //
            // When we are backtracking back out of a capturing subpattern we need
            // to clear the start index in the matches output array, to record that
            // this subpattern has not been captured.
            //
            // When backtracking back out of a Greedy quantified subpattern we need
            // to catch this, and try running the remainder of the alternative after
            // the subpattern again, skipping the parentheses.
            //
            // Upon backtracking back into a quantified set of parentheses we need to
            // check whether we were currently skipping the subpattern. If not, we
            // can backtrack into them, if we were we need to either backtrack back
            // out of the start of the parentheses, or jump back to the forwards
            // matching start, depending of whether the match is Greedy or NonGreedy.
            case YarrOpCode::ParenthesesSubpatternOnceBegin: {
                PatternTerm* term = op.m_term;
                ASSERT(term->quantityMaxCount == 1);

                // We only need to backtrack to this point if capturing or greedy.
                if ((term->capture() && shouldRecordSubpatterns()) || term->quantityType == QuantifierType::Greedy) {
                    m_backtrackingState.link(*this, op);

                    // If capturing, clear the capture (both start and end).
                    if (term->capture() && shouldRecordSubpatterns()) {
                        auto subpatternId = term->parentheses.subpatternId;
                        clearSubpattern(subpatternId);
                        if (m_pattern.m_numDuplicateNamedCaptureGroups) {
                            if (auto duplicateNamedGroupId = m_pattern.m_duplicateNamedGroupForSubpatternId[subpatternId])
                                storeDuplicateNamedGroupSubpatternId(duplicateNamedGroupId, 0);
                        }
                    }

                    // If Greedy, jump to the end.
                    if (term->quantityType == QuantifierType::Greedy) {
                        // Clear the flag in the stackframe indicating we ran through the subpattern.
                        unsigned parenthesesFrameLocation = term->frameLocation;
                        storeToFrame(MacroAssembler::TrustedImm32(-1), parenthesesFrameLocation + BackTrackInfoParenthesesOnce::beginIndex());

                        // Clear out any nested captures.
                        if (shouldRecordSubpatterns() && term->containsAnyCaptures()) {
                            unsigned firstPatternId = term->parentheses.subpatternId;
                            if (term->capture())
                                firstPatternId++;
                            for (unsigned subpattern = firstPatternId; subpattern <= term->parentheses.lastSubpatternId; subpattern++) {
                                clearSubpattern(subpattern);

                                if (m_pattern.m_numDuplicateNamedCaptureGroups) {
                                    if (auto duplicateNamedGroupId = m_pattern.m_duplicateNamedGroupForSubpatternId[subpattern])
                                        storeDuplicateNamedGroupSubpatternId(duplicateNamedGroupId, 0);
                                }
                            }
                        }

                        // Jump to after the parentheses, skipping the subpattern.
                        m_jit.jump(m_ops[op.m_nextOp].m_reentry);
                        // A backtrack from after the parentheses, when skipping the subpattern,
                        // will jump back to here.
                        op.m_jumps.link(&m_jit);
                    }

                    m_backtrackingState.fallthrough();
                }
                break;
            }
            case YarrOpCode::ParenthesesSubpatternOnceEnd: {
                PatternTerm* term = op.m_term;

                if (term->quantityType != QuantifierType::FixedCount) {
                    m_backtrackingState.link(*this, op);

                    // Check whether we should backtrack back into the parentheses, or if we
                    // are currently in a state where we had skipped over the subpattern
                    // (in which case the flag value on the stack will be -1).
                    unsigned parenthesesFrameLocation = term->frameLocation;
                    MacroAssembler::Jump hadSkipped = m_jit.branch32(MacroAssembler::Equal, frameAddress().withOffset((parenthesesFrameLocation + BackTrackInfoParenthesesOnce::beginIndex()) * sizeof(void*)), MacroAssembler::TrustedImm32(-1));

                    if (term->quantityType == QuantifierType::Greedy) {
                        // For Greedy parentheses, we skip after having already tried going
                        // through the subpattern, so if we get here we're done.
                        YarrOp& beginOp = m_ops[op.m_previousOp];
                        beginOp.m_jumps.append(hadSkipped);
                    } else {
                        // For NonGreedy parentheses, we try skipping the subpattern first,
                        // so if we get here we need to try running through the subpattern
                        // next. Jump back to the start of the parentheses in the forwards
                        // matching path.
                        ASSERT(term->quantityType == QuantifierType::NonGreedy);
                        YarrOp& beginOp = m_ops[op.m_previousOp];
                        hadSkipped.linkTo(beginOp.m_reentry, &m_jit);
                    }

                    m_backtrackingState.fallthrough();
                }

                m_backtrackingState.append(op.m_jumps);
                break;
            }

            // YarrOpCode::ParenthesesSubpatternTerminalBegin/End
            //
            // Terminal subpatterns will always match - there is nothing after them to
            // force a backtrack, and they have a minimum count of 0, and as such will
            // always produce an acceptable result.
            case YarrOpCode::ParenthesesSubpatternTerminalBegin: {
                // We will backtrack to this point once the subpattern cannot match any
                // more. Since no match is accepted as a successful match (we are Greedy
                // quantified with a minimum of zero) jump back to the forwards matching
                // path at the end.
                YarrOp& endOp = m_ops[op.m_nextOp];
                m_backtrackingState.linkTo(endOp.m_reentry, &m_jit);
                break;
            }
            case YarrOpCode::ParenthesesSubpatternTerminalEnd:
                // We should never be backtracking to here (hence the 'terminal' in the name).
                ASSERT(m_backtrackingState.isEmpty());
                m_backtrackingState.append(op.m_jumps);
                break;

            // YarrOpCode::ParenthesesSubpatternFixedCountBegin/End
            //
            // For FixedCount parentheses (capturing or non-capturing) on the fast
            // path, any failure means the entire pattern fails. There's no partial
            // backtracking - we either match exactly N times or we fail completely.
            case YarrOpCode::ParenthesesSubpatternFixedCountBegin: {
                // Any backtrack to Begin means we failed to match the required count.
                // Link any pending backtrack state from the content inside, restore
                // the index to the position when we entered the group (since one or
                // more iterations may have advanced it), clear this paren's capture
                // and any nested captures, then propagate the failure upward.
                PatternTerm* term = op.m_term;
                unsigned parenthesesFrameLocation = term->frameLocation;

                m_backtrackingState.link(*this, op);

                loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheses::returnAddressIndex(), m_regs.index);

                emitClearCapturesForTerm(term);

                m_backtrackingState.fallthrough();
                break;
            }
            case YarrOpCode::ParenthesesSubpatternFixedCountEnd:
                // Backtracking into the End means something after the parentheses failed.
                // For FixedCount, we don't try alternative counts, so just fail.
                m_backtrackingState.append(op.m_jumps);
                break;

            // YarrOpCode::ParenthesesSubpatternBegin/End
            //
            // Capturing or non-capturing subpatterns that need a ParenContext for
            // inter-iteration backtracking: FixedCount with backtrackable content,
            // multi-alt FixedCount, or Greedy/NonGreedy quantifiers. (Capturing
            // FixedCount with non-backtrackable single-alt body is routed instead
            // to ParenthesesSubpatternFixedCountBegin/End above.)
            //
            // In all quantifiers, we always save the state at BEGIN (save-at-BEGIN model).
            // Each iteration's pre-iteration state is snapshotted into a ParenContext that
            // is pushed at BEGIN.fwd and popped at BEGIN.bt. matchAmount counts the
            // ParenContexts currently on the stack: it is incremented once on push at
            // BEGIN.forward, and the matching decrement happens on pop at BEGIN.bt (performed
            // by restoreParenContext, which restores the popped context's saved pre-push
            // count). END.forward does not touch it.
            //
            // On backtrack we restore the popped context and either
            //     1. accept fewer iterations (Greedy at/above min) or,
            //     2. re-drive the previous iteration's content (FixedCount / below min / NonGreedy).
            // FixedCount is simply min == max == N.

            case YarrOpCode::ParenthesesSubpatternBegin: {
#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
                PatternTerm* term = op.m_term;
                unsigned parenthesesFrameLocation = term->frameLocation;
                m_backtrackingState.link(*this, op);

                MacroAssembler::RegisterID currParenContextReg = m_regs.regT0;
                MacroAssembler::RegisterID newParenContextReg = m_regs.regT1;
                MacroAssembler::RegisterID countTemporary = m_regs.regT2;

                loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheses::parenContextHeadIndex(), currParenContextReg);

                // restoreParenContext pops the most recent iteration: it restores m_regs.index
                // and matchAmount to the popped context's pre-push values (that restore is the
                // pop's decrement). We then free the context and either accept fewer iterations
                // (Greedy at/above min) or re-drive the topmost one (FixedCount / below min /
                // NonGreedy; FixedCount has min == max so it never accepts fewer). It returns
                // matchAmount in regT2, which survives freeParenContext below, so we skip
                // reloading it from the frame.
                bool savesReturnAddress = term->parentheses.disjunction->m_alternatives.size() > 1;
                restoreParenContext(currParenContextReg, /* matchAmountGPR */ countTemporary, /* scratchGPR */ m_regs.regT3, term->parentheses.subpatternId, term->parentheses.lastSubpatternId, parenthesesFrameLocation, savesReturnAddress);

                m_jit.loadPtr(MacroAssembler::Address(currParenContextReg, ParenContext::nextOffset()), newParenContextReg);
                freeParenContext(currParenContextReg);
                storeToFrame(newParenContextReg, parenthesesFrameLocation + BackTrackInfoParentheses::parenContextHeadIndex());

                YarrOp& endOp = m_ops[op.m_nextOp];

                auto noPreviousIteration = m_jit.branchTest32(MacroAssembler::Zero, countTemporary);
                m_jit.load32(MacroAssembler::Address(newParenContextReg, ParenContext::beginOffset()), m_regs.regT3);
                storeToFrame(m_regs.regT3, parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex());

                switch (term->quantityType) {
                case QuantifierType::NonGreedy: {
                    ASSERT(endOp.m_op == YarrOpCode::ParenthesesSubpatternEnd);
                    ASSERT(endOp.m_contentBacktrackEntryLabel.isSet());
                    m_jit.jump(endOp.m_contentBacktrackEntryLabel);

                    // No earlier iteration to retry: the parens fail. Clear captures, mark
                    // the frame as not-run, and propagate to the previous backtrack point.
                    //
                    // Even if quantityMinCount == 0, we should make it a complete failure.
                    // This is because NonGreedy already tried with count = 0, failing and reaching here.
                    // So there is no way to proceed further, just propagate a failure.
                    noPreviousIteration.link(&m_jit);
                    // Zero iterations of THIS quantified group means its captures are
                    // undefined -- except for a quantified split's optional copy, whose
                    // capture ids belong to the mandatory sibling: restoreParenContext has
                    // already restored the sibling's committed values, and clearing them
                    // here would erase captures the copy never owned.
                    if (!term->parentheses.isCopy)
                        emitClearCapturesForTerm(term);
                    storeToFrame(MacroAssembler::TrustedImm32(-1), parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex());
                    m_backtrackingState.fallthrough();
                    break;
                }

                case QuantifierType::FixedCount:
                case QuantifierType::Greedy: {
                    // Greedy: we took as many iterations as possible, so backtracking pops
                    // the topmost one. countTemporary now holds the surviving count (the
                    // number of contexts still on the stack after the pop). We can accept
                    // fewer iterations when that surviving count is still >= min.
                    //
                    // FixedCount is min == max: the surviving count after a pop is always
                    // < min, so the accept-fewer test below is never satisfied and it always
                    // re-drives the previous iteration's content (jumping to the content
                    // backtrack entry), exactly the desired "match exactly N" behavior.
                    // A count of 0 (no previous iteration) is a complete failure.
                    if (term->quantityMinCount) {
                        // Accept the surviving iterations when there are still at least min
                        // of them; otherwise re-drive the previous iteration's content.
                        m_jit.branch32(MacroAssembler::AboveOrEqual, countTemporary, MacroAssembler::Imm32(term->quantityMinCount)).linkTo(endOp.m_reentry, &m_jit);

                        // Jump to content backtrack entry. The earlier restoreParenContext already left m_regs.index at the end of iter k.
                        ASSERT(endOp.m_op == YarrOpCode::ParenthesesSubpatternEnd);
                        ASSERT(endOp.m_contentBacktrackEntryLabel.isSet());
                        m_jit.jump(endOp.m_contentBacktrackEntryLabel);

                        // Now we are not able to try any backtracking. So parentheses with min-count gets complete failure.
                        // We need to clear the state, and go back to the further former backtracking.
                        noPreviousIteration.link(&m_jit);
                        op.m_jumps.link(&m_jit);
                        // As above: an optional copy owns none of its capture ids.
                        if (!term->parentheses.isCopy)
                            emitClearCapturesForTerm(term);
                    } else {
                        // Try fewer iterations.
                        m_jit.jump(endOp.m_reentry);

                        noPreviousIteration.link(&m_jit);
                        // Clear the flag in the stackframe indicating we didn't run through the subpattern.
                        storeToFrame(MacroAssembler::TrustedImm32(-1), parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex());

                        // After marking parens as not run, jump to reentry to skip the subpattern.
                        m_jit.jump(endOp.m_reentry);
                        // A backtrack from after the parentheses, when skipping the subpattern,
                        // will jump back to here.
                        op.m_jumps.link(&m_jit);
                    }

                    storeToFrame(MacroAssembler::TrustedImm32(-1), parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex());
                    m_backtrackingState.fallthrough();
                    break;
                }
                }
#else // !YARR_JIT_ALL_PARENS_EXPRESSIONS
                RELEASE_ASSERT_NOT_REACHED();
#endif
                break;
            }
            case YarrOpCode::ParenthesesSubpatternEnd: {
#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
                PatternTerm* term = op.m_term;
                YarrOp& beginOp = m_ops[op.m_previousOp];
                unsigned parenthesesFrameLocation = term->frameLocation;

                m_backtrackingState.link(*this, op);
                switch (term->quantityType) {
                case QuantifierType::Greedy: {
                    // Check whether we should backtrack back into the parentheses, or if we
                    // are currently in a state where we had skipped over the subpattern
                    // (in which case the flag value on the stack will be -1).
                    MacroAssembler::Jump hadSkipped = m_jit.branch32(MacroAssembler::Equal, frameAddress().withOffset((parenthesesFrameLocation  + BackTrackInfoParentheses::beginIndex()) * sizeof(void*)), MacroAssembler::TrustedImm32(-1));

                    // For Greedy parentheses, we skip after having already tried going
                    // through the subpattern, so if we get here we're done.
                    beginOp.m_jumps.append(hadSkipped);
                    break;
                }
                case QuantifierType::NonGreedy: {
                    // For NonGreedy parentheses, we try skipping the subpattern first,
                    // so if we get here we need to try running through the subpattern
                    // next. Jump back to the start of the parentheses in the forwards
                    // matching path.
                    ASSERT(term->quantityType == QuantifierType::NonGreedy);

                    const MacroAssembler::RegisterID beginTemporary = m_regs.regT0;
                    const MacroAssembler::RegisterID countTemporary = m_regs.regT1;

                    loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheses::beginIndex(), beginTemporary);
                    m_jit.branch32(MacroAssembler::Equal, beginTemporary, MacroAssembler::TrustedImm32(-1)).linkTo(beginOp.m_reentry, &m_jit);

                    MacroAssembler::JumpList exceededMatchLimit;

                    if (term->quantityMaxCount != quantifyInfinite) {
                        loadFromFrame(parenthesesFrameLocation + BackTrackInfoParentheses::matchAmountIndex(), countTemporary);
                        exceededMatchLimit.append(m_jit.branch32(MacroAssembler::AboveOrEqual, countTemporary, MacroAssembler::Imm32(term->quantityMaxCount)));
                    }

                    // Try one more (lazy) iteration only if the last one made progress:
                    // the frontier moved right in a forward frame, left (down) in a
                    // mirrored one. Without the direction split a leftward-consuming
                    // lazy group stops re-entering as soon as its frontier has moved.
                    m_jit.branch32(op.m_direction == Backward ? MacroAssembler::Below : MacroAssembler::Above, m_regs.index, beginTemporary).linkTo(beginOp.m_reentry, &m_jit);

                    exceededMatchLimit.link(&m_jit);
                    break;
                }
                case QuantifierType::FixedCount: {
                    // Backtracking into the End means something after the parentheses failed.
                    // For FixedCount we just fall through to the content backtrack entry below
                    // to re-drive the most recent iteration's content; if it exhausts, it
                    // reaches BEGIN.bt which pops to the previous iteration. No special
                    // per-context bookkeeping is needed in the unified save-at-BEGIN model.
                    break;
                }
                }

                m_backtrackingState.fallthrough();
                m_backtrackingState.append(op.m_jumps);
                op.m_contentBacktrackEntryLabel = m_jit.label();
#else // !YARR_JIT_ALL_PARENS_EXPRESSIONS
                RELEASE_ASSERT_NOT_REACHED();
#endif
                break;
            }

            // YarrOpCode::ParentheticalAssertionBegin/End
            case YarrOpCode::ParentheticalAssertionBegin: {
                PatternTerm* term = op.m_term;
                YarrOp& endOp = m_ops[op.m_nextOp];

                // We need to handle the backtracks upon backtracking back out
                // of a parenthetical assertion if either we need to correct
                // the input index, or the assertion was inverted, or the body ran
                // in a different direction from this (enclosing) frame -- then the
                // body left index on its own coordinate line, and only the position
                // saved at entry (beginIndex) is meaningful to the code we back
                // out to. An arithmetic undo cannot cross that boundary.
                bool bodyDirectionDiffers = term->matchDirection() != op.m_direction;
                if (op.m_checkAdjust || term->invert() || bodyDirectionDiffers) {
                    m_backtrackingState.link(*this, op);

                    if (bodyDirectionDiffers)
                        loadFromFrame(term->frameLocation + BackTrackInfoParentheticalAssertion::beginIndex(), m_regs.index);
                    else if (op.m_checkAdjust) {
                        // Undo the realignment done on entry (see the forward path).
                        if (op.m_direction == Backward)
                            m_jit.sub32(MacroAssembler::Imm32(op.m_checkAdjust), m_regs.index);
                        else
                            m_jit.add32(MacroAssembler::Imm32(op.m_checkAdjust), m_regs.index);
                    }

                    // In an inverted assertion failure to match the subpattern
                    // is treated as a successful match - jump to the end of the
                    // subpattern. We already have adjusted the input position
                    // back to that before the assertion, which is correct.
                    if (term->invert()) {
                        emitClearCapturesForTerm(term);
                        m_jit.jump(endOp.m_reentry);
                    }

                    m_backtrackingState.fallthrough();
                }

                // The End node's jump list will contain any backtracks into
                // the end of the assertion. Also, if inverted, we will have
                // added the failure caused by a successful match to this.
                m_backtrackingState.append(endOp.m_jumps);
                break;
            }
            case YarrOpCode::ParentheticalAssertionEnd: {
                // Never backtrack into an assertion; later failures bail to before the begin.
                // For positive assertions with captures, we must clear the captures before
                // propagating the backtrack. The assertion matched and set captures, but
                // something after it failed, so those captures must be reset to undefined.
                PatternTerm* term = op.m_term;
                if (!term->invert() && shouldRecordSubpatterns() && term->containsAnyCaptures()) {
                    m_backtrackingState.link(*this, op);
                    emitClearCapturesForTerm(term);
                    m_backtrackingState.fallthrough();
                }
                m_backtrackingState.takeBacktracksToJumpList(op.m_jumps, &m_jit);
                // Assertions are atomic to backtracking. Once assertion completes, we can do anything at backtracking: since assertion does not consume any characters,
                // this does not change character position at this place, thus, once it completes, no backtracking exists to change the status to do the following matching again.
                // Thus, we should just jump to corresponding ParentheticalAssertionBegin's backtracking state.
                op.m_jumps.append(m_jit.jump());
                break;
            }

            case YarrOpCode::MatchFailed:
                break;
            }
        } while (opIndex && !hasExceededCodeSizeLimit());
    }

    bool hasExceededCodeSizeLimit()
    {
        if (m_failureReason)
            return true;
        if (m_jit.m_assembler.buffer().codeSize() > Options::maximumRegExpJITCodeSize()) [[unlikely]] {
            m_failureReason = JITFailureReason::GeneratedCodeSizeTooLarge;
            return true;
        }
        return false;
    }

    // Compilation methods:
    // ====================

    // opCompileParenthesesSubpattern
    // Emits ops for a subpattern (set of parentheses). These consist
    // of a set of alternatives wrapped in an outer set of nodes for
    // the parentheses.
    // Supported types of parentheses are 'Once' (quantityMaxCount == 1),
    // 'Terminal' (non-capturing parentheses quantified as greedy
    // and infinite), and 0 based greedy / non-greedy quantified parentheses.
    // Alternatives will use the 'Simple' set of ops if either the
    // subpattern is terminal (in which case we will never need to
    // backtrack), or if the subpattern only contains one alternative.
    void opCompileParenthesesSubpattern(Checked<unsigned> checkedOffset, PatternTerm* term)
    {
        YarrOpCode parenthesesBeginOpCode;
        YarrOpCode parenthesesEndOpCode;
        YarrOpCode alternativeBeginOpCode = YarrOpCode::SimpleNestedAlternativeBegin;
        YarrOpCode alternativeNextOpCode = YarrOpCode::SimpleNestedAlternativeNext;
        YarrOpCode alternativeEndOpCode = YarrOpCode::SimpleNestedAlternativeEnd;

        if (!isSafeToRecurse()) [[unlikely]] {
            m_failureReason = JITFailureReason::ParenthesisNestedTooDeep;
            return;
        }

        if (term->quantityMaxCount == 1 && !term->parentheses.isCopy) {
            // Select the 'Once' nodes.
            parenthesesBeginOpCode = YarrOpCode::ParenthesesSubpatternOnceBegin;
            parenthesesEndOpCode = YarrOpCode::ParenthesesSubpatternOnceEnd;

            if (term->parentheses.isStringList) {
                // This is an anchored non-capturing string list parenthesis that can't backtrack, we use the 'string list' nodes.
                // We may need to reorder these if we have an EOL after.

                if (term->parentheses.isEOLStringList) {
                    PatternDisjunction* nestedDisjunction = term->parentheses.disjunction;
                    nestedDisjunction->m_alternatives.last()->m_isLastAlternative = false;

                    std::ranges::sort(nestedDisjunction->m_alternatives, [](auto& l, auto& r) {
                        return l->m_terms.size() > r->m_terms.size();
                    });
                    nestedDisjunction->m_alternatives.last()->m_isLastAlternative = true;
                }

                alternativeBeginOpCode = YarrOpCode::StringListAlternativeBegin;
                alternativeNextOpCode = YarrOpCode::StringListAlternativeNext;
                alternativeEndOpCode = YarrOpCode::StringListAlternativeEnd;
            } else if (term->parentheses.disjunction->m_alternatives.size() != 1) {
                // Otherwise, check if there is more than one alternative. If so, we cannot use the 'simple' nodes.
                alternativeBeginOpCode = YarrOpCode::NestedAlternativeBegin;
                alternativeNextOpCode = YarrOpCode::NestedAlternativeNext;
                alternativeEndOpCode = YarrOpCode::NestedAlternativeEnd;
            }
        } else if (term->parentheses.isTerminal) {
            // Select the 'Terminal' nodes.
            parenthesesBeginOpCode = YarrOpCode::ParenthesesSubpatternTerminalBegin;
            parenthesesEndOpCode = YarrOpCode::ParenthesesSubpatternTerminalEnd;
        } else {
#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
            if (term->quantityType == QuantifierType::FixedCount) {
                bool hasMultipleAlternatives = term->parentheses.disjunction->m_alternatives.size() != 1;
                bool hasBacktrackableContent = term->quantityMaxCount > 1 && disjunctionContainsBacktrackableContent(term->parentheses.disjunction);

                if (!hasBacktrackableContent && !hasMultipleAlternatives) {
                    // Single-alt FixedCount whose body has nothing backtrackable —
                    // capture support is also legal here because no inner term can
                    // observe an intermediate capture value (see ParenthesesSubpattern-
                    // FixedCountBegin's header for the full argument).
                    parenthesesBeginOpCode = YarrOpCode::ParenthesesSubpatternFixedCountBegin;
                    parenthesesEndOpCode = YarrOpCode::ParenthesesSubpatternFixedCountEnd;
                } else {
                    // Multi-alt FixedCount (needs to remember which alternative each
                    // iteration took) or backtrackable inner content (needs to roll
                    // state back across iterations) — use the ParenContext path.
                    m_containsNestedSubpatterns = true;
                    m_usesT2 = true;
                    parenthesesBeginOpCode = YarrOpCode::ParenthesesSubpatternBegin;
                    parenthesesEndOpCode = YarrOpCode::ParenthesesSubpatternEnd;
                }

                // If there is more than one alternative we cannot use the 'simple' nodes.
                if (hasMultipleAlternatives) {
                    alternativeBeginOpCode = YarrOpCode::NestedAlternativeBegin;
                    alternativeNextOpCode = YarrOpCode::NestedAlternativeNext;
                    alternativeEndOpCode = YarrOpCode::NestedAlternativeEnd;
                }
            } else {
                // Greedy/NonGreedy quantifiers: use generic ParenContext-based nodes.
                m_containsNestedSubpatterns = true;
                parenthesesBeginOpCode = YarrOpCode::ParenthesesSubpatternBegin;
                parenthesesEndOpCode = YarrOpCode::ParenthesesSubpatternEnd;

                // If there is more than one alternative we cannot use the 'simple' nodes.
                if (term->parentheses.disjunction->m_alternatives.size() != 1) {
                    alternativeBeginOpCode = YarrOpCode::NestedAlternativeBegin;
                    alternativeNextOpCode = YarrOpCode::NestedAlternativeNext;
                    alternativeEndOpCode = YarrOpCode::NestedAlternativeEnd;
                }
            }
#else
            // This subpattern is not supported by the JIT.
            m_failureReason = JITFailureReason::ParenthesizedSubpattern;
            return;
#endif
        }

        // Try first-character dispatch for a plain once-group with several
        // alternatives (its ops are the ParenthesesSubpatternOnce + Nested* set).
        // The sets must be computed before opCompileAlternative reorders terms.
        DispatchInfo* dispatch = nullptr;
        if (parenthesesBeginOpCode == YarrOpCode::ParenthesesSubpatternOnceBegin && alternativeBeginOpCode == YarrOpCode::NestedAlternativeBegin && Options::useRegExpAlternationDispatch())
            dispatch = tryPrepareDispatch(term, checkedOffset);
        if (Options::verboseRegExpCompilation() && term->parentheses.disjunction->m_alternatives.size() >= alternationDispatchMinAlternatives) {
            dumpFirstCharacterSets(*term->parentheses.disjunction);
            dataLogLn(dispatch ? "First-character dispatch enabled" : "First-character dispatch not used");
        }

        size_t parenBegin = m_ops.size();
        appendOp(parenthesesBeginOpCode);

        appendOp(alternativeBeginOpCode);
        m_ops.last().m_previousOp = notFound;
        m_ops.last().m_term = term;
        m_ops.last().m_dispatch = dispatch;
        m_ops.last().m_dispatchAlternativeIndex = 0;
        PatternDisjunction* disjunction = term->parentheses.disjunction;
        auto& alternatives = disjunction->m_alternatives;
        for (unsigned i = 0; i < alternatives.size(); ++i) {
            size_t lastOpIndex = m_ops.size() - 1;

            PatternAlternative* nestedAlternative = alternatives[i].get();
            {
                // Calculate how much input we need to check for, and if non-zero check.
                YarrOp& lastOp = m_ops[lastOpIndex];
                lastOp.m_checkAdjust = nestedAlternative->m_minimumSize;
                if ((term->quantityType == QuantifierType::FixedCount) && (term->quantityMaxCount == 1) && (term->type != PatternTerm::Type::ParentheticalAssertion))
                    lastOp.m_checkAdjust -= disjunction->m_minimumSize;

                Checked<unsigned, RecordOverflow> checkedOffsetResult(checkedOffset);
                checkedOffsetResult += lastOp.m_checkAdjust;

                if (checkedOffsetResult.hasOverflowed()) [[unlikely]] {
                    m_failureReason = JITFailureReason::OffsetTooLarge;
                    return;
                }

                lastOp.m_checkedOffset = checkedOffsetResult;
            }
            opCompileAlternative(m_ops[lastOpIndex].m_checkedOffset, nestedAlternative);

            size_t thisOpIndex = m_ops.size();
            appendOp(YarrOp(alternativeNextOpCode));

            YarrOp& lastOp = m_ops[lastOpIndex];
            YarrOp& thisOp = m_ops[thisOpIndex];

            lastOp.m_alternative = nestedAlternative;
            lastOp.m_nextOp = thisOpIndex;
            thisOp.m_previousOp = lastOpIndex;
            thisOp.m_term = term;
            thisOp.m_dispatch = dispatch;
            thisOp.m_dispatchAlternativeIndex = i + 1;
        }
        YarrOp& lastOp = m_ops.last();
        ASSERT(lastOp.m_op == alternativeNextOpCode);
        lastOp.m_op = alternativeEndOpCode;
        lastOp.m_alternative = nullptr;
        lastOp.m_nextOp = notFound;
        lastOp.m_checkedOffset = checkedOffset;
        if (dispatch)
            dispatch->endOpIndex = m_ops.size() - 1;

        size_t parenEnd = m_ops.size();
        appendOp(parenthesesEndOpCode);

        m_ops[parenBegin].m_term = term;
        m_ops[parenBegin].m_previousOp = notFound;
        m_ops[parenBegin].m_nextOp = parenEnd;
        m_ops[parenBegin].m_checkedOffset = checkedOffset;
        m_ops[parenEnd].m_term = term;
        m_ops[parenEnd].m_previousOp = parenBegin;
        m_ops[parenEnd].m_nextOp = notFound;
        m_ops[parenEnd].m_checkedOffset = checkedOffset;
    }

    // Lookbehind body mirroring.
    //
    // A lookbehind body matches right-to-left ending at the assertion point.
    // Matching backward over the input is equivalent to matching a term-reversed
    // copy of the body forward over the virtually reversed input, so we build
    // such a reversed copy and compile it with the standard forward machinery,
    // marking its ops Backward so the input-check/read/end-of-input primitives
    // use the leftward cursor variants.
    //
    // assignAlternativeOffsets numbers a stored term list from a given origin
    // using exactly the width rules of
    // YarrPatternConstructor::setupAlternativeOffsets. Applied to a mirrored
    // copy (stored reversed) it yields the virtual reversed coordinates; applied
    // to an ordinary forward body it renumbers that body from the origin. Frame
    // locations always stay those the pattern constructor assigned: the
    // original lookbehind body is never itself compiled, so a mirrored copy may
    // reuse its slots, and a renumbered forward body keeps its own.
    //
    // Returns false (with m_failureReason set) only on offset overflow.

    bool assignAlternativeOffsets(PatternAlternative* alternative, unsigned initialInputPosition)
    {
        if (!isSafeToRecurse()) [[unlikely]] {
            m_failureReason = JITFailureReason::ParenthesisNestedTooDeep;
            return false;
        }
        CheckedUint32 currentInputPosition = initialInputPosition;

        for (auto& term : alternative->m_terms) {
            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
                term.inputPosition = currentInputPosition;
                break;

            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                break;

            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                // Dynamic length: positioned but contributing no width, as in
                // setupAlternativeOffsets. Its frame slot (allocated there) is kept.
                term.inputPosition = currentInputPosition;
                break;

            case PatternTerm::Type::PatternCharacter:
                term.inputPosition = currentInputPosition;
                if (term.quantityType == QuantifierType::FixedCount) {
                    if (m_pattern.eitherUnicode()) {
                        CheckedUint32 tempCount = term.quantityMaxCount;
                        tempCount *= U16_LENGTH(term.patternCharacter);
                        currentInputPosition += tempCount;
                    } else
                        currentInputPosition += term.quantityMaxCount;
                }
                break;

            case PatternTerm::Type::CharacterClass:
                term.inputPosition = currentInputPosition;
                if (term.quantityType == QuantifierType::FixedCount) {
                    if (m_pattern.eitherUnicode()) {
                        if (term.characterClass->hasOneCharacterSize() && !term.invert()) {
                            CheckedUint32 tempCount = term.quantityMaxCount;
                            tempCount *= term.characterClass->hasNonBMPCharacters() ? 2 : 1;
                            currentInputPosition += tempCount;
                        } else
                            currentInputPosition += term.quantityMaxCount;
                    } else
                        currentInputPosition += term.quantityMaxCount;
                }
                break;

            case PatternTerm::Type::ParenthesesSubpattern:
                // Nested group positions continue the enclosing numbering, as in
                // setupAlternativeOffsets: the once shape (max count 1, not a copy)
                // pre-claims its body minimum and is positioned after it; every
                // counted/copied shape claims dynamically and is positioned before it.
                if (term.quantityMaxCount == 1 && !term.parentheses.isCopy) {
                    if (!assignDisjunctionOffsets(term.parentheses.disjunction, currentInputPosition))
                        return false;
                    if (term.quantityType == QuantifierType::FixedCount)
                        currentInputPosition += term.parentheses.disjunction->m_minimumSize;
                    term.inputPosition = currentInputPosition;
                } else {
                    term.inputPosition = currentInputPosition;
                    if (!assignDisjunctionOffsets(term.parentheses.disjunction, currentInputPosition))
                        return false;
                }
                break;

            case PatternTerm::Type::ParentheticalAssertion:
                // The assertion's own position is the virtual assertion point. A
                // nested lookbehind keeps its original 0-based body (mirrored and
                // renumbered on demand when compiled); a nested forward assertion's
                // body is numbered from this position, as in
                // setupAlternativeOffsets: the enclosing checkedOffset continues into
                // the body, so read offsets (checkedOffset - inputPosition) are the
                // same as any forward body's, and the body is renumbered from this
                // position to stay on that line -- recursively, through further
                // nesting. BOL/EOL inside such a body test the real assertion point
                // (see generateAssertionBOL/EOL), never the absolute inputPosition.
                term.inputPosition = currentInputPosition;
                if (term.matchDirection() != Backward) {
                    if (!assignDisjunctionOffsets(term.parentheses.disjunction, currentInputPosition))
                        return false;
                }
                break;

            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            if (currentInputPosition.hasOverflowed()) {
                m_failureReason = JITFailureReason::OffsetTooLarge;
                return false;
            }
        }

        // Mirroring reverses term order but must preserve the alternative's width.
        RELEASE_ASSERT(alternative->m_minimumSize == (currentInputPosition - initialInputPosition));
        return true;
    }

    bool assignDisjunctionOffsets(PatternDisjunction* disjunction, unsigned initialInputPosition)
    {
        for (auto& alternative : disjunction->m_alternatives) {
            if (!assignAlternativeOffsets(alternative.get(), initialInputPosition))
                return false;
        }
        return true;
    }

    // Structurally reverse `source` into `destination`. Nested plain groups are
    // deep-copied and reversed as part of the same backward match.
    bool mirrorAlternativeInto(PatternAlternative& source, PatternDisjunction& destination)
    {
        PatternAlternative* mirrored = destination.addNewAlternative(source.m_firstSubpatternId, Backward);
        mirrored->m_minimumSize = source.m_minimumSize;
        mirrored->m_lastSubpatternId = source.m_lastSubpatternId;
        mirrored->m_hasFixedSize = source.m_hasFixedSize;
        mirrored->m_containsBOL = source.m_containsBOL;
        mirrored->m_isLastAlternative = source.m_isLastAlternative;

        for (size_t i = source.m_terms.size(); i--;) {
            PatternTerm term = source.m_terms[i];

            switch (term.type) {
            case PatternTerm::Type::AssertionBOL:
            case PatternTerm::Type::AssertionEOL:
            case PatternTerm::Type::AssertionWordBoundary:
            case PatternTerm::Type::PatternCharacter:
            case PatternTerm::Type::CharacterClass:
            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                break;

            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                break;

            case PatternTerm::Type::ParenthesesSubpattern:
                // Groups of any quantifier are mirrored recursively: the paren
                // iteration/backtracking machinery keeps positions in saved frame
                // slots and per-iteration ParenContexts rather than deriving them
                // from counts, so it is direction-neutral. The string-list and
                // terminal group forms are only ever produced for the top-level
                // pattern's own alternatives, never inside a lookbehind body.
                ASSERT(!term.parentheses.isStringList);
                ASSERT(!term.parentheses.isEOLStringList);
                ASSERT(!term.parentheses.isTerminal);
                term.parentheses.disjunction = mirrorDisjunctionForLookbehind(term.parentheses.disjunction);
                if (!term.parentheses.disjunction)
                    return false;
                break;

            case PatternTerm::Type::ParentheticalAssertion:
                // A nested lookbehind keeps its 0-based body: it is mirrored into its
                // own copy on demand when compiled and never mutated in place. A nested
                // FORWARD assertion's body is about to be renumbered onto this frame's
                // line by assignAlternativeOffsets, so the mirror must own it -- a
                // shallow copy would rewrite the pattern's own body, which the
                // bytecode interpreter reuses if this JIT compile later bails.
                if (term.matchDirection() != Backward) {
                    term.parentheses.disjunction = copyForwardDisjunctionForMirror(term.parentheses.disjunction);
                    if (!term.parentheses.disjunction)
                        return false;
                }
                break;

            case PatternTerm::Type::DotStarEnclosure:
                // Only synthesized for the top-level pattern body (see
                // optimizeDotStarWrappedExpressions), never inside a lookbehind.
                RELEASE_ASSERT_NOT_REACHED();
            }

            mirrored->m_terms.append(term);
        }
        return true;
    }

    // Build the reversed copy of a lookbehind body disjunction (recursively
    // reversing nested groups), then assign virtual input positions from 0.
    // Returns null with m_failureReason set on unsupported content.
    PatternDisjunction* mirrorDisjunctionForLookbehind(PatternDisjunction* disjunction)
    {
        if (!isSafeToRecurse()) [[unlikely]] {
            m_failureReason = JITFailureReason::ParenthesisNestedTooDeep;
            return nullptr;
        }

        auto mirroredDisjunction = makeUnique<PatternDisjunction>();
        mirroredDisjunction->m_minimumSize = disjunction->m_minimumSize;
        mirroredDisjunction->m_callFrameSize = disjunction->m_callFrameSize;
        mirroredDisjunction->m_hasFixedSize = disjunction->m_hasFixedSize;

        for (auto& alternative : disjunction->m_alternatives) {
            if (!mirrorAlternativeInto(*alternative, *mirroredDisjunction))
                return nullptr;
        }

        PatternDisjunction* result = mirroredDisjunction.get();
        m_mirroredDisjunctions.append(WTF::move(mirroredDisjunction));
        return result;
    }

    // Structural (in-order) deep copy of a forward body reached from inside a
    // mirrored lookbehind body, so that renumbering it (assignAlternativeOffsets)
    // touches only mirror-owned terms and never the pattern's own disjunctions.
    // Nested groups and nested forward assertions are copied recursively for the
    // same reason; a nested lookbehind's body is left shared (it is mirrored into
    // its own copy on demand and never renumbered in place).
    bool copyForwardAlternativeInto(PatternAlternative& source, PatternDisjunction& destination)
    {
        PatternAlternative* copy = destination.addNewAlternative(source.m_firstSubpatternId, source.matchDirection());
        copy->m_minimumSize = source.m_minimumSize;
        copy->m_lastSubpatternId = source.m_lastSubpatternId;
        copy->m_hasFixedSize = source.m_hasFixedSize;
        copy->m_containsBOL = source.m_containsBOL;
        copy->m_startsWithBOL = source.m_startsWithBOL;
        copy->m_onceThrough = source.m_onceThrough;
        copy->m_isLastAlternative = source.m_isLastAlternative;

        copy->m_terms.reserveInitialCapacity(source.m_terms.size());
        for (auto& sourceTerm : source.m_terms) {
            PatternTerm term = sourceTerm;
            switch (term.type) {
            case PatternTerm::Type::ParenthesesSubpattern:
                term.parentheses.disjunction = copyForwardDisjunctionForMirror(term.parentheses.disjunction);
                if (!term.parentheses.disjunction)
                    return false;
                break;
            case PatternTerm::Type::ParentheticalAssertion:
                if (term.matchDirection() != Backward) {
                    term.parentheses.disjunction = copyForwardDisjunctionForMirror(term.parentheses.disjunction);
                    if (!term.parentheses.disjunction)
                        return false;
                }
                break;
            default:
                break;
            }
            copy->m_terms.append(term);
        }
        return true;
    }

    PatternDisjunction* copyForwardDisjunctionForMirror(PatternDisjunction* disjunction)
    {
        if (!isSafeToRecurse()) [[unlikely]] {
            m_failureReason = JITFailureReason::ParenthesisNestedTooDeep;
            return nullptr;
        }

        auto copiedDisjunction = makeUnique<PatternDisjunction>();
        copiedDisjunction->m_minimumSize = disjunction->m_minimumSize;
        copiedDisjunction->m_callFrameSize = disjunction->m_callFrameSize;
        copiedDisjunction->m_hasFixedSize = disjunction->m_hasFixedSize;

        for (auto& alternative : disjunction->m_alternatives) {
            if (!copyForwardAlternativeInto(*alternative, *copiedDisjunction))
                return nullptr;
        }

        PatternDisjunction* result = copiedDisjunction.get();
        m_mirroredDisjunctions.append(WTF::move(copiedDisjunction));
        return result;
    }

    // opCompileParentheticalAssertion
    // Emits ops for a parenthetical assertion. These consist of an
    // YarrOpCode::SimpleNestedAlternativeBegin/Next/End set of nodes wrapping
    // the alternatives, with these wrapped by an outer pair of
    // YarrOpCode::ParentheticalAssertionBegin/End nodes.
    // We can always use the OpSimpleNestedAlternative nodes in the
    // case of parenthetical assertions since these only ever match
    // once, and will never backtrack back into the assertion.
    void opCompileParentheticalAssertion(Checked<unsigned> checkedOffset, PatternTerm* term)
    {
        if (!isSafeToRecurse()) [[unlikely]] {
            m_failureReason = JITFailureReason::ParenthesisNestedTooDeep;
            return;
        }

        PatternDisjunction* disjunction = term->parentheses.disjunction;
        MatchDirection bodyDirection = term->matchDirection();
        if (bodyDirection == Backward) {
            // Lookbehind: compile a term-reversed mirrored copy of the body whose
            // input positions are in virtual (reversed) coordinates starting at 0.
            disjunction = mirrorDisjunctionForLookbehind(disjunction);
            if (!disjunction)
                return;
            if (!assignDisjunctionOffsets(disjunction, 0))
                return;
        }

        auto originalCheckedOffset = checkedOffset;
        size_t parenBegin = m_ops.size();
        // The Begin/End ops realign the index register to the assertion point;
        // they belong to the enclosing direction (m_direction is unchanged here).
        appendOp(YarrOpCode::ParentheticalAssertionBegin);
        m_ops.last().m_checkAdjust = checkedOffset - term->inputPosition;
        checkedOffset -= m_ops.last().m_checkAdjust;
        m_ops.last().m_checkedOffset = checkedOffset;

        // The body's ops are emitted in the body's own direction. A body starts a
        // fresh frame at position 0 whenever its coordinates are not the enclosing
        // forward line: a lookbehind body is mirrored and numbered from 0, and a
        // forward body nested inside a mirrored (Backward) body is renumbered from
        // 0 by assignAlternativeOffsets, since it cannot inherit mirrored offsets.
        // A forward body under a forward frame keeps the pattern layer's numbering
        // and continues the enclosing checkedOffset. The Begin op has already
        // placed the index register at the assertion point in every case.
        MatchDirection enclosingDirection = m_direction;
        m_direction = bodyDirection;
        if (bodyDirection == Backward)
            checkedOffset = 0;

        appendOp(YarrOpCode::SimpleNestedAlternativeBegin);
        m_ops.last().m_previousOp = notFound;
        m_ops.last().m_term = term;
        auto& alternatives = disjunction->m_alternatives;
        for (unsigned i = 0; i < alternatives.size(); ++i) {
            size_t lastOpIndex = m_ops.size() - 1;

            PatternAlternative* nestedAlternative = alternatives[i].get();
            {
                // Calculate how much input we need to check for, and if non-zero check.
                YarrOp& lastOp = m_ops[lastOpIndex];
                lastOp.m_checkAdjust = nestedAlternative->m_minimumSize;
                if ((term->quantityType == QuantifierType::FixedCount) && (term->type != PatternTerm::Type::ParentheticalAssertion))
                    lastOp.m_checkAdjust -= disjunction->m_minimumSize;
                lastOp.m_checkedOffset = checkedOffset + lastOp.m_checkAdjust;
            }
            opCompileAlternative(m_ops[lastOpIndex].m_checkedOffset, nestedAlternative);

            size_t thisOpIndex = m_ops.size();
            appendOp(YarrOp(YarrOpCode::SimpleNestedAlternativeNext));

            YarrOp& lastOp = m_ops[lastOpIndex];
            YarrOp& thisOp = m_ops[thisOpIndex];

            lastOp.m_alternative = nestedAlternative;
            lastOp.m_nextOp = thisOpIndex;
            thisOp.m_previousOp = lastOpIndex;
            thisOp.m_term = term;
        }
        YarrOp& lastOp = m_ops.last();
        ASSERT(lastOp.m_op == YarrOpCode::SimpleNestedAlternativeNext);
        lastOp.m_op = YarrOpCode::SimpleNestedAlternativeEnd;
        lastOp.m_alternative = nullptr;
        lastOp.m_nextOp = notFound;
        lastOp.m_checkedOffset = checkedOffset;

        // The End op is back in the enclosing direction.
        m_direction = enclosingDirection;

        size_t parenEnd = m_ops.size();
        appendOp(YarrOpCode::ParentheticalAssertionEnd);

        m_ops[parenBegin].m_term = term;
        m_ops[parenBegin].m_previousOp = notFound;
        m_ops[parenBegin].m_nextOp = parenEnd;
        m_ops[parenEnd].m_term = term;
        m_ops[parenEnd].m_previousOp = parenBegin;
        m_ops[parenEnd].m_nextOp = notFound;
        m_ops[parenEnd].m_checkedOffset = originalCheckedOffset;
    }

    // opCompileAlternative
    // Called to emit nodes for all terms in an alternative.
    // `beginsAtMatchStart` is true only for the pattern's own body alternatives:
    // a nested group's or assertion's alternative begins wherever its enclosing
    // term sits, so its head term's read says nothing about the character at the
    // match start (see YarrOp::m_readsStartCharacter).
    void opCompileAlternative(Checked<unsigned> checkedOffset, PatternAlternative* alternative, bool beginsAtMatchStart = false)
    {
        optimizeAlternative(alternative);

        // The first term reads the match start's character until some term can
        // move the frontier at runtime; from then on a read may land anywhere.
        bool frontierMayHaveMoved = !beginsAtMatchStart;
        for (unsigned i = 0; i < alternative->m_terms.size(); ++i) {
            PatternTerm* term = &alternative->m_terms[i];

            switch (term->type) {
            case PatternTerm::Type::ParenthesesSubpattern:
                opCompileParenthesesSubpattern(checkedOffset, term);
                frontierMayHaveMoved = true;
                break;

            case PatternTerm::Type::ParentheticalAssertion:
                opCompileParentheticalAssertion(checkedOffset, term);
                break;

            default: {
                appendOp(term);
                m_ops.last().m_checkedOffset = checkedOffset;
                // Only a literal or a class term makes a single fixed-position read of
                // the start character; assertions, forward references, and backreferences
                // (a variable-length span match, never the start's one code point) do not.
                bool readsInput = term->type == PatternTerm::Type::PatternCharacter || term->type == PatternTerm::Type::CharacterClass;
                if (readsInput) {
                    // Only a term that reads exactly once has its single read pinned to
                    // the match start; a quantified term's later iterations peek beyond
                    // it. The read variant only records that the start character is
                    // non-BMP (it never moves the frontier), so a literal or a class head
                    // that reads once at the start both qualify.
                    bool readsOnce = term->quantityType == QuantifierType::FixedCount && term->quantityMaxCount == 1;
                    m_ops.last().m_readsStartCharacter = !frontierMayHaveMoved && readsOnce && !term->inputPosition;
                    frontierMayHaveMoved = true;
                }
                break;
            }
            }
        }
    }

    // opCompileBody
    // This method compiles the body disjunction of the regular expression.
    // The body consists of two sets of alternatives - zero or more 'once
    // through' (BOL anchored) alternatives, followed by zero or more
    // repeated alternatives.
    // For each of these two sets of alteratives, if not empty they will be
    // wrapped in a set of YarrOpCode::BodyAlternativeBegin/Next/End nodes (with the
    // 'begin' node referencing the first alternative, and 'next' nodes
    // referencing any further alternatives. The begin/next/end nodes are
    // linked together in a doubly linked list. In the case of repeating
    // alternatives, the end node is also linked back to the beginning.
    // If no repeating alternatives exist, then a YarrOpCode::MatchFailed node exists
    // to return the failing result.
    void opCompileBody(PatternDisjunction* disjunction)
    {
        if (!isSafeToRecurse()) [[unlikely]] {
            m_failureReason = JITFailureReason::ParenthesisNestedTooDeep;
            return;
        }
        
        auto& alternatives = disjunction->m_alternatives;
        size_t currentAlternativeIndex = 0;

        // Emit the 'once through' alternatives.
        if (alternatives.size() && alternatives[0]->onceThrough()) {
            appendOp(YarrOp(YarrOpCode::BodyAlternativeBegin));
            m_ops.last().m_previousOp = notFound;

            do {
                size_t lastOpIndex = m_ops.size() - 1;

                PatternAlternative* alternative = alternatives[currentAlternativeIndex].get();
                m_ops[lastOpIndex].m_checkedOffset = alternative->m_minimumSize;
                opCompileAlternative(alternative->m_minimumSize, alternative, /* beginsAtMatchStart */ true);

                size_t thisOpIndex = m_ops.size();
                appendOp(YarrOp(YarrOpCode::BodyAlternativeNext));

                YarrOp& lastOp = m_ops[lastOpIndex];
                YarrOp& thisOp = m_ops[thisOpIndex];

                lastOp.m_alternative = alternative;
                lastOp.m_nextOp = thisOpIndex;
                thisOp.m_previousOp = lastOpIndex;
                
                ++currentAlternativeIndex;
            } while (currentAlternativeIndex < alternatives.size() && alternatives[currentAlternativeIndex]->onceThrough());

            YarrOp& lastOp = m_ops.last();

            ASSERT(lastOp.m_op == YarrOpCode::BodyAlternativeNext);
            lastOp.m_op = YarrOpCode::BodyAlternativeEnd;
            lastOp.m_alternative = nullptr;
            lastOp.m_nextOp = notFound;
            lastOp.m_checkedOffset = 0;
        }

        if (currentAlternativeIndex == alternatives.size()) {
            appendOp(YarrOp(YarrOpCode::MatchFailed));
            m_ops.last().m_checkedOffset = 0;
            return;
        }

        // Emit the repeated alternatives.
        size_t firstRepeatedAlternative = currentAlternativeIndex;
        BodyDispatchInfo* bodyDispatch = Options::useRegExpAlternationDispatch() ? tryPrepareBodyDispatch(disjunction, firstRepeatedAlternative) : nullptr;
        if (bodyDispatch)
            dataLogLnIf(Options::verboseRegExpCompilation(), "Body first-character dispatch enabled: ", bodyDispatch->alternativeCount, " alternatives");

        size_t repeatLoop = m_ops.size();
        appendOp(YarrOp(YarrOpCode::BodyAlternativeBegin));
        m_ops.last().m_previousOp = notFound;
        m_ops.last().m_bodyDispatch = bodyDispatch;
        m_ops.last().m_bodyDispatchAlternativeIndex = 0;

        if (disjunction->m_minimumSize && !m_pattern.sticky()) {
            // Collect BoyerMooreInfo if it is possible and profitable. BoyerMooreInfo will be used to emit fast skip path with large stride
            // at the beginning of the body alternatives.
            // We do not emit these fast path when RegExp has sticky flag. Sticky case does not need this since
            // it fails when the body alternatives fail to match with the current offset.
            auto bmInfo = BoyerMooreInfo::create(m_charSize, std::min<unsigned>(disjunction->m_minimumSize, BoyerMooreInfo::maxLength));
            if (collectBoyerMooreInfo(disjunction, currentAlternativeIndex, bmInfo.get())) {
                dataLogLnIf(YarrJITInternal::verbose, bmInfo.get());
                m_ops.last().m_bmInfo = bmInfo.ptr();
                m_bmInfos.append(WTF::move(bmInfo));
                m_usesT2 = true;
                if (m_sampleString)
                    m_sampler.sample(m_sampleString.value());
            } else
                dataLogLnIf(YarrJITInternal::verbose, "BM collection failed");

#if CPU(ARM64) || CPU(X86_64)
            // Try multi-pattern SIMD search for alternations with 2 fixed alternatives
            // This is more effective than bitmap lookahead for patterns like /agggtaaa|tttaccct/i
            if (!m_pattern.eitherUnicode() && m_charSize == CharSize::Char8 && alternatives.size() >= 2) {
                if (auto maskedInfo = MaskedAlternativeInfo::create(*disjunction, m_pattern.ignoreCase(), m_charSize)) {
                    dataLogLnIf(Options::verboseRegExpCompilation(), "Found multi-pattern SIMD candidate: ", alternatives.size(), " alternatives, minLen=", maskedInfo->minPatternLength);
                    auto info = makeUniqueRef<MaskedAlternativeInfo>(*maskedInfo);
                    m_ops.last().m_maskedAltInfo = info.ptr();
                    m_maskedAltInfos.append(WTF::move(info));
                }
            }
#endif

            // If every repeated alternative starts with a non-BMP character class whose members
            // all share the same UTF-16 lead surrogate, then any candidate match position must
            // have that lead at the first character. We hoist a single load+compare above the
            // per-alt dispatch and skip positions that cannot match any alternative.
            if (m_decodeSurrogatePairs && !m_ops.last().m_maskedAltInfo) {
                std::optional<char16_t> sharedLead;
                for (size_t i = currentAlternativeIndex; i < alternatives.size(); ++i) {
                    auto lead = findFirstTermSharedLead(alternatives[i].get());
                    if (!lead) {
                        sharedLead = std::nullopt;
                        break;
                    }

                    if (!sharedLead)
                        sharedLead = lead;
                    else if (sharedLead.value() != lead.value()) {
                        sharedLead = std::nullopt;
                        break;
                    }
                }
                if (sharedLead) {
                    dataLogLnIf(Options::verboseRegExpCompilation(), "Found shared-lead body-alt prefilter: U+", hex(sharedLead.value()));
                    m_ops.last().m_bodyAltSharedLead = sharedLead;
                }
            }
        }

        do {
            size_t lastOpIndex = m_ops.size() - 1;

            PatternAlternative* alternative = alternatives[currentAlternativeIndex].get();
            ASSERT(!alternative->onceThrough());
            m_ops[lastOpIndex].m_checkedOffset = alternative->m_minimumSize;
            opCompileAlternative(alternative->m_minimumSize, alternative, /* beginsAtMatchStart */ true);

            size_t thisOpIndex = m_ops.size();
            appendOp(YarrOp(YarrOpCode::BodyAlternativeNext));

            YarrOp& lastOp = m_ops[lastOpIndex];
            YarrOp& thisOp = m_ops[thisOpIndex];

            lastOp.m_alternative = alternative;
            lastOp.m_nextOp = thisOpIndex;
            thisOp.m_previousOp = lastOpIndex;
            // The Next op belongs to the alternative that follows the one just
            // compiled; its dispatch entry binds after that op's claim delta. The
            // End op keeps the pointer (with an index past the last alternative)
            // so the last alternative's failure flow can absorb the tree's
            // no-alternative leaves.
            thisOp.m_bodyDispatch = bodyDispatch;
            thisOp.m_bodyDispatchAlternativeIndex = (currentAlternativeIndex - firstRepeatedAlternative) + 1;

            ++currentAlternativeIndex;
        } while (currentAlternativeIndex < alternatives.size());
        YarrOp& lastOp = m_ops.last();
        ASSERT(lastOp.m_op == YarrOpCode::BodyAlternativeNext);
        lastOp.m_op = YarrOpCode::BodyAlternativeEnd;
        lastOp.m_alternative = nullptr;
        lastOp.m_nextOp = repeatLoop;
        lastOp.m_checkedOffset = 0;
    }

    // Walk an alternative's first significant term and report the shared UTF-16 lead surrogate
    // of its character class, if every codepoint that could match at position 0 shares the same
    // UTF-16 lead surrogate. Recurses through single-alternative groups so that capture groups
    // and non-capturing groups are transparent.
    std::optional<char16_t> findFirstTermSharedLead(PatternAlternative* alt)
    {
        if (alt->m_terms.isEmpty())
            return std::nullopt;

        PatternTerm& firstTerm = alt->m_terms[0];
        switch (firstTerm.type) {
        case PatternTerm::Type::PatternCharacter: {
            if (!firstTerm.quantityMinCount)
                return std::nullopt;
            char32_t cp = firstTerm.patternCharacter;
            if (U_IS_BMP(cp))
                return std::nullopt;
            return static_cast<char16_t>(U16_LEAD(cp));
        }

        case PatternTerm::Type::CharacterClass: {
            if (firstTerm.invert())
                return std::nullopt;
            if (!firstTerm.quantityMinCount)
                return std::nullopt;
            return firstTerm.characterClass->hasSharedLeadSurrogate();
        }

        case PatternTerm::Type::ParenthesesSubpattern: {
            if (!firstTerm.quantityMinCount)
                return std::nullopt;
            if (firstTerm.m_matchDirection != MatchDirection::Forward)
                return std::nullopt;
            if (firstTerm.m_invert)
                return std::nullopt;
            PatternDisjunction* sub = firstTerm.parentheses.disjunction;
            if (sub->m_alternatives.size() != 1)
                return std::nullopt;
            return findFirstTermSharedLead(sub->m_alternatives[0].get());
        }

        default:
            return std::nullopt;
        }
    }

    // First-character set analysis (see FirstCharacterSet).
    //
    // Adds the characters that `term`, matched at the start of what remains,
    // can begin with. Returns true if the term must consume at least one
    // character (so the alternative's first character is now fully accounted
    // for), false if the term can match the empty string and the analysis must
    // continue into the following term.
    bool addFirstCharactersOfTerm(const PatternTerm& term, FirstCharacterSet& set)
    {
        switch (term.type) {
        case PatternTerm::Type::AssertionBOL:
        case PatternTerm::Type::AssertionEOL:
        case PatternTerm::Type::AssertionWordBoundary:
        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
            // Zero-width: contributes no first character.
            return false;

        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
        case PatternTerm::Type::DotStarEnclosure:
            set.any = true;
            return true;

        case PatternTerm::Type::ParentheticalAssertion:
            // An assertion consumes no input; a positive lookahead could narrow the
            // set but ignoring it keeps the set a valid over-approximation.
            return false;

        case PatternTerm::Type::PatternCharacter: {
            char32_t ch = term.patternCharacter;
            if (term.ignoreCase() && isASCIIAlpha(ch)) {
                set.add(toASCIIUpper(ch));
                set.add(toASCIILower(ch));
            } else if (term.ignoreCase() && !isASCII(ch))
                set.any = true; // Non-ASCII case folding: don't try to be clever.
            else
                set.add(ch);
            return !!term.quantityMinCount;
        }

        case PatternTerm::Type::CharacterClass: {
            const CharacterClass* characterClass = term.characterClass;
            if (term.invert() || characterClass->m_anyCharacter || characterClass->m_table || term.ignoreCase())
                set.any = true;
            else {
                for (char32_t ch : characterClass->m_matches8)
                    set.add(ch);
                for (auto& range : characterClass->m_ranges8)
                    set.addRange(range.begin, range.end);
                if (!characterClass->m_matches32.isEmpty() || !characterClass->m_ranges32.isEmpty() || characterClass->hasStrings())
                    set.wide = true;
            }
            return !!term.quantityMinCount;
        }

        case PatternTerm::Type::ParenthesesSubpattern: {
            PatternDisjunction* disjunction = term.parentheses.disjunction;
            bool allConsumed = true;
            for (auto& alternative : disjunction->m_alternatives) {
                FirstCharacterSet nested = computeFirstCharacterSet(*alternative);
                set.merge(nested);
                allConsumed &= nested.consumed;
            }
            // The group only guarantees a consumed first character if it is
            // required (min count >= 1) and every alternative consumes.
            return term.quantityMinCount && allConsumed;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // First-character dispatch code generation.
    //
    // Emitted at the group's alternative Begin op, before any alternative body:
    //
    //   char = input[index - firstCharacterOffset]      ; inside claimed input
    //   [16-bit: char >= 0x100 -> wide chain]
    //   binary decision tree over char -> jump to chain head, or group failure
    //   chain stubs:  head_C:  frame.chainResume = &stub_C[1]; jump entry(alt i0)
    //                 stub_C[1]: frame.chainResume = &stub_C[2]; jump entry(alt i1)
    //                 ...        stub_C[n]: jump groupFailure
    //
    // A failing alternative releases its claim and jumps to frame.chainResume, so
    // it continues its own chain rather than a fixed successor. Entry labels of
    // later alternatives are bound as they are generated; stubs that target them
    // are patched then via DispatchInfo::pendingEntryJumps.

    // Chain stubs are all emitted at the Begin op, before any alternative's
    // entry point exists, so every stub target is a forward reference: collected
    // per alternative here and linked when the alternative's entry is generated.
    void emitJumpToDispatchEntry(DispatchInfo& info, unsigned alternativeIndex)
    {
        info.pendingEntryJumps[alternativeIndex].append(m_jit.jump());
    }

    void bindDispatchEntry(DispatchInfo& info, unsigned alternativeIndex)
    {
        info.pendingEntryJumps[alternativeIndex].link(&m_jit);
        info.pendingEntryJumps[alternativeIndex].clear();
    }

    // Binary decision tree over the character's Latin-1 value. `ranges` are
    // maximal [begin,end] intervals with a constant target, sorted and contiguous
    // over 0..255. For group dispatch the target is a chain id; for body dispatch
    // it is the index of the first alternative that can start with the range.
    struct DispatchRange {
        unsigned begin;
        unsigned end;
        unsigned chainId;
    };

    void bindBodyDispatchEntry(BodyDispatchInfo& info, unsigned alternativeIndex)
    {
        info.pendingEntryLabels[alternativeIndex] = m_jit.label();
        info.pendingEntryJumps[alternativeIndex].link(&m_jit);
        info.pendingEntryJumps[alternativeIndex].clear();
    }

    // Advance the body search past the current position after a dispatched
    // alternative (or the no-candidate leaf) failed. `claimedMinimumSize` is the
    // claim the index currently holds (index = matchStart + claimedMinimumSize,
    // those characters verified in bounds). The next candidate position is
    // matchStart + 1, which needs index' = (matchStart + 1) + min[0]. This is
    // the last alternative's loop-back trampoline specialised to an arbitrary
    // claim: record the new match start, move the claim, and re-check input
    // only when the claim grew (a shrunk or unchanged claim is already covered
    // by the characters this position verified).
    void emitBodyDispatchAdvance(BodyDispatchInfo& info, YarrOp& beginOp, unsigned claimedMinimumSize)
    {
        ASSERT(claimedMinimumSize);
        if (!m_pattern.m_body->m_hasFixedSize) {
            // New match start = old start + 1 = index - (claimedMinimumSize - 1).
            if (claimedMinimumSize == 1)
                setMatchStart(m_regs.index);
            else {
                m_jit.sub32(m_regs.index, MacroAssembler::Imm32(claimedMinimumSize - 1), m_regs.regT0);
                setMatchStart(m_regs.regT0);
            }
        }
        // The Begin op's m_reentry is the loop-back target the last
        // alternative's trampoline uses: when a search prefilter runs it points
        // into the search's own resume path, which continues scanning from the
        // advanced position (its backtrack entry re-establishes its state).
        unsigned nextClaim = beginOp.m_alternative->m_minimumSize + 1; // relative to the old start
        if (nextClaim > claimedMinimumSize) {
            m_jit.add32(MacroAssembler::Imm32(nextClaim - claimedMinimumSize), m_regs.index);
            checkInput().linkTo(beginOp.m_reentry, &m_jit);
            // Not enough input for even the first alternative at the next
            // position: hand off to the body loop's input-exhaustion flow.
            if (info.haveFirstInputCheckFailed)
                m_jit.jump().linkTo(info.firstInputCheckFailed, &m_jit);
            else
                info.inputExhaustedJumps.append(m_jit.jump());
        } else {
            if (claimedMinimumSize > nextClaim)
                m_jit.sub32(MacroAssembler::Imm32(claimedMinimumSize - nextClaim), m_regs.index);
            m_jit.jump(beginOp.m_reentry);
        }
    }

    // Group the routing table into maximal ranges of equal target so a
    // decision tree gets one leaf per run rather than one per character.
    Vector<DispatchRange, 16> bodyDispatchRanges(BodyDispatchInfo& info)
    {
        Vector<DispatchRange, 16> ranges;
        for (unsigned ch = 0; ch < 256;) {
            unsigned target = info.firstAlternativeForCharacter[ch];
            unsigned begin = ch;
            while (ch < 256 && info.firstAlternativeForCharacter[ch] == target)
                ++ch;
            ranges.append({ begin, ch - 1, target });
        }
        return ranges;
    }

    // Binary decision tree over the character; `leaf(target)` emits the code
    // for a target alternative index (or BodyDispatchInfo::noAlternative).
    template<typename LeafFunction>
    void emitBodyDispatchRangeTree(MacroAssembler::RegisterID character, const Vector<DispatchRange, 16>& ranges, unsigned low, unsigned high, const LeafFunction& leaf)
    {
        if (low == high) {
            leaf(ranges[low].chainId);
            return;
        }
        unsigned middle = (low + high + 1) / 2;
        MacroAssembler::Jump upper = m_jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(ranges[middle].begin));
        emitBodyDispatchRangeTree(character, ranges, low, middle - 1, leaf);
        upper.link(&m_jit);
        emitBodyDispatchRangeTree(character, ranges, middle, high, leaf);
    }

    // With `character` holding the routing character, dispatch over the
    // Latin-1 ranges plus (16-bit strings) the wide leaf, via `leaf(target)`.
    template<typename LeafFunction>
    void emitBodyDispatchOverCharacter(BodyDispatchInfo& info, MacroAssembler::RegisterID character, const LeafFunction& leaf)
    {
        auto ranges = bodyDispatchRanges(info);
        if (m_charSize == CharSize::Char16) {
            MacroAssembler::Jump wide = m_jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(256));
            emitBodyDispatchRangeTree(character, ranges, 0, ranges.size() - 1, leaf);
            wide.link(&m_jit);
            leaf(info.wideFirstAlternative);
        } else
            emitBodyDispatchRangeTree(character, ranges, 0, ranges.size() - 1, leaf);
    }

    YarrOp& bodyDispatchAlternativeOp(YarrOp& beginOp, unsigned target)
    {
        YarrOp* targetOp = &beginOp;
        for (unsigned i = 0; i < target; ++i)
            targetOp = &m_ops[targetOp->m_nextOp];
        return *targetOp;
    }

    // Body first-character dispatch (see BodyDispatchInfo). Emitted for the
    // repeated body's Begin op once the index sits at the first alternative's
    // claimed frontier (index = matchStart + min[0]):
    //
    //   char = input[index - min[0]]            ; the shared first character
    //   [16-bit: char >= 0x100 -> wide leaf]
    //   binary decision tree over char -> leaf(k), or the no-candidate leaf
    //   leaf(k):  index += min[k] - min[0]  (checked if it grows); jump entry(k)
    //
    // entry(k) is bound right after alternative k's Begin/Next input-claim
    // delta, so an entered alternative sees exactly the index its own op would
    // have produced. Because first-character sets are pairwise disjoint (see
    // tryPrepareBodyDispatch), k is the only alternative that can match here:
    // its failure (see the BodyAlternativeNext backtrack case) and the
    // no-candidate leaf both advance the search via emitBodyDispatchAdvance
    // instead of walking the remaining alternatives.
    void generateBodyDispatch(YarrOp& beginOp)
    {
        BodyDispatchInfo& info = *beginOp.m_bodyDispatch;
        const MacroAssembler::RegisterID character = m_regs.regT0;
        unsigned firstMinimumSize = beginOp.m_alternative->m_minimumSize;

        JIT_COMMENT(m_jit, "body first-character dispatch");
        readCharacter(info.firstCharacterOffset, character);
        emitBodyDispatchOverCharacter(info, character, [&](unsigned target) {
            if (target == BodyDispatchInfo::noAlternative) {
                // No repeated alternative can start with this character: the
                // search advances. The index is at the first alternative's claim.
                emitBodyDispatchAdvance(info, beginOp, firstMinimumSize);
                return;
            }
            // Move the claim from the first alternative's to the target's, then
            // enter the target where its own op's delta would have left the index.
            YarrOp& targetOp = bodyDispatchAlternativeOp(beginOp, target);
            unsigned targetMinimumSize = targetOp.m_alternative->m_minimumSize;
            if (targetMinimumSize > firstMinimumSize) {
                m_jit.add32(MacroAssembler::Imm32(targetMinimumSize - firstMinimumSize), m_regs.index);
                // Claiming more input than the first alternative did: if it is not
                // there, the target cannot match here; that is the target's own
                // input-check failure (its op's failure flow gives the input back).
                targetOp.m_jumps.append(jumpIfNoAvailableInput());
            } else if (firstMinimumSize > targetMinimumSize)
                m_jit.sub32(MacroAssembler::Imm32(firstMinimumSize - targetMinimumSize), m_regs.index);
            info.pendingEntryJumps[target].append(m_jit.jump());
        });
    }

    // Tail dispatch, emitted in the backtrack pass at the body's
    // first-input-check-failed point in place of the plain in-order
    // input-exhaustion chain. That chain re-entered shorter alternatives blindly
    // near the end of input; under body dispatch an entered alternative must be
    // the sole candidate for the match-start character (or its dispatched
    // failure would advance past the true candidate). So the tail routes on
    // the character too: index = matchStart + min[0] here (min[0] characters
    // unavailable), and for the character at matchStart the sole candidate k
    // is entered only if min[k] fits the remaining input. Every path that finds
    // no fitting candidate falls out with the index normalised to the last
    // alternative's claim (matchStart + min[last]), the contract of the loop's
    // advance-and-recheck continuation that follows.
    void generateBodyDispatchTail(BodyDispatchInfo& info, YarrOp& beginOp, YarrOp& lastAlternativeOp)
    {
        const MacroAssembler::RegisterID character = m_regs.regT0;
        unsigned firstMinimumSize = beginOp.m_alternative->m_minimumSize;
        unsigned lastMinimumSize = lastAlternativeOp.m_alternative->m_minimumSize;

        // Move the index from `fromClaim` to the last alternative's claim.
        auto normalizeToLastClaim = [&](unsigned fromClaim) {
            if (lastMinimumSize > fromClaim)
                m_jit.add32(MacroAssembler::Imm32(lastMinimumSize - fromClaim), m_regs.index);
            else if (fromClaim > lastMinimumSize)
                m_jit.sub32(MacroAssembler::Imm32(fromClaim - lastMinimumSize), m_regs.index);
        };

        MacroAssembler::JumpList noFittingCandidate; // arrivals hold the last alternative's claim

        // No character at matchStart at all (matchStart >= length): no candidate.
        JIT_COMMENT(m_jit, "body first-character dispatch (tail)");
        m_jit.sub32(m_regs.index, MacroAssembler::Imm32(firstMinimumSize), character);
        MacroAssembler::Jump noCharacter = m_jit.branch32(MacroAssembler::AboveOrEqual, character, m_regs.length);
        readCharacter(info.firstCharacterOffset, character);
        emitBodyDispatchOverCharacter(info, character, [&](unsigned target) {
            if (target == BodyDispatchInfo::noAlternative) {
                normalizeToLastClaim(firstMinimumSize);
                noFittingCandidate.append(m_jit.jump());
                return;
            }
            // Enter the sole candidate only if its minimum fits the input left.
            YarrOp& targetOp = bodyDispatchAlternativeOp(beginOp, target);
            unsigned targetMinimumSize = targetOp.m_alternative->m_minimumSize;
            if (targetMinimumSize > firstMinimumSize)
                m_jit.add32(MacroAssembler::Imm32(targetMinimumSize - firstMinimumSize), m_regs.index);
            else if (firstMinimumSize > targetMinimumSize)
                m_jit.sub32(MacroAssembler::Imm32(firstMinimumSize - targetMinimumSize), m_regs.index);
            checkInput().linkTo(info.pendingEntryLabels[target], &m_jit); // fits: enter k
            normalizeToLastClaim(targetMinimumSize); // does not fit
            noFittingCandidate.append(m_jit.jump());
        });
        noCharacter.link(&m_jit);
        normalizeToLastClaim(firstMinimumSize);

        // Every earlier alternative's own input-check failures (index at that
        // alternative's claim) mean the same: no fitting candidate here. The
        // last alternative's own such failures are already linked by the loop's
        // continuation below, at exactly the state these arrivals reproduce.
        // (The Begin op's own m_jumps were already linked at the tail's entry.)
        YarrOp* alternativeOp = &m_ops[beginOp.m_nextOp];
        while (alternativeOp != &lastAlternativeOp) {
            if (!alternativeOp->m_jumps.empty()) {
                noFittingCandidate.append(m_jit.jump()); // skip the pad on fall-through
                alternativeOp->m_jumps.link(&m_jit);
                normalizeToLastClaim(alternativeOp->m_alternative->m_minimumSize);
            }
            alternativeOp = &m_ops[alternativeOp->m_nextOp];
        }
        noFittingCandidate.link(&m_jit);
    }

    // An alternative that is a fixed-length literal string (only fixed-count
    // pattern characters) can never be backtracked into: it either matches its
    // exact characters or not. Such alternatives are emitted as inline compare
    // blocks inside the chain (no frame store/load, no indirect jump), with a
    // static fall-through to the next chain element on mismatch.
    // Inlining unrolls one compare per character, so it is only for genuinely
    // short literals; a counted literal like a{200000000} keeps the entered
    // path, whose fixed-count loop runs at match time instead of being unrolled.
    static constexpr unsigned maxInlineLiteralLength = 32;

    bool isInlineLiteralAlternative(const PatternAlternative& alternative)
    {
        if (alternative.m_terms.isEmpty() || !alternative.m_hasFixedSize)
            return false;
        if (alternative.m_minimumSize > maxInlineLiteralLength)
            return false;
        for (auto& term : alternative.m_terms) {
            if (term.type != PatternTerm::Type::PatternCharacter)
                return false;
            if (term.quantityType != QuantifierType::FixedCount || !term.quantityMaxCount || term.quantityMinCount != term.quantityMaxCount)
                return false;
            if (term.matchDirection() != Forward)
                return false;
        }
        return true;
    }

    // Emit an inline literal alternative as part of a chain: claim its extra
    // input, compare its characters, and on a full match join the group's
    // success path; on mismatch release the claim and fall to the next chain
    // element. The return-address slot points at an unwind thunk that releases
    // the claim and continues the chain, so backtracking into this matched
    // alternative from after the group tries the following alternatives.
    void emitInlineLiteralAlternative(DispatchInfo& info, const PatternAlternative& alternative, MacroAssembler::JumpList& toNextElement)
    {
        const MacroAssembler::RegisterID character = m_regs.regT0;
        unsigned extraClaim = alternative.m_minimumSize - info.disjunction->m_minimumSize;
        Checked<unsigned> frameChecked = info.enclosingCheckedOffset + extraClaim;

        MacroAssembler::JumpList mismatch;
        if (extraClaim) {
            m_jit.add32(MacroAssembler::Imm32(extraClaim), m_regs.index);
            mismatch.append(m_jit.branch32(MacroAssembler::Above, m_regs.index, m_regs.length));
        }
        for (auto& term : alternative.m_terms) {
            char32_t ch = term.patternCharacter;
            for (unsigned repeat = 0; repeat < term.quantityMaxCount; ++repeat) {
                Checked<unsigned> offset = frameChecked - term.inputPosition - repeat;
                mismatch.append(jumpIfCharNotEquals(ch, offset, character, term.ignoreCase()));
            }
        }

        // Full match: record where to unwind to, then skip to the group's End.
        MacroAssembler::DataLabelPtr returnAddress = storeToFrameWithPatch(info.frameLocation + BackTrackInfoParenthesesOnce::returnAddressIndex());
        m_ops[info.endOpIndex].m_jumps.append(m_jit.jump());

        // Unwind thunk (reached only through the return-address slot when
        // something after the group fails): release the claim and continue the
        // chain by falling through to the next element.
        MacroAssembler::Label unwind(m_jit);
        m_backtrackingState.addReturnAddressRecord(returnAddress, unwind);
        mismatch.link(&m_jit);
        if (extraClaim)
            m_jit.sub32(MacroAssembler::Imm32(extraClaim), m_regs.index);
        toNextElement.append(m_jit.jump());
    }

    // Emit one chain: literal alternatives are compared inline; other
    // alternatives are entered through a stub that stores the address of the
    // following element as the chain-resume point before jumping in. The final
    // element is the group failure. Returns the label of the chain head.
    MacroAssembler::Label emitDispatchChainStubs(DispatchInfo& info, const Vector<unsigned, 4>& alternativeIndices)
    {
        JIT_COMMENT(m_jit, "dispatch chain (", alternativeIndices.size(), " alternatives)");
        MacroAssembler::Label head = m_jit.label();
        Vector<MacroAssembler::DataLabelPtr, 4> pendingStores;
        MacroAssembler::JumpList toNextElement;
        for (unsigned index : alternativeIndices) {
            // Bind the previous element's continuations to here.
            if (!pendingStores.isEmpty())
                m_backtrackingState.addReturnAddressRecord(pendingStores.takeLast(), m_jit.label());
            toNextElement.link(&m_jit);
            toNextElement.clear();

            const PatternAlternative& alternative = *info.disjunction->m_alternatives[index];
            if (isInlineLiteralAlternative(alternative))
                emitInlineLiteralAlternative(info, alternative, toNextElement);
            else {
                pendingStores.append(storeToFrameWithPatch(info.frameLocation + BackTrackInfoParenthesesOnce::chainResumeIndex()));
                emitJumpToDispatchEntry(info, index);
            }
        }
        // Chain exhausted: no more applicable alternatives at this position.
        if (!pendingStores.isEmpty())
            m_backtrackingState.addReturnAddressRecord(pendingStores.takeLast(), m_jit.label());
        toNextElement.link(&m_jit);
        info.groupFailJumps.append(m_jit.jump());
        return head;
    }

    // Binary decision tree over the character's Latin-1 value. `ranges` are
    // maximal [begin,end] intervals with a constant chain id, sorted and
    // contiguous over 0..255. Leaves jump to the chain head (or group failure).
    void emitDispatchTree(DispatchInfo& info, MacroAssembler::RegisterID character, const Vector<DispatchRange, 16>& ranges, unsigned low, unsigned high)
    {
        if (low == high) {
            unsigned chainId = ranges[low].chainId;
            if (chainId == DispatchInfo::noChain)
                info.groupFailJumps.append(m_jit.jump());
            else
                m_jit.jump().linkTo(info.chains[chainId].head, &m_jit);
            return;
        }
        unsigned middle = (low + high + 1) / 2;
        MacroAssembler::Jump upper = m_jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(ranges[middle].begin));
        emitDispatchTree(info, character, ranges, low, middle - 1);
        upper.link(&m_jit);
        emitDispatchTree(info, character, ranges, middle, high);
    }

    void generateDispatch(YarrOp& op)
    {
        DispatchInfo& info = *op.m_dispatch;
        const MacroAssembler::RegisterID character = m_regs.regT0;

        JIT_COMMENT(m_jit, "first-character dispatch");
        readCharacter(info.firstCharacterOffset, character);

        MacroAssembler::JumpList wideCharacter;
        if (m_charSize == CharSize::Char16)
            wideCharacter.append(m_jit.branch32(MacroAssembler::AboveOrEqual, character, MacroAssembler::TrustedImm32(256)));

        // Emit the chain stubs first so tree leaves can jump backward to them;
        // the tree itself is entered by jumping over the stubs.
        MacroAssembler::Jump toTree = m_jit.jump();
        for (auto& chain : info.chains)
            chain.head = emitDispatchChainStubs(info, chain.alternativeIndices);
        if (m_charSize == CharSize::Char16) {
            if (info.wideChain.alternativeIndices.isEmpty()) {
                MacroAssembler::Label failHead(m_jit);
                info.wideChain.head = failHead;
                info.groupFailJumps.append(m_jit.jump());
            } else
                info.wideChain.head = emitDispatchChainStubs(info, info.wideChain.alternativeIndices);
        }
        toTree.link(&m_jit);
        if (m_charSize == CharSize::Char16)
            wideCharacter.linkTo(info.wideChain.head, &m_jit);

        // Group the routing table into maximal ranges of equal chain id.
        Vector<DispatchRange, 16> ranges;
        for (unsigned ch = 0; ch < 256;) {
            unsigned chainId = info.chainForCharacter[ch];
            unsigned begin = ch;
            while (ch < 256 && info.chainForCharacter[ch] == chainId)
                ++ch;
            ranges.append({ begin, ch - 1, chainId });
        }
        emitDispatchTree(info, character, ranges, 0, ranges.size() - 1);
    }

    // Decide whether a nested group's alternatives are worth dispatching on the
    // first character, and if so build the DispatchInfo (chains and character
    // routing). Called at op-compile time, before the alternatives' ops are
    // emitted (so the first term of each alternative is still in position order).
    // First-character dispatch pays for its extra read only from a handful of
    // alternatives up; below that the sequential chain is faster.
    static constexpr size_t alternationDispatchMinAlternatives = 4;
    static constexpr size_t alternationDispatchMinTotalSize = 12;
    static constexpr size_t bodyDispatchMatchOnlyMinTotalSize = 64;

    DispatchInfo* tryPrepareDispatch(PatternTerm* term, Checked<unsigned> checkedOffset)
    {
        // Only once-quantified fixed groups take the ParenthesesSubpatternOnce +
        // NestedAlternative path whose frame holds BackTrackInfoParenthesesOnce.
        if (term->type != PatternTerm::Type::ParenthesesSubpattern)
            return nullptr;
        if (term->quantityType != QuantifierType::FixedCount || term->quantityMinCount != 1 || term->quantityMaxCount != 1)
            return nullptr;
        if (term->parentheses.isCopy || term->parentheses.isTerminal || term->parentheses.isStringList)
            return nullptr;
        if (term->matchDirection() != Forward || m_direction != Forward)
            return nullptr;
        if (m_decodeSurrogatePairs)
            return nullptr;

        PatternDisjunction* disjunction = term->parentheses.disjunction;
        auto& alternatives = disjunction->m_alternatives;
        if (alternatives.size() < alternationDispatchMinAlternatives)
            return nullptr;
        // The first character is only guaranteed inside claimed input when every
        // alternative must consume at least one character.
        if (!disjunction->m_minimumSize)
            return nullptr;
        size_t totalMinimumSize = 0;
        for (auto& alternative : alternatives)
            totalMinimumSize += alternative->m_minimumSize;
        if (totalMinimumSize < alternationDispatchMinTotalSize)
            return nullptr;

        auto info = makeUniqueRef<DispatchInfo>();
        Vector<FirstCharacterSet, 8> sets; // per alternative; only needed to build the routing
        unsigned anyCount = 0;
        for (auto& alternative : alternatives) {
            FirstCharacterSet set = computeFirstCharacterSet(*alternative);
            if (set.any)
                ++anyCount;
            sets.append(WTF::move(set));
        }
        // Dispatch is pointless when most alternatives can start with anything.
        if (anyCount * 4 > alternatives.size())
            return nullptr;

        // Route each Latin-1 character to the ordered chain of alternatives whose
        // set contains it; identical chains are shared (linear dedupe: chains are
        // few in practice).
        for (unsigned ch = 0; ch < 256; ++ch) {
            Vector<unsigned, 4> members;
            for (unsigned i = 0; i < alternatives.size(); ++i) {
                if (sets[i].matches(ch))
                    members.append(i);
            }
            if (members.isEmpty()) {
                info->chainForCharacter[ch] = DispatchInfo::noChain;
                continue;
            }
            unsigned chainId = DispatchInfo::noChain;
            for (unsigned c = 0; c < info->chains.size(); ++c) {
                if (info->chains[c].alternativeIndices == members) {
                    chainId = c;
                    break;
                }
            }
            if (chainId == DispatchInfo::noChain) {
                chainId = info->chains.size();
                DispatchInfo::Chain chain;
                chain.alternativeIndices = WTF::move(members);
                info->chains.append(WTF::move(chain));
            }
            info->chainForCharacter[ch] = chainId;
        }
        // Bound the emitted stub code.
        unsigned stubCount = 0;
        for (auto& chain : info->chains)
            stubCount += chain.alternativeIndices.size();
        if (info->chains.size() > 96 || stubCount > 768)
            return nullptr;
        if (m_charSize == CharSize::Char16) {
            for (unsigned i = 0; i < alternatives.size(); ++i) {
                if (sets[i].matchesWide())
                    info->wideChain.alternativeIndices.append(i);
            }
        }

        // The group's first character is at the frame position where the group
        // starts, inside the input the enclosing alternative already claimed.
        Checked<unsigned> groupStart = term->inputPosition - disjunction->m_minimumSize;
        if (checkedOffset < term->inputPosition)
            return nullptr;
        info->firstCharacterOffset = checkedOffset - groupStart;
        if (info->firstCharacterOffset.hasOverflowed())
            return nullptr;
        info->frameLocation = term->frameLocation;
        info->disjunction = disjunction;
        info->enclosingCheckedOffset = checkedOffset;
        info->pendingEntryJumps.resize(alternatives.size());

        DispatchInfo* result = info.ptr();
        m_dispatchInfos.append(WTF::move(info));
        return result;
    }

    void dumpFirstCharacterSets(PatternDisjunction& disjunction)
    {
        dataLogLn("First-character sets for ", disjunction.m_alternatives.size(), " alternatives:");
        for (size_t i = 0; i < disjunction.m_alternatives.size(); ++i) {
            FirstCharacterSet set = computeFirstCharacterSet(*disjunction.m_alternatives[i]);
            dataLog("  alt ", i, ": ");
            if (set.any)
                dataLog("ANY");
            else {
                for (unsigned ch = 0; ch < 256; ++ch) {
                    if (set.latin1.get(ch)) {
                        if (isASCIIPrintable(ch))
                            dataLog("'", static_cast<char>(ch), "'");
                        else
                            dataLog("\\x", hex(ch, 2));
                    }
                }
                if (set.wide)
                    dataLog(" +wide");
            }
            dataLogLn();
        }
    }

    FirstCharacterSet computeFirstCharacterSet(const PatternAlternative& alternative)
    {
        FirstCharacterSet set;
        if (!isSafeToRecurse()) [[unlikely]] {
            set.any = true;
            return set;
        }
        for (auto& term : alternative.m_terms) {
            if (addFirstCharactersOfTerm(term, set)) {
                set.consumed = true;
                break;
            }
            if (set.any) {
                set.consumed = true;
                break;
            }
        }
        // An alternative that can match without consuming a character can start
        // at any character (its "first character" is whatever follows).
        if (!set.consumed)
            set.any = true;
        return set;
    }

    // Decide whether the body's repeated alternatives can be dispatched on the
    // first character, and if so build the routing (character -> the SOLE
    // alternative that can start with it). Called from opCompileBody before the
    // repeated alternatives' ops are emitted.
    //
    // Dispatch requires the alternatives' first-character sets to be pairwise
    // disjoint. Then each character has exactly one candidate, so once that
    // candidate fails no other alternative can match at this position and the
    // right action is to advance the search -- a static decision, needing no
    // per-match state and hence no Yarr frame. (Overlapping sets are left to
    // the plain alternative chain, whose per-alternative first-character
    // rejection is already as cheap as a decision-tree node.)
    BodyDispatchInfo* tryPrepareBodyDispatch(PatternDisjunction* disjunction, size_t firstRepeated)
    {
        // The advance stub jumps into the body's re-scan loop, which sticky
        // patterns do not have (they never advance), and it does not model the
        // non-BMP first-character read adjustment used by unicode patterns.
        if (m_direction != Forward || m_decodeSurrogatePairs || m_pattern.sticky() || m_pattern.eitherUnicode())
            return nullptr;
#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
        if (m_canUseFirstNonBMPCharacterOptimization)
            return nullptr;
#endif

        auto& alternatives = disjunction->m_alternatives;
        size_t count = alternatives.size() - firstRepeated;
        if (count < alternationDispatchMinAlternatives)
            return nullptr;

        // Every alternative must consume its first character inside the input
        // its Begin/Next op claimed; a zero-minimum alternative has no first
        // character to route on.
        size_t totalMinimumSize = 0;
        for (size_t i = firstRepeated; i < alternatives.size(); ++i) {
            if (!alternatives[i]->m_minimumSize)
                return nullptr;
            totalMinimumSize += alternatives[i]->m_minimumSize;
        }
        if (totalMinimumSize < alternationDispatchMinTotalSize)
            return nullptr;
        if (m_executionMode != ExecutionMode::IncludeSubpatterns && totalMinimumSize <= bodyDispatchMatchOnlyMinTotalSize)
            return nullptr;

        Vector<FirstCharacterSet, 8> sets;
        unsigned wideCount = 0;
        for (size_t i = firstRepeated; i < alternatives.size(); ++i) {
            FirstCharacterSet set = computeFirstCharacterSet(*alternatives[i]);
            // An `any` set overlaps every other alternative: no sole candidate.
            if (set.any)
                return nullptr;
            if (set.matchesWide())
                ++wideCount;
            sets.append(WTF::move(set));
        }
        // A character >= 0x100 (16-bit strings) needs a sole candidate too.
        if (m_charSize == CharSize::Char16 && wideCount > 1)
            return nullptr;

        auto info = makeUniqueRef<BodyDispatchInfo>();
        info->alternativeCount = count;
        for (unsigned ch = 0; ch < 256; ++ch) {
            unsigned candidate = BodyDispatchInfo::noAlternative;
            for (unsigned i = 0; i < count; ++i) {
                if (!sets[i].matches(ch))
                    continue;
                if (candidate != BodyDispatchInfo::noAlternative)
                    return nullptr; // two alternatives can start with ch: not disjoint
                candidate = i;
            }
            info->firstAlternativeForCharacter[ch] = candidate;
        }
        if (m_charSize == CharSize::Char16) {
            for (unsigned i = 0; i < count; ++i) {
                if (sets[i].matchesWide()) {
                    info->wideFirstAlternative = i;
                    break;
                }
            }
        }
        // The dispatch reads the shared first character of every repeated
        // alternative: back from the first alternative's claimed frontier
        // (index = matchStart + min[0]) to the match start.
        info->firstCharacterOffset = alternatives[firstRepeated]->m_minimumSize;
        info->pendingEntryJumps.resize(count);
        info->pendingEntryLabels.resize(count);

        BodyDispatchInfo* result = info.ptr();
        m_bodyDispatchInfos.append(WTF::move(info));
        return result;
    }

    std::optional<unsigned> collectBoyerMooreInfoFromTerm(PatternTerm& term, unsigned cursor, BoyerMooreInfo& bmInfo)
    {
        switch (term.type) {
        case PatternTerm::Type::AssertionBOL:
        case PatternTerm::Type::AssertionEOL:
        case PatternTerm::Type::AssertionWordBoundary:
            // Conservatively say any assertions just match.
            return cursor;

        case PatternTerm::Type::NumberedBackReference:
        case PatternTerm::Type::NamedBackReference:
            return std::nullopt;

        case PatternTerm::Type::NumberedForwardReference:
        case PatternTerm::Type::NamedForwardReference:
            return cursor;

        case PatternTerm::Type::ParenthesesSubpattern: {
            // Right now, we only support /(...)/ or /(...)?/ case.
            PatternDisjunction* disjunction = term.parentheses.disjunction;
            if (term.quantityType != QuantifierType::FixedCount && term.quantityType != QuantifierType::Greedy)
                return std::nullopt;
            if (term.quantityMaxCount != 1)
                return std::nullopt;
            if (term.m_matchDirection != MatchDirection::Forward)
                return std::nullopt;
            if (term.m_invert)
                return std::nullopt;

            auto& alternatives = disjunction->m_alternatives;
            std::optional<unsigned> minimumCursor;
            for (unsigned i = 0; i < alternatives.size(); ++i) {
                PatternAlternative* alternative = alternatives[i].get();
                unsigned alternativeCursor = cursor;
                for (unsigned index = 0; index < alternative->m_terms.size() && alternativeCursor < bmInfo.length(); ++index) {
                    PatternTerm& term = alternative->m_terms[index];
                    std::optional<unsigned> nextCursor = collectBoyerMooreInfoFromTerm(term, alternativeCursor, bmInfo);
                    if (!nextCursor) {
                        dataLogLnIf(YarrJITInternal::verbose, "Shortening to ", alternativeCursor);
                        bmInfo.shortenLength(alternativeCursor);
                        break;
                    }
                    alternativeCursor = nextCursor.value();
                }
                if (!minimumCursor)
                    minimumCursor = alternativeCursor;
                else if (minimumCursor.value() != alternativeCursor) {
                    // Alternatives have different size.
                    // Let's say we have /(aaa|b)c/. Then, we would like to create BM info,
                    //
                    //     offset     0 1
                    //     characters a a
                    //                b c
                    //
                    // And we do not want to create 2, 3, 4 offsets since it changes based on whether we pick "aaa" or "b".
                    // So, when we encounter (aaa|b), after applying each alternative to BMInfo, we cut BMInfo candidate length
                    // with the shortest + 1 size, in this case "2".
                    if (minimumCursor.value() > alternativeCursor)
                        minimumCursor = alternativeCursor;
                    dataLogLnIf(YarrJITInternal::verbose, "Shortening to ", minimumCursor.value() + 1);
                    bmInfo.shortenLength(minimumCursor.value() + 1);
                }
            }

            if (term.quantityType == QuantifierType::FixedCount)
                cursor = minimumCursor.value();
            else {
                // Let's see /(aaaa|bbbb)?c/. In this case, we do not update the cursor since "(aaaa|bbbb)" is optional.
                // And let's shorten the candidate to "1" in this case since we do not want to apply "c" to all possible subsequent cases.
                dataLogLnIf(YarrJITInternal::verbose, "Shortening to ", cursor + 1);
                bmInfo.shortenLength(cursor + 1);
            }
            return cursor;
        }

        case PatternTerm::Type::ParentheticalAssertion:
            // An assertion consumes no input, so it contributes no characters at any
            // offset of the leading-character sets. A lookbehind constrains input
            // before the match start (not tracked here); a lookahead only narrows what
            // the following terms may match, so ignoring it keeps the sets a valid
            // over-approximation. Conservatively say it just matches, like BOL/EOL/\b.
            return cursor;

        case PatternTerm::Type::DotStarEnclosure:
            return std::nullopt;

        case PatternTerm::Type::CharacterClass: {
            if (term.quantityType != QuantifierType::FixedCount && term.quantityType != QuantifierType::Greedy)
                return std::nullopt;
            if (term.quantityMaxCount != 1 && term.quantityType != QuantifierType::FixedCount)
                return std::nullopt;
            if (term.inputPosition != cursor)
                return std::nullopt;
            auto& characterClass = *term.characterClass;
            // In Unicode mode, a class which can match a non-BMP code point consumes one or two code
            // units, so the offsets of subsequent terms are not fixed. End the BM window here to avoid
            // false negatives (e.g. /(.A)\1/u).
            if (m_decodeSurrogatePairs && (term.invert() || characterClass.hasNonBMPCharacters()))
                return std::nullopt;
            auto addCharacterClass = [&](unsigned index) {
                if (term.invert() || characterClass.m_anyCharacter) {
                    bmInfo.setAll(index);
                    return;
                }
                if (!characterClass.m_ranges32.isEmpty())
                    bmInfo.addRanges(index, characterClass.m_ranges32);
                if (!characterClass.m_matches32.isEmpty())
                    bmInfo.addCharacters(index, characterClass.m_matches32);
                if (!characterClass.m_ranges8.isEmpty())
                    bmInfo.addRanges(index, characterClass.m_ranges8);
                if (!characterClass.m_matches8.isEmpty())
                    bmInfo.addCharacters(index, characterClass.m_matches8);
            };

            if (term.quantityType == QuantifierType::FixedCount) {
                unsigned count = term.quantityMaxCount;
                for (unsigned i = 0; i < count && cursor < bmInfo.length(); ++i)
                    addCharacterClass(cursor++);
                return cursor;
            }

            // If this is greedy one-character pattern "a?", we should not increase cursor.
            // If we see greedy pattern, then we cut bmInfo here to avoid possibility explosion.
            addCharacterClass(cursor);
            bmInfo.shortenLength(cursor + 1);
            return cursor;
        }
        case PatternTerm::Type::PatternCharacter: {
            if (term.quantityType != QuantifierType::FixedCount && term.quantityType != QuantifierType::Greedy)
                return std::nullopt;
            if (term.quantityMaxCount != 1 && term.quantityType != QuantifierType::FixedCount)
                return std::nullopt;
            if (term.inputPosition != cursor)
                return std::nullopt;
            if (U16_LENGTH(term.patternCharacter) != 1 && m_decodeSurrogatePairs)
                return std::nullopt;
            // For case-insesitive compares, non-ascii characters that have different
            // upper & lower case representations are already converted to a character class.
            ASSERT(!term.ignoreCase() || isASCIIAlpha(term.patternCharacter) || isCanonicallyUnique(term.patternCharacter, m_canonicalMode));
            auto addCharacter = [&](unsigned index) {
                if (term.ignoreCase() && isASCIIAlpha(term.patternCharacter)) {
                    bmInfo.set(index, toASCIIUpper(term.patternCharacter));
                    bmInfo.set(index, toASCIILower(term.patternCharacter));
                } else
                    bmInfo.set(index, term.patternCharacter);
            };

            if (term.quantityType == QuantifierType::FixedCount) {
                unsigned count = term.quantityMaxCount;
                for (unsigned i = 0; i < count && cursor < bmInfo.length(); ++i)
                    addCharacter(cursor++);
                return cursor;
            }

            // If this is greedy one-character pattern "a?", we should not increase cursor.
            // If we see greedy pattern, then we cut bmInfo here to avoid possibility explosion.
            addCharacter(cursor);
            bmInfo.shortenLength(cursor + 1);
            return cursor;
        }
        }
        return std::nullopt;
    }

    bool collectBoyerMooreInfo(PatternDisjunction* disjunction, size_t currentAlternativeIndex, BoyerMooreInfo& bmInfo)
    {
        // If we have a searching pattern /abcdef/, then we can check the 6th character against a set of {a, b, c, d, e, f}.
        // If it does not match, we can shift 6 characters. We use this strategy since this way can be extended easily to support
        // disjunction, character-class, and ignore-cases. For example, in the case of /(?:abc|def)/, we can check 3rd character
        // against {a, b, c, d, e, f} and shift 3 characters if it does not match.
        //
        // Then, the best way to perform the above shifting is that finding the longest character sequence which does not have
        // many candidates. In the case of /[a-z]aaaaaaa[a-z]/, we can extract "aaaaaaa" sequence and check 8th character against {a}.
        // If it does not match, then we can shift 7 characters (length of "aaaaaaa"). This shifting is better than using "[a-z]aaaaaaa[a-z]"
        // sequence and {a-z} set since {a-z} set will almost always match.
        //
        // We first collect possible characters for each character position. Then, apply heuristics to extract good character sequence from
        // that and construct fast searching with long stride.

        ASSERT(disjunction->m_minimumSize);

        // FIXME: Support non-fixed-sized lookahead (e.g. /.*abc/ and extract "abc" sequence).
        // https://bugs.webkit.org/show_bug.cgi?id=228612
        auto& alternatives = disjunction->m_alternatives;
        for (; currentAlternativeIndex < alternatives.size(); ++currentAlternativeIndex) {
            unsigned cursor = 0;
            PatternAlternative* alternative = alternatives[currentAlternativeIndex].get();
            for (unsigned index = 0; index < alternative->m_terms.size() && cursor < bmInfo.length(); ++index) {
                PatternTerm& term = alternative->m_terms[index];
                std::optional<unsigned> nextCursor = collectBoyerMooreInfoFromTerm(term, cursor, bmInfo);
                if (!nextCursor) {
                    dataLogLnIf(YarrJITInternal::verbose, "Shortening to ", cursor);
                    bmInfo.shortenLength(cursor);
                    break;
                }
                cursor = nextCursor.value();
            }
        }
        return bmInfo.length();
    }

    std::span<const BoyerMooreBitmap::Map::WordType> getBoyerMooreBitmap(const BoyerMooreBitmap::Map& map)
    {
        if (auto existing = m_boyerMooreData->tryReuseBoyerMooreBitmap(map); existing.size())
            return existing;

        auto heapMap = makeUniqueRef<BoyerMooreBitmap::Map>(map);
        auto pointer = heapMap->storage();
        m_bmMaps.append(WTF::move(heapMap));
        return pointer;
    }

    // Generate the scalar Boyer-Moore search loop.
    // This handles both the characters fast path (1-2 candidate characters) and the bitmap path.
    // Parameters:
    //   - map: the Boyer-Moore bitmap
    //   - charactersFastPath: optional fast path for 1-2 character candidates
    //   - strideLength: how much to advance on each iteration (endIndex - beginIndex)
    //   - endIndex: the end index of the BM range
    //   - checkedOffset: the offset being checked
    //   - matched: JumpList to append match jumps to
    void generateBoyerMooreScalarLoop(const BoyerMooreBitmap::Map& map, const BoyerMooreFastCandidates& charactersFastPath, unsigned strideLength, unsigned endIndex, unsigned checkedOffset, MacroAssembler::JumpList& matched)
    {
        if (charactersFastPath.isValid() && !charactersFastPath.isEmpty()) {
            static_assert(BoyerMooreFastCandidates::maxSize == 2);
            dataLogLnIf(Options::verboseRegExpCompilation(), "Found characters fastpath lookahead ", charactersFastPath);
            JIT_COMMENT(m_jit, "BMSearch characters fastpath");
            auto loopHead = m_jit.label();
            readCharacterRaw(checkedOffset - endIndex + 1, m_regs.regT0);
            matched.append(m_jit.branch32(MacroAssembler::Equal, m_regs.regT0, MacroAssembler::TrustedImm32(charactersFastPath.at(0))));
            if (charactersFastPath.size() > 1)
                matched.append(m_jit.branch32(MacroAssembler::Equal, m_regs.regT0, MacroAssembler::TrustedImm32(charactersFastPath.at(1))));
            jumpIfAvailableInput(strideLength).linkTo(loopHead, &m_jit);
            return;
        }

        dataLogLnIf(Options::verboseRegExpCompilation(), "Found bitmap lookahead count:(", map.count(), ")");
        auto span = getBoyerMooreBitmap(map);
        JIT_COMMENT(m_jit, "BMSearch bitmap lookahead");
        ASSERT(span.size());
        m_jit.move(MacroAssembler::TrustedImmPtr(span.data()), m_regs.regT1);
        auto loopHead = m_jit.label();
        readCharacterRaw(checkedOffset - endIndex + 1, m_regs.regT0);
#if CPU(ARM64) || CPU(RISCV64)
        static_assert(sizeof(BoyerMooreBitmap::Map::WordType) == sizeof(uint64_t));
        static_assert(1 << 6 == 64);
        static_assert(1 << (6 + 1) == BoyerMooreBitmap::Map::size());
        m_jit.extractUnsignedBitfield32(m_regs.regT0, MacroAssembler::TrustedImm32(6), MacroAssembler::TrustedImm32(1), m_regs.regT2);
        m_jit.load64(MacroAssembler::BaseIndex(m_regs.regT1, m_regs.regT2, MacroAssembler::TimesEight), m_regs.regT2);
        m_jit.urshift64(m_regs.regT0, m_regs.regT2);
        matched.append(m_jit.branchTest64(MacroAssembler::NonZero, m_regs.regT2, MacroAssembler::TrustedImm32(1)));
#elif CPU(X86_64)
        static_assert(sizeof(BoyerMooreBitmap::Map::WordType) == sizeof(uint64_t));
        static_assert(1 << 6 == 64);
        static_assert(1 << (6 + 1) == BoyerMooreBitmap::Map::size());
        m_jit.urshift32(m_regs.regT0, MacroAssembler::TrustedImm32(6), m_regs.regT2);
        m_jit.and32(MacroAssembler::TrustedImm32(1), m_regs.regT2);
        m_jit.load64(MacroAssembler::BaseIndex(m_regs.regT1, m_regs.regT2, MacroAssembler::TimesEight), m_regs.regT2);
        matched.append(m_jit.branchTestBit64(MacroAssembler::NonZero, m_regs.regT2, m_regs.regT0));
#else
        static_assert(sizeof(BoyerMooreBitmap::Map::WordType) == sizeof(uint32_t));
        static_assert(1 << 5 == 32);
        static_assert(1 << (5 + 2) == BoyerMooreBitmap::Map::size());
        m_jit.move(m_regs.regT0, m_regs.regT2);
        m_jit.urshift32(MacroAssembler::TrustedImm32(5), m_regs.regT2);
        m_jit.and32(MacroAssembler::TrustedImm32(0b11), m_regs.regT2);
        m_jit.load32(MacroAssembler::BaseIndex(m_regs.regT1, m_regs.regT2, MacroAssembler::TimesFour), m_regs.regT2);
        m_jit.urshift32(m_regs.regT0, m_regs.regT2);
        matched.append(m_jit.branchTest32(MacroAssembler::NonZero, m_regs.regT2, MacroAssembler::TrustedImm32(1)));
#endif
        jumpIfAvailableInput(strideLength).linkTo(loopHead, &m_jit);
    }

#if CPU(ARM64) || CPU(X86_64)
    // Generate SIMD-accelerated multi-pattern search for alternation patterns like /agggtaaa|tttaccct/i:
    // The idea comes from the SkipUntilOneOfMasked optimization from V8.
    //
    // Register allocation:
    //   Pattern constants (set once before loop, never modified):
    //   - vectorTemp0 = chars1 (masked)
    //   - vectorTemp1 = mask1
    //   - vectorTemp2 = chars2 (masked)
    //   - vectorTemp3 = mask2
    //   - vectorTemp4 = tbl extraction mask
    //
    //   Input data (loaded fresh each iteration):
    //   - vectorInput0-3 = input at offsets 0-3
    //
    //   Scratch for computation (clobbered each iteration):
    //   - vectorScratch0-3 = scratch for AND/CMEQ
    //
    // Algorithm:
    //   1. Load 16 bytes at 4 offsets (checking 4 consecutive starting positions)
    //   2. Check pattern 1 (chars1/mask1), check pattern 2 (chars2/mask2)
    //   3. If matches: fall through to scalar matching
    //   4. advance 16, loop
    //
    // Returns a label to the SIMD loop head (after constant setup) for efficient backtracking.
    // The caller should use this label as the reentry point to avoid re-executing constant setup.
    struct MultiPatternSIMDResult {
        MacroAssembler::Label simdLoopHead;
        MacroAssembler::Label backtrackTarget; // Scalar loop for efficient retry after verification failure
    };

    std::optional<MultiPatternSIMDResult> generateMultiPatternSIMDSearch(const MaskedAlternativeInfo& info, unsigned checkedOffset, MacroAssembler::JumpList& matched)
    {
        // Only for Latin1 (8-bit characters)
        if (m_charSize != CharSize::Char8)
            return std::nullopt;

        // Call can clobber SIMD registers. We avoid this case. Practically speaking,
        // this happens when m_charSize is not Char8. So this is covered by the previous `m_charSize != CharSize::Char8` check.
        // But doing this check explicitly for safety.
        if (mayCall())
            return std::nullopt;

        // Only use SIMD if we have valid SIMD registers
        if (m_regs.vectorTemp0 == InvalidFPRReg)
            return std::nullopt;

        // Need the new input/scratch registers
        if (m_regs.vectorInput0 == InvalidFPRReg)
            return std::nullopt;

        if (checkedOffset > 0x7fffffff)
            return std::nullopt;

#if CPU(X86_64)
        if (!MacroAssembler::supportsAVX())
            return std::nullopt;
#endif

        auto baseOffset = Checked<int32_t, RecordOverflow>(-static_cast<int32_t>(checkedOffset));
        int32_t minCharsNeeded = 16 + 3; // Need 19 chars from current position
        auto totalOffset = minCharsNeeded + baseOffset;
        if (totalOffset.hasOverflowed())
            return std::nullopt;

        JIT_COMMENT(m_jit, "Multi-pattern SIMD search (", info.numAlternatives, " alternatives)");

        // ==================== SETUP PATTERN CONSTANTS (OUTSIDE LOOP) ====================
        // These are set once and NEVER modified inside the loop.
        // - Skip vectorTemp0/vectorTemp1 (not needed, go straight to pattern checks)
        // - vectorTemp0 = maskedChars1, vectorTemp1 = mask1 (for pattern 1)

        // vectorTemp0 = chars1 (masked)
        uint32_t maskedChars1 = info.alternatives[0].chars & info.alternatives[0].mask;
        v128_t maskedChars1Vector { };
        maskedChars1Vector.u32x4[0] = maskedChars1;
        maskedChars1Vector.u32x4[1] = maskedChars1;
        maskedChars1Vector.u32x4[2] = maskedChars1;
        maskedChars1Vector.u32x4[3] = maskedChars1;
        m_jit.move128ToVector(maskedChars1Vector, m_regs.vectorTemp0);

        // vectorTemp1 = mask1
        uint32_t mask1 = info.alternatives[0].mask;
        v128_t mask1Vector { };
        mask1Vector.u32x4[0] = mask1;
        mask1Vector.u32x4[1] = mask1;
        mask1Vector.u32x4[2] = mask1;
        mask1Vector.u32x4[3] = mask1;
        m_jit.move128ToVector(mask1Vector, m_regs.vectorTemp1);

        // vectorTemp2 = chars2 (masked)
        uint32_t maskedChars2 = info.alternatives[1].chars & info.alternatives[1].mask;
        v128_t maskedChars2Vector { };
        maskedChars2Vector.u32x4[0] = maskedChars2;
        maskedChars2Vector.u32x4[1] = maskedChars2;
        maskedChars2Vector.u32x4[2] = maskedChars2;
        maskedChars2Vector.u32x4[3] = maskedChars2;
        if (maskedChars1 == maskedChars2)
            m_jit.moveVector(m_regs.vectorTemp0, m_regs.vectorTemp2);
        else
            m_jit.move128ToVector(maskedChars2Vector, m_regs.vectorTemp2);

        // vectorTemp3 = mask2
        uint32_t mask2 = info.alternatives[1].mask;
        v128_t mask2Vector { };
        mask2Vector.u32x4[0] = mask2;
        mask2Vector.u32x4[1] = mask2;
        mask2Vector.u32x4[2] = mask2;
        mask2Vector.u32x4[3] = mask2;
        if (mask1 == mask2)
            m_jit.moveVector(m_regs.vectorTemp1, m_regs.vectorTemp3);
        else
            m_jit.move128ToVector(mask2Vector, m_regs.vectorTemp3);

#if CPU(ARM64)
        // vectorTemp4 = TBL extraction mask (extracts byte 0 of each 32-bit word from 2-register table)
        // Indices: 0, 4, 8, 12, 16, 20, 24, 28 = 0x1c1814100c080400 (little-endian)
        constexpr uint64_t tblMask = 0x1c1814100c080400ULL;
        m_jit.move64ToDouble(MacroAssembler::TrustedImm64(tblMask), m_regs.vectorTemp4);
#endif

        // ==================== SIMD LOOP ====================
        // Bounds check at bottom, single compare instruction.
        // Pre-compute the maximum index for SIMD processing: length - (16 + 3 + baseOffset)
        // This allows a single compare instead of add+compare.
        MacroAssembler::JumpList scalarLoop;

        // Backtrack entry point - re-computes threshold since scalar loop clobbers regT1
        // When verification fails and backtracks, we need to re-compute the SIMD threshold
        // because the scalar loop at <268>+ uses regT1 as a scratch register.
        auto backtrackEntry = m_jit.label();

        // Check for underflow: if length < totalOffset, skip SIMD entirely
        // This prevents the threshold calculation from wrapping to a huge value
        scalarLoop.append(m_jit.branchSub32(MacroAssembler::Signed, m_regs.length, MacroAssembler::TrustedImm32(totalOffset), m_regs.regT1));

        // Initial bounds check before entering loop - need index <= length - totalOffset (upper bound)
        scalarLoop.append(m_jit.branch32(MacroAssembler::Above, m_regs.index, m_regs.regT1));

        // Also need index >= -baseOffset (lower bound) when baseOffset is negative
        // This prevents reading before the start of the string
        if (baseOffset < 0)
            scalarLoop.append(m_jit.branch32(MacroAssembler::Below, m_regs.index, MacroAssembler::TrustedImm32(-baseOffset)));

        auto simdLoopHead = m_jit.label();

        // Calculate base load address: input + index
        // We incorporate baseOffset into the load addresses below to save an instruction.
        m_jit.add64(m_regs.input, m_regs.index, m_regs.regT0);

        // Load 16 bytes at 4 offsets into vectorInput0-3 (these are reloaded each iteration)
        // baseOffset is incorporated into the immediate offset to avoid an extra add instruction.
        m_jit.loadVector(MacroAssembler::Address(m_regs.regT0, baseOffset + 0), m_regs.vectorInput0);
        m_jit.loadVector(MacroAssembler::Address(m_regs.regT0, baseOffset + 1), m_regs.vectorInput1);
        m_jit.loadVector(MacroAssembler::Address(m_regs.regT0, baseOffset + 2), m_regs.vectorInput2);
        m_jit.loadVector(MacroAssembler::Address(m_regs.regT0, baseOffset + 3), m_regs.vectorInput3);

        auto maskAndCompareJump = [&](uint32_t mask, FPRReg charsFPR, FPRReg maskFPR) {
            // ALL ANDs first
            auto s0 = m_regs.vectorInput0;
            auto s1 = m_regs.vectorInput1;
            auto s2 = m_regs.vectorInput2;
            auto s3 = m_regs.vectorInput3;

            if (mask != 0xffffffffU) {
                s0 = m_regs.vectorScratch0;
                s1 = m_regs.vectorScratch1;
                s2 = m_regs.vectorScratch2;
                s3 = m_regs.vectorScratch3;
                m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorInput0, maskFPR, s0);
                m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorInput1, maskFPR, s1);
                m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorInput2, maskFPR, s2);
                m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorInput3, maskFPR, s3);
            }

            // ALL CMEQs next
            m_jit.compareIntegerVector(MacroAssembler::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, s0, charsFPR, m_regs.vectorScratch0);
            m_jit.compareIntegerVector(MacroAssembler::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, s1, charsFPR, m_regs.vectorScratch1);
            m_jit.compareIntegerVector(MacroAssembler::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, s2, charsFPR, m_regs.vectorScratch2);
            m_jit.compareIntegerVector(MacroAssembler::Equal, SIMDInfo { SIMDLane::i32x4, SIMDSignMode::None }, s3, charsFPR, m_regs.vectorScratch3);

            // ORRs at the end
            m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorScratch0, m_regs.vectorScratch1, m_regs.vectorScratch0);
            m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorScratch2, m_regs.vectorScratch3, m_regs.vectorScratch1);

#if CPU(ARM64)
            // TBL2: extract byte 0 of each 32-bit word from {scratch0, scratch1}
            m_jit.vectorSwizzle2(m_regs.vectorScratch0, m_regs.vectorScratch1, m_regs.vectorTemp4, m_regs.vectorScratch2);
            m_jit.moveDoubleTo64(m_regs.vectorScratch2, m_regs.regT0);

            // Check if pattern matched anywhere - if so, fall through to scalar loop to find exact position
            // SIMD only tells us there's a match SOMEWHERE in the 16-char window, not WHERE
            scalarLoop.append(m_jit.branchTest64(MacroAssembler::NonZero, m_regs.regT0));
#else
            m_jit.vectorOr(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorScratch0, m_regs.vectorScratch1, m_regs.vectorScratch0);
            scalarLoop.append(m_jit.branchTest128(MacroAssembler::NonZero, m_regs.vectorScratch0));
#endif
        };

        // ==================== CHECK PATTERN 1 ====================
        // Input data still in vectorInput0-3, pattern constants still in vectorTemp0-1

        maskAndCompareJump(mask1, m_regs.vectorTemp0, m_regs.vectorTemp1);

        // ==================== CHECK PATTERN 2 ====================
        // Input data still in vectorInput0-3, pattern constants still in vectorTemp2-3

        maskAndCompareJump(mask2, m_regs.vectorTemp2, m_regs.vectorTemp3);

        // Neither pattern matched at any of the 16 positions checked.
        // The SIMD loop checked 16 distinct starting positions (P through P+15),
        // so we can safely advance by 16.
        // Bounds check at bottom of loop.
        m_jit.add32(MacroAssembler::TrustedImm32(16), m_regs.index);
        m_jit.branch32(MacroAssembler::BelowOrEqual, m_regs.index, m_regs.regT1).linkTo(simdLoopHead, &m_jit);
        // Fall through to scalar path when bounds check fails

        // ==================== SCALAR PRE-FILTER LOOP ====================
        // This handles the tail positions that can't be processed by SIMD.

        scalarLoop.link(m_jit);
        auto scalarLoopHead = m_jit.label();
        MacroAssembler::JumpList failed;

        // Bounds: index <= length keeps the body in range, and also covers
        // the 4-byte load below since MaskedAlternativeInfo::create guarantees
        // checkedOffset >= 4. baseOffset (= -checkedOffset) is therefore < 0.
        ASSERT(baseOffset < 0);
        failed.append(m_jit.branch32(MacroAssembler::Above, m_regs.index, m_regs.length));
        failed.append(m_jit.branch32(MacroAssembler::Below, m_regs.index, MacroAssembler::TrustedImm32(-baseOffset)));

        // Calculate load address: input + index
        // We incorporate baseOffset into the load address below to save an instruction.
        m_jit.add64(m_regs.input, m_regs.index, m_regs.regT0);

        // Load 4 bytes at current position (baseOffset incorporated into offset)
        m_jit.load32(MacroAssembler::Address(m_regs.regT0, baseOffset), m_regs.regT1);

        // Pattern 1: (word & mask1) == maskedChars1
        if (mask1 == 0xffffffffU)
            matched.append(m_jit.branch32(MacroAssembler::Equal, m_regs.regT1, MacroAssembler::TrustedImm32(maskedChars1)));
        else {
            m_jit.and32(MacroAssembler::TrustedImm32(mask1), m_regs.regT1, m_regs.regT0);
            matched.append(m_jit.branch32(MacroAssembler::Equal, m_regs.regT0, MacroAssembler::TrustedImm32(maskedChars1)));
        }

        // Pattern 2: (word & mask2) == maskedChars2
        if (mask2 == 0xffffffffU)
            matched.append(m_jit.branch32(MacroAssembler::Equal, m_regs.regT1, MacroAssembler::TrustedImm32(maskedChars2)));
        else {
            m_jit.and32(MacroAssembler::TrustedImm32(mask2), m_regs.regT1, m_regs.regT0);
            matched.append(m_jit.branch32(MacroAssembler::Equal, m_regs.regT0, MacroAssembler::TrustedImm32(maskedChars2)));
        }

        // Neither pattern matched - advance by 1 and continue
        m_jit.add32(MacroAssembler::TrustedImm32(1), m_regs.index);
        m_jit.jump().linkTo(scalarLoopHead, &m_jit);

        // Scalar loop exhausted. Caller routes this to op.m_jumps.
        failed.link(&m_jit);

        // Return both labels:
        // - simdLoopHead: for initial entry (after constant setup)
        // - backtrackEntry: for backtracking (re-computes threshold since scalar loop clobbers regT1)
        // We return backtrackEntry instead of simdLoopHead to ensure bounds checking is correct
        // after the scalar loop modifies regT1.
        return MultiPatternSIMDResult { simdLoopHead, backtrackEntry };
    }

    // Generate SIMD-accelerated skip search for Boyer-Moore bitmap using nibble table lookup.
    //
    // The algorithm compresses the 128-entry bitmap into a 16-byte nibble table where:
    //   - Index = low nibble of character (bits 0-3)
    //   - Value = bitmask of valid high nibbles (bits 4-6)
    //
    // For 16 input characters simultaneously:
    //   1. Extract low nibbles -> TBL lookup gets row from nibble_table
    //   2. Extract high nibbles -> TBL lookup builds bitmask
    //   3. CMTST tests if (row & mask) is non-zero for each position
    //   4. If any lane matches, fall through to scalar loop to find exact position
    //   5. Otherwise advance by 16 and continue
    //
    // Register allocation (ARM64):
    //   Pattern constants (set once, never modified):
    //   - vectorTemp0 = nibble_table[16] (character membership lookup)
    //   - vectorTemp1 = 0x0F repeated (low nibble mask)
    //   - vectorTemp2 = hi-nibble lookup table (0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0,0,0,0,0,0,0,0)
    //
    //   Input/scratch (reloaded/clobbered each iteration):
    //   - vectorInput0 = 16 input characters
    //   - vectorScratch0-3 = computation scratch
    struct SkipBitInTableSIMDResult {
        MacroAssembler::Label simdLoopHead;
        MacroAssembler::Label backtrackTarget;
    };

    std::optional<SkipBitInTableSIMDResult> generateBitInTableSIMDSearch(const BoyerMooreBitmap::Map& bitmap, const BoyerMooreFastCandidates& charactersFastPath, unsigned strideLength, unsigned endIndex, unsigned checkedOffset, MacroAssembler::JumpList& matched)
    {
        // Only for Latin1 (8-bit characters)
        if (m_charSize != CharSize::Char8)
            return std::nullopt;

        // Only uses SIMD when advance_by == 1. For larger strides,
        // the scalar Boyer-Moore version can skip multiple characters per iteration,
        // which performs better than SIMD checking every character.
        if (strideLength != 1)
            return std::nullopt;

        // Leave the search scalar once candidates are too common for the vector loop's exit to pay
        // for its reads. A sample too short to rate them falls back to counting them, as an absent
        // sample already does.
        int32_t frequency = 0;
        if (m_sampler.canResolveCandidateFrequency()) {
            bitmap.forEachSetBit([&](unsigned character) {
                frequency += m_sampler.frequency(character);
            });
        } else
            frequency = bitmap.count();
        if (frequency > SubjectSampler::maxCandidateFrequencyForSIMDSearch)
            return std::nullopt;

        // Call can clobber SIMD registers
        if (mayCall())
            return std::nullopt;

        // Need valid SIMD registers
        if (m_regs.vectorTemp0 == InvalidFPRReg || m_regs.vectorInput0 == InvalidFPRReg)
            return std::nullopt;

        if (checkedOffset > 0x7fffffff)
            return std::nullopt;

        // baseOffset determines where SIMD reads relative to index.
        // Scalar loop reads at: index - (checkedOffset - endIndex + 1)
        // So SIMD should read at the same position: baseOffset = -(checkedOffset - endIndex + 1) = -checkedOffset + endIndex - 1
        auto baseOffset = Checked<int32_t, RecordOverflow>(-static_cast<int32_t>(checkedOffset)) + static_cast<int32_t>(endIndex) - 1;
        if (baseOffset.hasOverflowed())
            return std::nullopt;

        // Need at least 16 characters from the current position to use SIMD
        int32_t minCharsNeeded = 16;
        auto totalOffset = minCharsNeeded + baseOffset;
        if (totalOffset.hasOverflowed())
            return std::nullopt;

#if CPU(X86_64)
        if (!MacroAssembler::supportsAVX())
            return std::nullopt;
#endif

#if CPU(ARM64) || CPU(X86_64)
        JIT_COMMENT(m_jit, "BitInTable SIMD search");

        // ==================== SETUP CONSTANTS (OUTSIDE LOOP) ====================
        //
        // This code leverages our BoyerMoore Bitmap (128 entries) for SIMD search.
        // We spread this bitmap into SIMD register (128bits), and use this with SIMD register to
        // parallelize the matching.
        //
        // For each character c in 0-127:
        //   - Low nibble (c & 0x0F) selects which of 16 table entries to use
        //   - High nibble ((c >> 4) & 0x07) selects which bit within that entry
        // This allows checking membership via:
        //   nibble_table[low_nibble] & (1 << high_nibble)
        // SIMD can test 16 characters simultaneously using TBL/PSHUFB + CMTST/PAND+PCMPEQB.
        auto fromBitmap = [](const BoyerMooreBitmap::Map& bitmap) -> v128_t {
            v128_t table { };
            for (unsigned i = 0; i < BoyerMooreBitmap::mapSize; ++i) {
                if (bitmap.get(i)) {
                    uint8_t lowNibble = i & 0x0F;
                    uint8_t highNibble = (i >> 4) & 0x07;
                    table.u8x16[lowNibble] |= (1 << highNibble);
                }
            }
            return table;
        };

        // vectorTemp0 = nibble_table (16 bytes, for TBL/PSHUFB lookup)
        FPRReg nibbleTableFPR = m_regs.vectorTemp0;
        m_jit.move128ToVector(fromBitmap(bitmap), nibbleTableFPR);

        // vectorTemp1 = 0x0F repeated (low nibble mask)
        FPRReg lowNibbleMaskFPR = m_regs.vectorTemp1;
        v128_t lowNibbleMask;
        lowNibbleMask.u64x2[0] = 0x0f0f0f0f'0f0f0f0fULL;
        lowNibbleMask.u64x2[1] = 0x0f0f0f0f'0f0f0f0fULL;
        m_jit.move128ToVector(lowNibbleMask, lowNibbleMaskFPR);

        // vectorTemp2 = hi-nibble lookup (1 << (hi_nibble & 7) for each position 0-15)
        // Used to convert high nibble (0-7) into a bitmask (0x01, 0x02, 0x04, ..., 0x80)
        // The pattern is repeated twice so that positions 8-15 map to the same values as 0-7.
        // This implicitly performs & 0x7 on the high nibble, which is needed because
        // characters > 127 can have high nibbles in the range 8-15.
        FPRReg highNibbleLookupFPR = m_regs.vectorTemp2;
        v128_t hiNibbleLookup;
        hiNibbleLookup.u64x2[0] = 0x80402010'08040201ULL;
        hiNibbleLookup.u64x2[1] = 0x80402010'08040201ULL;
        m_jit.move128ToVector(hiNibbleLookup, highNibbleLookupFPR);

        // ==================== SIMD LOOP ====================
        MacroAssembler::JumpList scalarLoop;

        // Backtrack entry point - re-computes threshold since scalar loop clobbers regT1
        // When verification fails and backtracks, we need to re-compute the SIMD threshold
        auto backtrackEntry = m_jit.label();

        // Pre-compute max index for SIMD: length - totalOffset
        // This prevents underflow when length < totalOffset
        scalarLoop.append(m_jit.branchSub32(MacroAssembler::Signed, m_regs.length, MacroAssembler::TrustedImm32(totalOffset), m_regs.regT1));

        // Bounds check: index <= length - totalOffset
        scalarLoop.append(m_jit.branch32(MacroAssembler::Above, m_regs.index, m_regs.regT1));

        // Lower bound check when baseOffset is negative
        if (baseOffset < 0)
            scalarLoop.append(m_jit.branch32(MacroAssembler::Below, m_regs.index, MacroAssembler::TrustedImm32(-baseOffset)));

        auto simdLoopHead = m_jit.label();

        // Load 16 input bytes
        m_jit.loadVector(MacroAssembler::BaseIndex(m_regs.input, m_regs.index, MacroAssembler::TimesOne, baseOffset), m_regs.vectorInput0);

        // Step 1: Extract low nibbles (input & 0x0F) -> vectorScratch0
        m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorInput0, lowNibbleMaskFPR, m_regs.vectorScratch0);

        // Step 2: Extract high nibbles ((input >> 4) & 0x0F) -> vectorScratch1
#if CPU(ARM64)
        // Use byte-wise shift (USHR)
        m_jit.vectorUshrInt8(m_regs.vectorInput0, 4, m_regs.vectorScratch1);
#elif CPU(X86_64)
        // Use word-wise shift (VPSRLW). This shifts 16-bit lanes, but since we
        // immediately AND with 0x0F, the cross-byte contamination is masked out.
        m_jit.vectorUshr8(SIMDInfo { SIMDLane::i16x8, SIMDSignMode::Unsigned }, m_regs.vectorInput0, MacroAssembler::TrustedImm32(4), m_regs.vectorScratch1);
#endif
        m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorScratch1, lowNibbleMaskFPR, m_regs.vectorScratch1);

        // Step 3: TBL/PSHUFB lookup row = nibble_table[lo] -> vectorScratch2
        m_jit.vectorSwizzle(nibbleTableFPR, m_regs.vectorScratch0, m_regs.vectorScratch2);

        // Step 4: TBL/PSHUFB lookup mask = hi_lookup[hi] -> vectorScratch3
        m_jit.vectorSwizzle(highNibbleLookupFPR, m_regs.vectorScratch1, m_regs.vectorScratch3);

        // Step 5: Test if character is in bitmap for each lane
#if CPU(ARM64)
        // CMTST - test if (row & mask) != 0 for each lane -> vectorScratch0
        m_jit.vectorTest(SIMDInfo { SIMDLane::i8x16, SIMDSignMode::Unsigned }, m_regs.vectorScratch2, m_regs.vectorScratch3, m_regs.vectorScratch0);

        // Step 6: Narrow the 128-bit result to 64 bits using SHRN
        // SHRN treats the input as 8 16-bit elements and narrows to 8 8-bit elements.
        // With shift=4, it takes (element >> 4) & 0xFF, placing results in low 64 bits.
        // For each byte: 0xFF -> high nibble is 0xF, 0x00 -> high nibble is 0x0.
        // This gives us a 64-bit value where each nibble (4 bits) indicates a match.
        m_jit.vectorShrnInt8(m_regs.vectorScratch0, 4, m_regs.vectorScratch0);

        // Extract the 64-bit result
        m_jit.moveDoubleTo64(m_regs.vectorScratch0, m_regs.regT0);

        // Check if any match found
        auto matchInVector = m_jit.branchTest64(MacroAssembler::NonZero, m_regs.regT0);
#elif CPU(X86_64)
        // Compute (row & mask) and compare with mask to test if (row & mask) == mask
        // This is equivalent to testing (row & mask) != 0 because mask has exactly one bit set.
        m_jit.vectorAnd(SIMDInfo { SIMDLane::v128, SIMDSignMode::None }, m_regs.vectorScratch2, m_regs.vectorScratch3, m_regs.vectorScratch0);
        m_jit.compareIntegerVector(MacroAssembler::Equal, SIMDInfo { SIMDLane::i8x16, SIMDSignMode::None }, m_regs.vectorScratch0, m_regs.vectorScratch3, m_regs.vectorScratch0);

        // PMOVMSKB extracts the high bit of each byte into a 16-bit mask directly
        m_jit.vectorBitmask(SIMDInfo { SIMDLane::i8x16, SIMDSignMode::None }, m_regs.vectorScratch0, m_regs.regT0, m_regs.vectorScratch1);

        // Check if any match found
        auto matchInVector = m_jit.branchTest32(MacroAssembler::NonZero, m_regs.regT0);
#endif

        // ==================== NO MATCH - ADVANCE ====================

        // No match - advance by 16 and continue
        m_jit.add32(MacroAssembler::TrustedImm32(16), m_regs.index);
        m_jit.branch32(MacroAssembler::BelowOrEqual, m_regs.index, m_regs.regT1).linkTo(simdLoopHead, &m_jit);
        // Jump to scalar loop when bounds check fails
        scalarLoop.append(m_jit.jump());

        // ==================== MATCH FOUND IN VECTOR ====================

        matchInVector.link(&m_jit);

#if CPU(ARM64)
        // Find exact position using RBIT + CLZ
        // RBIT reverses bits so first match becomes highest bit
        // CLZ counts leading zeros to find position
        m_jit.reverseBits64(m_regs.regT0, m_regs.regT0);
        m_jit.countLeadingZeros64(m_regs.regT0, m_regs.regT0);

        // Character index = bit position / 4 (since SHRN compressed by 4)
        m_jit.urshift64(MacroAssembler::TrustedImm32(2), m_regs.regT0);
#elif CPU(X86_64)
        // BSF/TZCNT directly gives the byte index (no division needed since PMOVMSKB gives one bit per byte)
        m_jit.countTrailingZeros32(m_regs.regT0, m_regs.regT0);
#endif

        // Add to current index
        m_jit.add32(m_regs.regT0, m_regs.index);

        // When baseOffset >= 0, totalOffset = 16 + baseOffset >= 16, and the SIMD loop bound
        // (index <= length - totalOffset) guarantees index + p <= length - 1 - baseOffset <= length - 1.
        // But when baseOffset < 0, the SIMD load starts at index + baseOffset (before index),
        // so the loop bound allows index up to length - 16 - baseOffset = length - 16 + |baseOffset|.
        // Adding the match offset p (0-15) gives index up to length - 1 + |baseOffset|, which
        // exceeds length.
        //
        // Example: /<([^>]+?)>[\s\S]*?<\/\1>/g
        //   checkedOffset=6, endIndex=1 baseOffset = -6, totalOffset = 10
        //   SIMD loop bound: index <= length - 10 (= 185 when length = 195)
        //   At index=183, SIMD loads input[177..192] and finds '<' at vector offset 14
        //   index becomes 183 + 14 = 197, but length = 195 => out of bounds
        //
        // Falling to the scalar loop is correct here: CTZ/CLZ finds the first (lowest
        // offset) bitmap match in the vector, so all earlier positions in this window
        // did not match the bitmap and cannot be candidates. The scalar loop's own
        // bounds check (index > length) will immediately fail, producing "no match."
        if (baseOffset < 0)
            scalarLoop.append(m_jit.branch32(MacroAssembler::Above, m_regs.index, m_regs.length));

        // Jump to matched! (index now points to the matching character)
        matched.append(m_jit.jump());

        // ==================== SCALAR LOOP ====================
        // Handles tail positions after SIMD exhausts.
        scalarLoop.link(&m_jit);

        JIT_COMMENT(m_jit, "BitInTable scalar loop");

        // Initial bounds check before first iteration - SIMD may have left index beyond bounds
        auto scalarFailed = m_jit.branch32(MacroAssembler::Above, m_regs.index, m_regs.length);

        // Use the shared scalar loop implementation
        ASSERT(strideLength == 1);
        generateBoyerMooreScalarLoop(bitmap, charactersFastPath, 1, endIndex, checkedOffset, matched);

        // Fall through when out of bounds - no more characters to check
        scalarFailed.link(&m_jit);

        return SkipBitInTableSIMDResult { simdLoopHead, backtrackEntry };
#else
        UNUSED_PARAM(bitmap);
        UNUSED_PARAM(charactersFastPath);
        UNUSED_PARAM(strideLength);
        UNUSED_PARAM(endIndex);
        UNUSED_PARAM(matched);
        return std::nullopt;
#endif
    }
#endif

    RegisterSet NODELETE calleeSaveRegisters()
    {
        RegisterSet registers;
#if CPU(X86_64)
        if (m_containsNestedSubpatterns) {
            registers.add(X86Registers::ebx, IgnoreVectors); // matchingContext
            registers.add(X86Registers::r12, IgnoreVectors); // remainingMatchCount
        }

        if (mayCall() || m_callFrameSizeInBytes) {
            registers.add(X86Registers::r13, IgnoreVectors);
            registers.add(X86Registers::r14, IgnoreVectors);
            registers.add(X86Registers::r15, IgnoreVectors);
        } else if (m_pattern.hasDuplicateNamedCaptureGroups())
            registers.add(X86Registers::r14, IgnoreVectors);
#elif CPU(ARM64)
#elif CPU(ARM_THUMB2)
        registers.add(ARMRegisters::r4, IgnoreVectors);
        registers.add(ARMRegisters::r5, IgnoreVectors);
        registers.add(ARMRegisters::r6, IgnoreVectors);
        registers.add(ARMRegisters::r8, IgnoreVectors);
        registers.add(ARMRegisters::r10, IgnoreVectors);
#elif CPU(RISCV64)
#endif
        return registers;
    }

    void generateEnter()
    {
#if CPU(X86_64) || CPU(ARM_THUMB2) || CPU(RISCV64)
        m_jit.emitFunctionPrologue();
#elif CPU(ARM64)
        // JITCage code is doing prologue and epilogue in thunk.
        if (!Options::useJITCage()) {
            if (mayCall() || m_callFrameSizeInBytes || m_containsNestedSubpatterns)
                m_jit.emitFunctionPrologue();
            else
                m_jit.tagReturnAddress();
        }
#endif

        if (m_calleeSaves.registerCount()) {
            size_t stackSizeForCalleeSaves = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(m_calleeSaves.registerCount() * sizeof(UCPURegister));
#if CPU(X86_64) || CPU(ARM64)
            m_jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(stackSizeForCalleeSaves), MacroAssembler::stackPointerRegister);
#else
            m_jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(stackSizeForCalleeSaves), m_regs.regT0);
            m_jit.move(m_regs.regT0, MacroAssembler::stackPointerRegister);
#endif
            m_jit.emitSaveCalleeSavesFor(&m_calleeSaves);
        }
    }

    void generateReturn()
    {
#if ENABLE(YARR_JIT_REGEXP_TEST_INLINE)
        if (m_executionMode == ExecutionMode::InlineTest) {
            m_inlinedMatched.append(m_jit.jump());
            return;
        }
#endif

        m_jit.emitRestoreCalleeSavesFor(&m_calleeSaves);
#if CPU(X86_64) || CPU(ARM_THUMB2) || CPU(RISCV64)
        m_jit.emitFunctionEpilogue();
#elif CPU(ARM64)
        // JITCage code is doing prologue and epilogue in thunk.
        if (!Options::useJITCage()) {
            if (mayCall() || m_callFrameSizeInBytes || m_containsNestedSubpatterns)
                m_jit.emitFunctionEpilogue();
        }
#endif

#if CPU(ARM64E)
        if (Options::useJITCage())
            m_jit.farJump(MacroAssembler::TrustedImmPtr(retagCodePtr<void*, CFunctionPtrTag, OperationPtrTag>(&vmEntryToYarrJITAfter)), OperationPtrTag);
        else
            m_jit.ret();
#else
        m_jit.ret();
#endif
    }

    void loadSubPattern(unsigned subpatternId, MacroAssembler::RegisterID startIndexGPR, MacroAssembler::RegisterID endIndexOrLenGPR)
    {
        m_jit.loadPair32(subpatternStartAddress(subpatternId), startIndexGPR, endIndexOrLenGPR);
    }

    void loadSubPatternIdForDuplicateNamedGroup(unsigned duplicateNamedGroupId, MacroAssembler::RegisterID subpatternIdGPR)
    {
        m_jit.load32(duplicateNamedGroupAddress(duplicateNamedGroupId), subpatternIdGPR);
    }

    void loadSubPattern(MacroAssembler::RegisterID subpatternIdGPR, MacroAssembler::RegisterID startIndexGPR, MacroAssembler::RegisterID endIndexOrLenGPR)
    {
        if (m_needsInternalSubpatternOutput) {
            auto frameBase = frameAddress();
            m_jit.getEffectiveAddress(MacroAssembler::BaseIndex(frameBase.base, subpatternIdGPR, MacroAssembler::TimesEight, frameBase.offset + m_internalSubpatternOutputOffsetInFrame * sizeof(void*)), endIndexOrLenGPR);
        } else
            m_jit.getEffectiveAddress(MacroAssembler::BaseIndex(m_regs.output, subpatternIdGPR, MacroAssembler::TimesEight), endIndexOrLenGPR);
        m_jit.loadPair32(endIndexOrLenGPR, startIndexGPR, endIndexOrLenGPR);
    }

    void loadSubPatternEnd(MacroAssembler::RegisterID subpatternIdGPR, MacroAssembler::RegisterID endIndex)
    {
        if (m_needsInternalSubpatternOutput) {
            auto frameBase = frameAddress();
            m_jit.getEffectiveAddress(MacroAssembler::BaseIndex(frameBase.base, subpatternIdGPR, MacroAssembler::TimesEight, frameBase.offset + m_internalSubpatternOutputOffsetInFrame * sizeof(void*)), endIndex);
        } else
            m_jit.getEffectiveAddress(MacroAssembler::BaseIndex(m_regs.output, subpatternIdGPR, MacroAssembler::TimesEight), endIndex);
        m_jit.load32(MacroAssembler::Address(endIndex, sizeof(int)), endIndex);
    }

public:
    YarrGenerator(CCallHelpers& jit, VM* vm, YarrCodeBlock* codeBlock, const YarrJITRegs& regs, YarrPattern& pattern, StringView patternString, CharSize charSize, ExecutionMode executionMode, std::optional<StringView> sampleString)
        : m_jit(jit)
        , m_vm(vm)
        , m_codeBlock(codeBlock)
        , m_boyerMooreData(static_cast<YarrBoyerMooreData*>(codeBlock))
        , m_regs(regs)
        , m_pattern(pattern)
        , m_patternString(patternString)
        , m_charSize(charSize)
        , m_executionMode(executionMode)
        , m_decodeSurrogatePairs(m_charSize == CharSize::Char16 && m_pattern.eitherUnicode())
        , m_unicodeIgnoreCase(m_pattern.eitherUnicode() && m_pattern.ignoreCase())
        , m_decode16BitForBackreferencesWithCalls(m_charSize == CharSize::Char16 && m_pattern.m_containsBackreferences && (m_pattern.ignoreCase() || m_pattern.m_containsModifiers))
        , m_callFrameSizeInBytes(alignCallFrameSizeInBytes(m_pattern.m_body->m_callFrameSize))
        , m_canonicalMode(m_pattern.eitherUnicode() ? CanonicalMode::Unicode : CanonicalMode::UCS2)
#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
        , m_parenContextSizes(needsSubpatternRecording(executionMode, m_pattern) ? m_pattern.m_numSubpatterns : 0, needsSubpatternRecording(executionMode, m_pattern) ? m_pattern.m_numDuplicateNamedCaptureGroups : 0, m_pattern.m_body->m_callFrameSize)
#endif
        , m_sampleString(sampleString)
        , m_sampler(charSize)
    {
        initializeInternalSubpatternStorageIfNeeded();
    }

    YarrGenerator(CCallHelpers& jit, VM* vm, YarrBoyerMooreData* yarrBMData, const YarrJITRegs& regs, YarrPattern& pattern, StringView patternString, CharSize charSize, ExecutionMode executionMode)
        : m_jit(jit)
        , m_vm(vm)
        , m_codeBlock(nullptr)
        , m_boyerMooreData(yarrBMData)
        , m_regs(regs)
        , m_pattern(pattern)
        , m_patternString(patternString)
        , m_charSize(charSize)
        , m_executionMode(executionMode)
        , m_decodeSurrogatePairs(m_charSize == CharSize::Char16 && m_pattern.eitherUnicode())
        , m_unicodeIgnoreCase(m_pattern.eitherUnicode() && m_pattern.ignoreCase())
        , m_decode16BitForBackreferencesWithCalls(m_charSize == CharSize::Char16 && m_pattern.m_containsBackreferences && (m_pattern.ignoreCase() || m_pattern.m_containsModifiers))
        , m_callFrameSizeInBytes(alignCallFrameSizeInBytes(m_pattern.m_body->m_callFrameSize))
        , m_canonicalMode(m_pattern.eitherUnicode() ? CanonicalMode::Unicode : CanonicalMode::UCS2)
#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
        , m_parenContextSizes(needsSubpatternRecording(executionMode, m_pattern) ? m_pattern.m_numSubpatterns : 0, needsSubpatternRecording(executionMode, m_pattern) ? m_pattern.m_numDuplicateNamedCaptureGroups : 0, m_pattern.m_body->m_callFrameSize)
#endif
        , m_sampler(charSize)
    {
        if (m_pattern.m_containsBackreferences)
            m_usesT2 = true;
        initializeInternalSubpatternStorageIfNeeded();
    }

    void NODELETE initializeInternalSubpatternStorageIfNeeded()
    {
#if ENABLE(YARR_JIT_BACKREFERENCES)
        // For MatchOnly mode with backreferences, we need internal storage for subpattern results
        // since m_regs.output is not available for subpattern storage in MatchOnly mode.
        if (m_executionMode == ExecutionMode::MatchOnly && m_pattern.m_containsBackreferences) {
            m_needsInternalSubpatternOutput = true;
            // Store subpattern output after the regular frame data
            m_internalSubpatternOutputOffsetInFrame = m_pattern.m_body->m_callFrameSize;
            // Each subpattern needs 2 slots (start and end index), plus space for duplicate named groups
            unsigned subpatternSlots = (m_pattern.m_numSubpatterns + 1) * 2;
            unsigned duplicateNamedGroupSlots = m_pattern.m_numDuplicateNamedCaptureGroups;
            unsigned totalAdditionalSlots = subpatternSlots + duplicateNamedGroupSlots;
            m_callFrameSizeInBytes = alignCallFrameSizeInBytes(m_pattern.m_body->m_callFrameSize + totalAdditionalSlots);
        }
#endif
    }

    bool isSafeToRecurse() const
    {
        if (m_compilationThreadStackChecker)
            return m_compilationThreadStackChecker->isSafeToRecurse();

        return m_vm->isSafeToRecurse();
    }

    // Check if a disjunction contains terms that could require within-iteration backtracking.
    // This includes multiple alternatives (switching between them) and backtrackable content.
    static bool NODELETE disjunctionContainsBacktrackableContent(PatternDisjunction* disjunction)
    {
        // Multiple alternatives require backtracking to try the next alternative
        if (disjunction->m_alternatives.size() > 1)
            return true;

        for (auto& alternative : disjunction->m_alternatives) {
            for (auto& term : alternative->m_terms) {
                // Non-fixed quantifiers can backtrack
                if (term.quantityType != QuantifierType::FixedCount)
                    return true;

                // Back references can cause backtracking
                if (term.type == PatternTerm::Type::NumberedBackReference || term.type == PatternTerm::Type::NamedBackReference)
                    return true;
                // ForwardReference always matches empty string - no backtracking needed.

                // Recursively check nested parentheses (use the full check for nested disjunctions)
                if (term.type == PatternTerm::Type::ParenthesesSubpattern || term.type == PatternTerm::Type::ParentheticalAssertion) {
                    if (disjunctionContainsBacktrackableContent(term.parentheses.disjunction))
                        return true;
                }
            }
        }
        return false;
    }

    void NODELETE setStackChecker(StackCheck* stackChecker)
    {
        m_compilationThreadStackChecker = stackChecker;
    }

    template<typename OperationType>
    static constexpr void NODELETE functionChecks()
    {
        static_assert(FunctionTraits<OperationType>::cCallArity() == 5, "YarrJITCode takes 5 arguments");
        static_assert(std::is_same<MatchingContextHolder*, typename FunctionTraits<OperationType>::template ArgumentType<4>>::value, "MatchingContextHolder* is expected as the function 5th argument");
    }

    void compile(YarrCodeBlock& codeBlock)
    {
        MacroAssembler::Label startOfMainCode;

#if !ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs) {
            codeBlock.setFallBackWithFailureReason(JITFailureReason::DecodeSurrogatePair);
            return;
        }
#endif

        // With YARR_JIT_BACKREFERENCES enabled, we can now handle backreferences in MatchOnly mode
        // by using internal frame storage for subpattern results.
#if ENABLE(YARR_JIT_BACKREFERENCES)
#if !ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
        // Without 16-bit backreference support, fail for ignoreCase 16-bit patterns
        if (m_pattern.m_containsBackreferences && m_pattern.ignoreCase() && m_charSize != CharSize::Char8) {
            codeBlock.setFallBackWithFailureReason(JITFailureReason::BackReference);
            return;
        }
#endif
#else
        // Without YARR_JIT_BACKREFERENCES, fail for any backreference pattern
        if (m_pattern.m_containsBackreferences) {
            codeBlock.setFallBackWithFailureReason(JITFailureReason::BackReference);
            return;
        }
#endif

        // Lookbehind bodies are JIT-compiled via mirroring (see
        // mirrorDisjunctionForLookbehind): the reversed body copy runs through the
        // forward machinery with Backward ops, decoding unicode text leftward
        // (readUnicodeCharBackward, matching the interpreter's tryReadBackward).

        // The first-non-BMP-character skip advances the next match start past the
        // trailing surrogate of an astral start character. That is only sound when
        // no match can begin at a mid-pair position -- i.e. when the pattern cannot
        // match zero-width (a body minimum size of 0 means some alternative can, and
        // a zero-width alternative such as a negative lookahead over . or an
        // assertion can then succeed at a dangling-trail position, whose match the
        // skip would drop).
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
        if (m_decodeSurrogatePairs && m_executionMode != ExecutionMode::InlineTest && !m_pattern.multiline() && !m_pattern.m_containsBOL && !m_pattern.m_containsLookbehinds && !m_pattern.m_containsModifiers && m_pattern.m_body->m_minimumSize) {
            ASSERT(m_regs.firstCharacterAdditionalReadSize != InvalidGPRReg);
            m_canUseFirstNonBMPCharacterOptimization = true;
        }
#endif

        // We need to compile before generating code since we set flags based on compilation that
        // are used during generation.
        opCompileBody(m_pattern.m_body);

        if (m_failureReason) {
            codeBlock.setFallBackWithFailureReason(*m_failureReason);
            return;
        }

        if (Options::dumpDisassembly() || Options::dumpRegExpDisassembly()) [[unlikely]]
            m_disassembler = makeUnique<YarrDisassembler>(this);

        if (m_disassembler)
            m_disassembler->setStartOfCode(m_jit.label());

        m_calleeSaves = RegisterAtOffsetList(calleeSaveRegisters());

        generateEnter();

        startOfMainCode = m_jit.label();

        MacroAssembler::Jump hasInput = checkInput();
        generateFailReturn();
        hasInput.link(&m_jit);

        if (m_callFrameSizeInBytes) {
            // Check stack size
            m_jit.addPtr(MacroAssembler::TrustedImm32(-m_callFrameSizeInBytes), MacroAssembler::stackPointerRegister, m_regs.regT0);

            // Make sure that the JITed functions have 5 parameters and that the 5th argument is a MatchingContextHolder*
            functionChecks<YarrCodeBlock::YarrJITCode8>();
            functionChecks<YarrCodeBlock::YarrJITCode16>();
            functionChecks<YarrCodeBlock::YarrJITCodeMatchOnly8>();
            functionChecks<YarrCodeBlock::YarrJITCodeMatchOnly16>();
#if CPU(ARM_THUMB2)
            // Not enough argument registers: try to load the 5th argument from the stack
            MacroAssembler::RegisterID matchingContext = m_regs.regT1;
            unsigned offset = POKE_ARGUMENT_OFFSET;
            m_jit.loadPtr(MacroAssembler::Address(GPRInfo::callFrameRegister, offset * sizeof(void*)), matchingContext);
#else
            // The MatchingContextHolder* arrives in the 5th argument register.
            MacroAssembler::RegisterID matchingContext = GPRInfo::argumentGPR4;
#endif
            MacroAssembler::Jump stackOk = m_jit.branchPtr(MacroAssembler::BelowOrEqual, MacroAssembler::Address(matchingContext, MatchingContextHolder::offsetOfStackLimit()), m_regs.regT0);

            // Exceeded stack limit, punt to the interpreter.
            m_jit.move(MacroAssembler::TrustedImmPtr((void*)static_cast<size_t>(JSRegExpResult::JITCodeFailure)), m_regs.returnRegister);
            m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.returnRegister2);
            generateReturn();

            stackOk.link(&m_jit);
            m_jit.move(m_regs.regT0, MacroAssembler::stackPointerRegister);
        }

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs)
            m_jit.getEffectiveAddress(MacroAssembler::BaseIndex(m_regs.input, m_regs.length, MacroAssembler::TimesTwo), m_regs.endOfStringAddress);
#endif

#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
        if (m_containsNestedSubpatterns) {
#if CPU(X86_64)
            // Preserve the MatchingContextHolder* in m_regs.matchingContext.
            m_jit.move(GPRInfo::argumentGPR4, m_regs.matchingContext);
#else
            ASSERT(GPRInfo::argumentGPR4 == m_regs.matchingContext);
#endif
            m_jit.move(MacroAssembler::TrustedImm32(matchLimit), m_regs.remainingMatchCount);

            // Initialize freelist to null - contexts will be allocated from stack
            // and freed contexts will be added to the freelist for reuse
            if (m_regs.freelistRegister != InvalidGPRReg)
                m_jit.move(MacroAssembler::TrustedImmPtr(nullptr), m_regs.freelistRegister);
            else
                m_jit.storePtr(MacroAssembler::TrustedImmPtr(nullptr), MacroAssembler::Address(m_regs.matchingContext, MatchingContextHolder::offsetOfFreeList()));
        }
#endif

        // Initialize subpatterns' starts. And initialize matchStart if `!m_pattern.m_body->m_hasFixedSize`.
        // If shouldRecordSubpatterns(), then matchStart is subpatterns[0]'s start.
        emitClearAllCaptures();
        if (!m_pattern.m_body->m_hasFixedSize)
            setMatchStart(m_regs.index);

        if (m_pattern.m_saveInitialStartValue)
            storeToFrame(m_regs.index, m_pattern.m_initialStartValueFrameLocation);

        generate();
        if (m_disassembler)
            m_disassembler->setEndOfGenerate(m_jit.label());
        if (m_failureReason) {
            codeBlock.setFallBackWithFailureReason(*m_failureReason);
            return;
        }
        backtrack();
        if (m_disassembler)
            m_disassembler->setEndOfBacktrack(m_jit.label());
        if (m_failureReason) {
            codeBlock.setFallBackWithFailureReason(*m_failureReason);
            return;
        }

        ptrdiff_t codeSize = MacroAssembler::differenceBetween(startOfMainCode, m_jit.label());
        bool canInline = ([&] -> bool {
            if (m_executionMode == ExecutionMode::IncludeSubpatterns)
                return false;
            if (m_pattern.global())
                return false;
            if (m_pattern.sticky())
                return false;
            if (m_pattern.eitherUnicode())
                return false;
            if (mayCall())
                return false;
            if (m_callFrameSizeInBytes)
                return false;
#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
            if (m_containsNestedSubpatterns)
                return false;
#endif
            if (m_pattern.m_containsBackreferences)
                return false;
            if (m_pattern.m_saveInitialStartValue)
                return false;

            // SIMD search path uses Vector scratch registers which is not assigned from DFG / FTL.
            if (m_usesSIMD)
                return false;

            return true;
        }());

        generateJITFailReturn();

        if (m_disassembler)
            m_disassembler->setEndOfCode(m_jit.label());

        auto backtrackRecords = m_backtrackingState.backtrackRecords();
        if (!backtrackRecords.isEmpty()) {
            m_jit.addLinkTask([=] (LinkBuffer& linkBuffer) {
                BacktrackingState::linkBacktrackRecords(linkBuffer, backtrackRecords);
            });
        }

        if (m_disassembler) {
            // Disassemble after all link tasks are complete.
            m_jit.addLinkTask([=, this] (LinkBuffer& linkBuffer) {
                m_disassembler->dump(linkBuffer);
            });
        }

        LinkBuffer linkBuffer(m_jit, &codeBlock, LinkBuffer::Profile::YarrJIT, JITCompilationCanFail);
        if (linkBuffer.didFailToAllocate()) {
            codeBlock.setFallBackWithFailureReason(JITFailureReason::ExecutableMemoryAllocationFailure);
            return;
        }

        if (m_executionMode == ExecutionMode::MatchOnly) {
            if (m_charSize == CharSize::Char8) {
                codeBlock.set8BitCodeMatchOnly(FINALIZE_REGEXP_CODE(linkBuffer, YarrMatchOnly8BitPtrTag, nullptr, "Match-only 8-bit regular expression"), WTF::move(m_bmMaps));
                codeBlock.set8BitInlineStats(codeSize, m_callFrameSizeInBytes, canInline, m_usesT2);
            } else {
                codeBlock.set16BitCodeMatchOnly(FINALIZE_REGEXP_CODE(linkBuffer, YarrMatchOnly16BitPtrTag, nullptr, "Match-only 16-bit regular expression"), WTF::move(m_bmMaps));
                codeBlock.set16BitInlineStats(codeSize, m_callFrameSizeInBytes, canInline, m_usesT2);
            }
        } else {
            if (m_charSize == CharSize::Char8)
                codeBlock.set8BitCode(FINALIZE_REGEXP_CODE(linkBuffer, Yarr8BitPtrTag, nullptr, "8-bit regular expression"), WTF::move(m_bmMaps));
            else
                codeBlock.set16BitCode(FINALIZE_REGEXP_CODE(linkBuffer, Yarr16BitPtrTag, nullptr, "16-bit regular expression"), WTF::move(m_bmMaps));
        }
        if (m_failureReason)
            codeBlock.setFallBackWithFailureReason(*m_failureReason);
    }

#if ENABLE(YARR_JIT_REGEXP_TEST_INLINE)
    void compileInline(YarrBoyerMooreData& boyerMooreData)
    {
        RELEASE_ASSERT(!m_pattern.m_containsBackreferences);

        // We need to compile before generating code since we set flags based on compilation that
        // are used during generation.
        opCompileBody(m_pattern.m_body);

#if !ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        RELEASE_ASSERT(!m_decodeSurrogatePairs);
#endif

#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
        RELEASE_ASSERT(!m_containsNestedSubpatterns);
#endif

        if (Options::dumpDisassembly() || Options::dumpRegExpDisassembly()) [[unlikely]]
            m_disassembler = makeUnique<YarrDisassembler>(this);

        if (m_disassembler)
            m_disassembler->setStartOfCode(m_jit.label());

        if (m_failureReason) {
            m_jit.move(MacroAssembler::TrustedImmPtr((void*)static_cast<size_t>(JSRegExpResult::JITCodeFailure)), m_regs.returnRegister);
            m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.returnRegister2);
            return;
        }

        if (m_usesT2)
            ASSERT(m_regs.regT2 != MacroAssembler::InvalidGPRReg);

        MacroAssembler::Jump hasInput = checkInput();
        generateFailReturn();
        hasInput.link(&m_jit);

        if (m_callFrameSizeInBytes) {
            // Create space on stack for matching context data.
            // Note that this stack check cannot clobber m_regs.regT1 as it is needed for the slow path we call if we fail the stack check.
            m_jit.addPtr(MacroAssembler::TrustedImm32(-m_callFrameSizeInBytes), MacroAssembler::stackPointerRegister, m_regs.regT0);
            MacroAssembler::Jump stackOk = m_jit.branchPtr(MacroAssembler::LessThanOrEqual, MacroAssembler::AbsoluteAddress(const_cast<VM*>(m_vm)->addressOfSoftStackLimit()), m_regs.regT0);

            // Exceeded stack limit, punt to the interpreter.
            m_jit.move(MacroAssembler::TrustedImmPtr((void*)static_cast<size_t>(JSRegExpResult::JITCodeFailure)), m_regs.returnRegister);
            m_jit.move(MacroAssembler::TrustedImm32(0), m_regs.returnRegister2);
            m_inlinedFailedMatch.append(m_jit.jump());

            stackOk.link(&m_jit);
            m_jit.move(m_regs.regT0, MacroAssembler::stackPointerRegister);
        }

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
        if (m_decodeSurrogatePairs)
            m_jit.getEffectiveAddress(MacroAssembler::BaseIndex(m_regs.input, m_regs.length, MacroAssembler::TimesTwo), m_regs.endOfStringAddress);
#endif

        emitClearAllCaptures();
        if (!m_pattern.m_body->m_hasFixedSize)
            setMatchStart(m_regs.index);

        if (m_pattern.m_saveInitialStartValue)
            storeToFrame(m_regs.index, m_pattern.m_initialStartValueFrameLocation);

        generate();
        if (m_disassembler)
            m_disassembler->setEndOfGenerate(m_jit.label());
        backtrack();
        if (m_disassembler)
            m_disassembler->setEndOfBacktrack(m_jit.label());

        generateJITFailReturn();

        if (m_disassembler)
            m_disassembler->setEndOfCode(m_jit.label());

        m_inlinedFailedMatch.link(&m_jit);
        m_inlinedMatched.link(&m_jit);

        auto backtrackRecords = m_backtrackingState.backtrackRecords();
        if (!backtrackRecords.isEmpty()) {
            m_jit.addLinkTask([=] (LinkBuffer& linkBuffer) {
                BacktrackingState::linkBacktrackRecords(linkBuffer, backtrackRecords);
            });
        }

        boyerMooreData.saveMaps(WTF::move(m_bmMaps));
    }
#endif

    const char* variant() final
    {
        if (m_executionMode == ExecutionMode::MatchOnly) {
            if (m_charSize == CharSize::Char8)
                return "Match-only 8-bit regular expression";

            return "Match-only 16-bit regular expression";
        }

        if (m_charSize == CharSize::Char8)
            return "8-bit regular expression";

        return "16-bit regular expression";
    }

    unsigned opCount() final
    {
        return m_ops.size();
    }

    void dumpPatternString(PrintStream& out) final
    {
        m_pattern.dumpPatternString(out, m_patternString);
    }

    int dumpFor(PrintStream& out, unsigned opIndex) final
    {
        if (opIndex >= opCount())
            return 0;

        out.printf("%4d:", opIndex);

        YarrOp& op = m_ops[opIndex];
        PatternTerm* term = op.m_term;
        switch (op.m_op) {
        case YarrOpCode::Term: {
            out.print("Term ");
            switch (term->type) {
            case PatternTerm::Type::AssertionBOL:
                out.printf("Assert BOL checked-offset:(%u)", op.m_checkedOffset.value());
                break;

            case PatternTerm::Type::AssertionEOL:
                out.printf("Assert EOL checked-offset:(%u)", op.m_checkedOffset.value());
                break;

            case PatternTerm::Type::NumberedBackReference:
            case PatternTerm::Type::NamedBackReference:
                out.printf("%sBackReference pattern #%u checked-offset:(%u)", term->type == PatternTerm::Type::NumberedBackReference ? "Numbered" : "Named", term->backReferenceSubpatternId, op.m_checkedOffset.value());
                term->dumpQuantifier(out);
                break;

            case PatternTerm::Type::PatternCharacter:
                out.printf("PatternCharacter checked-offset:(%u) ", op.m_checkedOffset.value());
                dumpChar32(out, term->patternCharacter);
                if (op.m_term->ignoreCase())
                    out.print("ignore case ");

                term->dumpQuantifier(out);
                break;

            case PatternTerm::Type::CharacterClass:
                out.printf("PatternCharacterClass checked-offset:(%u) ", op.m_checkedOffset.value());
                if (term->invert())
                    out.print("not ");
                dumpCharacterClass(out, &m_pattern, term->characterClass);
                term->dumpQuantifier(out);
                break;

            case PatternTerm::Type::AssertionWordBoundary:
                out.printf("%sword boundary checked-offset:(%u)", term->invert() ? "non-" : "", op.m_checkedOffset.value());
                break;

            case PatternTerm::Type::DotStarEnclosure:
                out.printf(".* enclosure checked-offset:(%u)", op.m_checkedOffset.value());
                break;

            case PatternTerm::Type::NumberedForwardReference:
            case PatternTerm::Type::NamedForwardReference:
                out.printf("%sForwardReference checked-offset:(%u)", term->type == PatternTerm::Type::NumberedForwardReference ? "Numbered" : "Named", op.m_checkedOffset.value());
                break;

            case PatternTerm::Type::ParenthesesSubpattern:
            case PatternTerm::Type::ParentheticalAssertion:
                RELEASE_ASSERT_NOT_REACHED();
                break;
            }

            if (op.m_isDeadCode)
                out.print(" already handled");
            out.print("\n");
            return 0;
        }

        case YarrOpCode::BodyAlternativeBegin:
            out.printf("BodyAlternativeBegin minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::BodyAlternativeNext:
            out.printf("BodyAlternativeNext minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::BodyAlternativeEnd:
            out.printf("BodyAlternativeEnd checked-offset:(%u)\n", op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::SimpleNestedAlternativeBegin:
            out.printf("SimpleNestedAlternativeBegin minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 1;

        case YarrOpCode::StringListAlternativeBegin:
            out.printf("StringListAlternativeBegin minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 1;

        case YarrOpCode::NestedAlternativeBegin:
            out.printf("NestedAlternativeBegin minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 1;

        case YarrOpCode::SimpleNestedAlternativeNext:
            out.printf("SimpleNestedAlternativeNext minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::StringListAlternativeNext:
            out.printf("StringListAlternativeNext minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::NestedAlternativeNext:
            out.printf("NestedAlternativeNext minimum-size:(%u),checked-offset:(%u)\n", op.m_alternative->m_minimumSize, op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::SimpleNestedAlternativeEnd:
            out.printf("SimpleNestedAlternativeEnd checked-offset:(%u) ", op.m_checkedOffset.value());
            term->dumpQuantifier(out);
            out.print("\n");
            return -1;

        case YarrOpCode::StringListAlternativeEnd:
            out.printf("StringListAlternativeEnd checked-offset:(%u) ", op.m_checkedOffset.value());
            term->dumpQuantifier(out);
            out.print("\n");
            return -1;

        case YarrOpCode::NestedAlternativeEnd:
            out.printf("NestedAlternativeEnd checked-offset:(%u) ", op.m_checkedOffset.value());
            term->dumpQuantifier(out);
            out.print("\n");
            return -1;

        case YarrOpCode::ParenthesesSubpatternOnceBegin:
            out.printf("ParenthesesSubpatternOnceBegin checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u ", term->parentheses.subpatternId);
            else
                out.print("non-capturing ");
            term->dumpQuantifier(out);
            out.print("\n");
            return 0;

        case YarrOpCode::ParenthesesSubpatternOnceEnd:
            out.printf("ParenthesesSubpatternOnceEnd checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u ", term->parentheses.subpatternId);
            else
                out.print("non-capturing ");
            term->dumpQuantifier(out);
            out.print("\n");
            return 0;

        case YarrOpCode::ParenthesesSubpatternTerminalBegin:
            out.printf("ParenthesesSubpatternTerminalBegin checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u\n", term->parentheses.subpatternId);
            else
                out.print("non-capturing\n");
            return 0;

        case YarrOpCode::ParenthesesSubpatternTerminalEnd:
            out.printf("ParenthesesSubpatternTerminalEnd checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u\n", term->parentheses.subpatternId);
            else
                out.print("non-capturing\n");
            return 0;

        case YarrOpCode::ParenthesesSubpatternFixedCountBegin:
            out.printf("ParenthesesSubpatternFixedCountBegin checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u", term->parentheses.subpatternId);
            else
                out.print("non-capturing");
            term->dumpQuantifier(out);
            out.print("\n");
            return 0;

        case YarrOpCode::ParenthesesSubpatternFixedCountEnd:
            out.printf("ParenthesesSubpatternFixedCountEnd checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u", term->parentheses.subpatternId);
            else
                out.print("non-capturing");
            term->dumpQuantifier(out);
            out.print("\n");
            return 0;

        case YarrOpCode::ParenthesesSubpatternBegin:
            out.printf("ParenthesesSubpatternBegin checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u", term->parentheses.subpatternId);
            else
                out.print("non-capturing");
            term->dumpQuantifier(out);
            out.print("\n");
            return 0;

        case YarrOpCode::ParenthesesSubpatternEnd:
            out.printf("ParenthesesSubpatternEnd checked-offset:(%u) ", op.m_checkedOffset.value());
            if (term->capture())
                out.printf("capturing pattern #%u", term->parentheses.subpatternId);
            else
                out.print("non-capturing");
            term->dumpQuantifier(out);
            out.print("\n");
            return 0;

        case YarrOpCode::ParentheticalAssertionBegin:
            out.printf("ParentheticalAssertionBegin%s checked-offset:(%u)\n", term->invert() ? " inverted" : "", op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::ParentheticalAssertionEnd:
            out.printf("ParentheticalAssertionEnd%s checked-offset:(%u)\n", term->invert() ? " inverted" : "", op.m_checkedOffset.value());
            return 0;

        case YarrOpCode::MatchFailed:
            out.printf("MatchFailed checked-offset:(%u)\n", op.m_checkedOffset.value());
            return 0;
        }

        return 0;
    }

    bool NODELETE mayCall() const
    {
        return m_decodeSurrogatePairs || m_decode16BitForBackreferencesWithCalls;
    }

    void defineReentryLabel(YarrOp& op)
    {
        op.m_reentry = m_jit.label();
        if (Options::traceRegExpJITExecution()) [[unlikely]] {
            GPRReg indexReg = m_regs.index;
            auto opcode = op.m_op;
            auto index = op.m_index;
            m_jit.probeDebug([=](Probe::Context& ctx) {
                int32_t indexValue = static_cast<int32_t>(ctx.gpr(indexReg));
                dataLogLn("RegExpJIT [", index, "] ", opcode, " index=", indexValue);
            });
        }
    }

    template<typename... Args>
    void appendOp(Args&&... args)
    {
        unsigned index = m_ops.size();
        m_ops.constructAndAppend(std::forward<Args>(args)...);
        m_ops.last().m_index = index;
        m_ops.last().m_direction = m_direction;
    }

private:
    CCallHelpers& m_jit;
    VM* m_vm;
    YarrCodeBlock* const m_codeBlock;
    YarrBoyerMooreData* const m_boyerMooreData;
    const YarrJITRegs& m_regs;

    StackCheck* m_compilationThreadStackChecker { nullptr };
    YarrPattern& m_pattern;
    const StringView m_patternString;

    const CharSize m_charSize;
    const ExecutionMode m_executionMode;

    // Used to detect regular expression constructs that are not currently
    // supported in the JIT; fall back to the interpreter when this is detected.
    std::optional<JITFailureReason> m_failureReason;

    const bool m_decodeSurrogatePairs : 1;
    const bool m_unicodeIgnoreCase : 1;
    const bool m_decode16BitForBackreferencesWithCalls : 1;

    bool m_usesSIMD : 1 { false };
    bool m_usesT2 : 1 { false };
    // True when MatchOnly mode needs internal subpattern output storage for backreferences
    bool m_needsInternalSubpatternOutput : 1 { false };
    unsigned m_callFrameSizeInBytes;
    // Frame offset (in slots) for internal subpattern output storage
    unsigned m_internalSubpatternOutputOffsetInFrame { 0 };
    const CanonicalMode m_canonicalMode;
#if ENABLE(YARR_JIT_ALL_PARENS_EXPRESSIONS)
    bool m_containsNestedSubpatterns { false };
    ParenContextSizes m_parenContextSizes;
#endif
#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS) && ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
    // Whether this pattern may use the index-incrementing first-character read
    // (decided once at compile). m_useFirstNonBMPCharacterOptimization is the
    // AMBIENT state consulted at emission time: it is off everywhere except while
    // generating the one term whose read is the match start (m_readsStartCharacter),
    // so loop peeks and backtrack-pass re-reads can never write the register.
    bool m_canUseFirstNonBMPCharacterOptimization { false };
    bool m_useFirstNonBMPCharacterOptimization { false };
#endif
    RegisterAtOffsetList m_calleeSaves;
    MacroAssembler::JumpList m_abortExecution;
    MacroAssembler::JumpList m_hitMatchLimit;
    MacroAssembler::JumpList m_inlinedMatched;
    MacroAssembler::JumpList m_inlinedFailedMatch;

    // The matching direction currently in effect. During op-compilation this is
    // the direction ops are being emitted in (stamped onto each YarrOp); during
    // code generation and backtracking it is set from the op being processed so
    // that the direction-aware primitives (input checks, character addressing,
    // end-of-input) emit the leftward variants inside lookbehind bodies.
    MatchDirection m_direction { Forward };

    // Owns the mirrored (term-reversed) copies of lookbehind bodies. The original
    // YarrPattern is left untouched so the bytecode fallback remains valid.
    Vector<std::unique_ptr<PatternDisjunction>> m_mirroredDisjunctions;

    // First-character dispatch state for nested disjunctions (see DispatchInfo).
    Vector<UniqueRef<DispatchInfo>, 2> m_dispatchInfos;
    // First-character dispatch state for the body alternation (see BodyDispatchInfo).
    Vector<UniqueRef<BodyDispatchInfo>, 2> m_bodyDispatchInfos;

    // The regular expression expressed as a linear sequence of operations.
    Vector<YarrOp, 128> m_ops;
    Vector<UniqueRef<BoyerMooreInfo>, 4> m_bmInfos;
    Vector<UniqueRef<BoyerMooreBitmap::Map>> m_bmMaps;
    Vector<UniqueRef<MaskedAlternativeInfo>, 2> m_maskedAltInfos; // For multi-pattern SIMD search

    // This class records state whilst generating the backtracking path of code.
    BacktrackingState m_backtrackingState;

    std::unique_ptr<YarrDisassembler> m_disassembler;

    std::optional<StringView> m_sampleString;
    SubjectSampler m_sampler;
};

#if ENABLE(YARR_JIT_UNICODE_EXPRESSIONS)
static MacroAssemblerCodeRef<JITThunkPtrTag> tryReadUnicodeCharSlowThunkGenerator(VM&)
{
    CCallHelpers jit(nullptr);

    jit.tagReturnAddress();
    tryReadUnicodeCharSlowImpl<TryReadUnicodeCharGenFirstNonBMPOptimization::DontUseOptimization>(jit);
    jit.ret();

    LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk);

    return FINALIZE_THUNK(patchBuffer, JITThunkPtrTag, "Yarr tryReadUnicodeChar"_s, "YARR tryReadUnicodeChar thunk");
}

#if ENABLE(YARR_JIT_UNICODE_CAN_INCREMENT_INDEX_FOR_NON_BMP)
static MacroAssemblerCodeRef<JITThunkPtrTag> tryReadUnicodeCharIncForNonBMPSlowThunkGenerator(VM&)
{
    CCallHelpers jit(nullptr);

    jit.tagReturnAddress();
    tryReadUnicodeCharSlowImpl<TryReadUnicodeCharGenFirstNonBMPOptimization::UseOptimization>(jit);
    jit.ret();

    LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk);

    return FINALIZE_THUNK(patchBuffer, JITThunkPtrTag, "Yarr tryReadUnicodeChar w/Inc for non-BMP"_s, "YARR tryReadUnicodeChar w/Inc for non-BMP thunk");
}
#endif
#endif

#if ENABLE(YARR_JIT_BACKREFERENCES_FOR_16BIT_EXPRS)
static MacroAssemblerCodeRef<JITThunkPtrTag> areCanonicallyEquivalentThunkGenerator(VM&)
{
    CCallHelpers jit(nullptr);

    unsigned pushCount = 0;

#if CPU(ARM64)
    constexpr unsigned registersToSave = 16;

    auto pushCallerSavePair = [&]() {
        jit.pushPair(GPRInfo::toRegister(pushCount), GPRInfo::toRegister(pushCount + 1));
        pushCount += 2;
    };

    auto popCallerSavePair = [&]() {
        pushCount -= 2;
        jit.popPair(GPRInfo::toRegister(pushCount), GPRInfo::toRegister(pushCount + 1));
    };
#elif CPU(X86_64)
    constexpr unsigned registersToSave = 7;

    constexpr GPRReg callerSaves[registersToSave] = {
        // We don't save RAX since the return value ends up there.
        X86Registers::ecx,
        X86Registers::edx,
        X86Registers::esi,
        X86Registers::edi,
        X86Registers::r8,
        X86Registers::r9,
        X86Registers::r10
    };

    auto pushCallerSave = [&]() {
        jit.push(callerSaves[pushCount]);
        pushCount++;
    };

    auto popCallerSave = [&]() {
        pushCount--;
        jit.pop(callerSaves[pushCount]);
    };
#endif

    jit.emitFunctionPrologue();

#if CPU(ARM64)
    while (pushCount < registersToSave)
        pushCallerSavePair();
#elif CPU(X86_64)
    while (pushCount < registersToSave)
        pushCallerSave();
#endif

    jit.setupArguments<decltype(operationAreCanonicallyEquivalent)>(areCanonicallyEquivalentCharArgReg, areCanonicallyEquivalentPattCharArgReg, areCanonicallyEquivalentCanonicalModeArgReg);
    jit.callOperation<OperationPtrTag>(operationAreCanonicallyEquivalent);

#if CPU(ARM64)
    // Convert 8-bit bool result into 32 bit value and save in IP0 while restoring callee saves.
    jit.zeroExtend8To32(GPRInfo::returnValueGPR, ARM64Registers::ip0);

    while (pushCount)
        popCallerSavePair();

    jit.move(ARM64Registers::ip0, areCanonicallyEquivalentCharArgReg);
#elif CPU(X86_64)
    // Convert 8-bit bool result into 32 bit value.
    jit.zeroExtend8To32(GPRInfo::returnValueGPR, GPRInfo::returnValueGPR);

    while (pushCount)
        popCallerSave();
#endif

    ASSERT(!pushCount);

    jit.emitFunctionEpilogue();
    jit.ret();

    LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk);

    return FINALIZE_THUNK(patchBuffer, JITThunkPtrTag, "Yarr areCanonicallyEquivalent", "YARR areCanonicallyEquivalent call");
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAreCanonicallyEquivalent, bool, (unsigned a, unsigned b, CanonicalMode canonicalMode))
{
    return areCanonicallyEquivalent(static_cast<char32_t>(a), static_cast<char32_t>(b), canonicalMode);
}
#endif

static void dumpCompileFailure(JITFailureReason failure)
{
    switch (failure) {
    case JITFailureReason::DecodeSurrogatePair:
        dataLog("Can't JIT a pattern decoding surrogate pairs\n");
        break;
    case JITFailureReason::BackReference:
        dataLog("Can't JIT some patterns containing back references\n");
        break;
    case JITFailureReason::ParenthesizedSubpattern:
        dataLog("Can't JIT a pattern containing parenthesized subpatterns\n");
        break;
    case JITFailureReason::ParenthesisNestedTooDeep:
        dataLog("Can't JIT pattern due to parentheses nested too deeply\n");
        break;
    case JITFailureReason::ExecutableMemoryAllocationFailure:
        dataLog("Can't JIT because of failure of allocation of executable memory\n");
        break;
    case JITFailureReason::OffsetTooLarge:
        dataLog("Can't JIT because pattern exceeds string length limits\n");
        break;
    case JITFailureReason::GeneratedCodeSizeTooLarge:
        dataLog("Can't JIT because generated code size exceeds limit\n");
        break;
    }
}

void jitCompile(YarrPattern& pattern, StringView patternString, CharSize charSize, std::optional<StringView> sampleString, VM* vm, YarrCodeBlock& codeBlock, ExecutionMode mode)
{
    CCallHelpers masm;

    ASSERT(mode == ExecutionMode::MatchOnly || mode == ExecutionMode::IncludeSubpatterns);

    YarrJITDefaultRegisters jitRegisters;
    YarrGenerator<YarrJITDefaultRegisters>(masm, vm, &codeBlock, jitRegisters, pattern, patternString, charSize, mode, sampleString).compile(codeBlock);

    if (auto failureReason = codeBlock.failureReason()) {
        if (Options::dumpCompiledRegExpPatterns()) [[unlikely]] {
            pattern.dumpPatternString(WTF::dataFile(), patternString);
            dataLog(" : ");
            dumpCompileFailure(*failureReason);
        }
    }
}

#if ENABLE(YARR_JIT_REGEXP_TEST_INLINE)
#if !(CPU(ARM64) || CPU(X86_64) || CPU(RISCV64))
#error "No support for inlined JIT'ing of RegExp.test for this CPU / OS combination."
#endif

void jitCompileInlinedTest(StackCheck* m_compilationThreadStackChecker, StringView patternString, OptionSet<Yarr::Flags> flags, CharSize charSize, VM* vm, YarrBoyerMooreData& boyerMooreData, CCallHelpers& jit, YarrJITRegisters& jitRegisters)
{
    Yarr::ErrorCode errorCode;
    Yarr::YarrPattern pattern(patternString, flags, errorCode, ExecutionMode::InlineTest);

    if (errorCode != Yarr::ErrorCode::NoError) {
        // This path cannot clobber jitRegisters.regT1 as it is needed for the slow path we'll end up in.
        jit.move(MacroAssembler::TrustedImmPtr((void*)static_cast<size_t>(JSRegExpResult::JITCodeFailure)), jitRegisters.returnRegister);
        return;
    }

    jitRegisters.validate();

    YarrGenerator<YarrJITRegisters> yarrGenerator(jit, vm, &boyerMooreData, jitRegisters, pattern, patternString, charSize, ExecutionMode::InlineTest);
    yarrGenerator.setStackChecker(m_compilationThreadStackChecker);
    yarrGenerator.compileInline(boyerMooreData);
}
#endif

void YarrCodeBlock::dumpSimpleName(PrintStream& out) const
{
    if (m_regExp)
        RegExp::dumpToStream(m_regExp, out);
    else
        out.print("unspecified");
}

}}

#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
