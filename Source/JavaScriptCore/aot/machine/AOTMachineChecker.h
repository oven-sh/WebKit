/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"
#include "AOTMachineSlots.h"

// Checks of the back end on itself. prepareForGeneration() runs them where ProcedureTraits::checksAllocation is set: debug and ASAN builds, --aot-validate, and Options::verifyAOTMachineCode(), which alone checks the code that an ordinary build makes. No other build pays for them.
// Each reports through Procedure::fail() and returns whether it found nothing.

namespace JSC { namespace AOT { namespace Machine {

// Behind the emitters, ahead of Order. AOTMachineValidate.cpp.
//  - Every block that an edge or an entrypoint leads to ends in one operation that ends a block, and has as many successors as that operation wants.
//  - The children of each operation have the types that its opcode wants, and its own type follows from them.
//  - A child with a position stands in a block that dominates the user, or ahead of it in the same block. For the child of an Upsilon the user is the end of the block.
//  - Every Upsilon has its Phi, and each edge into a block with Phis brings one Upsilon for each. The Phi stands in a successor of the block of the Upsilon, unless that block ends in a Jump: a Branch or a Switch on a constant is appended as a Jump, and the Upsilons for the edges it does not take stay behind unread.
//  - No edge leads to an entrypoint. A block that is marked as having one predecessor has one.
//  - A load or a store at an offset from a SlotBase lies within the StackSlot. Behind it lies a spilled value, a callee save or another slot.
//  - A Patch with a tuple type has one result constraint for each member. A constraint is of a kind that its side allows. A Patch that clobbers on a successor ends its block.
//  - Operation::useCount() is at least the number of times the operation appears as a child. The children of an unread operation for which countsChildrenAtFirstUse() holds are not counted. A count that is too high is not a fault.
// Not asked here, since it holds of what the scan leaves standing: the Extracts of a Patch stand right behind it, at the head of its first successor where it ends its block, with nothing between them, and a member has one. That successor has no other predecessor.
// Allocator::allocatePatch() asks it, in every build: MalformedProcedure.
bool validate(Procedure&);
// Behind threadJumps() and mergeIdenticalBlocks(), ahead of Order: the same rules, of what the two passes leave. A block that they have passed by or merged away still stands in the procedure with its edges,
// and an Upsilon that they have taken out of its block still names the block. Nothing orders or encodes either, so the rules are asked of the blocks that the entrypoints lead to and of the operations that those list.
bool validateBehindBlockPasses(Procedure&);

// Behind allocateRegisters(). AOTMachineAllocationChecker.cpp.
// A symbolic run over the steps: each register and each spill holds the set of values that it is known to hold, and every read asks whether the value that the operation names is among them.
//  - The state at the head of a block is the intersection of the states at the ends of its predecessors behind their edge moves. Blocks are visited in the order of allocation, and once more for each back edge that takes something away.
//  - An Execute reads each leaf from the location in the step, then forgets what its clobbers, scratch registers and results overwrite. A Check forgets nothing on the passing side but the scratch registers of the assembler.
//  - The store of a spilled value stands behind its definition, so the spill holds the value from there on. One with SpillFacts::isStoredAtHeadOfFramedPart holds it from the Save at a head of the framed part on.
//  - On an edge to a head outside the range of a spill, or in its gap, the spill holds nothing, and no step names it there: its place may be another's.
//  - On an edge out of the framed part no callee-saved register, spill or outgoing word holds anything, unless Allocation::needsFrameAheadOfFramedPart().
//  - At a landing pad no register holds anything. Behind a suspension only SavedFrame::preservedRegister and the spills that it holds do: Allocation::spillsHeldAt().
//  - A constraint of a Patch is met by the location it was given, no two results share a register, and no late operand lies in a register that is clobbered late.
//  - A pinned register is never a destination. A scratch register may be the destination of a Move, a Reload or a Materialize, and the source of a Move or a Save. It is never a leaf, a result or a scratch register of an Execute, unless a Patch constrains an operand to it by name.
// Failures: RegisterLacksItsValue, SpillSlotLacksItsValue.
bool checkAllocation(Procedure&);

// Behind assignSlots() and planFrame(). AOTMachineFrameChecker.cpp.
//  - Two spills whose ranges overlap outside their gaps do not share a byte, nor does a spill share one with a StackSlot or a callee save. No suspension holds a spill with a gap. Failure: SpillSlotLacksItsValue.
//  - A spill that a suspension holds lies in the part that suspensions keep, and none of its words is listed as dead at that suspension. That each holds enough is for checkAllocation() to find.
//  - A saved frame holds whole pairs of words. Where that makes it hold one word below the part that suspensions keep, no spill, StackSlot or outgoing word lies in that word.
//  - No word that is cleared at a suspension, in front of it or by whoever suspends the function for the first time, holds a spill that the suspension or anything behind it reads, a callee save or a StackSlot that suspensions keep.
//    A spill that the suspension holds counts as read, unless one store behind the definition of its value writes it and a path leads from an entrypoint to the suspension without that store.
//  - Wherever a step names a spill, an outgoing word, a SlotBase or the FramePointer, or executes a Patch that does not run without a frame, the frame is set up: NeedsFrameWhereNoneIsSetUp.
//  - With FrameCoverage::FramedPart: the head lies in no loop and in no rare region and is no entrypoint, every edge into the framed part leads to the head, and beside the head only a block that enters a rare region outside sets a frame up.
//    The two ends of every edge agree on whether the frame is set up, but for a jump that is written as a return: it leads to a block that holds a Return alone.
//    Where the frame is not set up no step writes a callee-saved register, none holds a value where such a block is entered, and no step outside the framed part names a spill that is stored at its head.
bool checkFrame(Procedure&);

// Behind allocateRegisters(). AOTMachineReferenceChecker.cpp. The three rules of the collector, asked of the outcome and not of the flags that brought it about.
//  - Across every Patch with Effects::mayCollect, rare ones too: a live value with OperationFlag::IsNarrowReference that names no permanent cell has a spill with isPointerBesideNarrowReference that covers the Patch. A pointer that is kept at the head of the framed part counts in the framed part alone. Across a suspension, whatever its effects say, it is also a LateColdAny operand of the Patch: a saved frame is visited by its map, which lists the roots and not the pointer. NarrowReferenceWithoutPointer.
//  - Wherever such a Patch stands between the definition and the last read of a value with OperationFlag::IsInnerPointer, its owner is in a register or a spill there. InnerPointerWithoutOwner.
//  - Across a suspension no live value with a position is computed from a SlotBase or the FramePointer through Add, Sub, Opaque, Select or Phi. FrameAddressAcrossSuspension.
bool checkReferences(Procedure&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
