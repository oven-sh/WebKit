/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCodeUnits.h"
#include "AOTCompiledCode.h"
#include "AOTImageTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTVector.h"
#include "AirBasicBlock.h"
#include "AirCode.h"
#include "AirInst.h"
#include "AirInstInlines.h"
#include "B3Const64Value.h"
#include "B3MemoryValue.h"
#include "B3Procedure.h"
#include "B3ProcedureInlines.h"
#include "CCallHelpers.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <bit>
#include <initializer_list>
#include <iterator>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

using Label = CCallHelpers::Label;
using Kind = CodeUnit::Kind;
using B3::Air::Arg;
using B3::Air::Inst;
using B3::Air::Tmp;
using ImageTests::changeUnits;

struct LinkedCode {
    Vector<uint8_t> bytes;
    Vector<CodeUnit> units;
    Vector<Relocation> relocations;
    Vector<LinkedSite> localSites;
};

struct Emission {
    CCallHelpers& jit;
    RelocationRecorder& relocations;
    CodeUnitRecorder& units;
};

struct Fixture {
    CompiledCode code;
    uint32_t plain;
    uint32_t pinned;
    uint32_t call;
    uint32_t address;
    uint32_t afterBranch;
    uint32_t end;
};

struct AirBlocks {
    explicit AirBlocks(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
            blocks.append(procedure.code().addBlock());
    }

    BitVector indicesOf(std::initializer_list<unsigned> chosen) const
    {
        BitVector indices;
        for (unsigned block : chosen)
            indices.set(blocks[block]->index());
        return indices;
    }

    const B3::Air::BasicBlock& operator[](unsigned block) const { return *blocks[block]; }

    B3::Procedure procedure;
    Vector<B3::Air::BasicBlock*> blocks;
};

struct BlockLabels {
    Label move;
    Label call;
    Label last;
    Label end;
};

struct BlockOffsets {
    uint32_t move;
    uint32_t call;
    uint32_t last;
    uint32_t end;
};

} // anonymous namespace

static constexpr CodeSymbol calledFunction { SymbolSpace::Function, 0 };
static constexpr CodeSymbol namedConstant { SymbolSpace::Constant, 0 };

#if CPU(X86_64)
static constexpr GPRReg forbiddenRegister = MacroAssembler::stackPointerRegister;
#elif CPU(ARM64)
static constexpr GPRReg forbiddenRegister = ARM64Registers::lr;
#endif

template<typename Emit, typename AfterLink>
static LinkedCode emitAndLink(const Emit& emit, const AfterLink& afterLink)
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, ImageTests::testAssemblerPolicy());
    CodeUnitRecorder recorder;
    Emission emission { jit, relocations, recorder };
    emit(emission);

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::FTL);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "test"_s);
    auto linkedRelocations = relocations.link(buffer, nullptr);
    AOT_TEST_CHECK(linkedRelocations);

    LinkedCode result;
    result.relocations = WTF::move(*linkedRelocations);
    result.units = recorder.link(buffer, result.relocations.span());
    result.localSites = buffer.linkBuffer().linkedSites();
    result.bytes.append(buffer.bytes());
    afterLink(buffer.linkBuffer());
    return result;
}

template<typename Emit>
static LinkedCode emitAndLink(const Emit& emit)
{
    return emitAndLink(emit, [](LinkBuffer&) { });
}

static void emitPlain(Emission& emission, unsigned bytes)
{
    Label begin = emission.jit.label();
    emission.jit.emitNops(bytes);
    emission.units.addPlain(begin, emission.jit.label(), RegisterSet { });
}

static void emitEnd(Emission& emission)
{
    Label begin = emission.jit.label();
    emission.jit.breakpoint();
    emission.units.addEnd(begin, emission.jit.label());
}

static Label emitInstruction(Emission& emission, const Inst& inst)
{
    Label begin = emission.jit.label();
    emission.jit.nop();
    emission.units.didGenerate(inst, begin, emission.jit.label());
    return begin;
}

static Label emitPatchWithCall(Emission& emission)
{
    Label begin = emission.jit.label();
    emission.relocations.call(calledFunction);
    emission.units.didGenerate(Inst(B3::Air::Patch, nullptr), begin, emission.jit.label());
    return begin;
}

static Inst moveBetweenRegisters()
{
    return Inst(B3::Air::Move, nullptr, Tmp(GPRInfo::regT0), Tmp(GPRInfo::regT1));
}

static Inst returnInstruction()
{
    return Inst(B3::Air::Ret64, nullptr, Tmp(GPRInfo::returnValueGPR));
}

static BlockLabels emitInstructionsOfBlock(Emission& emission)
{
    BlockLabels labels;
    labels.move = emitInstruction(emission, moveBetweenRegisters());
    labels.call = emitPatchWithCall(emission);
    labels.last = emitInstruction(emission, returnInstruction());
    labels.end = emission.jit.label();
    return labels;
}

static BlockLabels emitBlock(Emission& emission, const B3::Air::BasicBlock& block)
{
    emission.units.willGenerate(block);
    return emitInstructionsOfBlock(emission);
}

static BlockOffsets offsetsOf(LinkBuffer& linkBuffer, const BlockLabels& labels)
{
    return { linkBuffer.offsetOf(labels.move), linkBuffer.offsetOf(labels.call), linkBuffer.offsetOf(labels.last), linkBuffer.offsetOf(labels.end) };
}

static Vector<uint32_t> offsetsOf(LinkBuffer& linkBuffer, const WTF::Vector<Label>& labels)
{
    return labels.map([&](Label label) {
        return linkBuffer.offsetOf(label);
    });
}

static void checkTiles(const LinkedCode& code)
{
    AOT_TEST_CHECK(!code.units.isEmpty());
    size_t offset = 0;
    for (const CodeUnit& unit : code.units) {
        AOT_TEST_CHECK(unit.offset == offset);
        AOT_TEST_CHECK(unit.length);
        offset += unit.length;
    }
    AOT_TEST_CHECK(offset == code.bytes.size());
}

static const CodeUnit& unitContaining(const Vector<CodeUnit>& units, uint32_t offset)
{
    size_t index = units.findIf([&](const CodeUnit& unit) { return unit.offset <= offset && offset - unit.offset < unit.length; });
    AOT_TEST_CHECK(index != notFound);
    return units[index];
}

static size_t indexOfUnitAt(const Vector<CodeUnit>& units, uint32_t offset)
{
    size_t index = units.findIf([&](const CodeUnit& unit) { return unit.offset == offset; });
    AOT_TEST_CHECK(index != notFound);
    return index;
}

static size_t indexOfOnly(const Vector<CodeUnit>& units, Kind kind)
{
    AOT_TEST_CHECK(std::ranges::count(units, kind, &CodeUnit::kind) == 1);
    return units.findIf([&](const CodeUnit& unit) { return unit.kind == kind; });
}

static void checkUnit(const Vector<CodeUnit>& units, uint32_t begin, uint32_t end, Kind kind)
{
    const CodeUnit& unit = units[indexOfUnitAt(units, begin)];
    AOT_TEST_CHECK(unit.kind == kind);
    AOT_TEST_CHECK(unit.offset + unit.length == end);
}

static void checkAllPinned(const Vector<CodeUnit>& units, uint32_t begin, uint32_t end)
{
    for (const CodeUnit& unit : units) {
        if (unit.offset + unit.length > begin && unit.offset < end)
            AOT_TEST_CHECK(unit.kind == Kind::Pinned);
    }
}

static void testUnrecordedBytesArePinned()
{
    for (unsigned bytes : { 16u, 70000u }) {
        LinkedCode code = emitAndLink([&](Emission& emission) {
            emission.jit.emitNops(bytes);
        });
        checkTiles(code);
        AOT_TEST_CHECK(code.bytes.size() == bytes);
        for (const CodeUnit& unit : code.units)
            AOT_TEST_CHECK(unit.kind == Kind::Pinned);
    }
}

static void testPlainAndEnd()
{
    LinkedCode code = emitAndLink([&](Emission& emission) {
        emitPlain(emission, 8);
        emitEnd(emission);
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.units.size() == 2);
    AOT_TEST_CHECK(code.units[0].kind == Kind::Plain);
    AOT_TEST_CHECK(code.units[0].length == 8);
    AOT_TEST_CHECK(code.units[1].kind == Kind::End);
}

static void testPlainNamingForbiddenRegister()
{
    struct NamedRegistersCase {
        RegisterSet named;
        Kind expected;
    };
    const NamedRegistersCase cases[] = {
        { RegisterSet { }, Kind::Plain },
        { RegisterSet { forbiddenRegister }, Kind::Pinned },
        { RegisterSet { GPRInfo::regT0 }, Kind::Plain },
        { RegisterSet { GPRInfo::regT0, forbiddenRegister }, Kind::Pinned },
#if CPU(ARM64)
        { RegisterSet { MacroAssembler::stackPointerRegister }, Kind::Plain },
#endif
    };

    WTF::Vector<Label> labels;
    Vector<uint32_t> offsets;
    LinkedCode code = emitAndLink([&](Emission& emission) {
        for (const NamedRegistersCase& testCase : cases) {
            Label begin = emission.jit.label();
            emission.jit.nop();
            emission.units.addPlain(begin, emission.jit.label(), testCase.named);
            labels.append(begin);
        }
    }, [&](LinkBuffer& linkBuffer) {
        offsets = offsetsOf(linkBuffer, labels);
    });
    checkTiles(code);
    for (size_t i = 0; i < std::size(cases); ++i)
        AOT_TEST_CHECK(unitContaining(code.units, offsets[i]).kind == cases[i].expected);
}

static void testAirInstructions()
{
    struct InstCase {
        Inst inst;
        Kind expected;
    };

    Tmp t0(GPRInfo::regT0);
    Tmp t1(GPRInfo::regT1);
    Tmp forbidden(forbiddenRegister);
    Tmp framePointer(GPRInfo::callFrameRegister);
    B3::Air::Kind moveWithEffects(B3::Air::Move);
    moveWithEffects.effects = true;

    const InstCase cases[] = {
        { Inst(B3::Air::Move, nullptr, t0, t1), Kind::Plain },
        { Inst(B3::Air::Move, nullptr, forbidden, t0), Kind::Pinned },
        { Inst(B3::Air::Move, nullptr, t1, t0), Kind::Plain },
        { Inst(B3::Air::Move, nullptr, Arg::addr(forbidden, 8), t0), Kind::Pinned },
        { Inst(B3::Air::Move, nullptr, Arg::addr(framePointer, -8), t0), Kind::Plain },
        { Inst(B3::Air::Patch, nullptr), Kind::Pinned },
        { Inst(B3::Air::Move, nullptr, t0, t1), Kind::Plain },
        { Inst(B3::Air::EnterFrame, nullptr), Kind::Pinned },
        { Inst(B3::Air::Move, nullptr, t0, t1), Kind::Plain },
        { Inst(B3::Air::LeaveFrame, nullptr), Kind::Pinned },
        { Inst(B3::Air::Move, nullptr, t0, t1), Kind::Plain },
        { Inst(moveWithEffects, nullptr, Arg::addr(t0, 0), t1), Kind::Pinned },
#if CPU(ARM64)
        { Inst(B3::Air::Move, nullptr, Arg::addr(Tmp(MacroAssembler::stackPointerRegister), 8), t0), Kind::Plain },
#endif
        { Inst(B3::Air::Ret64, nullptr, Tmp(GPRInfo::returnValueGPR)), Kind::End },
    };

    AirBlocks blocks(1);
    WTF::Vector<Label> labels;
    Vector<uint32_t> offsets;
    LinkedCode code = emitAndLink([&](Emission& emission) {
        emission.units.setColdBlocks(blocks.indicesOf({ 0 }), { });
        emission.units.willGenerate(blocks[0]);
        for (const InstCase& testCase : cases)
            labels.append(emitInstruction(emission, testCase.inst));
        Label end = emission.jit.label();
        emission.units.didGenerate(cases[0].inst, end, end);
        labels.append(end);
    }, [&](LinkBuffer& linkBuffer) {
        offsets = offsetsOf(linkBuffer, labels);
    });
    checkTiles(code);
    for (size_t i = 0; i < std::size(cases); ++i)
        checkUnit(code.units, offsets[i], offsets[i + 1], cases[i].expected);
}

static void testHotLoopInColdFunction()
{
    AirBlocks blocks(3);
    BlockLabels labels[3];
    BlockOffsets offsets[3];
    LinkedCode code = emitAndLink([&](Emission& emission) {
        emission.units.setColdBlocks(blocks.indicesOf({ 0, 2 }), blocks.indicesOf({ 2 }));
        for (unsigned block = 0; block < 3; ++block)
            labels[block] = emitBlock(emission, blocks[block]);
    }, [&](LinkBuffer& linkBuffer) {
        for (unsigned block = 0; block < 3; ++block)
            offsets[block] = offsetsOf(linkBuffer, labels[block]);
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.relocations.size() == 3);
    for (unsigned block : { 0u, 2u }) {
        checkUnit(code.units, offsets[block].move, offsets[block].call, Kind::Plain);
        checkUnit(code.units, offsets[block].call, offsets[block].last, Kind::Call);
        checkUnit(code.units, offsets[block].last, offsets[block].end, Kind::End);
    }
    checkUnit(code.units, offsets[1].move, offsets[1].end, Kind::Pinned);
    // Block 0 is cold by an estimate only, block 2 is rarely run.
    for (const CodeUnit& unit : code.units)
        AOT_TEST_CHECK(unit.mayRun == (unit.offset >= offsets[0].move && unit.offset < offsets[0].end));
}

static void testWithoutColdBlocksEverythingIsPinned()
{
    for (bool announcesBlocks : { false, true }) {
        AirBlocks blocks(2);
        LinkedCode code = emitAndLink([&](Emission& emission) {
            for (unsigned block = 0; block < 2; ++block) {
                if (announcesBlocks)
                    emitBlock(emission, blocks[block]);
                else
                    emitInstructionsOfBlock(emission);
            }
        });
        checkTiles(code);
        AOT_TEST_CHECK(code.relocations.size() == 2);
        checkAllPinned(code.units, 0, code.bytes.size());
    }
}

static void testLatePathIsPlain()
{
    AirBlocks blocks(2);
    WTF::Vector<Label> labels;
    Vector<uint32_t> offsets;
    LinkedCode code = emitAndLink([&](Emission& emission) {
        for (unsigned block = 0; block < 2; ++block) {
            labels.append(emitBlock(emission, blocks[block]).move);
            labels.append(emission.jit.label());
            emitPlain(emission, 8);
            labels.append(emission.jit.label());
            emission.jit.emitNops(8);
        }
        labels.append(emission.jit.label());
    }, [&](LinkBuffer& linkBuffer) {
        offsets = offsetsOf(linkBuffer, labels);
    });
    checkTiles(code);
    for (unsigned block = 0; block < 2; ++block) {
        std::span<const uint32_t> part = offsets.span().subspan(3 * block);
        checkAllPinned(code.units, part[0], part[1]);
        checkUnit(code.units, part[1], part[2], Kind::Plain);
        checkAllPinned(code.units, part[2], part[3]);
    }
}

static void testLinkedSiteSplitsPlainRange()
{
    Label begin;
    Label end;
    uint32_t beginOffset = 0;
    uint32_t endOffset = 0;
    LinkedCode code = emitAndLink([&](Emission& emission) {
        CCallHelpers& jit = emission.jit;
        begin = jit.label();
        jit.emitNops(4);
        CCallHelpers::Jump jump = jit.branch32(CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1);
        jit.emitNops(4);
        end = jit.label();
        emission.units.addPlain(begin, end, RegisterSet { });
        jump.link(&jit);
        emitEnd(emission);
    }, [&](LinkBuffer& linkBuffer) {
        beginOffset = linkBuffer.offsetOf(begin);
        endOffset = linkBuffer.offsetOf(end);
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.localSites.size() == 1);
    const LinkedSite& site = code.localSites[0];
    uint32_t endOfSite = site.offset + MacroAssembler::relocationSize(site.kind);
    AOT_TEST_CHECK(site.target == endOffset);
    checkUnit(code.units, beginOffset, site.offset, Kind::Plain);
    checkUnit(code.units, site.offset, endOfSite, Kind::Pinned);
    checkUnit(code.units, endOfSite, endOffset, Kind::Plain);
    AOT_TEST_CHECK(code.units[indexOfUnitAt(code.units, endOffset)].kind == Kind::End);
}

static void testTargetCutsPlainRange()
{
    Label begin;
    Label target;
    Label end;
    uint32_t offsets[3] = { };
    LinkedCode code = emitAndLink([&](Emission& emission) {
        CCallHelpers& jit = emission.jit;
        CCallHelpers::Jump jump = jit.jump();
        begin = jit.label();
        jit.emitNops(8);
        target = jit.label();
        jump.link(&jit);
        jit.emitNops(8);
        end = jit.label();
        emission.units.addPlain(begin, end, RegisterSet { });
        emitEnd(emission);
    }, [&](LinkBuffer& linkBuffer) {
        offsets[0] = linkBuffer.offsetOf(begin);
        offsets[1] = linkBuffer.offsetOf(target);
        offsets[2] = linkBuffer.offsetOf(end);
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.localSites.size() == 1);
    AOT_TEST_CHECK(code.localSites[0].target == offsets[1]);
    checkUnit(code.units, 0, offsets[0], Kind::Pinned);
    checkUnit(code.units, offsets[0], offsets[1], Kind::Plain);
    checkUnit(code.units, offsets[1], offsets[2], Kind::Plain);
}

static void testTargetInUnrecordedStretch()
{
    Label target;
    Label end;
    uint32_t targetOffset = 0;
    uint32_t endOffset = 0;
    LinkedCode code = emitAndLink([&](Emission& emission) {
        CCallHelpers& jit = emission.jit;
        CCallHelpers::Jump jump = jit.jump();
        jit.emitNops(8);
        target = jit.label();
        jump.link(&jit);
        jit.emitNops(8);
        end = jit.label();
        emitEnd(emission);
    }, [&](LinkBuffer& linkBuffer) {
        targetOffset = linkBuffer.offsetOf(target);
        endOffset = linkBuffer.offsetOf(end);
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.localSites.size() == 1);
    AOT_TEST_CHECK(code.localSites[0].target == targetOffset);
    AOT_TEST_CHECK(targetOffset && targetOffset < endOffset);
    checkUnit(code.units, 0, endOffset, Kind::Pinned);
}

static void testRelocationsAreExactUnits()
{
    struct RelocationCase {
        RelocationKind kind;
        void (*emit)(Emission&);
    };
    static constexpr RelocationCase cases[] = {
        { RelocationKind::AddressNear, [](Emission& emission) { emission.relocations.moveAddress(namedConstant, GPRInfo::regT0, PCRelativeReach::Near); } },
        { RelocationKind::AddressFar, [](Emission& emission) { emission.relocations.moveAddress(namedConstant, GPRInfo::regT0, PCRelativeReach::Far); } },
        { RelocationKind::Load64, [](Emission& emission) { emission.relocations.load64(namedConstant, GPRInfo::regT0); } },
        { RelocationKind::InstanceOffset, [](Emission& emission) { emission.relocations.moveInstanceAddress({ SymbolSpace::SiteCache, 0 }, GPRInfo::regT0, GPRInfo::regT1); } },
        { RelocationKind::Distance32, [](Emission& emission) { emission.relocations.emitDistance32(calledFunction, 0); } },
        { RelocationKind::Jump, [](Emission& emission) { emission.relocations.jump(calledFunction); } },
    };

    LinkedCode code = emitAndLink([&](Emission& emission) {
        Label begin = emission.jit.label();
        for (const RelocationCase& testCase : cases) {
            emission.jit.emitNops(4);
            testCase.emit(emission);
        }
        emission.jit.emitNops(4);
        emission.units.addPlain(begin, emission.jit.label(), RegisterSet { });
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.relocations.size() == std::size(cases));
    AOT_TEST_CHECK(code.units.size() == 2 * std::size(cases) + 1);
    for (size_t i = 0; i < std::size(cases); ++i) {
        const Relocation& relocation = code.relocations[i];
        AOT_TEST_CHECK(relocation.kind == cases[i].kind);
        // On ARM64 the instruction that adds the instance to an InstanceOffset follows the relocated ones, so the plain unit after it is longer than the padding.
        const CodeUnit& padding = unitContaining(code.units, relocation.offset - 4);
        AOT_TEST_CHECK(padding.kind == Kind::Plain && padding.offset + padding.length == relocation.offset);
        checkUnit(code.units, relocation.offset, relocation.offset + MacroAssembler::relocationSize(relocation.kind), Kind::Pinned);
    }
    AOT_TEST_CHECK(code.units.last().kind == Kind::Plain);
}

static void testPinnedRanges()
{
    LinkedCode code = emitAndLink([&](Emission& emission) {
        emitPlain(emission, 4);

        Label secondBegin = emission.jit.label();
        emitPlain(emission, 4);
        Label thirdBegin = emission.jit.label();
        emission.units.pin(secondBegin, thirdBegin);

        emission.jit.emitNops(4);
        Label thirdEnd = emission.jit.label();
        emission.units.pin(thirdBegin, thirdEnd);
        emission.units.addPlain(thirdBegin, thirdEnd, RegisterSet { });
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.units.size() == 2);
    AOT_TEST_CHECK(code.units[0].kind == Kind::Plain);
    AOT_TEST_CHECK(code.units[0].length == 4);
    AOT_TEST_CHECK(code.units[1].kind == Kind::Pinned);
    AOT_TEST_CHECK(code.units[1].length == 8);
}

static void testCallInPin()
{
    LinkedCode code = emitAndLink([&](Emission& emission) {
        emitPlain(emission, 4);
        emission.relocations.call(calledFunction);
        emitPlain(emission, 4);
        Label begin = emission.jit.label();
        emission.relocations.call(calledFunction);
        emission.units.pin(begin, emission.jit.label());
        emitPlain(emission, 4);
        emitEnd(emission);
    });
    checkTiles(code);
    AOT_TEST_CHECK(code.relocations.size() == 2);
    checkUnit(code.units, code.relocations[0].offset, code.relocations[0].offset + sizeOfNearCall, Kind::Call);
    checkUnit(code.units, code.relocations[1].offset, code.relocations[1].offset + sizeOfNearCall, Kind::Pinned);
}

static void testTrappingLoadIsReportedOnce()
{
    B3::Procedure procedure;
    B3::Origin originOfTrappingLoad(std::bit_cast<DFG::Node*>(static_cast<uintptr_t>(8)));
    B3::Origin originOfPlainLoad(std::bit_cast<DFG::Node*>(static_cast<uintptr_t>(16)));
    B3::Value* address = procedure.add<B3::Const64Value>(B3::Origin(), 0);
    B3::Value* trappingLoad = procedure.add<B3::MemoryValue>(B3::trapping(B3::Load), B3::Int64, originOfTrappingLoad, address);
    B3::Value* plainLoad = procedure.add<B3::MemoryValue>(B3::Load, B3::Int64, originOfPlainLoad, address);

    Tmp t0(GPRInfo::regT0);
    Tmp t1(GPRInfo::regT1);
    B3::Air::Kind moveWithEffects(B3::Air::Move);
    moveWithEffects.effects = true;

    Label access;
    Vector<RecordedTrap> traps;
    uint32_t accessOffset = 0;
    uint32_t trapOffset = 0;
    emitAndLink([&](Emission& emission) {
        emitInstruction(emission, Inst(B3::Air::Move, plainLoad, Arg::addr(t0, 0), t1));
        emitInstruction(emission, Inst(moveWithEffects, plainLoad, Arg::addr(t0, 0), t1));
        emitInstruction(emission, Inst(moveWithEffects, nullptr, Arg::addr(t0, 0), t1));
        emitInstruction(emission, Inst(B3::Air::Move, trappingLoad, t0, t1));
        access = emitInstruction(emission, Inst(moveWithEffects, trappingLoad, Arg::addr(t0, 0), t1));
        Label end = emission.jit.label();
        emission.units.didGenerate(Inst(moveWithEffects, trappingLoad, Arg::addr(t0, 0), t1), end, end);
        traps = emission.units.takeTraps();
        AOT_TEST_CHECK(emission.units.takeTraps().isEmpty());
    }, [&](LinkBuffer& linkBuffer) {
        AOT_TEST_CHECK(traps.size() == 1);
        accessOffset = linkBuffer.offsetOf(access);
        trapOffset = linkBuffer.offsetOf(traps[0].instruction);
    });
    AOT_TEST_CHECK(trapOffset == accessOffset);
    AOT_TEST_CHECK(traps[0].origin.dfgOrigin() == originOfTrappingLoad.dfgOrigin());
}

static FaultingAccess faultingAccessAt(uint32_t offset)
{
    return { offset, { }, { 0 } };
}

static uint32_t codeSize(const Fixture& fixture)
{
    return static_cast<uint32_t>(fixture.code.body.bytes.size());
}

static Fixture makeFixture()
{
    Label plain;
    Label pinned;
    Label call;
    Label address;
    Label afterBranch;
    Label end;
    Fixture fixture { };
    LinkedCode linked = emitAndLink([&](Emission& emission) {
        CCallHelpers& jit = emission.jit;
        plain = jit.label();
        emitPlain(emission, 8);
        pinned = jit.label();
        jit.emitNops(8);
        call = jit.label();
        emission.relocations.call(calledFunction);
        address = jit.label();
        emission.relocations.moveAddress(namedConstant, GPRInfo::regT0, PCRelativeReach::Near);
        CCallHelpers::Jump toEnd = jit.branch32(CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1);
        afterBranch = jit.label();
        emitPlain(emission, 8);
        end = jit.label();
        toEnd.link(&jit);
        emitEnd(emission);
    }, [&](LinkBuffer& linkBuffer) {
        fixture.plain = linkBuffer.offsetOf(plain);
        fixture.pinned = linkBuffer.offsetOf(pinned);
        fixture.call = linkBuffer.offsetOf(call);
        fixture.address = linkBuffer.offsetOf(address);
        fixture.afterBranch = linkBuffer.offsetOf(afterBranch);
        fixture.end = linkBuffer.offsetOf(end);
    });
    checkTiles(linked);

    CodeBody& body = fixture.code.body;
    body.bytes = WTF::move(linked.bytes);
    body.units = WTF::move(linked.units);
    body.relocations = WTF::move(linked.relocations);
    body.localSites = WTF::move(linked.localSites);
    body.entries.append(CodeEntry { 0, primaryEntry });
    body.returnSites.append(ReturnSite { fixture.call + static_cast<uint32_t>(sizeOfNearCall), { } });
    body.paddingStart = codeSize(fixture);
    fixture.code.sites.faultingAccesses.append(faultingAccessAt(fixture.pinned));
    return fixture;
}

static CodeUnit unitAt(const Fixture& fixture, uint32_t offset)
{
    Vector<CodeUnit> units = fixture.code.body.units.unpack();
    return units[indexOfUnitAt(units, offset)];
}

static CodeUnit onlyUnit(const Fixture& fixture, Kind kind)
{
    Vector<CodeUnit> units = fixture.code.body.units.unpack();
    return units[indexOfOnly(units, kind)];
}

static void changeUnitAt(Fixture& fixture, uint32_t offset, const auto& change)
{
    changeUnits(fixture.code.body, [&](Vector<CodeUnit>& units) {
        change(units[indexOfUnitAt(units, offset)]);
    });
}

static void setKindOfUnitAt(Fixture& fixture, uint32_t offset, Kind kind)
{
    changeUnitAt(fixture, offset, [&](CodeUnit& unit) {
        unit.kind = kind;
    });
}

static void testFixtureIsValid()
{
    Fixture fixture = makeFixture();
    AOT_TEST_CHECK(verifyCodeUnits(fixture.code) == CodeUnitsError::None);

    const CodeBody& body = fixture.code.body;
    Vector<CodeUnit> units = body.units.unpack();
    checkUnit(units, fixture.plain, fixture.pinned, Kind::Plain);
    checkUnit(units, fixture.pinned, fixture.call, Kind::Pinned);
    AOT_TEST_CHECK(fixture.call - fixture.pinned == 8);
    AOT_TEST_CHECK(onlyUnit(fixture, Kind::Call).offset == fixture.call);
    AOT_TEST_CHECK(onlyUnit(fixture, Kind::Call).length == sizeOfNearCall);
    checkUnit(units, fixture.address, fixture.afterBranch, Kind::Pinned);
    checkUnit(units, fixture.afterBranch, fixture.end, Kind::Plain);
    AOT_TEST_CHECK(fixture.end - fixture.afterBranch == 8);
    AOT_TEST_CHECK(unitAt(fixture, fixture.end).kind == Kind::End);

    AOT_TEST_CHECK(body.relocations.size() == 2);
    AOT_TEST_CHECK(body.relocations[0].offset == fixture.call && body.relocations[0].kind == RelocationKind::Call);
    AOT_TEST_CHECK(body.relocations[1].offset == fixture.address && body.relocations[1].kind == RelocationKind::AddressNear);
    AOT_TEST_CHECK(body.localSites.size() == 1);
    AOT_TEST_CHECK(body.localSites[0].offset > fixture.address && body.localSites[0].offset < fixture.afterBranch);
    AOT_TEST_CHECK(body.localSites[0].target == fixture.end);

    fixture.code.sites.pollSites.append(PollSite { fixture.pinned, false });
    AOT_TEST_CHECK(verifyCodeUnits(fixture.code) == CodeUnitsError::None);

    fixture.code.sites.faultingAccesses[0].codeOffset = fixture.pinned + 4;
    AOT_TEST_CHECK(verifyCodeUnits(fixture.code) == CodeUnitsError::None);
}

static void checkChanged(CodeUnitsError expected, const auto& change)
{
    Fixture fixture = makeFixture();
    change(fixture);
    AOT_TEST_CHECK(verifyCodeUnits(fixture.code) == expected);
}

static void testBodyWithoutUnits()
{
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) {
        dropUnitsWithoutOutlinableCode(fixture.code.body);
        AOT_TEST_CHECK(!fixture.code.body.units.isEmpty());
        AOT_TEST_CHECK(!fixture.code.body.localSites.isEmpty());
    });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) {
        changeUnits(fixture.code.body, [](Vector<CodeUnit>& units) {
            for (CodeUnit& unit : units) {
                if (unit.kind == Kind::Plain)
                    unit.kind = Kind::Pinned;
            }
        });
        dropUnitsWithoutOutlinableCode(fixture.code.body);
        AOT_TEST_CHECK(fixture.code.body.units.isEmpty());
        AOT_TEST_CHECK(fixture.code.body.localSites.isEmpty());
    });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) {
        fixture.code.body.units.clear();
        fixture.code.body.returnSites[0].offset = fixture.afterBranch + 4;
    });
}

static void testUnitsDoNotTileCodeGap()
{
    checkChanged(CodeUnitsError::UnitsDoNotTileCode, [](Fixture& fixture) {
        changeUnits(fixture.code.body, [&](Vector<CodeUnit>& units) {
            units.removeAt(indexOfUnitAt(units, fixture.afterBranch));
        });
    });
}

static void testUnitsDoNotTileCodeOverlap()
{
    checkChanged(CodeUnitsError::UnitsDoNotTileCode, [](Fixture& fixture) {
        changeUnitAt(fixture, fixture.afterBranch, [](CodeUnit& unit) {
            unit.length += 4;
        });
    });
}

static void testUnitsDoNotTileCodeShort()
{
    checkChanged(CodeUnitsError::UnitsDoNotTileCode, [](Fixture& fixture) {
        changeUnits(fixture.code.body, [](Vector<CodeUnit>& units) {
            units.removeLast();
        });
    });
}

static void testUnitsDoNotTileCodeZeroLength()
{
    checkChanged(CodeUnitsError::UnitsDoNotTileCode, [](Fixture& fixture) {
        changeUnits(fixture.code.body, [&](Vector<CodeUnit>& units) {
            units.insert(indexOfUnitAt(units, fixture.afterBranch), CodeUnit { fixture.afterBranch, 0, Kind::Pinned });
        });
    });
}

static void testCallIsNotAUnit()
{
    checkChanged(CodeUnitsError::CallIsNotAUnit, [](Fixture& fixture) { setKindOfUnitAt(fixture, fixture.call, Kind::Plain); });
    checkChanged(CodeUnitsError::CallIsNotAUnit, [](Fixture& fixture) {
        changeUnits(fixture.code.body, [](Vector<CodeUnit>& units) {
            size_t index = indexOfOnly(units, Kind::Call);
            units[index].length += units[index + 1].length;
            units.removeAt(index + 1);
        });
    });
    checkChanged(CodeUnitsError::CallIsNotAUnit, [](Fixture& fixture) { setKindOfUnitAt(fixture, fixture.afterBranch, Kind::Call); });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { setKindOfUnitAt(fixture, fixture.call, Kind::Pinned); });
}

static void testSiteIsNotPinned()
{
    checkChanged(CodeUnitsError::SiteIsNotPinned, [](Fixture& fixture) { fixture.code.sites.faultingAccesses[0].codeOffset = fixture.afterBranch; });
    checkChanged(CodeUnitsError::SiteIsNotPinned, [](Fixture& fixture) { setKindOfUnitAt(fixture, fixture.address, Kind::Plain); });
    checkChanged(CodeUnitsError::SiteIsNotPinned, [](Fixture& fixture) { fixture.code.body.localSites[0].offset = fixture.afterBranch; });
    checkChanged(CodeUnitsError::SiteIsNotPinned, [](Fixture& fixture) { fixture.code.body.relocations[1].offset = fixture.afterBranch - 1; });
    checkChanged(CodeUnitsError::SiteIsNotPinned, [](Fixture& fixture) {
        fixture.code.frame.prologueChecks.append(PrologueCheck { 0, fixture.afterBranch, fixture.end });
    });
    checkChanged(hasFaultingLoopPolls ? CodeUnitsError::SiteIsNotPinned : CodeUnitsError::None, [](Fixture& fixture) {
        fixture.code.sites.pollSites.append(PollSite { fixture.afterBranch, false });
    });
}

static void testTargetIsNotABoundary()
{
    checkChanged(CodeUnitsError::TargetIsNotABoundary, [](Fixture& fixture) { fixture.code.body.localSites[0].target = fixture.afterBranch + 4; });
    checkChanged(CodeUnitsError::TargetIsNotABoundary, [](Fixture& fixture) { fixture.code.body.localSites[0].target = fixture.call + 1; });
    checkChanged(CodeUnitsError::TargetIsNotABoundary, [](Fixture& fixture) { fixture.code.body.entries.append(CodeEntry { fixture.afterBranch + 4, 1 }); });
    checkChanged(CodeUnitsError::TargetIsNotABoundary, [](Fixture& fixture) { fixture.code.body.jumpTables.append(JumpTable { { fixture.end, fixture.afterBranch + 4 } }); });
    checkChanged(CodeUnitsError::TargetIsNotABoundary, [](Fixture& fixture) {
        fixture.code.sites.handlers.append(SiteHandler { 0, fixture.afterBranch + 4, HandlerType::Catch });
    });
    checkChanged(CodeUnitsError::TargetIsNotABoundary, [](Fixture& fixture) {
        fixture.code.frame.prologueChecks.append(PrologueCheck { 0, fixture.pinned, fixture.afterBranch + 4 });
    });

    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { fixture.code.body.localSites[0].target = fixture.afterBranch; });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { fixture.code.body.localSites[0].target = fixture.pinned + 4; });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { fixture.code.body.localSites[0].target = codeSize(fixture); });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { fixture.code.body.jumpTables.append(JumpTable { { fixture.end, fixture.afterBranch } }); });
}

static void testReturnAddressIsNotABoundary()
{
    checkChanged(CodeUnitsError::ReturnAddressIsNotABoundary, [](Fixture& fixture) { fixture.code.body.returnSites[0].offset = fixture.afterBranch + 4; });
    checkChanged(CodeUnitsError::ReturnAddressIsNotABoundary, [](Fixture& fixture) { fixture.code.body.firstReadSites.append(FirstReadSite { fixture.afterBranch + 4, 0 }); });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { fixture.code.body.returnSites[0].offset = fixture.pinned + 4; });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { fixture.code.body.firstReadSites.append(FirstReadSite { fixture.address, 0 }); });
}

static void testOffsetsDoNotAscend()
{
    checkChanged(CodeUnitsError::OffsetsDoNotAscend, [](Fixture& fixture) { std::swap(fixture.code.body.relocations[0], fixture.code.body.relocations[1]); });
    checkChanged(CodeUnitsError::OffsetsDoNotAscend, [](Fixture& fixture) { fixture.code.body.relocations.append(Relocation { fixture.code.body.relocations.last() }); });
    checkChanged(CodeUnitsError::OffsetsDoNotAscend, [](Fixture& fixture) { fixture.code.body.localSites.append(LinkedSite { fixture.code.body.localSites.last() }); });
    checkChanged(CodeUnitsError::OffsetsDoNotAscend, [](Fixture& fixture) { fixture.code.body.returnSites.append(ReturnSite { fixture.code.body.returnSites.last() }); });
    checkChanged(CodeUnitsError::OffsetsDoNotAscend, [](Fixture& fixture) { fixture.code.body.returnSites.append(ReturnSite { fixture.pinned, { } }); });
}

static void testOffsetOutsideCode()
{
    checkChanged(CodeUnitsError::OffsetOutsideCode, [](Fixture& fixture) { fixture.code.body.localSites[0].target = codeSize(fixture) + 4; });
    checkChanged(CodeUnitsError::OffsetOutsideCode, [](Fixture& fixture) { fixture.code.body.relocations.last().offset = codeSize(fixture) - 1; });
    checkChanged(CodeUnitsError::OffsetOutsideCode, [](Fixture& fixture) { fixture.code.body.returnSites[0].offset = codeSize(fixture) + 1; });
    checkChanged(CodeUnitsError::OffsetOutsideCode, [](Fixture& fixture) { fixture.code.body.paddingStart = codeSize(fixture) + 1; });
    checkChanged(CodeUnitsError::OffsetOutsideCode, [](Fixture& fixture) { fixture.code.frame.frameRanges.append(CodeRange { 0, codeSize(fixture) + 1 }); });
    checkChanged(CodeUnitsError::OffsetOutsideCode, [](Fixture& fixture) { fixture.code.sites.faultingAccesses[0].codeOffset = codeSize(fixture); });
    checkChanged(CodeUnitsError::None, [](Fixture& fixture) { fixture.code.frame.frameRanges.append(CodeRange { 0, codeSize(fixture) }); });
}

static bool areEqual(const CodeUnit& a, const CodeUnit& b)
{
    return a.offset == b.offset && a.length == b.length && a.kind == b.kind && a.mayRun == b.mayRun;
}

static void checkUnpacksTo(const PackedCodeUnits& packed, std::initializer_list<CodeUnit> expected)
{
    Vector<CodeUnit> units = packed.unpack();
    AOT_TEST_CHECK(packed.size() == expected.size() && packed.isEmpty() == !expected.size());
    AOT_TEST_CHECK(std::ranges::equal(units, expected, areEqual));
    if (!expected.size()) {
        AOT_TEST_CHECK(!packed.codeSize());
        return;
    }
    AOT_TEST_CHECK(packed.codeSize() == units.last().offset + units.last().length);
    AOT_TEST_CHECK(areEqual(packed.first(), units.first()) && areEqual(packed.last(), units.last()));
}

static void testPackedUnitsRoundTrip()
{
    checkUnpacksTo({ }, { });
    Vector<CodeUnit> units {
        { 0, 1, Kind::Plain, true }, { 1, 15, Kind::PlainOffStackPointer, false }, { 16, 16, Kind::Pinned, false }, { 32, 5, Kind::Call, true },
        { 37, 0xffff, Kind::Pinned, false }, { 37 + 0xffff, 0x100, Kind::Plain, true }, { 37 + 0xffff + 0x100, 2, Kind::End, false },
    };
    PackedCodeUnits packed(units);
    AOT_TEST_CHECK(std::ranges::equal(packed.unpack(), units, areEqual));
    AOT_TEST_CHECK(packed.size() == units.size() && packed.codeSize() == 37 + 0xffff + 0x100 + 2);
    packed.clear();
    checkUnpacksTo(packed, { });
}

static void testPackedUnitsAppendAndPrepend()
{
    PackedCodeUnits packed;
    packed.append({ 0, 4, Kind::Plain, true });
    packed.append({ 4, 40, Kind::Pinned, false });
    checkUnpacksTo(packed, { { 0, 4, Kind::Plain, true }, { 4, 40, Kind::Pinned, false } });
    Vector<CodeUnit> head { { 0, 300, Kind::Pinned, false }, { 300, 8, Kind::Pinned, false } };
    packed.prepend(head.span());
    checkUnpacksTo(packed, { { 0, 300, Kind::Pinned, false }, { 300, 8, Kind::Pinned, false }, { 308, 4, Kind::Plain, true }, { 312, 40, Kind::Pinned, false } });
}

static void testPackedUnitsShrinkCodeSize()
{
    Vector<CodeUnit> units { { 0, 4, Kind::Plain, true }, { 4, 20, Kind::Pinned, false }, { 24, 4, Kind::End, false }, { 28, 12, Kind::Pinned, false } };
    auto shrunkTo = [&](uint32_t size) {
        PackedCodeUnits packed(units);
        packed.shrinkCodeSize(size);
        return packed;
    };
    checkUnpacksTo(shrunkTo(40), { units[0], units[1], units[2], units[3] });
    checkUnpacksTo(shrunkTo(44), { units[0], units[1], units[2], units[3] });
    checkUnpacksTo(shrunkTo(32), { units[0], units[1], units[2], { 28, 4, Kind::Pinned, false } });
    checkUnpacksTo(shrunkTo(28), { units[0], units[1], units[2] });
    checkUnpacksTo(shrunkTo(8), { units[0], { 4, 4, Kind::Pinned, false } });
    checkUnpacksTo(shrunkTo(0), { });
}

std::span<const NativeTest> codeUnitTests()
{
    static constexpr NativeTest tests[] = {
        { "code-units/unrecorded-bytes-are-pinned"_s, testUnrecordedBytesArePinned },
        { "code-units/plain-and-end"_s, testPlainAndEnd },
        { "code-units/plain-naming-forbidden-register"_s, testPlainNamingForbiddenRegister },
        { "code-units/air-instructions"_s, testAirInstructions },
        { "code-units/hot-loop-in-a-cold-function"_s, testHotLoopInColdFunction },
        { "code-units/without-cold-blocks-everything-is-pinned"_s, testWithoutColdBlocksEverythingIsPinned },
        { "code-units/late-path-is-plain"_s, testLatePathIsPlain },
        { "code-units/linked-site-splits-a-plain-range"_s, testLinkedSiteSplitsPlainRange },
        { "code-units/target-cuts-a-plain-range"_s, testTargetCutsPlainRange },
        { "code-units/target-in-an-unrecorded-stretch"_s, testTargetInUnrecordedStretch },
        { "code-units/relocations-are-exact-units"_s, testRelocationsAreExactUnits },
        { "code-units/pinned-ranges"_s, testPinnedRanges },
        { "code-units/call-in-a-pin"_s, testCallInPin },
        { "code-units/trapping-load-is-reported-once"_s, testTrappingLoadIsReportedOnce },
        { "code-units/fixture-is-valid"_s, testFixtureIsValid },
        { "code-units/body-without-units"_s, testBodyWithoutUnits },
        { "code-units/units-do-not-tile-code-gap"_s, testUnitsDoNotTileCodeGap },
        { "code-units/units-do-not-tile-code-overlap"_s, testUnitsDoNotTileCodeOverlap },
        { "code-units/units-do-not-tile-code-short"_s, testUnitsDoNotTileCodeShort },
        { "code-units/units-do-not-tile-code-zero-length"_s, testUnitsDoNotTileCodeZeroLength },
        { "code-units/call-is-not-a-unit"_s, testCallIsNotAUnit },
        { "code-units/site-is-not-pinned"_s, testSiteIsNotPinned },
        { "code-units/target-is-not-a-boundary"_s, testTargetIsNotABoundary },
        { "code-units/return-address-is-not-a-boundary"_s, testReturnAddressIsNotABoundary },
        { "code-units/offsets-do-not-ascend"_s, testOffsetsDoNotAscend },
        { "code-units/offset-outside-code"_s, testOffsetOutsideCode },
        { "code-units/packed-units-round-trip"_s, testPackedUnitsRoundTrip },
        { "code-units/packed-units-append-and-prepend"_s, testPackedUnitsAppendAndPrepend },
        { "code-units/packed-units-shrink-code-size"_s, testPackedUnitsShrinkCodeSize },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
