/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVarint.h"
#include "CodeType.h"
#include "LineColumn.h"
#include <compare>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/IterationStatus.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

enum class TraceVisibility : uint8_t;
enum class StackTraceBuiltinKind : uint8_t;

enum class CallableKind : uint8_t { Plain, Arrow, Method, Bound };

static constexpr unsigned classObjectClosureStructure = 4;
static constexpr unsigned numberOfClosureStructures = classObjectClosureStructure + 1;
static constexpr unsigned numberOfSlimClosureKinds = 2;
// In FunctionKindData::accessorKind, beside the members of MethodKind: no accessor, and a stack frame shows another name once something has looked for the property `name` of the function object.
static constexpr unsigned methodThatNotesLooksForName = 3;

struct FunctionKindData {
    uint16_t closureStructure : 3;
    uint16_t isSlim : 1;
    uint16_t isAsync : 1;
    uint16_t isGenerator : 1;
    uint16_t takesNewTarget : 1;
    uint16_t isCommonJSModule : 1;
    uint16_t isInRuntimeModule : 1;
    uint16_t accessorKind : 2;
    uint16_t isConstructor : 1;
    uint16_t callableKind : 2;
    uint16_t isDerivedConstructor : 1;
    uint16_t hasInferredName : 1;

    constexpr unsigned closureStructureOfKind() const { return isConstructor ? classObjectClosureStructure : isAsync | isGenerator << 1; }
    constexpr unsigned slimClosureKind() const { return isAsync; }
    constexpr CallableKind callable() const { return static_cast<CallableKind>(callableKind); }
    constexpr MethodKind accessor() const { return accessorKind == methodThatNotesLooksForName ? MethodKind::Method : static_cast<MethodKind>(accessorKind); }
    constexpr bool notesLooksForName() const { return accessorKind != static_cast<unsigned>(MethodKind::Method); }
    constexpr bool isBound() const { return callable() == CallableKind::Bound; }
    constexpr bool isConstructiblePlainFunction() const { return !isConstructor && callable() == CallableKind::Plain && !isAsync && !isGenerator; }
    // The function object stays as it is: the name that the look brings into its frames is the one that the tables have.
    constexpr bool notesLooksForNameInCell() const { return accessorKind == methodThatNotesLooksForName && !isBound(); }
    constexpr bool hasInitialNamePrefix() const { return isBound() || accessor() != MethodKind::Method; }
};
static_assert(sizeof(FunctionKindData) == 2 && isImageRecord<FunctionKindData>);
static_assert(classObjectClosureStructure < 1 << 3);

struct FunctionTraceData {
    uint16_t visibilityBits : 2;
    uint16_t builtinKindBits : 2;
    uint16_t codeTypeBits : 2;
    uint16_t rejectsOnThrow : 1;
    uint16_t isStrict : 1;
    uint16_t isConstructor : 1;
    uint16_t isAsync : 1;
    uint16_t isNamedByOwner : 1;
    uint16_t isNakedConstructor : 1;
    uint16_t implementsBuiltin : 1;
    // `{ [Symbol.dispose]() { } }`: the written name is what the property `name` holds. The engine gives such a function that property when the literal is evaluated, and the console goes on showing it without a name.
    uint16_t isNamedByComputedKey : 1;
    // Its frames are named by the function object that runs in them.
    uint16_t recordsCallee : 1;
    // They are from the first time on that the run time sees a function object of the function get a displayName.
    uint16_t recordsCalleeOnceRenamed : 1;

    TraceVisibility visibility() const { return static_cast<TraceVisibility>(visibilityBits); }
    // A function that is written inside a helper of the compiler implements no built-in. No other function that implements none has the kind of a constructor.
    static constexpr uint16_t kindBitsInsideHelperOfCompiler = 2;
    bool isInsideHelperOfCompiler() const { return !implementsBuiltin && builtinKindBits == kindBitsInsideHelperOfCompiler; }
    // Plain Bun keeps it in a module of its own, and a stack trace lists it like any other function.
    bool isHelperOfCompiler() const { return implementsBuiltin ? !builtinKindBits : isInsideHelperOfCompiler(); }
    StackTraceBuiltinKind builtinKind() const { return static_cast<StackTraceBuiltinKind>(isInsideHelperOfCompiler() ? 0 : builtinKindBits); }
    CodeType codeType() const { return static_cast<CodeType>(codeTypeBits); }
};
static_assert(sizeof(FunctionTraceData) == 2 && isImageRecord<FunctionTraceData>);

// A program has few different pairs of the two, so the image holds each pair once and, for every function, the number of its pair in one byte.
struct FunctionRecordData {
    FunctionKindData kind;
    FunctionTraceData trace;
};
static_assert(sizeof(FunctionRecordData) == 4 && isImageRecord<FunctionRecordData>);

// The number of a function whose pair is not among the most common ones. Its pair is listed under the function itself.
static constexpr uint8_t functionRecordIsListedByFunction = 0xff;

struct FunctionRecordByFunctionData {
    uint32_t function;
    FunctionRecordData record;
};
static_assert(sizeof(FunctionRecordByFunctionData) == 8 && isImageRecord<FunctionRecordByFunctionData>);

// What the compiler says of one function. The image has the bits of every function, the source of the functions from the first derived one on, and the few functions that implement a built-in.
struct FunctionTraceRecord : FunctionTraceData {
    uint32_t sourceFunctionIndex;
    uint32_t implementedBuiltinPlusOne;

    FunctionID sourceFunction() const { return FunctionID(sourceFunctionIndex); }
    std::optional<BuiltinID> implements() const
    {
        if (!implementedBuiltinPlusOne)
            return std::nullopt;
        return BuiltinID(implementedBuiltinPlusOne - 1);
    }
};

// The functions from firstFunction up to the next run that are their own source have the name.
struct FunctionNameRunData {
    uint32_t firstFunction;
    uint32_t name;
};
static_assert(sizeof(FunctionNameRunData) == 8 && isImageRecord<FunctionNameRunData>);

struct ImplementedBuiltinData {
    uint32_t function;
    uint32_t builtin;
};
static_assert(sizeof(ImplementedBuiltinData) == 8 && isImageRecord<ImplementedBuiltinData>);

struct ConstructorData {
    uint32_t function;
    uint32_t layoutIndex;
};
static_assert(sizeof(ConstructorData) == 8 && isImageRecord<ConstructorData>);

struct BoundFunctionData {
    uint32_t function;
    uint32_t target;
    uint32_t numberOfBoundArguments : 16;
    uint32_t targetClosureSlotPlusOne : 16;
    // The target constructs, so the bound function does: with the bound arguments that its closure holds, as values, one behind the other from this slot on.
    uint32_t constructs : 1;
    uint32_t firstArgumentSlot : 15;
    uint32_t numberOfCapturedArguments : 16;

    std::optional<unsigned> targetClosureSlot() const
    {
        if (!targetClosureSlotPlusOne)
            return std::nullopt;
        return targetClosureSlotPlusOne - 1;
    }
};
static_assert(sizeof(BoundFunctionData) == 16 && isImageRecord<BoundFunctionData>);

// Both functions by closure number.
struct ConstructEntrypointData {
    // The entrypoint allocates the object itself, and is entered only where the function is its own new.target. Under any other new.target [[Construct]] enters at the entry of [[Call]] with a new object.
    static constexpr uint32_t allocatesThis = 1u << 31;
    // In place of an entrypoint: a plain function that only returns undefined, so the new object is all that its [[Construct]] makes. No stub looks for such a function here.
    static constexpr uint32_t onlyReturns = UINT32_MAX;

    uint32_t function;
    uint32_t constructEntrypointAndFlag;

    constexpr uint32_t constructEntrypoint() const { return constructEntrypointAndFlag & ~allocatesThis; }
};
static_assert(sizeof(ConstructEntrypointData) == 8 && isImageRecord<ConstructEntrypointData>);

// Where a plain function makes a call in tail position. A function that is constructed is listed in stack traces until that call has returned, though its frame is gone.
// In front of those of a function stands the place where it is listed while `this` is made for it, which a stub does before the function has a frame.
// ProgramTables::placesOfTailCalls has them one behind the other, ascending by function. Each says how far it is from the one in front.
struct TailCallPlace {
    static constexpr uint32_t noCallee = UINT32_MAX;

    void append(Vector<uint8_t>& bytes, const TailCallPlace& inFront) const
    {
        bool namesCallee = callee != noCallee;
        bool isInOtherSource = sourcePlusOne != inFront.sourcePlusOne;
        appendVarint(bytes, static_cast<uint64_t>(function - inFront.function) << 3 | isWhereThisIsMade << 2 | isInOtherSource << 1 | namesCallee);
        if (isInOtherSource)
            appendVarint(bytes, sourcePlusOne);
        appendVarint(bytes, zigZagEncode(static_cast<int64_t>(line) - inFront.line));
        appendVarint(bytes, column);
        if (namesCallee)
            appendVarint(bytes, callee);
    }

    static TailCallPlace read(const uint8_t*& cursor, const TailCallPlace& inFront)
    {
        TailCallPlace place;
        uint64_t head = readVarint(cursor);
        place.function = inFront.function + static_cast<uint32_t>(head >> 3);
        place.isWhereThisIsMade = head & 4;
        place.sourcePlusOne = head & 2 ? static_cast<uint32_t>(readVarint(cursor)) : inFront.sourcePlusOne;
        place.line = static_cast<uint32_t>(inFront.line + zigZagDecode(readVarint(cursor)));
        place.column = static_cast<uint32_t>(readVarint(cursor));
        if (head & 1)
            place.callee = static_cast<uint32_t>(readVarint(cursor));
        return place;
    }

    // Both as they are written. The callee is named where the function has several places and the call says whom it enters.
    uint32_t function { 0 };
    uint32_t callee { noCallee };
    bool isWhereThisIsMade { false };
    // All three are zero in an image without positions.
    uint32_t sourcePlusOne { 0 };
    uint32_t line { 0 };
    uint32_t column { 0 };
};

inline void forEachTailCallPlace(std::span<const uint8_t> bytes, NOESCAPE const Invocable<IterationStatus(const TailCallPlace&)> auto& functor)
{
    TailCallPlace place;
    for (const uint8_t* cursor = bytes.data(); cursor < std::to_address(bytes.end());) {
        place = TailCallPlace::read(cursor, place);
        if (functor(place) == IterationStatus::Done)
            return;
    }
}

struct ClosureFunctionRangeData {
    uint32_t first;
    uint32_t end;
    uint32_t omittedWordCount;
    // The closure number of `first`. Not zero in an image that was built above others for the same realms: a closure says by its number which of them it is of.
    uint32_t numbersBelow;
};
static_assert(sizeof(ClosureFunctionRangeData) == 16 && isImageRecord<ClosureFunctionRangeData>);

struct FunctionTexts {
    uint32_t numberOfEntries;
    uint32_t entriesOffset;
    uint32_t textOffset;
    // One more PiecesOfModule follows them, which ends the pieces of the last.
    uint32_t numberOfModulesWithPieces;
    uint32_t modulesWithPiecesOffset;
    uint32_t piecesOffset;
};
static_assert(sizeof(FunctionTexts) == 24 && isImageRecord<FunctionTexts>);

// The text is pieces of files with the types cut out, so a line and a column of a file lead into it only by way of these. In the order of the modules.
// The pieces of a module follow one another as in its file, so a place lies in the last piece that starts at it or in front of it. Every number is a varint.
//   A piece: how far its text starts behind the end of the piece before it (zigzag), times eight, plus what it is a part of, doubled, plus one; its size;
//   how many lines it starts below the start of the piece before it; its column. See PieceOfKeptText::PartOf.
//   A cut in that piece: how far it lies behind the cut before it, or behind the start of the piece, doubled; the columns that are gone, doubled, plus one where lines are gone too; then how many lines.
//   Where lines are gone, the columns are those in front of what follows the cut on its line.
// What it takes to find a place does not grow with the file: a module with many pieces has several of these records, each of which counts from nothing as the first does,
// and the one that holds a place is found by where each starts; and a long piece comes in parts, so that no more than a part is followed character by character.
struct PiecesOfModule {
    static constexpr size_t bytesOfOneRecord = 2048;
    static constexpr uint32_t bytesOfOnePart = 32768;

    uint32_t module;
    uint32_t start;
};
static_assert(sizeof(PiecesOfModule) == 8 && isImageRecord<PiecesOfModule>);

struct PieceOfKeptText {
    // Listed: how much of the longer piece lies in front of this part follows, and how much behind it.
    enum class PartOf : uint8_t { Nothing, LongerPieceThatIsListed, SameAsPieceInFront };

    static PieceOfKeptText read(const uint8_t*& cursor, const PieceOfKeptText& inFront)
    {
        uint64_t head = readVarint(cursor);
        PieceOfKeptText piece;
        piece.start = static_cast<uint32_t>(inFront.end + zigZagDecode(head >> 3));
        piece.end = piece.start + static_cast<uint32_t>(readVarint(cursor));
        piece.line = inFront.line + static_cast<uint32_t>(readVarint(cursor));
        piece.column = static_cast<uint32_t>(readVarint(cursor));
        switch (static_cast<PartOf>(head >> 1 & 3)) {
        case PartOf::LongerPieceThatIsListed:
            piece.startOfWhole = piece.start - static_cast<uint32_t>(readVarint(cursor));
            piece.endOfWhole = piece.end + static_cast<uint32_t>(readVarint(cursor));
            break;
        case PartOf::SameAsPieceInFront:
            piece.startOfWhole = inFront.startOfWhole;
            piece.endOfWhole = inFront.endOfWhole;
            break;
        default:
            piece.startOfWhole = piece.start;
            piece.endOfWhole = piece.end;
        }
        return piece;
    }

    void appendTo(Vector<uint8_t>& bytes, const PieceOfKeptText& inFront) const
    {
        PartOf partOf = startOfWhole == start && endOfWhole == end ? PartOf::Nothing : startOfWhole == inFront.startOfWhole && endOfWhole == inFront.endOfWhole ? PartOf::SameAsPieceInFront : PartOf::LongerPieceThatIsListed;
        appendVarint(bytes, zigZagEncode(static_cast<int64_t>(start) - static_cast<int64_t>(inFront.end)) << 3 | static_cast<uint64_t>(partOf) << 1 | 1);
        appendVarint(bytes, end - start);
        appendVarint(bytes, line - inFront.line);
        appendVarint(bytes, column);
        if (partOf == PartOf::LongerPieceThatIsListed) {
            appendVarint(bytes, start - startOfWhole);
            appendVarint(bytes, endOfWhole - end);
        }
    }

    bool startsBehind(LineColumn place) const { return line > place.line || (line == place.line && column > place.column); }

    uint32_t start { 0 };
    uint32_t end { 0 };
    uint32_t line { 0 };
    uint32_t column { 0 };
    uint32_t startOfWhole { 0 };
    uint32_t endOfWhole { 0 };
};

// Follows a piece of text through the file that it is from. A column counts units of UTF-16, and the text is UTF-8.
struct PlaceOfTextInFile {
    // Goes over the character at text[at] and answers where the next one starts.
    size_t advanceOver(std::span<const uint8_t> text, size_t at)
    {
        uint8_t byte = text[at];
        bool wasBehindCarriageReturn = std::exchange(isBehindCarriageReturn, byte == '\r');
        if (byte == '\n' && wasBehindCarriageReturn)
            return at + 1;
        if (byte < 0x80) {
            if (byte == '\n' || byte == '\r')
                startLine();
            else
                ++column;
            return at + 1;
        }
        size_t length = byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
        // U+2028 and U+2029
        if (byte == 0xE2 && at + 2 < text.size() && text[at + 1] == 0x80 && (text[at + 2] == 0xA8 || text[at + 2] == 0xA9))
            startLine();
        else
            column += length == 4 ? 2 : 1;
        return at + length;
    }

    void advanceOverCut(uint32_t lines, uint32_t columns)
    {
        isBehindCarriageReturn = false;
        line += lines;
        column = lines ? columns + 1 : column + columns;
    }

    void startLine()
    {
        ++line;
        column = 1;
    }

    uint32_t line;
    uint32_t column;
    bool isBehindCarriageReturn { false };
};

// `bun run` runs what Bun's transpiler prints of a file, and an error quotes that: the value in place of the name of a constant, an initializer in place of the
// only use of a variable, `3` for `1 + 2`. These say what is printed in place of which piece of the text. In the order of the modules, and one more ends the bytes of the last.
// The first byte of textPrintedInPlace says what a start counts from: 0, the start of the text of all functions; 1, the start of the file, for a test run, which reads the files.
// A module with many pieces has several of these records, each of which counts from nothing as the first does: what is read for a part of the text does not grow with the file.
struct TextPrintedInPlaceOfModule {
    static constexpr size_t bytesOfOneRecord = 2048;

    uint32_t module;
    uint32_t start;
};
static_assert(sizeof(TextPrintedInPlaceOfModule) == 8 && isImageRecord<TextPrintedInPlaceOfModule>);

// Module::typesCutFromFunctionText: what the text that is kept of a file lacks of the file, in the order of the file. Every number is a varint.
//   How far the cut starts behind the end of the cut before it; its size; and behind a size of nothing, how much the text has there that the file lacks.
class CutsFromFunctionText {
public:
    explicit CutsFromFunctionText(std::span<const uint8_t> packed)
    {
        constexpr uint64_t largest = std::numeric_limits<uint32_t>::max();
        auto takeNumber = [&] {
            uint64_t number = 0;
            for (unsigned shift = 0; !packed.empty() && shift < 64; shift += 7) {
                uint8_t byte = packed.front();
                packed = packed.subspan(1);
                number |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if (!(byte & 0x80))
                    return number;
            }
            m_isValid = false;
            return number;
        };
        for (uint64_t end = 0, sizeOfCutsInFront = 0; !packed.empty();) {
            uint64_t distance = takeNumber();
            uint64_t size = takeNumber();
            uint64_t added = size ? 0 : takeNumber();
            if (distance > largest - end || size > largest - end - distance || added > largest)
                m_isValid = false;
            if (!m_isValid)
                break;
            uint64_t start = end + distance;
            m_cuts.append({ start, size, added, sizeOfCutsInFront });
            end = start + size;
            sizeOfCutsInFront += size - added;
        }
    }

    // No number is cut short, and every cut lies in a file. Then what stands in front of something in the file does so in the text.
    bool isValid() const { return m_isValid; }
    bool isEmpty() const { return m_cuts.isEmpty(); }

    uint32_t inText(uint32_t offsetInFile) const
    {
        auto next = std::ranges::lower_bound(m_cuts, static_cast<uint64_t>(offsetInFile), { }, &Cut::start);
        if (next == m_cuts.begin())
            return offsetInFile;
        const Cut& last = *std::prev(next);
        return static_cast<uint32_t>(std::min<uint64_t>(offsetInFile - last.sizeOfCutsInFront - std::min<uint64_t>(last.size, offsetInFile - last.start) + last.added, std::numeric_limits<uint32_t>::max()));
    }

private:
    struct Cut {
        uint64_t start;
        uint64_t size;
        uint64_t added;
        uint64_t sizeOfCutsInFront;
    };
    Vector<Cut> m_cuts;
    bool m_isValid { true };
};

// The pieces of a module ascend by their starts, and one that holds others stands in front of them. Every number is a varint.
//   How far the piece starts behind the start of the piece before it; its size; then either
//   the size of what is printed, doubled, and those bytes, or
//   the size of another piece that is printed there too, times four, plus two where it is printed in round brackets, plus one; and how far that piece starts in front of this one (zigzag).
// A file that `bun run` runs as it is written, such as the output of `bun build`, has one piece of no size.
struct TextPrintedInPlace {
    static bool sayFileRunsAsWritten(std::span<const TextPrintedInPlace> pieces) { return pieces.size() == 1 && !pieces[0].end; }

    static Vector<TextPrintedInPlace> readAll(std::span<const uint8_t> bytes)
    {
        Vector<TextPrintedInPlace> pieces;
        appendThoseThatStartIn(pieces, bytes, 0, std::numeric_limits<uint64_t>::max());
        return pieces;
    }

    // The first piece that breaks the format, if one does, as far as it could be read, and which it is. Such a piece is left out. Those behind it are read, unless its bytes do not say where it ends.
    struct Broken {
        size_t index;
        // Or it reaches out of every file.
        bool isCutShort;
        uint32_t start { 0 };
        uint32_t end { 0 };
        uint32_t startOfCopied { 0 };
        uint32_t endOfCopied { 0 };
    };
    static std::optional<Broken> appendThoseThatStartIn(Vector<TextPrintedInPlace>& pieces, std::span<const uint8_t> bytes, uint64_t from, uint64_t to)
    {
        constexpr uint64_t largest = std::numeric_limits<uint32_t>::max();
        const uint8_t* cursor = bytes.data();
        const uint8_t* end = std::to_address(bytes.end());
        // A number takes ten bytes at the most.
        bool isCutShort = false;
        auto readNumber = [&] {
            uint64_t number = 0;
            for (unsigned shift = 0; cursor < end && shift < 64; shift += 7) {
                uint8_t byte = *cursor++;
                number |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if (!(byte & 0x80))
                    return number;
            }
            isCutShort = true;
            return number;
        };
        std::optional<Broken> firstBroken;
        auto note = [&](Broken broken) {
            if (!firstBroken)
                firstBroken = broken;
        };
        uint64_t start = 0;
        for (size_t index = 0; cursor < end; ++index) {
            uint64_t distance = readNumber();
            uint64_t size = readNumber();
            uint64_t printed = readNumber();
            if (isCutShort || distance > largest - start) {
                note({ index, true });
                break;
            }
            start += distance;
            if (start >= to)
                break;
            bool reachesOut = size > largest - start;
            TextPrintedInPlace piece { static_cast<uint32_t>(start), static_cast<uint32_t>(reachesOut ? start : start + size) };
            if (printed & 1) {
                int64_t inFront = zigZagDecode(readNumber());
                uint64_t sizeOfCopied = printed >> 2;
                if (isCutShort) {
                    note({ index, true, piece.start, piece.end });
                    break;
                }
                reachesOut = reachesOut || inFront > static_cast<int64_t>(start) || inFront < -static_cast<int64_t>(largest) || sizeOfCopied > largest
                    || static_cast<uint64_t>(static_cast<int64_t>(start) - inFront) + sizeOfCopied > largest;
                if (reachesOut) {
                    note({ index, true, piece.start, piece.end });
                    continue;
                }
                piece.isCopy = true;
                piece.isInBrackets = printed & 2;
                piece.startOfCopied = static_cast<uint32_t>(static_cast<int64_t>(start) - inFront);
                piece.endOfCopied = static_cast<uint32_t>(piece.startOfCopied + sizeOfCopied);
                if (!piece.copiesWhatLiesInFrontOrBeneath()) {
                    if (!piece.copiesAllOfItself())
                        note({ index, false, piece.start, piece.end, piece.startOfCopied, piece.endOfCopied });
                    continue;
                }
            } else {
                if (printed >> 1 > static_cast<uint64_t>(end - cursor)) {
                    note({ index, true, piece.start, piece.end });
                    break;
                }
                piece.bytes = { cursor, static_cast<size_t>(printed >> 1) };
                cursor += printed >> 1;
                if (reachesOut) {
                    note({ index, true, piece.start, piece.end });
                    continue;
                }
            }
            if (start >= from)
                pieces.append(piece);
        }
        return firstBroken;
    }

    // An initializer ends in front of the name that it replaces, and an operand is smaller than its operator. So to follow the copies comes to an end:
    // the end of what is followed never grows, and where it stays, the size shrinks.
    bool copiesWhatLiesInFrontOrBeneath() const
    {
        if (start > end || startOfCopied > endOfCopied)
            return false;
        bool isInFront = endOfCopied <= start;
        bool isBeneath = startOfCopied >= start && endOfCopied <= end && endOfCopied - startOfCopied < end - start;
        return isInFront || isBeneath;
    }

    // Of `a ? b : c` with a constant `a`, `b` is printed, and `b` may be all that the text keeps of it. Then the text reads as `bun run` has it, and the piece says nothing.
    // The cuts keep what stands in front of something in front of it, so this is all that they can do to a piece that keeps the rule above.
    bool copiesAllOfItself() const { return isCopy && startOfCopied == start && endOfCopied == end; }

    TextPrintedInPlace inText(const CutsFromFunctionText& cuts) const
    {
        TextPrintedInPlace moved = *this;
        moved.start = cuts.inText(start);
        moved.end = cuts.inText(end);
        moved.startOfCopied = cuts.inText(startOfCopied);
        moved.endOfCopied = cuts.inText(endOfCopied);
        return moved;
    }

    void appendTo(Vector<uint8_t>& bytes, uint32_t startOfPieceInFront) const
    {
        appendVarint(bytes, start - startOfPieceInFront);
        appendVarint(bytes, end - start);
        if (!isCopy) {
            appendVarint(bytes, static_cast<uint64_t>(this->bytes.size()) << 1);
            bytes.append(this->bytes);
            return;
        }
        appendVarint(bytes, static_cast<uint64_t>(endOfCopied - startOfCopied) << 2 | isInBrackets << 1 | 1);
        appendVarint(bytes, zigZagEncode(static_cast<int64_t>(start) - static_cast<int64_t>(startOfCopied)));
    }

    uint32_t start { 0 };
    uint32_t end { 0 };
    std::span<const uint8_t> bytes { };
    bool isCopy { false };
    bool isInBrackets { false };
    uint32_t startOfCopied { 0 };
    uint32_t endOfCopied { 0 };
};

// In front of what is printed in place of a key, so that nobody takes `a["b"]` for what the program has written: that is printed `a.b`. No text holds this byte, and whoever shows the text takes it out.
// Behind the `/*` of the name that Bun prints behind the value of a member of an enum, `1 /* Green */`: a comment of the program is not printed.
static constexpr uint8_t markOfPrintedKey = 1;

// Behind the `[` or `{` of what is printed over several lines: one, and one more for each indentation of the line that holds the bracket. Whoever shows the text takes them out too.
static constexpr uint8_t markOfIndentation = 2;

// In front of and behind a function or a class as Bun's printer printed it at the build: it prints the statements of a body anew, and they are not what the file spells.
// Nobody reads what stands between the two, and whoever shows the text takes them out. A list in such a function is quoted by itself, so the pieces beneath this one count.
static constexpr uint8_t markOfTextFromPrinter = 3;

inline bool isFromPrinter(const TextPrintedInPlace& piece)
{
    return !piece.isCopy && !piece.bytes.empty() && piece.bytes.front() == markOfTextFromPrinter;
}

// Behind the first mark may stand markOfDifferences, and then what is printed, a byte for a byte, but for
//   copyOfWrittenText, how many bytes of the written text are passed over, how many are printed then;
//   lineBreakWithIndentation, how often the next line is indented;
//   textWithPieceOfItsOwn, how many bytes of the written text are passed over, in how many a function is written that has a piece of its own;
//   writtenTextAsEscapes, how many bytes of the written text are passed over, how many are printed then, none of them ASCII, each character as Bun's printer escapes it.
// The numbers are varints. There is no mark at the end.
static constexpr uint8_t markOfDifferences = 4;
static constexpr uint8_t copyOfWrittenText = 5;
static constexpr uint8_t lineBreakWithIndentation = 6;
static constexpr uint8_t textWithPieceOfItsOwn = 7;
static constexpr uint8_t writtenTextAsEscapes = 8;

// The piece that is to be printed next, where there is one; then this goes on with what is left of `written` and `differences`.
template<typename Character>
const TextPrintedInPlace* appendTextFromPrinterUpToInnerPiece(Vector<Character>& printed, std::span<const Character> text, const TextPrintedInPlace& piece, std::span<const Character>& written, std::span<const uint8_t>& differences, std::span<const TextPrintedInPlace> pieces)
{
    auto takeNumber = [&] {
        uint64_t number = 0;
        for (unsigned shift = 0; !differences.empty() && shift < 64; shift += 7) {
            uint8_t byte = differences.front();
            differences = differences.subspan(1);
            number |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                break;
        }
        return number;
    };
    while (!differences.empty()) {
        uint8_t byte = differences.front();
        differences = differences.subspan(1);
        if (byte == copyOfWrittenText || byte == textWithPieceOfItsOwn) {
            uint64_t passedOver = takeNumber();
            uint64_t size = takeNumber();
            if (passedOver > written.size() || size > written.size() - passedOver)
                break;
            size_t start = written.data() - text.data() + passedOver;
            std::span<const Character> copied = written.subspan(passedOver, size);
            written = written.subspan(passedOver + size);
            if (byte == textWithPieceOfItsOwn && size) {
                // What a piece holds stands behind it. So to follow the pieces comes to an end.
                auto inner = std::ranges::lower_bound(pieces, start, { }, &TextPrintedInPlace::start);
                while (inner != pieces.end() && inner->start == start && (inner->end != start + size || !isFromPrinter(*inner) || !std::less<const TextPrintedInPlace*>()(&piece, &*inner)))
                    ++inner;
                if (inner != pieces.end() && inner->start == start)
                    return &*inner;
            }
            printed.append(copied);
        } else if (byte == writtenTextAsEscapes) {
            uint64_t passedOver = takeNumber();
            uint64_t size = takeNumber();
            if (passedOver > written.size() || size > written.size() - passedOver)
                break;
            auto appendEscape = [&](char letter, unsigned digits, uint32_t number) {
                printed.append(static_cast<Character>('\\'));
                printed.append(static_cast<Character>(letter));
                while (digits--)
                    printed.append(static_cast<Character>("0123456789ABCDEF"[(number >> (digits * 4)) & 15]));
            };
            for (std::span<const Character> escaped = written.subspan(passedOver, size); !escaped.empty();) {
                uint8_t lead = static_cast<uint8_t>(escaped.front());
                size_t length = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                if (lead < 0xC2 || length > escaped.size())
                    break;
                uint32_t character = lead & (0xFF >> (length + 1));
                for (size_t index = 1; index < length; ++index)
                    character = character << 6 | (static_cast<uint8_t>(escaped[index]) & 0x3F);
                escaped = escaped.subspan(length);
                if (character <= 0xFF)
                    appendEscape('x', 2, character);
                else if (character <= 0xFFFF)
                    appendEscape('u', 4, character);
                else {
                    appendEscape('u', 4, 0xD800 + ((character - 0x10000) >> 10));
                    appendEscape('u', 4, 0xDC00 + ((character - 0x10000) & 0x3FF));
                }
            }
            written = written.subspan(passedOver + size);
        } else if (byte == lineBreakWithIndentation) {
            printed.append(static_cast<Character>('\n'));
            printed.appendUsingFunctor(std::min<uint64_t>(takeNumber(), text.size()) * 2, [](size_t) {
                return static_cast<Character>(' ');
            });
        } else
            printed.append(static_cast<Character>(byte));
    }
    differences = { };
    return nullptr;
}

// `outermost` is one of `pieces`.
template<typename Character>
void appendTextFromPrinterWithoutMarks(Vector<Character>& printed, std::span<const Character> text, const TextPrintedInPlace& outermost, std::span<const TextPrintedInPlace> pieces)
{
    // How deep functions stand in functions is up to the program, so the pieces that are begun are listed here, and the stack does not grow with them.
    struct Begun {
        const TextPrintedInPlace* piece;
        std::span<const Character> written;
        std::span<const uint8_t> differences;
    };
    Vector<Begun, 4> begun;
    auto begin = [&](const TextPrintedInPlace& piece) {
        if (piece.bytes.empty())
            return;
        if (piece.bytes.size() < 2 || piece.bytes[1] != markOfDifferences) {
            printed.append(byteCast<Character>(piece.bytes.subspan(1, piece.bytes.size() - std::min<size_t>(piece.bytes.size(), 2))));
            return;
        }
        if (piece.start > piece.end || piece.end > text.size())
            return;
        begun.append({ &piece, text.subspan(piece.start, piece.end - piece.start), piece.bytes.subspan(2) });
    };
    begin(outermost);
    while (!begun.isEmpty()) {
        Begun current = begun.takeLast();
        if (const TextPrintedInPlace* inner = appendTextFromPrinterUpToInnerPiece(printed, text, *current.piece, current.written, current.differences, pieces)) {
            begun.append(current);
            begin(*inner);
        }
    }
}

template<typename Character>
void appendTextFromPrinter(Vector<Character>& printed, std::span<const Character> text, const TextPrintedInPlace& piece, std::span<const TextPrintedInPlace> pieces)
{
    printed.append(static_cast<Character>(markOfTextFromPrinter));
    appendTextFromPrinterWithoutMarks(printed, text, piece, pieces);
    printed.append(static_cast<Character>(markOfTextFromPrinter));
}

// text[from, to) as `bun run` has it. `pieces` are those of the module, and count from the start of `text`.
template<typename Character>
void appendWithTextPrintedInPlace(Vector<Character>& printed, std::span<const Character> text, size_t from, size_t to, std::span<const TextPrintedInPlace> pieces)
{
    if (from > to || to > text.size())
        return;
    // An initializer may hold the only use of another variable, whose initializer holds the next. How long that goes on is up to the program,
    // so the ranges that are begun are listed here, and the stack does not grow with them.
    struct Begun {
        size_t at;
        size_t to;
        size_t nextPiece;
        // Its copied text is the range that was begun next.
        const TextPrintedInPlace* copy { nullptr };
        size_t startOfPrinted { 0 };
    };
    Vector<Begun, 4> begun;
    auto begin = [&](size_t start, size_t end) {
        begun.append({ start, end, static_cast<size_t>(std::ranges::lower_bound(pieces, start, { }, &TextPrintedInPlace::start) - pieces.begin()) });
    };
    auto finish = [&](Begun& range, const TextPrintedInPlace& piece) {
        range.at = piece.end;
        // `1.toString` is no program, so `1 .toString` is printed.
        bool isNumber = printed.size() > range.startOfPrinted && isASCIIDigit(printed[range.startOfPrinted]) && std::ranges::all_of(printed.subspan(range.startOfPrinted), [](Character character) {
            return isASCIIDigit(character) || character == '.' || character == 'e';
        });
        if (isNumber && range.at < text.size() && text[range.at] == '.')
            printed.append(' ');
    };
    begin(from, to);
    while (!begun.isEmpty()) {
        Begun range = begun.takeLast();
        if (const TextPrintedInPlace* copy = std::exchange(range.copy, nullptr)) {
            if (copy->isInBrackets)
                printed.append(')');
            finish(range, *copy);
        }
        while (!range.copy && range.nextPiece < pieces.size() && pieces[range.nextPiece].start < range.to) {
            const TextPrintedInPlace& piece = pieces[range.nextPiece++];
            // It stands in a piece that is done, or reaches out of the text that is asked for. Or the text lacks it: what was written there never runs.
            if (piece.start < range.at || piece.end > range.to || piece.end <= piece.start)
                continue;
            if (piece.isCopy && (piece.endOfCopied > text.size() || !piece.copiesWhatLiesInFrontOrBeneath()))
                continue;
            printed.append(text.subspan(range.at, piece.start - range.at));
            if (!printed.isEmpty() && printed.last() == '[')
                printed.append(static_cast<Character>(markOfPrintedKey));
            range.startOfPrinted = printed.size();
            if (piece.isCopy) {
                if (piece.isInBrackets)
                    printed.append('(');
                range.copy = &piece;
                continue;
            }
            if (isFromPrinter(piece))
                appendTextFromPrinter(printed, text, piece, pieces);
            else {
                size_t endOfOpening = 0;
                if (piece.bytes.size() > 4 && piece.bytes[piece.bytes.size() - 2] == '*' && piece.bytes.back() == '/') {
                    for (endOfOpening = piece.bytes.size() - 2; endOfOpening > 1 && (piece.bytes[endOfOpening - 2] != '/' || piece.bytes[endOfOpening - 1] != '*'); --endOfOpening) { }
                }
                printed.append(byteCast<Character>(piece.bytes.first(endOfOpening)));
                if (endOfOpening > 1)
                    printed.append(static_cast<Character>(markOfPrintedKey));
                printed.append(byteCast<Character>(piece.bytes.subspan(endOfOpening)));
            }
            finish(range, piece);
        }
        if (range.copy) {
            begun.append(range);
            begin(range.copy->startOfCopied, range.copy->endOfCopied);
            continue;
        }
        printed.append(text.subspan(range.at, range.to - range.at));
    }
}

// In the order of the sources of the functions, so that both a function and its source lead to the text.
struct FunctionTextEntry {
    Number24 function;
    Number32 start;
    Number32 end;
};
static_assert(sizeof(FunctionTextEntry) == 11 && isImageRecord<FunctionTextEntry>);

// The text of a constructor is that of its class, and the constructor begins at its parameters. This is how many lines of the text stand above them, where there are any.
// Ascending by the function, which is its own source.
struct ConstructorLineData {
    uint32_t function;
    uint32_t linesOfTextAbove;
};
static_assert(sizeof(ConstructorLineData) == 8 && isImageRecord<ConstructorLineData>);

// What failed is looked for in the line of the place, and a minifier leaves a whole function in one line. No expression of a function holds the start of a statement of that function,
// so in a long line the compiler lists some of them, and only what stands between two of them is looked through. See SourcePositions::startsOfStatementsInLongLines.
// Both count from the start of the text that is kept. Ascending by the function, then by the start.
struct StartOfStatementData {
    // The statement is one of the body of a file, which has no text of its own.
    static constexpr uint32_t isOfBody = UINT32_MAX;

    uint32_t startOfFunction;
    uint32_t start;

    friend auto operator<=>(const StartOfStatementData&, const StartOfStatementData&) = default;
};
static_assert(sizeof(StartOfStatementData) == 8 && isImageRecord<StartOfStatementData>);

// In functionLengths and in closureCaptureCounts: the function stands in functionsWithLargeCounts.
static constexpr uint8_t countsAreListed = UINT8_MAX;

struct CaptureCounts {
    uint8_t references;
    uint8_t captures;

    static constexpr CaptureCounts decode(uint8_t byte) { return { static_cast<uint8_t>(byte & 15), static_cast<uint8_t>(byte >> 4) }; }
    constexpr uint8_t encoded() const { return references < 15 && captures < 15 ? static_cast<uint8_t>(references | captures << 4) : countsAreListed; }
};
static_assert(CaptureCounts { 14, 14 }.encoded() != countsAreListed && CaptureCounts::decode(CaptureCounts { 3, 14 }.encoded()).captures == 14 && CaptureCounts { 0, 15 }.encoded() == countsAreListed);

// In the order of the functions.
struct FunctionWithLargeCountsData {
    Number24 function;
    CaptureCounts captureCounts;
    uint8_t bytesOfLength[2];

    constexpr uint32_t length() const { return bytesOfLength[0] | bytesOfLength[1] << 8; }
};
static_assert(sizeof(FunctionWithLargeCountsData) == 7 && isImageRecord<FunctionWithLargeCountsData>);

#define FOR_EACH_AOT_FUNCTION_PROGRAM_TABLE(v) \
    v(functionNames, Number24) \
    v(functionNameGroups, uint32_t) \
    v(functionNameRuns, FunctionNameRunData) \
    v(functionRecordNumbers, uint8_t) \
    v(functionRecords, FunctionRecordData) \
    v(functionRecordsByFunction, FunctionRecordByFunctionData) \
    v(functionSources, Number24) \
    v(closureFormsBySource, Number24) \
    v(implementedBuiltins, ImplementedBuiltinData) \
    v(closureFunctionKinds, FunctionKindData) \
    v(functionLengths, uint8_t) \
    v(closureCaptureCounts, uint8_t) \
    v(functionsWithLargeCounts, FunctionWithLargeCountsData) \
    v(closureCellNumbers, Number24) \
    v(functionTexts, FunctionTexts) \
    v(constructorLines, ConstructorLineData) \
    v(startsOfStatementsInLongLines, StartOfStatementData) \
    v(modulesWithTextPrintedInPlace, TextPrintedInPlaceOfModule) \
    v(textPrintedInPlace, uint8_t) \
    v(constructors, ConstructorData) \
    v(boundFunctions, BoundFunctionData) \
    v(constructEntrypoints, ConstructEntrypointData) \
    v(placesOfTailCalls, uint8_t)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
