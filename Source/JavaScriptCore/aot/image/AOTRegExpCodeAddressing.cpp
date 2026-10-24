/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpCodeAddressing.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTThreshold.h"
#include "AOTVector.h"
#include <wtf/MathExtras.h>

namespace JSC::AOT {

static constexpr size_t dataAlignment = 8;

namespace {

struct PackedClassTable {
    Vector<uint8_t> bytes;
    uint32_t offset;
};

} // anonymous namespace

static PackedClassTable packAlone(const Yarr::ClassTable& table)
{
    Yarr::ClassTablePacker packer;
    size_t index = packer.add(table);
    packer.pack();
    return { packer.bytes(), packer.offsetOf(index) };
}

static void invalidateIfTempRegister(CCallHelpers& jit, GPRReg reg)
{
#if CPU(ARM64)
    if (reg == MacroAssembler::dataTempRegister)
        (void)jit.getCachedDataTempRegisterIDAndInvalidate();
    else if (reg == MacroAssembler::memoryTempRegister)
        (void)jit.getCachedMemoryTempRegisterIDAndInvalidate();
#else
    UNUSED_PARAM(jit);
    UNUSED_PARAM(reg);
#endif
}

struct TableByte {
    CodeSymbol symbol;
    PCRelativeReach reach;
    uint8_t bit;
};

static MacroAssembler::Jump branchTestTableByte(CCallHelpers& jit, RelocationRecorder& recorder, MacroAssembler::ResultCondition condition, GPRReg character, TableByte tableByte)
{
    GPRReg scratch = jit.scratchRegister();
    RELEASE_ASSERT(character != scratch);
    recorder.moveAddress(tableByte.symbol, scratch, tableByte.reach);
#if CPU(ARM64)
    jit.load8(MacroAssembler::BaseIndex(scratch, character, MacroAssembler::TimesOne), scratch);
    return jit.branchTest32(condition, scratch, MacroAssembler::TrustedImm32(1 << tableByte.bit));
#elif CPU(X86_64)
    return jit.branchTest8(condition, MacroAssembler::BaseIndex(scratch, character, MacroAssembler::TimesOne), MacroAssembler::TrustedImm32(1 << tableByte.bit));
#else
#error "AOT images need a table test for this CPU."
#endif
}

static Yarr::JITFailureReason toJITFailureReason(LinkFailure failure)
{
    switch (failure) {
    case LinkFailure::ProcessAddressInCode:
        return Yarr::JITFailureReason::ProcessAddressInCode;
    case LinkFailure::OutOfPCRelativeReach:
        return Yarr::JITFailureReason::OutOfPCRelativeReach;
    case LinkFailure::OutOfMemory:
        return Yarr::JITFailureReason::ExecutableMemoryAllocationFailure;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// layOutRegExpSection relies on this: a body and a private copy of every symbol, placed side by side, span less than the near reach.
static bool isWithinNearReach(const RelocatableCode& body, const Vector<RegExpSymbol>& symbols)
{
    size_t size = body.bytes.size();
    for (auto& symbol : symbols)
        size += maxPrivateCopySize(symbol);
    return static_cast<int64_t>(size) < MacroAssembler::relocationReach(RelocationKind::AddressNear) - regExpReachSlack;
}

ImageCodeAddressing::ImageCodeAddressing(CCallHelpers& jit, PCRelativeReach reach, const AssemblerPolicy& policy, BuildContext* context)
    : CodeAddressing(jit)
    , m_recorder(jit, policy)
    , m_reach(reach)
    , m_context(context)
    , m_returnAddressesAreInReach(std::make_shared<bool>(true))
{
}

void ImageCodeAddressing::moveAddress(Yarr::SharedTableID id, GPRReg dest)
{
    invalidateIfTempRegister(m_jit, dest);
    m_recorder.moveAddress(symbolFor(id), dest, PCRelativeReach::Far);
}

void ImageCodeAddressing::moveAddress(const Yarr::ClassTable& table, GPRReg dest)
{
    invalidateIfTempRegister(m_jit, dest);
    m_recorder.moveAddress(symbolFor(Yarr::ClassTable(table)), dest, m_reach);
}

void ImageCodeAddressing::moveAddressOfData(std::span<const uint8_t> bytes, GPRReg dest)
{
    RELEASE_ASSERT(!bytes.empty());
    invalidateIfTempRegister(m_jit, dest);
    m_recorder.moveAddress(symbolFor(PlainData { Vector<uint8_t>(bytes) }), dest, m_reach);
}

MacroAssembler::Jump ImageCodeAddressing::branchTestBuiltInClassTable(MacroAssembler::ResultCondition condition, GPRReg character, Yarr::CharacterClass::Table table)
{
    std::optional<uint8_t> bitPlane = Yarr::builtInClassBitPlane(table);
    if (!bitPlane) {
        fail(Yarr::JITFailureReason::UnlistedSharedTable);
        return m_jit.jump();
    }
    return branchTestTableByte(m_jit, m_recorder, condition, character, { symbolFor(Yarr::SharedTableID::BuiltInClassBitPlanes), PCRelativeReach::Far, *bitPlane });
}

MacroAssembler::Jump ImageCodeAddressing::branchTestLatin1Table(MacroAssembler::ResultCondition condition, GPRReg character, const Yarr::CharacterClass::ByteTable& table)
{
    WTF::BitSet<Yarr::CharacterClass::latin1TableSize> members;
    for (size_t index = 0; index < table.data.size(); ++index) {
        if (table.data[index])
            members.set(index);
    }
    uint8_t bit = Latin1BitPlane::preferredBit(members);
    return branchTestTableByte(m_jit, m_recorder, condition, character, { symbolFor(Latin1BitPlane { members, bit }), m_reach, bit });
}

void ImageCodeAddressing::canonicalizeLatin1(GPRReg character)
{
    GPRReg scratch = m_jit.scratchRegister();
    RELEASE_ASSERT(character != scratch);
    m_recorder.moveAddress(symbolFor(Yarr::SharedTableID::Latin1Canonicalization), scratch, PCRelativeReach::Far);
    m_jit.load16(MacroAssembler::BaseIndex(scratch, character, MacroAssembler::TimesTwo), character);
}

bool ImageCodeAddressing::tryLoadConstant(uint64_t constant, GPRReg dest)
{
    AOT_THRESHOLD(unsigned, minInstructionsReplacedByLoad, 3);
    if (!isARM64() || m_reach != PCRelativeReach::Near || ARM64LogicalImmediate::create64(constant).isValid())
        return false;
    unsigned zeroHalfwords = 0;
    unsigned onesHalfwords = 0;
    for (unsigned shift = 0; shift < 64; shift += 16) {
        uint16_t halfword = static_cast<uint16_t>(constant >> shift);
        zeroHalfwords += !halfword;
        onesHalfwords += halfword == 0xffff;
    }
    if (4 - (zeroHalfwords >= onesHalfwords ? zeroHalfwords : onesHalfwords) < minInstructionsReplacedByLoad)
        return false;
    invalidateIfTempRegister(m_jit, dest);
    m_recorder.load64(symbolFor(Literal64 { constant }), dest);
    return true;
}

void ImageCodeAddressing::callRoutine(const Yarr::SharedRoutine& routine)
{
    if (!Yarr::canGenerateSharedRoutine(routine)) {
        fail(Yarr::JITFailureReason::UnlistedSharedTable);
        return;
    }
    m_recorder.call(symbolFor(Yarr::SharedRoutine(routine)));
}

Yarr::ReturnAddressSite ImageCodeAddressing::storeReturnAddress(MacroAssembler::Address slot)
{
    GPRReg scratch = m_jit.scratchRegister();
    RELEASE_ASSERT(slot.base != scratch);
    MacroAssembler::Label site = m_jit.movePCRelativeAddress(scratch, m_reach);
    m_jit.storePtr(scratch, slot);
    return { { }, site };
}

void ImageCodeAddressing::linkReturnAddresses(WTF::Vector<Yarr::ReturnAddressRecord>&& records)
{
    if (records.isEmpty())
        return;
    m_jit.addLinkTask([records = WTF::move(records), kind = addressKindFor(m_reach), areInReach = m_returnAddressesAreInReach](LinkBuffer& linkBuffer) {
        for (auto& record : records) {
            if (!linkBuffer.link(record.site.pcRelativeAddress, kind, record.target))
                *areInReach = false;
        }
    });
}

void ImageCodeAddressing::emitLocalRoutines()
{
}

std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason> ImageCodeAddressing::takeCode()
{
    if (auto reason = failureReason())
        return makeUnexpected(*reason);

    auto body = m_recorder.finalize(LinkBuffer::Profile::YarrJIT, m_context);
    if (!body)
        return makeUnexpected(toJITFailureReason(body.error()));
    if (!*m_returnAddressesAreInReach)
        return makeUnexpected(Yarr::JITFailureReason::OutOfPCRelativeReach);
    if (m_reach == PCRelativeReach::Near && !isWithinNearReach(*body, m_symbols))
        return makeUnexpected(Yarr::JITFailureReason::OutOfPCRelativeReach);

    auto code = makeUnique<RegExpCode>();
    code->body = WTF::move(*body);
    code->symbols = WTF::move(m_symbols);
    code->reach = m_reach;
    return code;
}

CodeSymbol ImageCodeAddressing::symbolFor(RegExpSymbol&& symbol)
{
    size_t index = m_symbols.find(symbol);
    if (index == notFound) {
        index = m_symbols.size();
        m_symbols.append(WTF::move(symbol));
    }
    return { SymbolSpace::RegExp, static_cast<uint32_t>(index) };
}

std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason> compileSharedRoutine(const Yarr::SharedRoutine& routine, const AssemblerPolicy& policy, BuildContext* context)
{
    if (!Yarr::canGenerateSharedRoutine(routine))
        return makeUnexpected(Yarr::JITFailureReason::UnlistedSharedTable);

    CCallHelpers jit;
    ImageCodeAddressing addressing(jit, PCRelativeReach::Far, policy, context);
    Yarr::generateSharedRoutine(jit, routine, addressing);
    auto code = addressing.takeCode();
    if (code) {
        bool callsSharedRoutine = (*code)->symbols.containsIf([](const RegExpSymbol& symbol) {
            return WTF::holdsAlternative<Yarr::SharedRoutine>(symbol);
        });
        RELEASE_ASSERT(!callsSharedRoutine);
    }
    return code;
}

std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason> compileFailureRoutine(const AssemblerPolicy& policy, BuildContext* context)
{
    CCallHelpers jit;
    ImageCodeAddressing addressing(jit, PCRelativeReach::Far, policy, context);
    jit.tagReturnAddress();
    jit.move(MacroAssembler::TrustedImm64(static_cast<int64_t>(Yarr::JSRegExpResult::JITCodeFailure)), GPRInfo::returnValueGPR);
    jit.move(MacroAssembler::TrustedImm32(0), GPRInfo::returnValueGPR2);
    jit.ret();
    return addressing.takeCode();
}

size_t maxPrivateCopySize(const RegExpSymbol& symbol)
{
    return WTF::switchOn(symbol,
        [](Yarr::SharedTableID) -> size_t { return 0; },
        [](const PlainData& data) -> size_t { return roundUpToMultipleOf<dataAlignment>(data.bytes.size()) + dataAlignment; },
        [](const Latin1BitPlane&) -> size_t { return Yarr::CharacterClass::latin1TableSize + dataAlignment; },
        [](const Yarr::ClassTable& table) -> size_t { return packAlone(table).bytes.size() + dataAlignment; },
        [](const Yarr::SharedRoutine&) -> size_t { return 0; },
        [](const Literal64&) -> size_t { return sizeof(Literal64::value) + dataAlignment; });
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
