/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEscapeAnalysis.h"
#include "AOTIRBuilder.h"
#include "AOTInductionVariables.h"
#include "AOTPass.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet constructionAllocations { Opcode::NewArrayLike, Opcode::NewArrayWithCapacity };
inline constexpr OpcodeSet arrayCapacityOpcodes = constructionAllocations | OpcodeSet { Opcode::NewArray, Opcode::AppendElement, Opcode::CallBuiltin, Opcode::HasArrayStorage };

enum class CapacityTermKind : uint8_t { Unknown, Constant, Integer, Number };

struct CapacityTerm {
    CapacityTermKind kind { CapacityTermKind::Unknown };
    ValueID value;
    int64_t constant { 0 };
};

struct TripCount {
    CapacityTerm added;
    CapacityTerm subtracted { CapacityTermKind::Constant, { }, 0 };
    int64_t adjustment { 0 };
};

struct CapacityPlan {
    bool hasRunTimeTerm() const { return !added.isEmpty() || !subtracted.isEmpty() || !roundedUp.isEmpty(); }
    bool tryAdd(const CapacityTerm&);
    bool trySubtract(const CapacityTerm&);

    ValueID literal;
    int64_t constant { 0 };
    Vector<ValueID, 2> added;
    Vector<ValueID, 2> subtracted;
    Vector<ValueID, 2> roundedUp;
};

// What an array still needs when its vector is full: the iterations a counted loop has left, times what each of them appends.
struct GrowthPlan {
    ValueID append;
    ValueID array;
    BlockID loopHeader;
    TripCount remainingIterations;
    unsigned elementsPerIteration { 1 };
    // Only of a collection.
    TripCount finishedIterations { };
    BlockID preheader { };
};

enum class AppendKind : uint8_t { Elements, List, StoreAtIndex };
enum class SiteExecution : uint8_t { Unknown, Always, Conditional, InNestedLoop };

struct AppendSite {
    AppendKind kind { AppendKind::Elements };
    ValueID value;
    BlockID block;
    unsigned count { 0 };
    ValueID operand;
};

struct PlannedLiteral {
    ValueID value;
    ProgramPoint point;
    const NaturalLoop* loop { nullptr };
};

struct BlockChain {
    bool contains(BlockID block) const { return members.get(block.index()); }

    BitVector members;
};

struct LoopCounter {
    explicit operator bool() const { return variable; }

    const InductionVariable* variable { nullptr };
    const InductionTest* test { nullptr };
};

struct MeasuredStorage {
    ValueID owner;
    ValueID guard;
    AbstractHeap heap;
};

std::optional<MeasuredStorage> storageMeasuredBy(const Program&, const Value& lengthRead);
// The test for null and undefined in front of a read of a length, where the read measures what the test lets through.
ValueID checkInFrontOf(const Function&, const MeasuredStorage&);
ValueID appendedArrayOf(const Value& append);

// A capacity is a prediction: a path through a loop counts when it is as likely as the loop.
BitVector blocksRarerThanHeader(FunctionAnalyses&, const NaturalLoop&);
// nullopt: the value is no step through a Map or a Set, or the class of the collection is not proven.
std::optional<Value> makeSizeReadOfWalkedCollection(FunctionPassContext&, const Value& step);
// nullopt: the class of the collection is not proven.
std::optional<Value> makeSizeRead(FunctionPassContext&, ValueID collection, BuiltinID getter);

class ArrayCapacityPlanner {
    WTF_MAKE_NONCOPYABLE(ArrayCapacityPlanner);
public:
    explicit ArrayCapacityPlanner(FunctionPassContext&);

    Vector<CapacityPlan, 4> collectPlans();
    Vector<GrowthPlan, 4> collectGrowthPlans();
    Vector<GrowthPlan, 4> collectCollectionGrowthPlans();

private:
    std::optional<CapacityPlan> plan(ValueID literal);
    bool isCandidate(const Value&) const;

    std::optional<AppendKind> appendKindOf(const Value&) const;
    Vector<AppendSite, 4> collectSites(ObjectID);
    SiteExecution executionOf(const PlannedLiteral&, const BlockChain&, const AppendSite&);
    BlockChain chainFrom(BlockID, const NaturalLoop*);
    BlockID chainSuccessorOf(BlockID, unsigned numberOfBranchesAround = 0);
    BlockID blockWhereLikelyArmsMeet(BlockID, unsigned numberOfBranchesAround);

    bool tryAddLoopSites(CapacityPlan&, const PlannedLiteral&, Vector<AppendSite, 2>&);
    bool appendsOncePerIteration(const PlannedLiteral&, const AppendSite&);
    bool runsOncePerIteration(const NaturalLoop&, BlockID);
    bool areAlternativesTo(BlockID header, const PlannedLiteral&, std::span<const AppendSite> others);

    LoopCounter findUnitStepCounter(const NaturalLoop&);
    TripCount tripCount(const PlannedLiteral&, const NaturalLoop&, const AppendSite&);
    TripCount tripCountOfCollectionWalk(const PlannedLiteral&, const NaturalLoop&, const AppendSite&);
    CapacityTerm termOf(const PlannedLiteral&, ValueID);
    bool followsWithoutVisibleEffect(ProgramPoint, ValueID);
    const NaturalLoop* likelyOuterLoopOf(const NaturalLoop&);
    ValueID valueOnLikelyEdges(ValueID);
    bool canMeasureBehindCheck(const MeasuredStorage&, ProgramPoint);
    bool isLengthOf(ValueID length, ValueID array) const;

    std::optional<GrowthPlan> planGrowth(const NaturalLoop&, ProgramPoint);
    std::optional<GrowthPlan> planCollectionGrowth(const NaturalLoop&, ProgramPoint);
    using GrowthPlanner = std::optional<GrowthPlan> (ArrayCapacityPlanner::*)(const NaturalLoop&, ProgramPoint);
    Vector<GrowthPlan, 4> collectGrowthPlans(Opcode reservation, GrowthPlanner);
    TripCount remainingIterations(const NaturalLoop&, ValueID array, ProgramPoint);
    TripCount finishedIterations(const NaturalLoop&, ValueID collection, ProgramPoint);
    BlockID findPreheader(const NaturalLoop&);
    bool hasUnconditionalCellType(ValueID array) const;
    bool isAllocatedWithPlannedCapacity(ValueID array) const;

    FunctionPassContext& m_context;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    Vector<BlockID> m_blocksWhereLikelyArmsMeet;
    BitVector m_branchesAsked;
};

unsigned applyCapacityPlans(FunctionPassContext&, std::span<const CapacityPlan>);
unsigned applyGrowthPlans(FunctionPassContext&, std::span<const GrowthPlan>);
unsigned applyCollectionGrowthPlans(FunctionPassContext&, std::span<const GrowthPlan>);

unsigned narrowStorageOfLocalArrays(FunctionPassContext&);
unsigned chooseStorageOfConstructedArrays(FunctionPassContext&);
// An array of whole numbers that fails a test for Double storage is converted on the failing edge, so that it passes the test from then on.
PassOutcome convertIntegerArraysBehindFailedTests(FunctionPassContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
