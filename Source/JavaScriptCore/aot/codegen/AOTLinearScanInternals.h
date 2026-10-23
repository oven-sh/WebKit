/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)

#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTLinearScanCodeGenerator.h"
#include "AOTFunctionCodeGenerator.h"
#include "AirCode.h"
#include "AirGenerationContext.h"
#include "B3BasicBlock.h"
#include "B3Opcode.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueRep.h"
#include "CCallHelpers.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Box.h>
#include <wtf/Vector.h>

namespace JSC {

namespace B3 {
class CheckValue;
class StackmapValue;
class SwitchValue;
}

namespace AOT { namespace Typed {

namespace LinearScan {

using NodeID = uint32_t;
using Position = uint32_t;

constexpr Position noPosition = UINT32_MAX;
constexpr unsigned numberOfRegisters = Reg::maxIndex() + 1;
static_assert(numberOfRegisters <= 64);

inline uint64_t bitOf(Reg reg) { return static_cast<uint64_t>(1) << reg.index(); }
inline uint64_t bitsOf(const RegisterSet& registers) { return registers.toScalarRegisterSet().bitsForDebugging(); }
inline uint8_t encode(Reg reg) { return static_cast<uint8_t>(reg.index() + 1); }
inline Reg decode(uint8_t encoded) { return encoded ? Reg::fromIndex(encoded - 1) : Reg(); }
inline bool fitsInt32(int64_t value) { return value == static_cast<int32_t>(value); }

constexpr uint8_t locationStackArgument = 0xfc;
constexpr uint8_t locationInSlot = 0xfd;
constexpr uint8_t locationMaterialized = 0xfe;
inline bool isRegisterLocation(uint8_t location) { return location && location <= numberOfRegisters; }

constexpr Position argumentPositionOf(uint32_t slot) { return slot * 4; }
constexpr Position mainPositionOf(uint32_t slot) { return slot * 4 + 2; }
constexpr uint32_t slotOf(Position position) { return position / 4; }

enum class NodeType : uint8_t {
    Void,
    Int32,
    Int64,
    Double,
    Tuple,
};

enum class Form : uint8_t {
    Plain,
    Dead,
    Constant,
    Alias,
    FixedRegister,
    Rematerialized,
    Folded,
};

enum NodeFlag : uint16_t {
    IsZeroExtended = 1 << 0,
    IsReadWide = 1 << 1,
    Traps = 1 << 2,
    IsChill = 1 << 3,
    HasSeveralSources = 1 << 4,
    HasSourceWithHighBits = 1 << 5,
    ArrivesInSlot = 1 << 6,
    IsImmutable = 1 << 7,
    IsTerminal = 1 << 8,
    HasArgumentUses = 1 << 9,
    IsPlaced = 1 << 10,
    IsCommuted = 1 << 11,
    TakesTrap = 1 << 12,
};

struct Node {
    B3::Opcode opcode;
    NodeType type;
    NodeType operandType;
    Form form;
    uint16_t flags;
    uint16_t rawUses;
    NodeID a;
    NodeID b;
    NodeID c;
    uint32_t block;
    int64_t constant;
    uint32_t slot;
    uint32_t epoch;
    uint32_t memoryEpoch;
    uint32_t next;
    uint32_t vreg;
    uint32_t firstOperand;
    uint16_t numberOfOperands;
    uint16_t uses;
    uint8_t hint;
    uint8_t base;

    bool isWide() const { return type == NodeType::Int64; }
    bool isDouble() const { return type == NodeType::Double; }
    bool has(NodeFlag flag) const { return flags & flag; }
    int32_t offset() const { return static_cast<int32_t>(constant); }
};

enum class OperandKind : uint8_t {
    Use,
    LateUse,
    UseOrSlot,
    LateUseOrSlot,
    AnyUse,
    LateAnyUse,
    ArgumentUse,
    EndUse,
    Definition,
    EarlyDefinition,
    BlockDefinition,
    Temporary,
};

enum OperandFlag : uint8_t {
    IsDoubleOperand = 1,
};

struct Operand {
    NodeID node;
    OperandKind kind;
    uint8_t fixed;
    uint8_t location;
    uint8_t flags;
};

enum class UseKind : uint8_t {
    Any,
    RegisterOrSlot,
    Register,
};

struct LiveRange {
    Position from;
    Position to;
    uint32_t next;
};

struct UsePosition {
    Position position;
    uint32_t operand;
    uint32_t next;
    UseKind kind;
    bool isLate;
};

enum class Place : uint8_t {
    Unassigned,
    Register,
    Slot,
    Nowhere,
};

struct Interval {
    uint32_t vreg { 0 };
    uint32_t firstRange { 0 };
    uint32_t currentRange { 0 };
    uint32_t firstUse { 0 };
    uint32_t nextSplit { 0 };
    Position from { 0 };
    Position to { 0 };
    uint32_t region { 0 };
    uint8_t reg { 0 };
    Place place { Place::Unassigned };
    bool isDouble { false };
    bool isSplitChild { false };

    uint8_t location() const
    {
        switch (place) {
        case Place::Register:
            return reg;
        case Place::Slot:
            return locationInSlot;
        case Place::Nowhere:
            return locationMaterialized;
        case Place::Unassigned:
            break;
        }
        return 0;
    }
};

enum class SpillState : uint8_t {
    NotSpilled,
    StoredWhereSpilled,
    StoredAtDefinition,
};

struct VirtualRegister {
    NodeID node { 0 };
    uint32_t firstInterval { 0 };
    uint32_t currentInterval { 0 };
    uint32_t currentRegion { UINT32_MAX };
    uint32_t global { 0 };
    uint32_t nextDefinedInBlock { 0 };
    uint32_t hintVirtualRegister { 0 };
    uint32_t targetVirtualRegister { 0 };
    bool hasPlaceInFrame { false };
    uint32_t lastBlockWithExposedUse { 0 };
    uint32_t numberOfSpills { 0 };
    Position definition { 0 };
    Position end { 0 };
    int32_t offset { 0 };
    float spillFrequency { 0 };
    float hintFrequency { 0 };
    uint8_t hint { 0 };
    uint8_t fixedDefinition { 0 };
    uint8_t calleeSaveMeantForIt { 0 };
    bool livesInMemory { false };
    SpillState spillState { SpillState::NotSpilled };
    bool isDouble { false };
    bool isRematerialized { false };
    bool hasSeveralIntervals { false };
    bool needsSlot { false };
};

struct ClobberEvent {
    Position from;
    Position to;
    uint64_t registers;
};

struct Move {
    Position position;
    NodeID source;
    NodeID destination;
    uint8_t from;
    uint8_t to;
    uint8_t priority;
};

struct EdgeMoves {
    uint32_t first { 0 };
    uint32_t count { 0 };
    uint32_t stub { 0 };
    uint32_t edgeWithSameMoves { 0 };
    bool isShared { false };
};

// A value at the start of a block that has a predecessor outside the live range which covers the start.
struct BoundaryEntry {
    uint32_t vreg;
    uint32_t next;
    // The blocks whose ends the same range covers: along an edge from one of them the value stays where it is.
    uint32_t firstBlockInRange;
    uint32_t endOfBlocksInRange;
    uint8_t location;
};

struct UpsilonRecord {
    NodeID upsilon;
    uint32_t next;
};

struct Loop {
    uint32_t firstBlock;
    uint32_t lastBlock;
    uint32_t parent;
};

struct Block {
    B3::BasicBlock* block { nullptr };
    B3::BasicBlock* returnBlock { nullptr };
    const B3::BasicBlock* branchTakenFrom { nullptr };
    uint32_t reversePostOrderIndex { 0 };
    uint32_t firstSlot { 0 };
    uint32_t endSlot { 0 };
    uint32_t firstSuccessor { 0 };
    uint32_t numberOfSuccessors { 0 };
    uint32_t firstPredecessor { 0 };
    uint32_t numberOfPredecessors { 0 };
    uint32_t predecessor { 0 };
    uint32_t incomingEdge { 0 };
    uint32_t region { 0 };
    uint32_t loop { 0 };
    uint32_t firstPhi { 0 };
    uint32_t firstUpsilon { 0 };
    uint32_t firstDefinedGlobal { 0 };
    uint32_t firstExposedUse { 0 };
    uint32_t numberOfExposedUses { 0 };
    uint32_t firstEntryAtStart { 0 };
    uint32_t firstMove { 0 };
    uint32_t epochAtEnd { 0 };
    uint32_t memoryEpochAtEnd { 0 };
    uint32_t forwardsTo { UINT32_MAX };
    uint32_t emissionIndex { 0 };
    uint8_t onlySuccessor { 0 };
    bool isRare { false };
    bool isReachedNormally { false };
    bool isReachable { false };
    bool hasLaterPredecessor { false };
    bool hasRetreatingPredecessor { false };
    uint32_t dominator { 0 };
    uint32_t livePredecessor { 0 };
    uint32_t numberOfLivePredecessors { 0 };
    // Where one edge of a branch alone leads into the block: what the branch tests, without its negations, and whether that holds here.
    NodeID truthKnownAtEntry { 0 };
    bool truthKnownAtEntryHolds { false };
    // The nearest block up the dominators, this one among them, that knows such a truth, and how many steps up it stands. The first block where there is none.
    uint32_t nearestBlockThatKnowsTruth { 0 };
    uint32_t stepsToBlockThatKnowsTruth { 0 };
    bool branchesToOneBlock { false };
    bool usesFrame { false };
    bool runsInFrame { true };
    bool setsUpFrame { false };
    uint32_t firstMoveAtStart { 0 };
    uint32_t numberOfMovesAtStart { 0 };
    NodeID testedPhi { 0 };
    NodeID firstUpsilonMovedIn { 0 };
    bool isEmittedLate { false };
    double frequency { 1 };
    MacroAssembler::Label label;
    MacroAssembler::JumpList jumps;
    MacroAssembler::JumpList jumpsFromInsideFrame;
    MacroAssembler::Label labelInsideFrame;
    Box<MacroAssembler::Label> sharedLabel;

    Position from() const { return argumentPositionOf(firstSlot); }
    Position to() const { return argumentPositionOf(endSlot); }
};

struct AddressPlan {
    NodeID base { 0 };
    NodeID index { 0 };
    const B3::Air::StackSlot* slot { nullptr };
    GPRReg fixedBase { InvalidGPRReg };
    uint8_t shift { 0 };
    int32_t offset { 0 };
};

struct MemoryOperand {
    GPRReg base { InvalidGPRReg };
    GPRReg index { InvalidGPRReg };
    uint8_t shift { 0 };
    int32_t offset { 0 };

    bool hasIndex() const { return index != InvalidGPRReg; }
    MacroAssembler::Address address() const { return MacroAssembler::Address(base, offset); }
    MacroAssembler::BaseIndex baseIndex() const { return MacroAssembler::BaseIndex(base, index, static_cast<MacroAssembler::Scale>(shift), offset); }
};

enum class OperandShape : uint8_t {
    Register,
    Immediate,
    PinnedMemory,
    PinnedRegister,
    Load,
    ScaledIndex,
    BitTest,
};

struct OperandPlan {
    OperandShape shape { OperandShape::Register };
    NodeID left { 0 };
    NodeID right { 0 };
    NodeID load { 0 };
    NodeID foldedShift { 0 };
    int64_t immediate { 0 };
    B3::PinnedConstant pinned { };
    bool isCommuted { false };
    bool loadIsLeft { false };
    uint8_t loadWidth { 0 };
    uint8_t shift { 0 };
};

struct KnownLoad {
    NodeID load { 0 };
    uint32_t memoryEpoch { 0 };
};

struct Transfer {
    uint8_t to;
    uint8_t from;
    NodeID source;
    NodeID destination;
    int32_t stackArgumentOffset;
    bool isDone;
};

struct CaseOfSwitch {
    int64_t value;
    uint32_t successor;
};

struct InferredSwitch {
    uint32_t firstCase;
    uint32_t numberOfCases;
};

struct LinkOfChain {
    uint32_t block;
    unsigned leavingIndex;
};

struct LeavingTest {
    B3::Opcode relation;
    bool leavesWhenItHolds;
    NodeType type;
    int64_t constant;
    uint32_t successor;
};

struct DispatchOfLeavingTests {
    MacroAssembler::Jump entrance;
    uint32_t block;
    NodeID terminal;
    GPRReg subject;
};

struct CaseTest {
    NodeID subject;
    int64_t constant;
    NodeType type;
    bool equalIsTaken;
};

struct BitTestOfSwitch {
    uint32_t successor;
    uint64_t mask;
};

struct EdgeStub {
    uint32_t edge;
    uint32_t source;
    uint32_t target;
    MacroAssembler::JumpList jumps;
    Box<MacroAssembler::Label> sharedLabel;
    MacroAssembler::Label label { };
};

struct PendingJump {
    MacroAssembler::Jump jump;
    uint32_t target;
    uint32_t offset;
};

struct Island {
    MacroAssembler::Label label;
    uint32_t target;
    uint32_t offset;
};

struct RareRegion {
    uint32_t firstBlock { 0 };
    uint32_t endBlock { 0 };
    uint32_t numberOfSlots { 0 };
    uint32_t numberOfJumps { 0 };
    size_t offsetOfFirstJump { 0 };
    bool isWritten { false };
};

enum class Layout : uint8_t {
    RareCodeAtEnd,
    RareCodeInGaps,
    AsControlFlows,
};

enum class Pass : uint8_t {
    Select,
    Emit,
};

struct ValueLocation {
    uint8_t location { 0 };
    NodeID node { 0 };

    bool isRegister() const { return isRegisterLocation(location); }
    bool isSlot() const { return location == locationInSlot; }
    bool isMaterialized() const { return location == locationMaterialized; }
    Reg reg() const { return decode(location); }
};

} // namespace LinearScan

class LinearScanCodeGenerator {
    WTF_MAKE_NONCOPYABLE(LinearScanCodeGenerator);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LinearScanCodeGenerator);
public:
    LinearScanCodeGenerator(B3::Procedure&, DirectCodeAttempt);
    ~LinearScanCodeGenerator();

    bool plan();
    void setLoops(LoopsInDirectCode loops) { m_loopsRule = loops; }
    bool generate(CCallHelpers&, CodeRecords&, const BlockColdnessRule&);

private:
    using NodeID = LinearScan::NodeID;
    using Position = LinearScan::Position;
    using Node = LinearScan::Node;
    using Block = LinearScan::Block;
    using Form = LinearScan::Form;
    using NodeType = LinearScan::NodeType;
    using Operand = LinearScan::Operand;
    using OperandKind = LinearScan::OperandKind;
    using Interval = LinearScan::Interval;
    using VirtualRegister = LinearScan::VirtualRegister;
    using UseKind = LinearScan::UseKind;
    using Place = LinearScan::Place;
    using Pass = LinearScan::Pass;
    using AddressPlan = LinearScan::AddressPlan;
    using MemoryOperand = LinearScan::MemoryOperand;
    using OperandPlan = LinearScan::OperandPlan;
    using OperandShape = LinearScan::OperandShape;
    using ValueLocation = LinearScan::ValueLocation;
    using Transfer = LinearScan::Transfer;
    using Move = LinearScan::Move;
    using LiveRange = LinearScan::LiveRange;
    using UsePosition = LinearScan::UsePosition;
    using ClobberEvent = LinearScan::ClobberEvent;
    using EdgeMoves = LinearScan::EdgeMoves;
    using BoundaryEntry = LinearScan::BoundaryEntry;
    using UpsilonRecord = LinearScan::UpsilonRecord;
    using Loop = LinearScan::Loop;
    using KnownLoad = LinearScan::KnownLoad;
    using EdgeStub = LinearScan::EdgeStub;
    using RareRegion = LinearScan::RareRegion;
    using Layout = LinearScan::Layout;
    using CaseOfSwitch = LinearScan::CaseOfSwitch;
    using InferredSwitch = LinearScan::InferredSwitch;
    using CaseTest = LinearScan::CaseTest;
    using LinkOfChain = LinearScan::LinkOfChain;
    using LeavingTest = LinearScan::LeavingTest;
    using DispatchOfLeavingTests = LinearScan::DispatchOfLeavingTests;
    using BitTestOfSwitch = LinearScan::BitTestOfSwitch;
    using SpillState = LinearScan::SpillState;
    using Jump = MacroAssembler::Jump;
    using Label = MacroAssembler::Label;
    using TrustedImm32 = MacroAssembler::TrustedImm32;
    using TrustedImm64 = MacroAssembler::TrustedImm64;
    using Address = MacroAssembler::Address;

    bool refuse()
    {
        m_isRefused = true;
        return false;
    }
    void fail() { m_hasFailed = true; }

    Node& node(NodeID id) { return m_nodes[id]; }
    const Node& node(NodeID id) const { return m_nodes[id]; }
    NodeID idOf(const B3::Value* value) const { return value->index() + 1; }
    B3::Value* b3ValueOf(NodeID id) const { return m_procedure.values().at(id - 1); }
    NodeID resolve(NodeID id) const
    {
        while (m_nodes[id].form == Form::Alias)
            id = m_nodes[id].a;
        return id;
    }
    NodeID resolved(const B3::Value* value) const { return resolve(idOf(value)); }
    bool isConstant(NodeID id) const { return m_nodes[id].form == Form::Constant; }
    bool constantOf(NodeID id, int64_t& result) const
    {
        if (m_nodes[id].form != Form::Constant)
            return false;
        result = m_nodes[id].constant;
        return true;
    }
    bool isZero(NodeID id) const { return m_nodes[id].form == Form::Constant && !m_nodes[id].constant; }

    // Order and import.
    void setUpRegisters();
    void findMergesOnlyBranchedOn();
    B3::BasicBlock* threadedTarget(B3::BasicBlock* source, B3::BasicBlock* target, B3::FrequencyClass* rarestEdge = nullptr) const;
    bool jumpedStraightTo(const B3::BasicBlock* source, const B3::BasicBlock* merge) const;
    void findBranchesToTakeOver();
    uint32_t commonDominator(uint32_t, uint32_t) const;
    void computeDominatorsOverAllEdges();
    bool dominates(uint32_t dominator, uint32_t block) const;
    bool enterBlock(uint32_t order, uint32_t& epoch, uint32_t& memoryEpoch);
    bool isSameOperand(NodeID, NodeID) const;
    uint64_t operandKeyOf(NodeID) const;
    bool tryReuseValue(NodeID);
    bool orderBlocks();
    void findLoops();
    std::optional<CaseTest> caseTestOf(const Node& branch, Vector<NodeID, 4>& chain) const;
    bool holdsOnlyItsTest(const Block&, const Vector<NodeID, 4>& chain) const;
    void inferSwitches();
    void takeOverChain(uint32_t order, std::span<const LinkOfChain>, uint32_t otherwise, Vector<uint32_t, 16>& successorOfLink);
    std::optional<std::pair<NodeID, LeavingTest>> leavingTestOf(const Block&, unsigned leavingIndex, Vector<NodeID, 4>& chain) const;
    void inferLeavingTests();
    void placeStrayUpsilons(NodeID phi, uint32_t order);
    std::optional<int64_t> valueIfPhiIs(NodeID value, NodeID phi, int64_t arriving, unsigned depth = 0) const;
    void findEdgesThatDecideTests();
    bool canPassMerge(uint32_t edge, const Block& source) const;
    bool canRunBeforeTrap(NodeID) const;
    void foldTrapsIntoAccesses();
    void noteTrapTakenBy(const Node& access, const MemoryOperand&);
    void estimateFrequencies();
    bool importValues();
    bool import(B3::Value*, NodeID, uint32_t block, uint32_t& epoch, uint32_t& memoryEpoch);
    void importIntegerBinary(Node&);
    void makeConstant(Node& target, int64_t constant)
    {
        target.form = Form::Constant;
        target.constant = constant;
    }
    void makeAlias(Node&, NodeID target, bool targetIsChild = true);
    void dropRawUse(NodeID);
    void releaseRawUse(NodeID);
    bool isSameValue(NodeID, NodeID, unsigned depth) const;
    std::pair<NodeID, bool> truthSourceOf(NodeID condition) const;
    std::optional<bool> knownTruthOf(uint32_t block, NodeID condition);
    void noteTruthKnownAtEntry(uint32_t block);
    bool tryReuseLoad(B3::Value*, NodeID, uint32_t memoryEpoch);
    bool tryReusePureCall(const B3::PatchpointValue*, NodeID);

    uint32_t successorOf(const Block& block, unsigned index) const { return m_successors[block.firstSuccessor + index]; }
    uint32_t targetOf(const Block& block, unsigned index) const
    {
        uint32_t target = successorOf(block, index);
        while (m_blocks[target].forwardsTo != UINT32_MAX)
            target = m_blocks[target].forwardsTo;
        return target;
    }

    // Selection.
    std::optional<B3::PinnedConstant> pinnedConstantFor(int64_t constant) const
    {
        auto finder = m_procedure.policy().addressing.findPinnedConstant;
        if (!finder)
            return std::nullopt;
        return finder(static_cast<uint64_t>(constant));
    }
    std::pair<NodeID, uint8_t> scaledOperandOf(NodeID candidate, const Node& user) const;
    AddressPlan planAddress(NodeID pointer, int64_t offset, const Node& user) const;
    template<Pass> bool conditionIsFolded(NodeID condition, NodeType expected, const Node& user) const;
    bool isFoldableCondition(NodeID condition, NodeType expected, const Node& user) const;
    template<Pass> bool loadIsFolded(NodeID load, const Node& user) const;
    bool canFoldLoad(NodeID load, const Node& user) const;
    bool isTestedAnew(const Node& comparison, const Node& user) const;
    template<Pass> bool isLogicalCondition(NodeID condition, const Node& user) const;
    bool replacesImmediate(int64_t constant, bool hasOneByteForm, OperandPlan&) const;
    template<Pass> OperandPlan planComparison(const Node& comparison, const Node& user, bool mayFoldLoads) const;
    template<Pass> OperandPlan planTest(NodeID left, NodeID right, bool isWide, const Node& user, bool mayFoldLoads) const;
    template<Pass> OperandPlan planBinary(const Node&) const;
    bool storesImmediate(const Node& store) const;

    uint32_t ensureVirtualRegister(NodeID);
    void noteUse(NodeID, Reg hint);
    void appendOperand(NodeID, OperandKind, uint8_t fixed = 0, uint8_t flags = 0);
    template<Pass> ValueLocation useRegisterOrSlot(NodeID, bool isLate = false, Reg hint = Reg());
    Operand& takeOperand(NodeID, OperandKind);
    void addClobber(Position from, Position to, uint64_t registers);
    void noteFrameUse(bool needsRecord);

    template<Pass> Reg use(NodeID, OperandKind = OperandKind::Use, Reg hint = Reg());
    template<Pass> ValueLocation useAnywhere(NodeID, OperandKind, Reg hint = Reg());
    template<Pass> Reg define(NodeID, OperandKind = OperandKind::Definition, Reg fixed = Reg(), NodeID sharedWith = 0);
    template<Pass> Reg temporary(bool isDouble);
    template<Pass> MemoryOperand operandFor(const AddressPlan&, OperandKind = OperandKind::Use);
    template<Pass> MemoryOperand operandForLoad(NodeID load, const Node& user, OperandKind = OperandKind::Use);
    template<Pass> void fold(NodeID);
    template<Pass> void chooseOperandOrder(NodeID);
    template<Pass> void prepareArguments(Vector<Transfer, 8>&);
    NodeType typeOfChild(const B3::Value*) const;

    void select();
    template<Pass> void lower(NodeID);
    template<Pass> void lowerIntegerBinary(NodeID);
    template<Pass> void lowerShift(NodeID);
    template<Pass> void lowerLoad(NodeID);
    template<Pass> void lowerStore(NodeID);
    template<Pass> void lowerComparison(NodeID);
    template<Pass> void lowerSelect(NodeID);
    template<Pass> void lowerDoubleBinary(NodeID);
    template<Pass> void lowerDoubleExtremum(NodeID);
    template<Pass> void lowerDivision(NodeID);
    template<Pass> void lowerUnary(NodeID);
    template<Pass> void lowerCheck(NodeID);
    template<Pass> void lowerCheckedArithmetic(NodeID);
    template<Pass> void lowerPatchpoint(NodeID);
    template<Pass> void lowerUpsilon(NodeID);
    template<Pass> void lowerPhi(NodeID);
    template<Pass> void lowerBranch(NodeID);
    template<Pass> void lowerSwitch(NodeID);
    template<Pass> void lowerLeavingTests(NodeID);
    Jump branchWhereLeaving(const LeavingTest&, GPRReg subject, int64_t constant);
    void emitDispatchesOfLeavingTests();
    bool planBitTests(std::span<const CaseOfSwitch>, unsigned start, unsigned end, Vector<BitTestOfSwitch, 4>&) const;
    bool usesJumpTable(std::span<const CaseOfSwitch>, unsigned start, unsigned end) const;
    bool switchNeedsTemporaries(std::span<const CaseOfSwitch>, unsigned start, unsigned end) const;
    void emitCasesOfSwitch(std::span<const CaseOfSwitch>, unsigned start, bool hasHardStart, unsigned end, bool isLast);
    template<Pass> void lowerReturn(NodeID);
    template<Pass> void lowerJump(NodeID);
    template<Pass> Jump branchOn(NodeID condition, NodeType expected, const Node& user, bool whenTrue);
    template<Pass> Jump branchOnFolded(NodeID condition, const Node& user, bool whenTrue);
    template<Pass> Jump branchOnComparison(NodeID comparison, const Node& user, bool whenTrue);
    template<Pass> Jump branchOnTest(const Node& test, const Node& user, bool whenNonZero, bool isMadeOnce = true);
    void noteFlagsOf(NodeID);
    bool testsBitsAnew(const Node& comparison) const;
    bool flagsTell(NodeID, bool isWide) const;
    template<Pass> bool canSetFlagsFor(NodeID condition, NodeType expected, const Node& user) const;
    template<Pass> MacroAssembler::RelationalCondition setFlagsFor(NodeID condition, NodeType expected, const Node& user, bool whenTrue);
    template<Pass> B3::ValueRep repForStackmapChild(NodeID child, NodeType, const B3::ValueRep& constraint);
    template<Pass> void noteStackmap(B3::StackmapValue*);

    // Liveness and register allocation.
    void computeLiveness();
    void buildIntervals();
    uint32_t newInterval(uint32_t vreg, uint32_t region, bool isDouble);
    uint32_t intervalFor(uint32_t vreg, uint32_t region);
    void addRange(uint32_t interval, Position from, Position to);
    void addUse(uint32_t interval, Position, UseKind, uint32_t operand, bool isLate = false);
    void allocateRegisters();
    void advanceTo(Position);
    void activate(uint32_t interval);
    bool staysOutOfRegisters(uint32_t current);
    uint32_t definingIntervalOf(const VirtualRegister&) const;
    void meanCalleeSavesForValuesThatCrossCalls();
    bool keepsInMemory(uint32_t current);
    template<typename Functor>
    void forEachSourceOf(NodeID phi, const Functor& functor) const
    {
        const Block& block = m_blocks[node(phi).block];
        for (unsigned index = 0; index < block.numberOfPredecessors; ++index) {
            uint32_t sender = m_predecessors[block.firstPredecessor + index];
            const Block& predecessor = m_blocks[sender];
            if (!predecessor.isReachable)
                continue;
            for (uint32_t record = predecessor.firstUpsilon; record; record = m_upsilons[record - 1].next) {
                const Node& upsilon = node(m_upsilons[record - 1].upsilon);
                if (upsilon.b == phi && upsilon.form == Form::Plain) {
                    functor(upsilon.a, sender);
                    break;
                }
            }
        }
    }
    bool tryAllocateFreeRegister(uint32_t current);
    void allocateBlockedRegister(uint32_t current);
    void limitByClobbers(const Interval&, uint64_t candidates, std::array<Position, LinearScan::numberOfRegisters>& limits, uint64_t definedHere) const;
    Position nextIntersection(const Interval&, const Interval&) const;
    bool covers(const Interval&, Position) const;
    Position firstUseFrom(const Interval&, Position, UseKind least) const;
    Position lastUseBefore(const Interval&, Position, UseKind least) const;
    uint32_t splitInterval(uint32_t interval, Position, bool overlaps = false);
    Position findSplitPosition(Position least, Position most, bool prefersEarly = false) const;
    Position earliestDeparture(const Interval&, Position limit, bool& overlaps) const;
    void splitWhereRegisterIsLost(uint32_t interval, Position limit);
    void splitBeforeUse(uint32_t interval, Position least, Position most);
    void splitAndSpill(uint32_t interval, bool isActive);
    void spillTail(uint32_t interval);
    void assignSlot(uint32_t interval, Position);
    void enqueue(uint32_t interval);
    uint64_t candidatesFor(const Interval&) const;
    unsigned collectHints(const Interval&, std::array<uint8_t, 6>&) const;
    uint8_t locationAt(uint32_t vreg, Position) const;
    bool isBlockStart(Position) const;
    uint32_t blockAt(Position position) const { return m_blockOfSlot[std::min<size_t>(LinearScan::slotOf(position), m_blockOfSlot.size() - 1)]; }
    double frequencyAt(Position position) const { return m_blocks[blockAt(position)].frequency; }
    void writeBackLocations();
    void insertMoves();
    void resolveEdges();
    void addEdgeMove(uint32_t edge, NodeID source, uint8_t from, NodeID destination, uint8_t to);
    void hoistMovesSharedByAllPredecessors();

    // Frame.
    void layOutFrame();
    bool liveRangesIntersect(uint32_t, uint32_t) const;
    void noteWhereFrameIsUsed();
    void chooseWhereToSetUpFrame();
    void emitFrameTeardownOnEdge();
    bool edgeLeavesFrame(uint32_t edge, const Block& source) const { return m_hasFrame && source.runsInFrame && !m_blocks[m_successors[edge]].runsInFrame; }
    bool edgeEntersFrame(uint32_t edge, const Block& source) const { return m_hasFrame && !source.runsInFrame && m_blocks[m_successors[edge]].runsInFrame; }
    void emitFrameSetup();
    Address addressOfSlot(int32_t offset) const;
    bool isAddressedOffStackPointer(int32_t offset) const;
    CodeUnit::Kind kindOfFrameAccess(int32_t offset) const { return isAddressedOffStackPointer(offset) ? CodeUnit::Kind::PlainOffStackPointer : CodeUnit::Kind::Plain; }

    // Emission.
    size_t bytesSoFar() const { return m_jit->m_assembler.buffer().codeSize(); }
    void finishUnit(CodeUnit::Kind = CodeUnit::Kind::Plain);
    void finishBranchUnit() { finishUnit(std::exchange(m_kindOfBranchUnit, CodeUnit::Kind::Plain)); }
    void orderEmission();
    void emitRareRegion(uint32_t region, uint32_t blockThatFollows);
    void emitRareCodeInFrontOf(uint32_t next);
    void emitBlock(Block&);
    void emitMovesAt(Position);
    void emitEdgeMoves(uint32_t edge);
    void emitMoves(uint32_t first, uint32_t count, NodeID skippedDestination = 0);
    void emitParallelMoves(Vector<Transfer, 8>&);
    void emitTransfer(const Transfer&);
    void moveRegister(Reg source, Reg destination);
    void materialize(NodeID, Reg destination);
    void storeToSlot(Reg, const Node& owner, int32_t offset);
    void loadFromSlot(Reg, const Node& owner, int32_t offset);
    int32_t offsetOfSlotOf(NodeID) const;
    void recordOrigin(NodeID);
    void noteNewLatePaths();
    void link(Jump, uint32_t target);
    void linkInsideFrame(Jump, uint32_t target);
    void linkEdge(Jump, uint32_t source, unsigned successorIndex);
    void jumpToEdge(uint32_t source, unsigned successorIndex);
    bool edgeHasMovesOnIt(uint32_t source, unsigned successorIndex) const;
    bool edgeMovesLieAtStartOfTarget(uint32_t edge) const;
    bool hasOneLiveSuccessor(const Block&) const;
    bool isNext(uint32_t target, uint32_t from) const;
    Box<Label> sharedLabelOf(Block&);
    Box<Label> sharedLabelOfEdge(uint32_t source, unsigned successorIndex);
    uint32_t stubFor(uint32_t source, unsigned successorIndex);
    void findEdgesWithSameMoves();
    void emitEdgeStubs();

    template<typename Functor>
    auto withAddress(const MemoryOperand& operand, const Functor& functor)
    {
        if (operand.hasIndex())
            return functor(operand.baseIndex());
        return functor(operand.address());
    }

    B3::Procedure& m_procedure;
    B3::Air::Code& m_code;
    CCallHelpers* m_jit { nullptr };
    CodeRecords* m_records { nullptr };
    const BlockColdnessRule* m_coldness { nullptr };
    B3::Air::GenerationContext m_context;

    bool m_isRefused { false };
    bool m_hasFailed { false };
    bool m_hasMergesOnlyBranchedOn { false };
    bool m_hasCycles { false };
    bool m_needsFrame { false };
    bool m_needsFrameRecord { false };
    bool m_hasFrame { false };
    bool m_hasFrameRecord { true };
    bool m_setsUpFrameAtEntry { true };
    bool m_codeIsReachable { true };
    bool m_prefersSmallCode { false };
    Layout m_layout { Layout::RareCodeAtEnd };
    int32_t m_slotBias { 0 };
    unsigned m_numberOfTeardowns { 0 };
    Label m_sharedTeardown;
    size_t m_offsetOfSharedTeardown { 0 };
    size_t m_bytesOfSharedTeardown { 0 };
    unsigned m_bytesOfStackArguments { 0 };
    unsigned m_numberOfLatePathsNoted { 0 };

    BitVector m_mergesOnlyBranchedOn;
    Vector<Node> m_nodes;
    Vector<Block> m_blocks;
    Vector<NodeID> m_sequence;
    Vector<uint32_t> m_blockOfSlot;
    Vector<uint32_t> m_linearOf;
    Vector<uint32_t> m_reversePostOrder;
    Vector<uint32_t, 64> m_successors;
    Vector<B3::FrequencyClass, 64> m_edgeClasses;
    Vector<uint8_t> m_successorDecidedOnEdge;
    Vector<NodeID, 4> m_strayUpsilons;
    Vector<uint32_t> m_branchesTakenOver;
    Vector<CaseOfSwitch> m_casesOfSwitches;
    Vector<InferredSwitch, 2> m_inferredSwitches;
    Vector<LeavingTest> m_leavingTests;
    Vector<DispatchOfLeavingTests> m_dispatchesOfLeavingTests;
    GPRReg m_switchSubject { InvalidGPRReg };
    GPRReg m_switchIndex { InvalidGPRReg };
    GPRReg m_switchTable { InvalidGPRReg };
    bool m_switchIsWide { false };
    Vector<uint32_t, 64> m_predecessors;
    Vector<uint32_t, 64> m_edgeOfPredecessor;
    Vector<LinearScan::Loop, 4> m_loops;
    Vector<std::pair<uint32_t, uint32_t>, 4> m_backEdges;
    Vector<LinearScan::UpsilonRecord> m_upsilons;
    Vector<LinearScan::KnownLoad, 64> m_knownLoads;
    Vector<NodeID, 64> m_knownValues;
    std::array<NodeID, 64> m_knownPureCalls { };
    std::array<NodeID, LinearScan::numberOfRegisters> m_argumentNodes { };
    uint32_t m_lastEpoch { 0 };
    uint32_t m_lastMemoryEpoch { 0 };

    uint64_t m_generalRegisters { 0 };
    uint64_t m_doubleRegisters { 0 };
    uint64_t m_calleeSaves { 0 };
    uint64_t m_allocatableCalleeSaves { 0 };
    uint64_t m_usedCalleeSaves { 0 };
    uint64_t m_clobberedRegisters { 0 };
    Vector<uint8_t, 32> m_registerOrder;

    Vector<Operand> m_operands;
    Vector<VirtualRegister> m_virtualRegisters;
    Vector<uint32_t> m_globals;
    Vector<uint32_t> m_exposedUses;
    Vector<uint64_t> m_liveIn;
    size_t m_liveWords { 0 };
    Vector<Interval> m_intervals;
    Vector<LinearScan::LiveRange> m_ranges;
    Vector<LinearScan::UsePosition> m_uses;
    Vector<LinearScan::ClobberEvent> m_clobbers;
    uint64_t m_everClobbered { 0 };
    Vector<uint64_t> m_unhandled;
    std::array<uint32_t, LinearScan::numberOfRegisters> m_activeInRegister { };
    uint64_t m_activeRegisters { 0 };
    Vector<uint32_t, 16> m_inactive;
    Position m_nextChangeOfActive { 0 };
    Position m_currentPosition { 0 };
    Vector<Position> m_probes;
    Vector<uint64_t> m_registersLiveAtProbes;

    Vector<Move> m_moves;
    Vector<Move> m_movesOnEdges;
    Vector<LinearScan::EdgeMoves> m_edges;
    Vector<LinearScan::BoundaryEntry> m_boundaryEntries;
    Vector<LinearScan::EdgeStub, 4> m_edgeStubs;
    uint32_t m_numberOfSlotAccesses { 0 };
    Vector<Transfer, 8> m_copiesBeforeInstruction;
    Vector<int32_t, 8> m_variableOffsets;

    Vector<uint32_t> m_emissionOrder;
    uint32_t m_blockThatFollows { UINT32_MAX };
    Vector<LinearScan::RareRegion, 8> m_rareRegions;
    Vector<uint32_t, 8> m_waitingRegions;
    unsigned m_numberOfEdgeStubsWritten { 0 };
    size_t m_offsetOfFirstWaitingEdgeStub { 0 };
    uint32_t m_currentBlock { 0 };
    uint32_t m_operandCursor { 0 };
    uint32_t m_moveCursor { 0 };
    NodeID m_currentNode { 0 };
    Label m_unitBegin;
    CodeUnit::Kind m_kindOfBranchUnit { CodeUnit::Kind::Plain };
    NodeID m_pendingTrap { 0 };
    NodeID m_nodeInFlags { 0 };
    size_t m_sizeWhenFlagsWereSet { 0 };
    Label m_pendingTrapBegin;
    GPRReg m_pendingTrapRegister { InvalidGPRReg };
    bool m_pendingTrapIsNarrow { false };
    bool m_hasLoadsThatOnlyTrap { false };
    bool m_allUsesAreCounted { false };
    Vector<Position, 32> m_positionsOfCalls;
    uint64_t m_calleeSavesMeantForValues { 0 };
    LoopsInDirectCode m_loopsRule { LoopsInDirectCode::AreGenerated };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)
