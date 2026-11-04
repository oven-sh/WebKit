/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFoldingOfConstants.h"
#include "AOTWalk.h"
#include <optional>
#include <span>
#include <wtf/HashMap.h>

// AOTWalkStrings.h declares CallByOneRow with every member that stands below and two more, and the two questions about code units.
#if __has_include("AOTWalkStrings.h")
#include "AOTWalkStrings.h"
#endif

namespace JSC { namespace AOT { namespace Typed {

// A ValueID named `input`, `number`, `dividend` .. is a value of the body being read. A Reference is of Code.
// A null Reference or TypeID, nothing and false always mean: nothing was built, nothing is claimed, and the caller goes on as if it had not asked.

// ---- AOTWalkNumberNotes.cpp

class NotesOfNumbers final : public Walker::KeptForBuild {
public:
    // Walker::beginUnit()
    void clear();

    // For good, no effect ends it: `integer` says in integers what `record` is. Of what charCodeAt, s[i], charAt, at gave: the code unit, -1 outside the text. Of a piece that a step over a text cut: its code point.
    void noteIntegerBeside(ValueID record, Reference integer);
    Reference integerBeside(ValueID record) const;
    void noteLengthOfPiece(ValueID piece, Reference numberOfCodeUnits);
    Reference lengthOfPiece(ValueID piece) const;

    // Of the one block of Code that is appended to. Asked with another block: nothing is found, and the next note empties the lists.
    Reference findConversion(BlockID, Reference value, Representation wanted) const;
    void noteConversion(BlockID, Reference value, Representation wanted, Reference converted);
    // Same opcode, form, payload, flags and operands. It cannot throw, writes nothing, and nothing built since writes what it reads.
    Reference findOperation(BlockID, const Draft&) const;
    void noteOperation(BlockID, const Draft&, const Effects&, Reference built);
    void endWhatIsWritten(const Effects&);

    // The call whose result rule runs. Walker::resultTypeOf() sets and clears it. Null: none.
    const Draft* callBeingTyped { nullptr };

private:
    using ReferencesByRecord = HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    // By Reference::bits() of the value above the form that is wanted.
    using ConversionsMade = HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    struct OperationBuilt {
        unsigned hash { 0 };
        uint32_t firstWord { 0 };
        uint32_t numberOfWords { 0 };
        // 0: it reads nothing that can be written. Else it answers while m_timesReadsWereEnded has not moved on.
        uint32_t readsAsOf { 0 };
        Reference built;
    };

    static constexpr uint32_t noOperation = UINT32_MAX;

    void standIn(BlockID);
    void forgetBlock();
    void growPlacesOfOperations();
    static uint64_t keyOfConversion(Reference value, Representation wanted) { return static_cast<uint64_t>(value.bits()) << 8 | static_cast<uint64_t>(wanted); }
    static void collectWords(const Draft&, Vector<uint32_t, 24>&);
    static unsigned hashOf(std::span<const uint32_t>);

    ReferencesByRecord m_integersBesideRecords;
    ReferencesByRecord m_lengthsOfPieces;

    BlockID m_block;
    ConversionsMade m_conversions;
    Vector<OperationBuilt, 16> m_operations;
    Vector<uint32_t, 64> m_words;
    // A power of two of places, at most half of them taken, each found from the hash of its words on.
    Vector<uint32_t, 32> m_placesOfOperations;
    LocationMasks m_readsOfOperations { Effects::none().reads };
    uint32_t m_timesReadsWereEnded { 1 };
};

// Made where it is first asked for.
NotesOfNumbers& notesOfNumbers(Walker&);

// What the files of the package share.
// TypedOperations and UnboxedNumbers: every rule here gives or takes a raw integer.
bool hasMechanismsOfNumberForms(const Walker&);
// A Double or String constant by its record, else the one value of the proven type. Unknown: it is not known.
PrimitiveKnownToFolding primitiveKnownInNumberForms(const Walker&, Reference);
// The one integer that the value is. Nothing: it may be another, or is none.
std::optional<int64_t> integerKnownInNumberForms(const Walker&, Reference);
// Null for Unknown.
Reference constantInNumberForms(Walker&, PrimitiveKnownToFolding);
Reference integerInNumberForms(Walker&, int64_t);
Reference booleanInNumberForms(Walker&, bool);
// An operator or a conversion in the form that the proven types of its operands allow. `right` null: it has one operand. Null: the walk stands nowhere.
Reference buildOperatorInNumberForms(Walker&, Opcode, Reference left, Reference right, Origin);
// The draft has its operands. It gets the form that they allow, and is built.
Reference buildInAllowedFormInNumberForms(Walker&, Draft&&);
// Bit i: an edge behind the Branch on the test hands on operand i of the test, narrowed. Nothing: the test has several readers.
std::optional<uint32_t> operandsNarrowedBehindTestInNumberForms(const Walker&, ValueID test);
// `passes` ends the block. Where it holds the walk goes on in a new block, and else in `otherwise`.
void goOnWhereTestPassesInNumberForms(Walker&, Reference passes, BlockID otherwise);
// What the test leaves of the value on that side of the Branch on it.
Reference buildNarrowedInNumberForms(Walker&, Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge, Origin);
// One way to a block in which ways meet: where it ends, and what it brings. No block: the way has ended.
struct WayInNumberForms {
    BlockID end;
    Reference brought;
    // Every way brings one, or none does.
    Reference broughtBeside;
};
// The ways jump to `meeting`, where the walk goes on. What they bring meets in a parameter, which is the answer, and what they bring beside it in a second one. Null: they bring nothing, or none of them comes there.
Reference meetInNumberForms(Walker&, BlockID meeting, std::span<const WayInNumberForms>, Reference* metBeside = nullptr);

// ---- AOTWalkNumberQuotients.cpp

struct DivisionAsWritten {
    ValueID dividend;
    ValueID divisor;
};
// `number`, behind its casts, is a Div of the body whose operands `inputs` proves integers within Range::exact().
std::optional<DivisionAsWritten> findQuotientOfIntegers(Walker&, ValueID number, const InputsOfTypedOperation&);
struct WholeRemainder {
    ValueID dividend;
    ValueID divisor;
    // The divisor may be 0.
    bool mayBeNaN { false };
};
// The same of a Mod. What stands for it gives 0 where `%` gives -0 or NaN: whether the asker can tell is the asker's to know.
std::optional<WholeRemainder> findWholeRemainder(Walker&, ValueID number, const InputsOfTypedOperation&);
bool canDivideQuotientAsIntegers(QuotientRounding, const Type& dividend, const Type& divisor);
std::optional<QuotientRounding> quotientRoundingOf(InlineBuiltin);
// The dividend itself by a divisor of 1, a constant of two known integers.
Reference buildDivisionOfIntegers(Walker&, Origin, Reference dividend, Reference divisor, QuotientRounding);
// Null exactly where buildDivisionOfIntegers() builds nothing for these or narrower types.
TypeID typeOfDivisionOfIntegers(Walker&, QuotientRounding, TypeID dividend, TypeID divisor);
// A constant, the dividend, or RemainderOfIntegers.
Reference buildWholeRemainder(Walker&, Origin, const WholeRemainder&);
// What buildWholeRemainder() gives lies within it.
TypeID typeOfWholeRemainder(Walker&, TypeID dividend, TypeID divisor);
// BitAnd with divisor - 1 for a power of two.
Reference buildZeroWhereWholeRemainderIsZero(Walker&, Origin, const WholeRemainder&);
struct BitsOfDividendCompared {
    Reference bits;
    Reference comparedWith;
};
std::optional<BitsOfDividendCompared> tryCompareBitsOfDividend(Walker&, Origin, const WholeRemainder&, int64_t comparedWith);
// `comparison` has its operands, whole remainders among them. A divisor that is 0 made NaN of its remainder, and NaN equals nothing and stands in no relation: a Rare block answers false, and the two meet.
Reference buildComparisonWhereDivisorsAreNotZero(Walker&, Draft&& comparison, std::span<const ValueID> divisorsThatMayBeZero);
// Of a Mod. `y % 4 === 0`, `if (n % 2)`, where all that is proven of the dividend is that it is a number and nothing reads the remainder but that one test, which stands behind it in the stretch being walked.
// True: the remainder and its test are built and bound, and all up to the test is walked.
bool buildNumberRemainderWithItsTest(Walker&, const BodyValue& remainder);

// ---- AOTWalkNumberOperators.cpp. For buildTypedOperation() and typedFormAsReceiverStands(), in front of buildTypedOperator().
// Asked ahead it says true only for what is closed without it and runs no code with it: a relation on number-likes. A rule that betters a typed form says false ahead, and buildTypedOperator() answers.
inline constexpr OpcodeSet opcodesThatBuildNumberFormTakes = allBitwiseOperators | allEqualities | allRelations | OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow, Opcode::Negate,
    Opcode::Increment, Opcode::Decrement, Opcode::DirectEval, Opcode::LogicalNot, Opcode::ToBoolean };
AOT_LINK_OF_WALK_WITH_INPUTS(buildNumberForm, opcodesThatBuildNumberFormTakes)

// ---- AOTWalkNumberBuiltins.cpp, AOTWalkNumberFolds.cpp. For CallInWalkBuiltins::build(), in front of buildWithRegExpWrittenForCall().

#if !__has_include("AOTWalkStrings.h")
struct CallByOneRow {
    const BodyValue& call;
    // Canonical.
    BuiltinID builtin;
    BuiltinOverloadID overload;
    // What the row names, whatever form was chosen.
    std::optional<InlineBuiltin> codeInLine;
    // Settled. Null: the row takes none.
    Reference receiver;
    // Settled: converted as the row says, in the order written.
    std::span<const Reference> arguments;
    // isTailCall is false.
    Origin origin;
    // `Math.floor.call(a, b)`: `call` writes other operands than the function takes, and nobody reads them off it.
    bool isForwarded { false };
};
inline Reference codeUnitOfCharacterInSight(Walker&, Reference) { return { }; }
inline Reference codeUnitOfCharCodeInSight(Walker&, Reference) { return { }; }
#endif

// What stands for the call. The caller binds it.
Reference buildCallOnNumbers(Walker&, const CallByOneRow&);
// Only what IEEE 754 and ECMAScript decide to the last bit.
std::optional<PrimitiveKnownToFolding> foldExactly(Walker&, const CallByOneRow&);
// For LooksAtLoops::withWhatCopyReturns(). Narrower than the row only where buildCallOnNumbers() binds within it for these and all narrower types: a rounded quotient of integers. `index`: of the body looked at, a callee's too.
TypeID typeOfCallOnNumbersAhead(Walker&, const BodyIndex&, const BodyValue& call, const TypesOfInputs&);

// ---- AOTWalkNumberCodeUnits.cpp

// The three belong to AOTWalkStringCharacters.cpp, which writes the reads of characters out. Each gives the default.
bool isPositionProvenInsideText(Walker&, Reference string, Reference position);
Reference buildCharCode(Walker&, const CallByOneRow&);
Reference buildCharacterOfString(Walker&, ValueID read, Reference string, Reference position, Origin);
// The integer that says what charCodeAt() gave: the code unit, and -1 where the number is NaN. Null: it is no such number, or nobody has the integer.
Reference codeUnitBesideCharCode(Walker&, Reference number);
// The same of what s[i], charAt() or at() gave: -1 where that is undefined or the empty text.
Reference codeUnitBesideCharacter(Walker&, Reference character);
// The code unit beside what charCodeAt() gave, where its type says that it is none of the -1 that stands for NaN.
Reference integerThatCharCodeIsKnownToBe(Walker&, Reference number);
// The same where the block of `user` is entered by one edge on which a relation or an equality that names `number` has held: NaN fails each of them.
Reference integerThatCharCodeIsKnownToBe(Walker&, const BodyValue& user, ValueID number);
// `s.charCodeAt(i) === 32`, `c >= 48 && c <= 57`.
bool buildComparisonByCodeUnit(Walker&, const BodyValue& comparison, const InputsOfTypedOperation&);

// ---- AOTWalkNumberKeys.cpp. For buildTypedElementAccess(.., inputs), first line.
bool buildElementAccessUnderNumber(Walker&, const BodyValue&, const InputsOfTypedOperation&);
// For buildCopyOfPropertiesForTypedAccess().
bool buildCopyOfPrimitive(Walker&, const BodyValue& copy);

// ---- AOTWalkNumberSwitches.cpp. For Walker::walkTerminator(), behind copyFields().
struct SwitchOnNumbers {
    // Null: as written.
    Reference subject;
    // A SwitchString becomes a Switch by code units: one for each text, in the order written. Empty: as written.
    Vector<int64_t, 8> cases;
    // The cases cover the subject, so `otherwise` cannot be taken and this case, the last that can be, needs no test.
    std::optional<uint32_t> caseLeftUntested;
};
std::optional<SwitchOnNumbers> planSwitchOnNumbers(Walker&, const BodyTerminator&);

// ---- AOTWalkNumberTallies.cpp. Counters and tallies are AOTWalkLoopCounters.cpp's and AOTWalkLoopTallies.cpp's. Each gives the default.
TypeID rangeOfTallyInLook(Walker&, const BodyIndex&, LoopID, uint32_t parameter, std::span<const TypeID> assumedAtHead, TypeID united);
struct TestOfSteppedCounter {
    ValueID relation;
    uint32_t operandIndex { 0 };
    ValueID stepped;
    int64_t distance { 0 };
};
std::optional<TestOfSteppedCounter> findTestOfSteppedCounter(const BodyIndex&, const BodyTerminator&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
