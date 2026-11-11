/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationData.h"
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

    static constexpr uint64_t infinityDoubled = 0xffe0000000000000ull;

    static PureDoubleBits resultOfArithmeticOnBoxedNumbers(GPRReg bits) { return PureDoubleBits(bits); }
    static PureDoubleBits branchIfNotFinite(StubAssembler&, GPRReg bits, GPRReg scratch, JumpList& isNotFinite);
    static PureDoubleBits branchIfNaN(StubAssembler&, GPRReg bits, GPRReg scratch, GPRReg holdsInfinityDoubled, Jump& isNaN);

private:
    explicit PureDoubleBits(GPRReg gpr)
        : m_gpr(gpr)
    {
    }

    GPRReg m_gpr;
};

void boxDouble(StubAssembler&, PureDouble, GPRReg result);
void boxDoubleBits(StubAssembler&, PureDoubleBits bitsAndResult);
void boxNaN(StubAssembler&, FPRReg nan, GPRReg result);
void boxInt32(StubAssembler&, GPRReg int32, GPRReg result);
void toDouble(StubAssembler&, GPRReg boxedNumber, GPRReg scratch, FPRReg result);
void branchIfNotExactInt32(StubAssembler&, FPRReg number, GPRReg integer, JumpList& isNotExact, FPRReg scratch);
void boxExactInt32(StubAssembler&, GPRReg integer, GPRReg result);
void returnBoxedNumber(StubAssembler&, PureDouble, GPRReg integer, FPRReg scratch, GPRReg result);
void returnBoxOfDoubleElement(StubAssembler&, BaseIndex element, FPRReg number, GPRReg bits, GPRReg integer, FPRReg scratch, JumpList& isHole, GPRReg result);
void decodeCell32(StubAssembler&, GPRReg word, GPRReg result);
void loadValueAtLocationOfGlobalBinding(StubAssembler&, bool seesScriptBindings, GPRReg record, GPRReg global, GPRReg result, JumpList& cannot);
void encodeCell32(StubAssembler&, GPRReg cell, GPRReg word);
void encodeCellOfProgram(StubAssembler&, GPRReg cell, GPRReg word);
void appendGranule(StubAssembler&, GPRReg extension, GPRReg granuleBits, GPRReg scratch, GPRReg secondScratch, JumpList& cannotAppend);
void insertGranule(StubAssembler&, GPRReg extension, GPRReg granuleBit, GPRReg place, GPRReg last, JumpList& cannotInsert);
void decodeExtension(StubAssembler&, GPRReg word, GPRReg result);
void decodeStructureID(CCallHelpers&, GPRReg idAndResult);
void loadStructure(StubAssembler&, GPRReg cell, GPRReg result);
void extractBits(StubAssembler&, GPRReg source, unsigned shift, unsigned width, GPRReg result);
void loadShapeWord(StubAssembler&, GPRReg cell, GPRReg result);

inline Jump branchTestBitAt(CCallHelpers& jit, CCallHelpers::ResultCondition condition, GPRReg word, GPRReg index)
{
#if CPU(X86_64)
    return jit.branchTestBit64(condition, word, index);
#else
    jit.urshift64(index, word);
    return jit.branchTest64(condition, word, CCallHelpers::TrustedImm32(1));
#endif
}

void loadShapeWordAsIndex(StubAssembler&, GPRReg cell, GPRReg result);
int32_t offsetOfRowsByShapeIndex(StubAssembler&);
bool loadsShapeAsHalfword(StubAssembler&);
void loadShapeWordWithMark(StubAssembler&, GPRReg cell, GPRReg result);
void setMarkOfCell(StubAssembler&, GPRReg cell, GPRReg scratch);
void setFlagThatFieldHasLeft(StubAssembler&, GPRReg cell, GPRReg scratch);
Jump branchIfCellBearsMark(StubAssembler&, GPRReg cell);
Jump branchUnlessCellBearsMark(StubAssembler&, GPRReg cell);
JumpList branchIfNotCompiledObjectOfThisImage(StubAssembler&, GPRReg cell);
inline JumpList branchIfNotCompiledObject(StubAssembler& jit, GPRReg cell) { return branchIfNotCompiledObjectOfThisImage(jit, cell); }
void loadCellVM(StubAssembler&, GPRReg cellAndResult);
Jump branchIfNotOfThisRealm(StubAssembler&, GPRReg structure);
void branchIfHasNamesOrIsNotOfThisRealm(StubAssembler&, GPRReg structure, JumpList&);
Jump branchIfNotPristine(StubAssembler&, GPRReg cell);
void loadPristineArrayKind(StubAssembler&, GPRReg array, GPRReg kind, unsigned mask);
Jump branchIfWatchpointSetIsNotWatched(StubAssembler&, Address set, GPRReg scratch);
// Falls through exactly when isFunctionType(cell->type()). A jump proves nothing: a callable Proxy or a host object needs C++. Preserves cell.
Jump branchIfNotFunctionType(CCallHelpers&, GPRReg cell, GPRReg scratch);
void branchUnlessAccessorPair(CCallHelpers&, GPRReg word, JumpList& isNoPair);

void allocateCell(StubAssembler&, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath);
[[nodiscard]] AssemblyHelpers::FreeListRefill allocateCellWithLateRefill(StubAssembler&, GPRReg result, GPRReg allocator, GPRReg scratch, JumpList& slowPath, std::optional<unsigned> cellSize = std::nullopt);
void emitLateRefill(StubAssembler&, const AssemblyHelpers::FreeListRefill&, JumpList& slowPath);
void fenceAfterAllocation(StubAssembler&);

constexpr unsigned maxGranulesOfExtensionFromStub = 13;
void loadUnitsOfExtensionWith(StubAssembler&, GPRReg granuleBits, GPRReg units, GPRReg scratch);

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
[[nodiscard]] AssemblyHelpers::FreeListRefill createOutOfLineStorage(StubAssembler&, const StorageGrowth&, JumpList& slowPath);
struct StorageGrowthWithElements {
    GPRReg object;
    GPRReg newStructureID;
    GPRReg butterfly;
    GPRReg newCapacity;
    GPRReg storage;
    GPRReg allocator;
    GPRReg scratch;
    GPRReg addedValue;
};
void growStorageWithElements(StubAssembler&, const StorageGrowthWithElements&, JumpList& slowPath);
struct VectorGrowth {
    GPRReg array;
    GPRReg butterfly;
    GPRReg length;
    GPRReg storage;
    GPRReg allocator;
    GPRReg scratch;
    bool isOfExtraProperties { false };
};
void growFullVector(StubAssembler&, const VectorGrowth&, JumpList& slowPath);
void widenStorageOfIntegersForAnyValue(StubAssembler&, GPRReg array, GPRReg scratch, JumpList& isLeftToOthers);
void growVectorFor(StubAssembler&, const VectorGrowth&, GPRReg neededLength, JumpList& slowPath);

void loadTextOfIndexForTrap(StubAssembler&, GPRReg base, GPRReg key, GPRReg storage, GPRReg index, GPRReg text, JumpList& hasNoText);
void loadKeptTextOfIndex(StubAssembler&, GPRReg key, GPRReg storage, GPRReg index, GPRReg text, JumpList& hasNoText);

Jump branchIfBarrierIsNeeded(StubAssembler&, GPRReg owner, GPRReg scratch);
Label returnAfterStoring(StubAssembler&, GPRReg owner, GPRReg value);

struct HostCallFrame {
    GPRReg callee;
    GPRReg thisValue;
    std::variant<unsigned, GPRReg> argumentCount;
    std::optional<GPRReg> arguments;
};
void emitHostCallFrame(StubAssembler&, const HostCallFrame&);
void emitHostCallFrameOfCallWithCount(StubAssembler&, GPRReg callee, GPRReg thisValue, GPRReg count, JumpList& doesNotFitOnStack);
constexpr unsigned hostCallFrameSize(unsigned argumentCount)
{
    return static_cast<unsigned>(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((CallFrame::headerSizeInRegisters + 1 + argumentCount) * sizeof(Register)));
}
Address hostCallFrameSlot(CallFrameSlot, ptrdiff_t offset = 0);
Address outgoingFrameSlot(CallFrameSlot, ptrdiff_t offset = 0);
Address incomingFrameSlot(CallFrameSlot, ptrdiff_t offset = 0);
void callNativeFunction(StubAssembler&, CodeSpecializationKind, Address function, GPRReg scratch);
void callNativeFunctionAndReturn(StubAssembler&, CodeSpecializationKind, GPRReg scratch, Address realm, Address function, std::optional<HostResult> = std::nullopt);

enum class StackIsTested : bool { No, Yes };
void toCodeAddress(StubAssembler&, GPRReg word, GPRReg scratch, StackIsTested);
void checkStubOperands(StubKey target, std::span<const GPRReg>);
inline void checkStubOperands(StubKey target, std::initializer_list<GPRReg> registers) { checkStubOperands(target, std::span<const GPRReg>(registers.begin(), registers.end())); }

inline Address softStackLimit()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit());
}

inline Address trapAwareSoftStackLimit()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
