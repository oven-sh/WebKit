/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include "AOTJSONEmission.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesOfWalks { Opcode::CopyDataProperties, Opcode::CopyObject, Opcode::CallBuiltin };
inline constexpr OpcodeSet opcodesOfKeyOrderTests { Opcode::HasKeyOrder, Opcode::HasNoExtraProperties };
// ExpandOwnKeys has nothing to do in a function without any of these.
inline constexpr OpcodeSet ownKeyExpansionOpcodes = opcodesOfWalks | opcodesOfKeyOrderTests | OpcodeSet { Opcode::ForInStep };

enum class OwnKeyWalk : uint8_t {
    Values,
    Entries,
    Assign,
    Spread,
    Copy,
    Stringify,
    StringifyIntoText,
};

constexpr bool copiesNothingFromNullish(OwnKeyWalk walk)
{
    switch (walk) {
    case OwnKeyWalk::Assign:
    case OwnKeyWalk::Spread:
        return true;
    // It makes an object of undefined and null too, which the original operation does.
    case OwnKeyWalk::Copy:
    case OwnKeyWalk::Values:
    case OwnKeyWalk::Entries:
    case OwnKeyWalk::Stringify:
    case OwnKeyWalk::StringifyIntoText:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct OwnKeyWalkSite {
    OwnKeyWalk walk { OwnKeyWalk::Values };
    BlockID block;
    uint32_t index { 0 };
    ValueID value;
    ValueID source;
    ValueID target;
    uint32_t sourceArgument { 0 };
    BuiltinID textForm;
    Vector<ValueID, 1> laterSources;
};

struct OwnKeyWalkArm {
    LayoutID layout;
    KeyOrderFields order;
    // The target of the copy is a new object of this layout that has no property yet: it takes the slots and the key order of the source as they are, whatever the order. The copy tests the source itself.
    bool copiesFieldsAtOnce { false };
};

struct OwnKeyWalkPlan {
    Vector<OwnKeyWalkArm, 4> arms;
    bool needsShapeTest { true };
    bool needsKeyOrderTest { true };
    bool needsOriginalCall { true };
    // Set when the plan needs no test because the source is this allocation and nothing has changed it. It is the guard of the reads.
    ValueID unchangedAllocation;
};

struct AssignedSource {
    ValueID source;
    OwnKeyWalkPlan plan;
    // Assigns this source and every later one. It is the last arm of the walk.
    Value slowPathCall;
};

struct AssignPlan {
    Vector<AssignedSource, 2> walkedSources;
    std::optional<Value> remainingSourcesCall;
};

struct AllocationAtPoint {
    ValueID allocation;
    ProgramPoint point;
};

// Bit i is set when nothing between the NewObject uses[i].allocation and uses[i].point can have changed its shape, its key order or its extra properties.
BitVector findUnchangedAllocations(FunctionPassContext&, std::span<const AllocationAtPoint> usesInProgramOrder);

// Answers for the sources and targets of all the sites of a function with one solve.
class UnchangedAllocations {
public:
    UnchangedAllocations(FunctionPassContext&, std::span<const OwnKeyWalkSite>);

    bool contains(ValueID allocation, ProgramPoint) const;

private:
    FunctionPassContext& m_context;
    Vector<AllocationAtPoint, 8> m_uses;
    mutable std::optional<BitVector> m_unchanged;
};

std::optional<OwnKeyWalk> ownKeyWalkOf(const Program&, const Value&);
Vector<OwnKeyWalkSite, 4> findOwnKeyWalkSites(FunctionPassContext&);

std::optional<OwnKeyWalkPlan> planOwnKeyWalk(FunctionPassContext&, const UnchangedAllocations&, const OwnKeyWalkSite&);
std::optional<AssignPlan> planAssign(FunctionPassContext&, const UnchangedAllocations&, const OwnKeyWalkSite&);
std::optional<OwnKeyWalkPlan> planWalkOfEnumeratedObject(FunctionPassContext&, ValueID forInOpen);

class OwnKeyWalkExpander final : public Emission {
public:
    OwnKeyWalkExpander(IRBuilder& builder, const EmissionInputs& inputs, FunctionPassContext& pass)
        : Emission(builder, inputs)
        , m_pass(pass)
    {
    }

    void expand(const OwnKeyWalkSite&, const OwnKeyWalkPlan&);
    void expandAssign(const OwnKeyWalkSite&, const AssignPlan&);
    bool expandStringify(const OwnKeyWalkSite&);

private:
    struct WalkedField {
        PropertyKeyID key;
        uint32_t field { 0 };
    };

    // A walk that is done goes on at afterWalk. Without one it ends the expansion, as the original operation always does.
    void emitArms(const OwnKeyWalkSite&, const OwnKeyWalkPlan&, const IRBuilder::Expansion&, BlockID afterWalk);
    Vector<WalkedField, 8> fieldsWalkedBy(OwnKeyWalk, const OwnKeyWalkArm&) const;
    TestedObject emitArmTests(ValueID source, const OwnKeyWalkPlan&, const OwnKeyWalkArm&, BlockID nextArm);

    ValueID emitValues(const TestedObject&, const OwnKeyWalkArm&, std::span<const WalkedField>);
    ValueID emitEntries(const TestedObject&, const OwnKeyWalkArm&, std::span<const WalkedField>);
    void emitAssign(const OwnKeyWalkSite&, const TestedObject&, const OwnKeyWalkArm&, std::span<const WalkedField>);
    void emitSpread(const OwnKeyWalkSite&, const TestedObject&, const OwnKeyWalkArm&, std::span<const WalkedField>);
    ValueID emitCopy(const TestedObject&, const OwnKeyWalkArm&, std::span<const WalkedField>);
    void emitCopyOfFields(const OwnKeyWalkSite&, const TestedObject&, const OwnKeyWalkArm&, BlockID nextArm);
    ValueID emitOriginalOperation(const OwnKeyWalkSite&);

    FunctionPassContext& m_pass;
    Value m_originalOperation;
};

// A for-in loop over an object of a few expected layouts gets, per layout and key order, one copy of its body for each name.
bool expandForInLoops(FunctionPassContext&, IRBuilder&, const EmissionInputs&);

bool foldKeyOrderTests(FunctionPassContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
