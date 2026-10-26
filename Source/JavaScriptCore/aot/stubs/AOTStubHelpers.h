/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubAssembler.h"
#include "AOTStubFrames.h"
#include "CallFrame.h"
#include "StackAlignment.h"
#include "Structure.h"
#include <initializer_list>
#include <optional>
#include <span>
#include <variant>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

using Jump = CCallHelpers::Jump;
using JumpList = CCallHelpers::JumpList;
using Label = CCallHelpers::Label;
using Address = CCallHelpers::Address;
using BaseIndex = CCallHelpers::BaseIndex;
using TrustedImm32 = CCallHelpers::TrustedImm32;
using TrustedImm64 = CCallHelpers::TrustedImm64;

#if CPU(X86_64)
inline constexpr CCallHelpers::Address numberTag { instanceGPR, AOT_INSTANCE_OFFSET(numberTag) };
#elif CPU(ARM64)
inline constexpr GPRReg numberTag = GPRInfo::numberTagRegister;
#endif

// AOTStubHelpersValues.cpp
void unboxDouble(StubAssembler&, GPRReg boxed, GPRReg scratch, FPRReg result);
class PureDouble {
public:
    FPRReg fpr() const { return m_fpr; }

    static PureDouble branchIfNaN(StubAssembler&, FPRReg, JumpList& isNaN);
    static PureDouble branchIfNaN(StubAssembler&, FPRReg, Jump& isNaN);
    static PureDouble purify(StubAssembler&, FPRReg number, FPRReg result);
    static PureDouble convertInt64(StubAssembler&, GPRReg integer, FPRReg result);
    static PureDouble resultOfArithmeticOnBoxedNumbers(FPRReg result) { return PureDouble(result); }

private:
    explicit PureDouble(FPRReg fpr)
        : m_fpr(fpr)
    {
    }

    FPRReg m_fpr;
};

class PureDoubleBits {
public:
    GPRReg gpr() const { return m_gpr; }

    static PureDoubleBits branchIfNotFinite(StubAssembler&, GPRReg bits, GPRReg scratch, JumpList& isNotFinite);
    static PureDoubleBits loadedAgainBehindTestForNaN(GPRReg bits) { return PureDoubleBits(bits); }

private:
    explicit PureDoubleBits(GPRReg gpr)
        : m_gpr(gpr)
    {
    }

    GPRReg m_gpr;
};

void boxDouble(StubAssembler&, PureDouble, GPRReg result);
void boxDoubleBits(StubAssembler&, PureDoubleBits bitsAndResult);
// A NaN keeps its sign and none of its other bits.
void boxNaN(StubAssembler&, FPRReg nan, GPRReg result);
void boxDoubleWithoutPayload(StubAssembler&, FPRReg number, GPRReg result);
void boxInt32(StubAssembler&, GPRReg int32, GPRReg result);
// The caller has proven that the value is a number.
void toDouble(StubAssembler&, GPRReg boxedNumber, GPRReg scratch, FPRReg result);
void branchIfNotExactInt32(StubAssembler&, FPRReg number, GPRReg integer, JumpList& isNotExact, FPRReg scratch);
// integer is as branchIfNotExactInt32 left it: nothing is above its 32 bits.
void boxExactInt32(StubAssembler&, GPRReg integer, GPRReg result);
// Bits 32 to 63 of word are zero. Zero decodes to the cage base, never to null.
void decodeCell32(StubAssembler&, GPRReg word, GPRReg result);
// The reference is bits 0 to 31 of word. The bits above are not cleared.
void encodeCell32(StubAssembler&, GPRReg cell, GPRReg word);
// Gives an Extension one more granule in the room it has. Leaves where granuleBits is more than one bit, where a granule held comes after it, and where the Extension is full.
void appendGranule(StubAssembler&, GPRReg extension, GPRReg granuleBits, GPRReg scratch, GPRReg secondScratch, JumpList& cannotAppend);
// The caller has proven that the word is a literal string word.
void loadLiteralString(StubAssembler&, GPRReg wordAndResult, GPRReg scratch, JumpList& isNotCreated);
void decodeStructureID(CCallHelpers&, GPRReg idAndResult);
void loadStructure(StubAssembler&, GPRReg cell, GPRReg result);
void extractBits(StubAssembler&, GPRReg source, unsigned shift, unsigned width, GPRReg result);
void loadShapeWord(StubAssembler&, GPRReg cell, GPRReg result);
void loadCellVM(StubAssembler&, GPRReg cellAndResult);
// Overwrites structure.
Jump branchIfNotOfThisRealm(StubAssembler&, GPRReg structure);
Jump branchIfNotPristine(StubAssembler&, GPRReg cell);
// The masked indexing byte of a pristine cell. For any other cell a value above 0xff, which equals no indexing shape.
void loadPristineArrayKind(StubAssembler&, GPRReg array, GPRReg kind, unsigned mask);
Jump branchIfWatchpointSetIsNotWatched(StubAssembler&, Address set, GPRReg scratch);
// Falls through exactly when isFunctionType(cell->type()). A jump proves nothing: a callable Proxy or a host object needs C++. Preserves cell.
Jump branchIfNotFunctionType(CCallHelpers&, GPRReg cell, GPRReg scratch);
void bumpRuntimeCounter(StubAssembler&, RuntimeCounterID, GPRReg scratch);

// AOTStubHelpersAllocation.cpp In a program that counts its allocations both count the cell under the family of the stub, before they look for room: see ExactAllocationCounts.
enum class CountedUnder : bool { FamilyOfStub, Allocation };
void allocateCell(StubAssembler&, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath);
// The bump path falls through. The generator calls emitLateRefill after an exit of its fast path and before it links slowPath.
[[nodiscard]] AssemblyHelpers::FreeListRefill allocateCellWithLateRefill(StubAssembler&, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath, std::optional<unsigned> cellSize = std::nullopt, CountedUnder = CountedUnder::FamilyOfStub);
// The upper 32 bits of allocation are zero.
void countCellOfAllocation(StubAssembler&, GPRReg allocation, GPRReg scratch);
// Behind an allocation with CountedUnder::Allocation, in a stub whose slow path calls an operation that is counted as making the cell.
void countAllocatedCellOfStub(StubAssembler&, GPRReg scratch, unsigned cellSize);
void emitLateRefill(StubAssembler&, const AssemblyHelpers::FreeListRefill&, JumpList& slowPath);
void fenceAfterAllocation(StubAssembler&);

// AOTStubHelpersStorage.cpp
struct StorageGrowth {
    GPRReg object;
    GPRReg newStructureID;
    GPRReg oldCapacity;
    GPRReg newCapacity;
    GPRReg storage;
    GPRReg scratch;
    std::optional<GPRReg> addedValue;
};
void growOutOfLineStorage(StubAssembler&, const StorageGrowth&, JumpList& slowPath);
struct VectorGrowth {
    GPRReg array;
    GPRReg butterfly;
    // The public length.
    GPRReg length;
    GPRReg storage;
    GPRReg allocator;
    GPRReg scratch;
};
// For an array whose vector is as long as the array. Gives it the vector that JSObject::ensureLength() gives it for one more element. The public length stays. Preserves array alone.
void growFullVector(StubAssembler&, const VectorGrowth&, JumpList& slowPath);
// The same for an array that is to have neededLength elements, which its vector has no room for. Preserves array and neededLength.
void growVectorFor(StubAssembler&, const VectorGrowth&, GPRReg neededLength, JumpList& slowPath);

// AOTStubsBarriers.cpp
Jump branchIfBarrierIsNeeded(StubAssembler&, GPRReg owner, GPRReg scratch);
void returnIfFencedAndNotBlack(StubAssembler&, GPRReg owner);
Label returnAfterStoring(StubAssembler&, GPRReg owner, GPRReg value);

// AOTStubHelpersFrames.cpp
struct HostCallFrame {
    GPRReg callee;
    GPRReg thisValue;
    std::variant<unsigned, GPRReg> argumentCount;
    std::optional<GPRReg> arguments;
};
void emitHostCallFrame(StubAssembler&, const HostCallFrame&);
constexpr unsigned hostCallFrameSize(unsigned argumentCount)
{
    return static_cast<unsigned>(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((CallFrame::headerSizeInRegisters + 1 + argumentCount) * sizeof(Register)));
}
Address hostCallFrameSlot(CallFrameSlot, ptrdiff_t offset = 0);
Address outgoingFrameSlot(CallFrameSlot, ptrdiff_t offset = 0);
Address incomingFrameSlot(CallFrameSlot, ptrdiff_t offset = 0);
// The frame of emitHostCallFrame is set up. Ends the stub: returns the result, or unwinds.
void callNativeFunction(StubAssembler&, CodeSpecializationKind, GPRReg function);
void callNativeFunction(StubAssembler&, CodeSpecializationKind, Address function, GPRReg scratch);
void callNativeFunctionAndReturn(StubAssembler&, CodeSpecializationKind, GPRReg scratch, Address realm, Address function);

// AOTStubHelpersFunctions.cpp
enum class StackIsTested : bool { No, Yes };
// Bits 0 to 31 of word are the offset of a ClosureWord. The bits above are ignored.
void toCodeAddress(StubAssembler&, GPRReg word, GPRReg scratch, StackIsTested);
// Crashes the build unless these are the operand registers of target, in order. For a generator that jumps to target.
void checkStubOperands(StubKey target, std::span<const GPRReg>);
inline void checkStubOperands(StubKey target, std::initializer_list<GPRReg> registers) { checkStubOperands(target, std::span<const GPRReg>(registers.begin(), registers.end())); }

inline Address softStackLimit()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
