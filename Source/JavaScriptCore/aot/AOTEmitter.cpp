/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEmitter.h"

#if ENABLE(AOT)

#include "B3ArgumentRegValue.h"
#include "B3Generate.h"
#include "B3InsertionSetInlines.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGenerator.h"
#include "JSLexicalEnvironment.h"
#include "JSPromise.h"
#include "StructureRareData.h"
#include "TypeInfoBlob.h"

namespace JSC { namespace AOT {

using namespace B3;

void Emitter::findPinnedRegisters()
{
    m_instance = registerOnEntry(instanceGPR);
    m_numberTag = registerOnEntry(numberTagArgumentGPR);
    m_notCellMask = registerOnEntry(GPRInfo::notCellMaskRegister);
    m_cellCageBase = registerOnEntry(cellCageBaseGPR);
}

void Emitter::branchUnless(LValue condition, LBasicBlock otherwise)
{
    LBasicBlock next = m_out.newBlock();
    m_out.branch(condition, usually(next), rarely(otherwise));
    m_out.appendTo(next);
}

static LValue notControlDependent(LValue load)
{
    load->as<B3::MemoryValue>()->setControlDependent(false);
    return load;
}

LValue Emitter::loadImmutablePointer(ptrdiff_t offset)
{
    return notControlDependent(m_out.loadPtr(m_out.address(m_heaps.AOTInstance_fixedFields, m_instance, offset)));
}

LValue Emitter::fixed32(ptrdiff_t offset)
{
    return notControlDependent(m_out.load32(m_out.address(m_heaps.AOTInstance_fixedFields, m_instance, offset)));
}

LValue Emitter::changing32(ptrdiff_t offset)
{
    return notControlDependent(m_out.load32(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, offset)));
}

LValue Emitter::fixed64(ptrdiff_t offset)
{
    return notControlDependent(m_out.load64(m_out.address(m_heaps.AOTInstance_fixedFields, m_instance, offset)));
}

LValue Emitter::loadMutable64(ptrdiff_t offset)
{
    return notControlDependent(m_out.load64(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, offset)));
}

LValue Emitter::decodeStructureID(LValue structureID)
{
    return m_out.bitOr(m_out.zeroExtPtr(structureID), loadImmutablePointer(Instance::offsetOfStructureIDBase()));
}

LValue Emitter::allocateHeapCell(LValue allocator, LBasicBlock slowPath)
{
    branchUnless(m_out.notNull(allocator), slowPath);
    return allocateHeapCell(allocator, std::nullopt, slowPath);
}

LValue Emitter::allocateHeapCell(LValue allocator, std::optional<size_t> cellSize, LBasicBlock slowPath)
{
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* allocation = m_out.patchpoint(pointerType());
    allocation->effects.terminal = true;
    allocation->append(ConstrainedValue(allocator, ValueRep::SomeRegister));
    allocation->numGPScratchRegisters = 1;
    allocation->resultConstraints = { ValueRep::SomeEarlyRegister };
    allocation->clobber(RegisterSet::macroClobberedGPRs());
    m_out.appendSuccessor(usually(continuation));
    m_out.appendSuccessor(rarely(slowPath));
    allocation->setGenerator([cellSize, refillsIntervalOnSlowPath = m_refillsIntervalOnSlowPath](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg result = params[0].gpr();
        GPRReg allocator = params[1].gpr();
        GPRReg scratch = params.gpScratch(0);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        if (Options::forceGCSlowPaths()) {
            CCallHelpers::Jump toSlowPath = jit.jump();
            params.addLatePath([=](CCallHelpers& jit) {
                toSlowPath.linkTo(*labels[1], &jit);
            });
            return;
        }
        constexpr int32_t intervalStart = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart();
        constexpr int32_t intervalEnd = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd();
        constexpr int32_t nextInterval = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval();
#if CPU(ARM64)
        static_assert(intervalEnd - intervalStart == sizeof(uintptr_t));
        static_assert(nextInterval - intervalEnd == sizeof(uintptr_t));
        static_assert(FreeList::offsetOfSecret() - FreeList::offsetOfNextInterval() == sizeof(uintptr_t));
        jit.loadPairPtr(allocator, CCallHelpers::TrustedImm32(intervalStart), result, scratch);
        CCallHelpers::Jump intervalIsUsedUp = jit.branchPtr(CCallHelpers::AboveOrEqual, result, scratch);
        CCallHelpers::Label bump = jit.label();
        if (cellSize)
            jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(*cellSize)), result, scratch);
        else {
            jit.load32(CCallHelpers::Address(allocator, LocalAllocator::offsetOfCellSize()), scratch);
            jit.addPtr(result, scratch);
        }
        jit.storePtr(scratch, CCallHelpers::Address(allocator, intervalStart));
#else
        constexpr int32_t secret = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret();
        jit.loadPtr(CCallHelpers::Address(allocator, intervalStart), result);
        CCallHelpers::Jump intervalIsUsedUp = jit.branchPtr(CCallHelpers::AboveOrEqual, result, CCallHelpers::Address(allocator, intervalEnd));
        CCallHelpers::Label bump = jit.label();
        if (cellSize)
            jit.add64(CCallHelpers::TrustedImm32(static_cast<int32_t>(*cellSize)), CCallHelpers::Address(allocator, intervalStart));
        else {
            jit.load32(CCallHelpers::Address(allocator, LocalAllocator::offsetOfCellSize()), scratch);
            jit.add64(scratch, CCallHelpers::Address(allocator, intervalStart));
        }
#endif
        CCallHelpers::Jump done;
        if (!params.fallsThroughToSuccessor(0))
            done = jit.jump();
        params.addLatePath([=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            if (refillsIntervalOnSlowPath) {
                intervalIsUsedUp.linkTo(*labels[1], &jit);
                if (done.isSet())
                    done.linkTo(*labels[0], &jit);
                return;
            }
            intervalIsUsedUp.link(&jit);
#if CPU(ARM64)
            GPRReg temporary = jit.scratchRegister();
            jit.loadPairPtr(allocator, CCallHelpers::TrustedImm32(nextInterval), result, scratch);
            CCallHelpers::Jump thereIsNone = jit.branchTestPtr(CCallHelpers::NonZero, result, CCallHelpers::TrustedImm32(1));
            jit.xor64(CCallHelpers::Address(result, FreeCell::offsetOfScrambledBits()), scratch);
            jit.addSignExtend64(result, scratch, temporary);
            jit.addUnsignedRightShift64(result, scratch, CCallHelpers::TrustedImm32(32), scratch);
            jit.storePairPtr(scratch, temporary, allocator, CCallHelpers::TrustedImm32(intervalEnd));
#else
            jit.loadPtr(CCallHelpers::Address(allocator, nextInterval), result);
            CCallHelpers::Jump thereIsNone = jit.branchTestPtr(CCallHelpers::NonZero, result, CCallHelpers::TrustedImm32(1));
            jit.load32(CCallHelpers::Address(allocator, secret), scratch);
            jit.xor32(CCallHelpers::Address(result, FreeCell::offsetOfScrambledBits()), scratch);
            jit.add64(scratch, CCallHelpers::Address(allocator, nextInterval));
            jit.load32(CCallHelpers::Address(allocator, secret + 4), scratch);
            jit.xor32(CCallHelpers::Address(result, FreeCell::offsetOfScrambledBits() + 4), scratch);
            jit.storePtr(result, CCallHelpers::Address(allocator, intervalStart));
            jit.addPtr(result, scratch);
            jit.storePtr(scratch, CCallHelpers::Address(allocator, intervalEnd));
#endif
            jit.jump().linkTo(bump, &jit);
            thereIsNone.linkTo(*labels[1], &jit);
            if (done.isSet())
                done.linkTo(*labels[0], &jit);
        });
    });
    m_out.appendTo(continuation);
    return allocation;
}

LValue Emitter::allocateAdjacentHeapCells(LValue allocator, size_t cellSize, unsigned count, LBasicBlock slowPath)
{
    branchUnless(m_out.notNull(allocator), slowPath);
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* allocation = m_out.patchpoint(pointerType());
    allocation->effects.terminal = true;
    allocation->append(ConstrainedValue(allocator, ValueRep::SomeRegister));
    allocation->numGPScratchRegisters = 1;
    allocation->resultConstraints = { ValueRep::SomeEarlyRegister };
    allocation->clobber(RegisterSet::macroClobberedGPRs());
    m_out.appendSuccessor(usually(continuation));
    m_out.appendSuccessor(rarely(slowPath));
    allocation->setGenerator([size = static_cast<int32_t>(cellSize * count)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg result = params[0].gpr();
        GPRReg allocator = params[1].gpr();
        GPRReg scratch = params.gpScratch(0);
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        constexpr int32_t intervalStart = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart();
        constexpr int32_t intervalEnd = LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd();
        CCallHelpers::Jump toSlowPath;
        if (Options::forceGCSlowPaths())
            toSlowPath = jit.jump();
        else {
            jit.loadPtr(CCallHelpers::Address(allocator, intervalStart), result);
            jit.addPtr(CCallHelpers::TrustedImm32(size), result, scratch);
            toSlowPath = jit.branchPtr(CCallHelpers::Above, scratch, CCallHelpers::Address(allocator, intervalEnd));
            jit.storePtr(scratch, CCallHelpers::Address(allocator, intervalStart));
        }
        CCallHelpers::Jump done;
        if (!Options::forceGCSlowPaths() && !params.fallsThroughToSuccessor(0))
            done = jit.jump();
        params.addLatePath([=](CCallHelpers& jit) {
            toSlowPath.linkTo(*labels[1], &jit);
            if (done.isSet())
                done.linkTo(*labels[0], &jit);
        });
    });
    m_out.appendTo(continuation);
    return allocation;
}

LValue Emitter::allocatorForSize(LValue subspace, LValue size, LBasicBlock slowPath)
{
    static_assert(!(MarkedSpace::sizeStep & (MarkedSpace::sizeStep - 1)), "MarkedSpace::sizeStep must be a power of two.");
    unsigned stepShift = getLSBSet(MarkedSpace::sizeStep);
    LValue sizeClassIndex = m_out.lShr(m_out.add(size, m_out.constIntPtr(MarkedSpace::sizeStep - 1)), m_out.constInt32(stepShift));
    branchUnless(m_out.belowOrEqual(sizeClassIndex, m_out.constIntPtr(MarkedSpace::largeCutoff >> stepShift)), slowPath);
    return m_out.loadPtr(m_out.baseIndex(m_heaps.CompleteSubspace_allocatorForSizeStep, subspace, sizeClassIndex));
}

LValue Emitter::allocatorForSize(LValue subspace, size_t size)
{
    RELEASE_ASSERT(size <= MarkedSpace::largeCutoff);
    return m_out.loadPtr(subspace, m_heaps.CompleteSubspace_allocatorForSizeStep[MarkedSpace::sizeClassToIndex(size)]);
}

void Emitter::storeHeader(LValue cell, LValue header)
{
    static_assert(!JSCell::structureIDOffset());
    m_out.store64(header, cell, m_heaps.JSCell_header);
}

void Emitter::splatWords(LValue base, LValue begin, LValue end, LValue value, const AbstractHeap& heap)
{
    LBasicBlock initLoop = m_out.newBlock();
    LBasicBlock initDone = m_out.newBlock();
    ValueFromBlock originalIndex = m_out.anchor(end);
    ValueFromBlock originalPointer = m_out.anchor(m_out.add(base, m_out.shl(m_out.signExt32ToPtr(begin), m_out.constInt32(3))));
    m_out.branch(m_out.notEqual(end, begin), unsure(initLoop), unsure(initDone));

    m_out.appendTo(initLoop, initDone);
    LValue index = m_out.phi(Int32, originalIndex);
    LValue pointer = m_out.phi(pointerType(), originalPointer);
    m_out.store64(value, TypedPointer(heap, pointer));
    LValue nextIndex = m_out.sub(index, m_out.int32One);
    m_out.addIncomingToPhi(index, m_out.anchor(nextIndex));
    m_out.addIncomingToPhi(pointer, m_out.anchor(m_out.add(pointer, m_out.intPtrEight)));
    m_out.branch(m_out.notEqual(nextIndex, begin), unsure(initLoop), unsure(initDone));

    m_out.appendTo(initDone);
}

void Emitter::mutatorFence()
{
#if CPU(X86_64)
    m_out.fence(&m_heaps.root, nullptr);
#else
    LBasicBlock fence = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.load8ZeroExt32(vm(), m_heaps.VM_heap_mutatorShouldBeFenced), rarely(fence), usually(done));
    m_out.appendTo(fence, done);
    m_out.fence(&m_heaps.root, nullptr);
    m_out.jump(done);
    m_out.appendTo(done);
#endif
}

uint32_t Emitter::arrayTypeInfoBlob(IndexingType indexingType)
{
    return TypeInfoBlob(indexingType, TypeInfo(ArrayType, JSArray::StructureFlags)).blob();
}

LValue Emitter::allocateObject(Instance::InlineAllocation kind, LBasicBlock giveUp)
{
    LValue result = allocateHeapCell(m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(kind))), giveUp);
    m_out.store64(m_out.load64(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfHeaderFor(kind))), result, m_heaps.JSCell_header);
    m_out.storePtr(m_out.intPtrZero, result, m_heaps.JSObject_butterfly);
    return result;
}

LValue Emitter::newPromise(LBasicBlock giveUp)
{
    LValue result = allocateObject(Instance::InlineAllocation::Promise, giveUp);
    m_out.store64(m_out.int64Zero, result, m_heaps.JSPromise_packed);
    m_out.store64(m_out.constInt64(JSValue::encode(JSValue())), result, m_heaps.JSPromise_slot);
    mutatorFence();
    return result;
}

LValue Emitter::isOriginalArray(LValue cell)
{
    LValue kind = m_out.bitAnd(m_out.lShr(m_out.load8ZeroExt32(cell, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(Instance::arrayKindShift)), m_out.constInt32(Instance::numberOfArrayKinds - 1));
    LValue expected = m_out.load32(TypedPointer(m_heaps.AOTInstance_fixedFields,
        m_out.add(m_instance, m_out.add(m_out.shl(m_out.zeroExtPtr(kind), m_out.constInt32(2)), m_out.constIntPtr(Instance::offsetOfOriginalArrayStructureIDs())))));
    return m_out.equal(m_out.load32(cell, m_heaps.JSCell_structureID), expected);
}

LValue Emitter::singleCharacterString(LValue character)
{
    return m_out.loadPtr(m_out.baseIndex(m_heaps.singleCharacterStrings, loadImmutablePointer(Instance::offsetOfSingleCharacterStrings()), m_out.zeroExtPtr(character)));
}

LValue Emitter::makeRope(LValue first, LValue second, LValue third, LBasicBlock giveUp)
{
    struct FlagsAndLength {
        LValue flags;
        LValue length;
    };
    auto getFlagsAndLength = [&](LValue child) {
        LBasicBlock continuation = m_out.newBlock();
        LBasicBlock ropeCase = m_out.newBlock();
        LBasicBlock notRopeCase = m_out.newBlock();
        m_out.branch(isRopeString(child), unsure(ropeCase), unsure(notRopeCase));

        m_out.appendTo(ropeCase);
        ValueFromBlock flagsForRope = m_out.anchor(m_out.load32NonNegative(child, m_heaps.JSRopeString_flags));
        ValueFromBlock lengthForRope = m_out.anchor(m_out.load32NonNegative(child, m_heaps.JSRopeString_length));
        m_out.jump(continuation);

        m_out.appendTo(notRopeCase);
        LValue stringImpl = m_out.loadPtr(child, m_heaps.JSString_value);
        ValueFromBlock flagsForNonRope = m_out.anchor(m_out.load32NonNegative(stringImpl, m_heaps.StringImpl_hashAndFlags));
        ValueFromBlock lengthForNonRope = m_out.anchor(m_out.load32NonNegative(stringImpl, m_heaps.StringImpl_length));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        return FlagsAndLength { m_out.phi(Int32, flagsForRope, flagsForNonRope), m_out.phi(Int32, lengthForRope, lengthForNonRope) };
    };

    FlagsAndLength firstInfo = getFlagsAndLength(first);
    FlagsAndLength secondInfo = getFlagsAndLength(second);
    LValue flags = m_out.bitAnd(firstInfo.flags, secondInfo.flags);
    LValue utf16Length = m_out.add(m_out.zeroExtPtr(firstInfo.length), m_out.zeroExtPtr(secondInfo.length));
    if (third) {
        FlagsAndLength thirdInfo = getFlagsAndLength(third);
        flags = m_out.bitAnd(flags, thirdInfo.flags);
        utf16Length = m_out.add(utf16Length, m_out.zeroExtPtr(thirdInfo.length));
    }
    branchUnless(m_out.belowOrEqual(utf16Length, m_out.constIntPtr(JSString::MaxLength)), giveUp);
    LValue length = m_out.castToInt32(utf16Length);

    LBasicBlock make = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;
    if (!third) {
        LBasicBlock firstNonEmptyCase = m_out.newBlock();
        results.append(m_out.anchor(second));
        m_out.branch(m_out.isZero32(firstInfo.length), rarely(continuation), usually(firstNonEmptyCase));
        m_out.appendTo(firstNonEmptyCase);
        results.append(m_out.anchor(first));
        m_out.branch(m_out.isZero32(secondInfo.length), rarely(continuation), usually(make));
    } else {
        results.append(m_out.anchor(emptyString()));
        m_out.branch(m_out.isZero32(length), rarely(continuation), usually(make));
    }

    m_out.appendTo(make);
    LValue result = allocateHeapCell(loadImmutablePointer(Instance::offsetOfRopeStringAllocator()), sizeof(JSRopeString), giveUp);
    storeHeader(result, fixed64(Instance::offsetOfStringHeader()));
    m_out.storePtr(m_out.bitOr(m_out.bitOr(first, m_out.constIntPtr(JSString::isRopeInPointer)), m_out.bitAnd(m_out.constIntPtr(JSRopeString::is8BitInPointer), m_out.zeroExtPtr(flags))), result, m_heaps.JSRopeString_fiber0);
    m_out.storePtr(m_out.bitOr(m_out.zeroExtPtr(length), m_out.shl(second, m_out.constInt32(32))), result, m_heaps.JSRopeString_fiber1);
    if (!third)
        m_out.storePtr(m_out.lShr(second, m_out.constInt32(32)), result, m_heaps.JSRopeString_fiber2);
    else
        m_out.storePtr(m_out.bitOr(m_out.lShr(second, m_out.constInt32(32)), m_out.shl(third, m_out.constInt32(16))), result, m_heaps.JSRopeString_fiber2);
    mutatorFence();
    results.append(m_out.anchor(result));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), results);
}

LValue Emitter::int32ToString(LValue value, LBasicBlock giveUp)
{
    LBasicBlock smallIntCase = m_out.newBlock();
    LBasicBlock intCacheCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.below(value, m_out.constInt32(NumericStrings::cacheSize)), unsure(smallIntCase), unsure(intCacheCase));

    m_out.appendTo(smallIntCase);
    LValue smallIntString = m_out.loadPtr(m_out.baseIndex(m_heaps.SmallIntCache, loadImmutablePointer(Instance::offsetOfSmallIntStrings()), m_out.zeroExtPtr(value), JSValue(), NumericStrings::StringWithJSString::offsetOfJSString()));
    ValueFromBlock smallIntResult = m_out.anchor(smallIntString);
    m_out.branch(m_out.isNull(smallIntString), unsure(giveUp), unsure(continuation));

    m_out.appendTo(intCacheCase);
    LValue wide = m_out.zeroExt(value, Int64);
    LValue first = m_out.bitXor(wide, fixed64(Instance::offsetOfHashSecrets()));
    LValue second = m_out.bitXor(wide, fixed64(Instance::offsetOfHashSecrets() + sizeof(uint64_t)));
    LValue hash = m_out.castToInt32(m_out.bitXor(m_out.mul(first, second), m_out.uMulHigh(first, second)));
    LValue index = m_out.zeroExtPtr(m_out.bitAnd(hash, m_out.constInt32(NumericStrings::cacheSize - 1)));
    LValue cache = loadImmutablePointer(Instance::offsetOfIntStrings());
    LValue key = m_out.load32(m_out.baseIndex(m_heaps.IntCache, cache, index, JSValue(), NumericStrings::CacheEntryWithJSString<int>::offsetOfKey()));
    LValue cachedString = m_out.loadPtr(m_out.baseIndex(m_heaps.IntCache, cache, index, JSValue(), NumericStrings::CacheEntryWithJSString<int>::offsetOfJSString()));
    branchUnless(m_out.equal(key, value), giveUp);
    ValueFromBlock intCacheResult = m_out.anchor(cachedString);
    m_out.branch(m_out.isNull(cachedString), unsure(giveUp), unsure(continuation));

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), smallIntResult, intCacheResult);
}

LValue Emitter::stringOrInt32ToString(LValue value, LBasicBlock giveUp)
{
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock notCellCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isCell(value), usually(cellCase), rarely(notCellCase));

    m_out.appendTo(cellCase);
    ValueFromBlock string = m_out.anchor(value);
    m_out.branch(isCellOfType(value, StringType), usually(continuation), rarely(giveUp));

    m_out.appendTo(notCellCase);
    branchUnless(isInt32(value), giveUp);
    ValueFromBlock converted = m_out.anchor(int32ToString(unboxInt32(value), giveUp));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(pointerType(), string, converted);
}

LValue Emitter::addStringsOrInt32s(LValue first, LValue second, LBasicBlock giveUp)
{
    branchUnless(m_out.bitOr(isCell(first), isCell(second)), giveUp);
    LValue firstString = stringOrInt32ToString(first, giveUp);
    return makeRope(firstString, stringOrInt32ToString(second, giveUp), nullptr, giveUp);
}

namespace {

class Helpers final : public Emitter {
public:
    explicit Helpers(Procedure& proc)
        : Emitter(proc)
    {
    }

    void build(Stub stub)
    {
        m_out.initialize(m_heaps);
        m_out.setFrequency(1);
        LBasicBlock start = m_out.newBlock();
        LBasicBlock giveUp = m_out.newBlock();
        m_out.appendTo(start);
        m_out.initializeConstants(m_proc, start);
        findPinnedRegisters();
        LValue arguments[4];
        for (unsigned i = 0; i < std::size(arguments); ++i)
            arguments[i] = registerOnEntry(GPRInfo::toArgumentRegister(i));

        LValue result = nullptr;
        switch (stub) {
        case Stub::HelperAddStrings:
            result = addStringsOrInt32s(arguments[0], arguments[1], giveUp);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        m_out.ret(result);
        m_out.appendTo(giveUp);
        m_out.ret(m_out.intPtrZero);
    }
};

} // anonymous namespace

void pinRegisters(Procedure& proc)
{
    proc.pinRegister(instanceGPR);
#if !CPU(X86_64)
    proc.pinRegister(GPRInfo::numberTagRegister);
    proc.pinRegister(GPRInfo::notCellMaskRegister);
#endif
#if CPU(X86_64)
    proc.pinRegister(cellCageBaseGPR);
#endif
#if CPU(ARM64)
    proc.pinRegister(cellCageBaseGPR);
    if (proc.mutableGPRs().contains(ARM64Registers::x18, IgnoreVectors))
        proc.pinRegister(ARM64Registers::x18);
#endif
}

void readTagsFromInstance(CCallHelpers& jit)
{
#if CPU(X86_64)
    jit.readTagsFromMemory(CCallHelpers::Address(instanceGPR, Instance::offsetOfNumberTag()), CCallHelpers::Address(instanceGPR, Instance::offsetOfNotCellMask()));
#else
    UNUSED_PARAM(jit);
#endif
}

void readTagsFromInstance(Procedure& proc)
{
#if CPU(X86_64)
    B3::Value* instance = nullptr;
    auto offsetOfTag = [](B3::Value* value) -> std::optional<ptrdiff_t> {
        if (value->opcode() != B3::ArgumentReg)
            return std::nullopt;
        Reg reg = value->as<ArgumentRegValue>()->argumentReg();
        if (reg == numberTagArgumentGPR)
            return Instance::offsetOfNumberTag();
        if (reg == GPRInfo::notCellMaskRegister)
            return Instance::offsetOfNotCellMask();
        return std::nullopt;
    };
    Vector<B3::Value*, 2> tags;
    for (B3::Value* value : proc.values()) {
        if (offsetOfTag(value))
            tags.append(value);
    }
    if (tags.isEmpty())
        return;
    for (B3::Value* value : *proc.at(0)) {
        if (value->opcode() == B3::ArgumentReg && value->as<ArgumentRegValue>()->argumentReg() == instanceGPR)
            instance = value;
    }
    if (!instance) {
        instance = proc.add<ArgumentRegValue>(Origin(), instanceGPR);
        proc.at(0)->values().insert(0, instance);
    }
    InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* value = block->at(index);
            if (value->opcode() == B3::Add && value->child(1)->hasInt64() && offsetOfTag(value->child(0)) == Instance::offsetOfNumberTag()) {
                value->replaceWithIdentity(insertionSet.insert<Const64Value>(index, value->origin(), static_cast<int64_t>(static_cast<uint64_t>(value->child(1)->asInt64()) + static_cast<uint64_t>(JSValue::NumberTag))));
                continue;
            }
            for (B3::Value*& child : value->children()) {
                auto offset = offsetOfTag(child);
                if (!offset)
                    continue;
                MemoryValue* load = insertionSet.insert<MemoryValue>(index, B3::Load, Int64, value->origin(), instance, static_cast<int32_t>(*offset));
                load->setControlDependent(false);
                load->setReadsMutability(B3::Mutability::Immutable);
                child = load;
            }
        }
        insertionSet.execute(block);
    }
    for (B3::Value* tag : tags)
        tag->replaceWithNopIgnoringType();
#else
    UNUSED_PARAM(proc);
#endif
}

void generateHelper(CCallHelpers& jit, Stub stub)
{
    Procedure proc(/* usesSIMD = */ false);
    proc.setPositionIndependent();
    pinRegisters(proc);
    proc.setLastPhase(createSharedTask<void(Procedure&)>([](Procedure& proc) {
        readTagsFromInstance(proc);
    }));
    Helpers helpers(proc);
    helpers.build(stub);
    for (Value* value : proc.values()) {
        if (value->hasInt64())
            RELEASE_ASSERT_WITH_MESSAGE(static_cast<uint64_t>(value->asInt64()) < 4 * GB || static_cast<uint64_t>(value->asInt64()) >= (1ULL << 47), "An address in the code of a helper");
    }
    prepareForGeneration(proc);
    jit.setOopsEmitsBreakpointOnly();
    generate(proc, jit);
}

LValue Emitter::registerOnEntry(Reg reg)
{
    return m_out.m_block->appendNew<ArgumentRegValue>(m_proc, Origin(), reg);
}

LValue Emitter::loadStructure(LValue cell)
{
    return decodeStructureID(m_out.load32(cell, m_heaps.JSCell_structureID));
}

LValue Emitter::numberToDouble(LValue value)
{
    return m_out.select(isInt32(value), m_out.intToDouble(unboxInt32(value)), unboxDouble(value));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
