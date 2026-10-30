/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVarint.h"
#include "CodeType.h"
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
    StackTraceBuiltinKind builtinKind() const { return static_cast<StackTraceBuiltinKind>(builtinKindBits); }
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
// The pieces of a module follow one another as in its file. Every number is a varint.
//   A piece: how far its text starts behind the end of the piece before it (zigzag), doubled, plus one; its size; how many lines it starts below the start of the piece before it; its column.
//   A cut in that piece: how far it lies behind the cut before it, or behind the start of the piece, doubled; the columns that are gone, doubled, plus one where lines are gone too; then how many lines.
//   Where lines are gone, the columns are those in front of what follows the cut on its line.
struct PiecesOfModule {
    uint32_t module;
    uint32_t start;
};
static_assert(sizeof(PiecesOfModule) == 8 && isImageRecord<PiecesOfModule>);

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
struct TextPrintedInPlaceOfModule {
    uint32_t module;
    uint32_t start;
};
static_assert(sizeof(TextPrintedInPlaceOfModule) == 8 && isImageRecord<TextPrintedInPlaceOfModule>);

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
        const uint8_t* cursor = bytes.data();
        const uint8_t* end = std::to_address(bytes.end());
        // A number takes ten bytes at the most, and the last piece may be cut short in a file that somebody has written by hand.
        auto readNumber = [&] {
            uint64_t number = 0;
            for (unsigned shift = 0; cursor < end && shift < 64; shift += 7) {
                uint8_t byte = *cursor++;
                number |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if (!(byte & 0x80))
                    break;
            }
            return number;
        };
        uint64_t start = 0;
        while (cursor < end) {
            start += readNumber();
            uint64_t size = readNumber();
            uint64_t printed = readNumber();
            if (start + size > std::numeric_limits<uint32_t>::max())
                break;
            TextPrintedInPlace piece { static_cast<uint32_t>(start), static_cast<uint32_t>(start + size) };
            if (printed & 1) {
                int64_t startOfCopied = static_cast<int64_t>(start) - zigZagDecode(readNumber());
                if (startOfCopied < 0 || static_cast<uint64_t>(startOfCopied) + (printed >> 2) > std::numeric_limits<uint32_t>::max())
                    break;
                // An initializer ends in front of the name that it replaces, and an operand is smaller than its operator. So to follow the copies comes to an end.
                uint64_t endOfCopied = static_cast<uint64_t>(startOfCopied) + (printed >> 2);
                bool isInFront = endOfCopied <= start;
                bool isBeneath = static_cast<uint64_t>(startOfCopied) >= start && endOfCopied <= start + size && (printed >> 2) < size;
                if (!isInFront && !isBeneath)
                    continue;
                piece.isCopy = true;
                piece.isInBrackets = printed & 2;
                piece.startOfCopied = static_cast<uint32_t>(startOfCopied);
                piece.endOfCopied = static_cast<uint32_t>(startOfCopied + (printed >> 2));
            } else {
                if (printed >> 1 > static_cast<uint64_t>(end - cursor))
                    break;
                piece.bytes = { cursor, static_cast<size_t>(printed >> 1) };
                cursor += printed >> 1;
            }
            pieces.append(piece);
        }
        return pieces;
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

// text[from, to) as `bun run` has it. `pieces` are those of the module, and count from the start of `text`.
template<typename Character>
void appendWithTextPrintedInPlace(Vector<Character>& printed, std::span<const Character> text, size_t from, size_t to, std::span<const TextPrintedInPlace> pieces)
{
    size_t at = from;
    auto first = std::ranges::lower_bound(pieces, from, { }, &TextPrintedInPlace::start);
    for (const TextPrintedInPlace& piece : pieces.subspan(first - pieces.begin())) {
        if (piece.start >= to)
            break;
        // It stands in a piece that is done, or reaches out of the text that is asked for.
        if (piece.start < at || piece.end > to || (piece.isCopy && piece.endOfCopied > text.size()))
            continue;
        printed.append(text.subspan(at, piece.start - at));
        if (!printed.isEmpty() && printed.last() == '[')
            printed.append(static_cast<Character>(markOfPrintedKey));
        size_t startOfPrinted = printed.size();
        if (piece.isCopy) {
            if (piece.isInBrackets)
                printed.append('(');
            appendWithTextPrintedInPlace(printed, text, piece.startOfCopied, piece.endOfCopied, pieces);
            if (piece.isInBrackets)
                printed.append(')');
        } else
            printed.append(byteCast<Character>(piece.bytes));
        at = piece.end;
        // `1.toString` is no program, so `1 .toString` is printed.
        bool isNumber = printed.size() > startOfPrinted && isASCIIDigit(printed[startOfPrinted]) && std::ranges::all_of(printed.subspan(startOfPrinted), [](Character character) {
            return isASCIIDigit(character) || character == '.' || character == 'e';
        });
        if (isNumber && at < text.size() && text[at] == '.')
            printed.append(' ');
    }
    printed.append(text.subspan(at, to - at));
}

// In the order of the sources of the functions, so that both a function and its source lead to the text.
struct FunctionTextEntry {
    uint32_t function;
    uint32_t start;
    uint32_t end;
};
static_assert(sizeof(FunctionTextEntry) == 12 && isImageRecord<FunctionTextEntry>);

// The text of a constructor is that of its class, and the constructor begins at its parameters. This is how many lines of the text stand above them, where there are any.
// Ascending by the function, which is its own source.
struct ConstructorLineData {
    uint32_t function;
    uint32_t linesOfTextAbove;
};
static_assert(sizeof(ConstructorLineData) == 8 && isImageRecord<ConstructorLineData>);

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
    v(modulesWithTextPrintedInPlace, TextPrintedInPlaceOfModule) \
    v(textPrintedInPlace, uint8_t) \
    v(constructors, ConstructorData) \
    v(boundFunctions, BoundFunctionData) \
    v(constructEntrypoints, ConstructEntrypointData) \
    v(placesOfTailCalls, uint8_t)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
