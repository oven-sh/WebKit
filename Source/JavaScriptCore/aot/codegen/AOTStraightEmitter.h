/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBodyIndex.h"
#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTConvention.h"
#include "AOTRuntimeTable.h"
#include "AOTStubDescriptor.h"
#include "AOTStubKey.h"
#include <initializer_list>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct StraightArgument {
    enum class Kind : uint8_t { Value, Word, Number, Register, AddressInScratchArea };

    StraightArgument(ValueID value)
        : kind(Kind::Value)
        , value(value)
    {
    }

    static StraightArgument word(uint64_t bits) { return { Kind::Word, bits }; }
    static StraightArgument boxed(JSValue constant) { return { Kind::Word, static_cast<uint64_t>(JSValue::encode(constant)) }; }
    static StraightArgument number(uint32_t mention) { return { Kind::Number, mention }; }
    static StraightArgument inRegister(GPRReg reg) { return { Kind::Register, static_cast<uint64_t>(reg) }; }
    static StraightArgument addressInScratchArea(uint32_t word) { return { Kind::AddressInScratchArea, word }; }

    Kind kind;
    ValueID value;
    uint64_t bits { 0 };

private:
    StraightArgument(Kind kind, uint64_t bits)
        : kind(kind)
        , bits(bits)
    {
    }
};

struct StraightCallOptions {
    std::optional<uint32_t> immediate { };
    std::optional<NumberedImmediate> numberedImmediate { };
    std::optional<Origin> origin { };
    std::optional<Origin> readBeforeCall { };
    std::optional<ThrowSite> throwSite { };
    BuiltinID calledBuiltin { };
    FunctionID constructedCallee { };
    bool constructs { false };
    bool isTailCall { false };
    bool callerHasLeft { false };
    bool capturesStackTrace { false };
    bool followsThrow { false };
    bool neverReturns { false };
};

struct StraightTest {
    int32_t mask { 0 };
    int32_t word { 0 };
};

struct PlacedSlot;

class StraightEmitter {
    WTF_MAKE_NONCOPYABLE(StraightEmitter);
    static constexpr GPRReg resultGPR = GPRInfo::returnValueGPR;
    static constexpr FPRReg resultFPR = FPRInfo::returnValueFPR;
public:
    StraightEmitter(BuildContext&, TypeArena&, const CodeGenerationInputs&, CodeUnit, const BodyIndex&, CodeRecords&, Machine::Arena&);

    static bool covers(const CodeGenerationInputs&, CodeUnit, const BodyIndex&);

    uint32_t numberOfSlots() const { return m_numberOfSlots; }
    bool entersUnits() const { return m_entersUnits; }
    uint32_t wordsOfScratchArea() const { return m_wordsOfScratchArea; }
    void generate(CCallHelpers&, const ArgumentLocations&, unsigned bytesBelowFrameRecord);
    bool hasFailed() const { return m_hasFailed; }
    uint32_t numberOfRawSlotsOfScopesRead() const { return m_numberOfRawSlotsOfScopesRead; }
    uint32_t numberOfNumbersToldByTheirWord() const { return m_numberOfNumbersToldByTheirWord; }
    uint32_t numberOfReadsThatAskTheirSite() const { return m_numberOfReadsThatAskTheirSite; }
    void countReadThatAsksItsSite() { ++m_numberOfReadsThatAskTheirSite; }
    Opcode opcodeThatFailed() const { return m_opcodeThatFailed; }

    BuildContext& build() { return m_build; }
    TypeArena& types() { return m_types; }
    const Program& program() const { return m_inputs.program; }
    const Facts& facts() const { return m_inputs.facts; }
    const Plan& plan() const { return m_inputs.plan; }
    const CallingConventions& conventions() const;
    const StubCatalog& catalog() const { return m_inputs.stubCatalog; }
    CodeUnit unit() const { return m_unit; }
    FunctionID function() const { return m_unit.function; }
    const BodyIndex& index() const { return m_index; }
    CCallHelpers& jit()
    {
        storeWhatWaitsInRegister();
        return *m_jit;
    }
    CodeRecords& records() { return m_records; }
    MentionList& mentions() { return m_records.mentions; }
    RelocationRecorder& relocations() { return m_records.relocations; }
    void recordInlined(std::span<const uint8_t> programString)
    {
        if (m_records.inlinedStrings.areRecorded()) [[unlikely]]
            m_records.inlinedStrings.append(programString);
    }
    BodyBlock currentBlock() const { return m_block; }
    bool isNeverEntered(BlockID block) const { return !m_blocksNeverEntered.empty() && std::ranges::binary_search(m_blocksNeverEntered, block); }

    GPRReg takeResultRegister()
    {
        storeWhatWaitsInRegister();
        m_valueCopiedInRegister = { };
        return resultGPR;
    }
    GPRReg resultOfCall() const
    {
        RELEASE_ASSERT(!m_valueInRegister);
        return resultGPR;
    }
    GPRReg registerOfWaitingValue(ValueID value) const
    {
        RELEASE_ASSERT(waitsInRegister(value));
        return resultGPR;
    }
    FPRReg takeResultFPR() { return resultFPR; }
    FPRReg numberOfCall() const { return resultFPR; }
    static constexpr FPRReg conversionFPR = CCallHelpers::fpTempRegister;
    static constexpr bool isResultRegister(GPRReg reg) { return reg == resultGPR; }

    static GPRReg temporary(unsigned index) { return GPRInfo::toArgumentRegister(index + (GPRInfo::argumentGPR0 == resultGPR)); }
    static constexpr unsigned numberOfTemporaries = GPRInfo::numberOfArgumentRegisters - (GPRInfo::argumentGPR0 == resultGPR);

    ValueID rootOf(ValueID value) const { return ValueID(m_roots[value.index()] & ~isConstantBit); }
    bool isConstant(ValueID value) const { return m_roots[value.index()] & isConstantBit; }
    std::optional<JSValue> constantOf(ValueID) const;
    bool isUndefinedConstant(ValueID value) const { return isConstant(value) && m_index.opcodeOf(rootOf(value)) == Opcode::Undefined; }
    CCallHelpers::Address slotOf(ValueID);
    CCallHelpers::Address wordOfScratchArea(uint32_t word) const;

    void load(ValueID, GPRReg);
    GPRReg loadIntoResultRegister(ValueID value)
    {
        if (!waitsInRegister(value))
            storeWhatWaitsInRegister();
        load(value, resultGPR);
        return resultGPR;
    }
    void bring(const StraightArgument&, GPRReg);
    void storeInScratchArea(ValueID, uint32_t word);

    void define(const BodyValue&, GPRReg boxed);
    void define(const BodyValue& value) { define(value, resultGPR); }
    void defineBoolean(const BodyValue&, GPRReg zeroOrOne);
    void defineBoolean(const BodyValue& value) { defineBoolean(value, resultGPR); }
    void defineInt32(const BodyValue&, GPRReg);
    void defineInt32(const BodyValue& value) { defineInt32(value, resultGPR); }
    void defineDouble(const BodyValue&, FPRReg);
    void defineDouble(const BodyValue& value) { defineDouble(value, resultFPR); }
    void defineSameAs(const BodyValue&, ValueID);
    bool isReadByBranchAlone(const BodyValue&) const;
    void defineTest(const BodyValue&, StraightTest);
    void throwIfEmpty(const BodyValue& check, ValueID checked, const ThrowSite&);
    void defineConstant(const BodyValue&);

    void defineResult(const BodyValue& producer, uint32_t index, GPRReg boxed);
    bool resultIsRead(const BodyValue& producer, uint32_t index) const { return !!resultAt(producer, index); }

    void boxInForm(Representation, GPRReg);
    void unboxIntoForm(Representation, GPRReg);
    void boxDouble(FPRReg, GPRReg);
    void unboxNumber(GPRReg boxedNumber, FPRReg);

    void callStub(const BodyValue&, StubKey, std::initializer_list<StraightArgument>, const StraightCallOptions& = { });
    void callStub(Origin, StubKey, std::span<const StraightArgument>, const StraightCallOptions&);
    void callOperation(const BodyValue&, RuntimeOperation, std::initializer_list<StraightArgument>, const StraightCallOptions& = { });
    void callOperation(Origin, RuntimeOperation, std::span<const StraightArgument>, const StraightCallOptions&);
    void callByConvention(const BodyValue&, StubKey, std::span<const StraightArgument> stubOperands, std::optional<StraightArgument> callee, std::optional<StraightArgument> thisValue, std::span<const ValueID> arguments, const StraightCallOptions&);

    uint32_t addSite(Origin, const StraightCallOptions&, Opcode = Opcode::Undefined);
    void noteSiteMayThrow(uint32_t site);
    uint32_t mentionOfName(PropertyKeyID);

    void throwAt(Origin, const ThrowSite&, std::optional<GPRReg> boxedOperand = std::nullopt);
    void throwError(Origin, ErrorKind, StringID message);
    ThrowSite throwSiteWithName(ThrowKind, StringID name);

    bool canTestType(TypeID tested);
    static std::optional<StraightTest> testOfWordAlone(TypeID);
    void testType(GPRReg boxed, TypeID tested, GPRReg zeroOrOne);
    void trapUnlessOfType(ValueID, TypeID tested, ProofSource, Origin);

    void loadSlot(GPRReg cell, const PlacedSlot&, GPRReg boxedResult);
    void storeSlot(GPRReg cell, const PlacedSlot&, GPRReg boxed);
    void loadVariable(VariableID, GPRReg boxedResult);
    void storeVariable(VariableID, GPRReg boxed);
    void storeVariable(VariableID, ValueID);
    void barrierAfterStore(GPRReg owner);

    COLD NEVER_INLINE void fail(Opcode);
    COLD void fail(const BodyValue& value) { fail(value.opcode()); }

    static OpcodeSet opcodesOfConstants();
    static OpcodeSet opcodesOfCasts();
    static OpcodeSet opcodesOfOperators();
    static OpcodeSet opcodesOfAccesses();
    static OpcodeSet opcodesOfCalls();
    static OpcodeSet opcodesOfBindings();
    static OpcodeSet opcodesOfHints();
    static OpcodeSet opcodesOfAllocations();
    static OpcodeSet opcodesOfArrays();
    static OpcodeSet opcodesOfArrayIntrinsics();
    static OpcodeSet opcodesOfClasses();
    static OpcodeSet opcodesOfCollectionIntrinsics();
    static OpcodeSet opcodesOfEngineIntrinsics();
    static OpcodeSet opcodesOfEvals();
    static OpcodeSet opcodesOfExceptions();
    static OpcodeSet opcodesOfForIns();
    static OpcodeSet opcodesOfIterations();
    static OpcodeSet opcodesOfModules();
    static OpcodeSet opcodesOfPrivateAccesses();
    static OpcodeSet opcodesOfPromiseIntrinsics();
    static OpcodeSet opcodesOfRuntimeIntrinsics();
    void generateConstant(const BodyValue&);
    void generateCast(const BodyValue&);
    void generateOperator(const BodyValue&);
    void generateAccess(const BodyValue&);
    void generateCall(const BodyValue&);
    void generateBinding(const BodyValue&);
    void generateHint(const BodyValue&);
    void generateAllocation(const BodyValue&);
    void generateArray(const BodyValue&);
    void generateArrayIntrinsic(const BodyValue&);
    void generateClass(const BodyValue&);
    bool classRecordCreatesMemberDefinedBy(const BodyValue&);
    void generateCollectionIntrinsic(const BodyValue&);
    void generateEngineIntrinsic(const BodyValue&);
    void generateEval(const BodyValue&);
    void generateException(const BodyValue&);
    void generateForIn(const BodyValue&);
    void generateIteration(const BodyValue&);
    void generateModule(const BodyValue&);
    void generatePrivateAccess(const BodyValue&);
    void generatePromiseIntrinsic(const BodyValue&);
    void generateRuntimeIntrinsic(const BodyValue&);

    static bool coversReturnCalls();
    void generateReturnCall(const BodyTerminator&);
    static bool coversSwitchesOnStrings();
    void generateSwitchOnString(const BodyTerminator&);

    bool edgeMovesNothing(const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    void passArguments(const BodyTerminator&, uint32_t targetIndex, BodyTarget);
    void jumpTo(BlockID);
    void jumpTo(CCallHelpers::Jump, BlockID);
    void tearDownFrame();
    void returnToCaller(ValueID returned, Origin);

private:
    static constexpr uint32_t isConstantBit = 1u << 31;

    struct JumpToBlock {
        CCallHelpers::Jump jump;
        BlockID block;
    };

    struct LandingPad {
        BlockID from;
        MacroAssembler::Label* label;
    };

    enum class FormInRegister : uint8_t { Boxed, ZeroOrOne, Test };

    static const OpcodeSet& opcodesWithStraightForm();
    void findValuesNobodyReads();
    bool isReadByNobody(ValueID value) const { return value.index() < m_valuesNobodyReads.size() && m_valuesNobodyReads.quickGet(value.index()); }
    bool mayCallOnlyCalleeDirectly(const BodyValue&, ValueID& checkThatPasses);
    bool callsOnlyCalleeDirectly(ValueID call) const { return call.index() < m_callsOfOnlyCallees.size() && m_callsOfOnlyCallees.quickGet(call.index()); }
    void measureFrame();
    static constexpr uint32_t mostWordsOfFrame = 512;

    void takeParameters(const ArgumentLocations&);
    void generateBlock(BodyBlock);

    struct ImportCheck {
        VariableID variable;
        ModuleFailureID failure;
    };
    static constexpr uint32_t fewestImportChecksInList = 2;
    std::optional<ImportCheck> importCheckIn(BodyBlock) const;
    Vector<ImportCheck, 8> importChecksFrom(BodyBlock) const;
    void generateImportChecks(BodyBlock first, std::span<const ImportCheck>);
    void generateOperation(const BodyValue&);
    bool leavesDefinitionToFirstRead(const BodyValue&);
    void generateTerminator(BodyBlock, const BodyTerminator&);
    void generateReturn(const BodyTerminator&);
    void generateSwitch(BodyBlock, const BodyTerminator&);
    void generateLandingPads(unsigned bytesBelowFrameRecord);
    HandlerType handlerTypeOf(BlockID handler) const;
    ValueID resultAt(const BodyValue& producer, uint32_t index) const;
    void moveBetweenSlots(std::span<const std::pair<ValueID, ValueID>>);
    CCallHelpers::Address addressOfSlot(ValueID);
    std::optional<uint32_t> positionOfLastReadInBlock(ValueID) const;
    void giveBackSlotAt(uint32_t position, ValueID root);
    void giveBackSlotsReadLastAt(uint32_t position);
    bool isReadByNextCodeAlone(const BodyValue&) const;
    bool waitsInRegister(ValueID value) const { return m_valueInRegister && rootOf(value) == m_valueInRegister; }
    template<typename OperationOrTerminator> bool readsWhatWaitsInRegister(const OperationOrTerminator&) const;
    void storeWhatWaitsInRegister();
    void boxWhatWaitsInRegister();
    void noteCopyInRegister(ValueID);
    bool isCopiedInRegister(ValueID) const;
    std::optional<BodyUser> onlyReader(ValueID) const;
    void findProofsNobodyReads(const BodyTerminator&);
    void bringAll(std::span<const StraightArgument>, NOESCAPE const Invocable<GPRReg(size_t)> auto& registerOf, ArgumentMoves* leftToThunk = nullptr);
    bool leavesArgumentsToThunk(StubKey, const StraightCallOptions&) const;
    void branchOnCondition(ValueID condition, bool jumpsIfTrue, BlockID);
    bool givesBoolean(ValueID) const;
    enum class ImmediateOfThunk : bool { IsMovedBySite, IsMovedByThunk };
    void emitCallInstruction(StubKey, const StubSignature&, OptionalIndex site, OptionalIndex firstRead, const StraightCallOptions&, const ArgumentMoves& = { }, ImmediateOfThunk = ImmediateOfThunk::IsMovedBySite);

    BuildContext& m_build;
    TypeArena& m_types;
    const CodeGenerationInputs& m_inputs;
    const CodeUnit m_unit;
    const BodyIndex& m_index;
    CodeRecords& m_records;
    Machine::Arena& m_arena;
    CCallHelpers* m_jit { nullptr };
    BodyBlock m_block;
    static constexpr uint32_t slotWasGivenBack = std::numeric_limits<uint32_t>::max();

    Vector<uint32_t, 128> m_roots;
    Vector<uint32_t, 128> m_slots;
    Vector<uint32_t, 16> m_freeSlots;
    Vector<uint32_t, 128> m_firstValueGivenBackAt;
    Vector<uint32_t, 128> m_nextValueGivenBack;
    uint32_t m_numberOfSlotsTaken { 0 };
    uint32_t m_numberOfSlots { 0 };
    bool m_entersUnits { false };
    Vector<CCallHelpers::Label, 32> m_labels;
    Vector<JumpToBlock, 32> m_jumps;
    Vector<LandingPad, 4> m_landingPads;
    uint32_t m_landingPadOfBlockPlusOne { 0 };
    ValueID m_valueInRegister;
    ValueID m_valueTakenOutOfRegister;
    FormInRegister m_formInRegister { FormInRegister::Boxed };
    StraightTest m_testInRegister;
    ValueID m_valueCopiedInRegister;
    size_t m_sizeOfCodeAtCopy { 0 };
    ValueID m_subjectOfProofsNobodyReads;
    uint32_t m_numberOfProofsNobodyReads { 0 };
    ValueID m_returnedCall;
    bool m_hasHandlers { false };
    BitVector m_blocksGenerated;
    BitVector m_valuesNobodyReads;
    BitVector m_callsOfOnlyCallees;
    std::span<const BlockID> m_blocksNeverEntered;
    uint32_t m_wordsOfScratchArea { 0 };
    bool m_hasFailed { false };
    uint32_t m_numberOfRawSlotsOfScopesRead { 0 };
    uint32_t m_numberOfNumbersToldByTheirWord { 0 };
    uint32_t m_numberOfReadsThatAskTheirSite { 0 };
    Opcode m_opcodeThatFailed { Opcode::Undefined };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
