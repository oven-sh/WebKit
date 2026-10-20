/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTImageForward.h"
#include "AOTRegExpCode.h"
#include "YarrCodeAddressing.h"
#include <expected>
#include <memory>
#include <span>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

namespace AOT {

class JS_EXPORT_PRIVATE ImageCodeAddressing final : public Yarr::CodeAddressing {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(ImageCodeAddressing);
public:
    ImageCodeAddressing(CCallHelpers&, PCRelativeReach, const AssemblerPolicy&, BuildContext*);

    void moveAddress(Yarr::SharedTableID, GPRReg dest) final;
    void moveAddress(const Yarr::ClassTable&, GPRReg dest) final;
    void moveAddressOfData(std::span<const uint8_t>, GPRReg dest) final;
    MacroAssembler::Jump branchTestBuiltInClassTable(MacroAssembler::ResultCondition, GPRReg character, Yarr::CharacterClass::Table) final;
    MacroAssembler::Jump branchTestLatin1Table(MacroAssembler::ResultCondition, GPRReg character, const Yarr::CharacterClass::ByteTable&) final;
    void canonicalizeLatin1(GPRReg character) final;
    bool tryLoadConstant(uint64_t, GPRReg dest) final;
    void callRoutine(const Yarr::SharedRoutine&) final;
    Yarr::ReturnAddressSite storeReturnAddress(MacroAssembler::Address slot) final;
    void linkReturnAddresses(Vector<Yarr::ReturnAddressRecord>&&) final;
    void emitLocalRoutines() final;

    std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason> takeCode();

private:
    CodeSymbol symbolFor(RegExpSymbol&&);

    RelocationRecorder m_recorder;
    PCRelativeReach m_reach;
    BuildContext* m_context;
    Vector<RegExpSymbol> m_symbols;
    std::shared_ptr<bool> m_returnAddressesAreInReach;
};

JS_EXPORT_PRIVATE std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason> compileSharedRoutine(const Yarr::SharedRoutine&, const AssemblerPolicy&, BuildContext*);

// Takes the arguments of a body and answers JITCodeFailure, as a body does that gives a match up.
JS_EXPORT_PRIVATE std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason> compileFailureRoutine(const AssemblerPolicy&, BuildContext*);

JS_EXPORT_PRIVATE size_t maxPrivateCopySize(const RegExpSymbol&);

inline constexpr int64_t regExpReachSlack = 16;

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
