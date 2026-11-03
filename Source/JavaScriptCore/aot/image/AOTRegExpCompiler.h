/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTCodeTemplates.h"
#include "AOTImageForward.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpRequests.h"
#include "AOTRegExpSummary.h"
#include "AOTRelocation.h"
#include "AOTVector.h"
#include "YarrClassTable.h"
#include "YarrCodeAddressing.h"
#include "YarrSharedRoutines.h"
#include "YarrSharedTables.h"
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Variant.h>
#include <wtf/text/StringView.h>

namespace JSC::AOT {

struct PlainData {
    Vector<uint8_t> bytes;

    friend bool operator==(const PlainData&, const PlainData&) = default;
};

struct Latin1BitPlane {
    static constexpr unsigned planesPerByteTable = 8;
    static_assert(Yarr::CharacterClass::latin1TableSize == 256);

    static uint8_t preferredBit(const WTF::BitSet<256>& members) { return WTF::computeHash(members.storage()) % planesPerByteTable; }

    WTF::BitSet<256> members;
    uint8_t bit { 0 };

    friend bool operator==(const Latin1BitPlane&, const Latin1BitPlane&) = default;
};

struct Literal64 {
    uint64_t value { 0 };

    friend bool operator==(const Literal64&, const Literal64&) = default;
};

using RegExpSymbol = Variant<Yarr::SharedTableID, PlainData, Latin1BitPlane, Yarr::ClassTable, Yarr::SharedRoutine, Literal64>;

inline void add(Hasher& hasher, const PlainData& data) { add(hasher, data.bytes); }
inline void add(Hasher& hasher, const Latin1BitPlane& plane) { add(hasher, plane.members.storage(), plane.bit); }
inline void add(Hasher& hasher, const Literal64& literal) { add(hasher, literal.value); }

struct RegExpCode {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RegExpCode);

    RelocatableCode body;
    Vector<RegExpSymbol> symbols;
    PCRelativeReach reach { PCRelativeReach::Near };
    unsigned analysisLimitHits { 0 };
    bool hasElidedFrame { false };
    bool handsLongSearchesOver { false };
};

using RegExpCodeOrFailureReason = std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason>;

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
    void linkReturnAddresses(WTF::Vector<Yarr::ReturnAddressRecord>&&) final;
    void emitLocalRoutines() final { }

    RegExpCodeOrFailureReason takeCode();

private:
    CodeSymbol symbolFor(RegExpSymbol&&);
    void moveAddressOfSymbol(RegExpSymbol&&, GPRReg dest, PCRelativeReach);
    MacroAssembler::Jump branchTestTableByte(MacroAssembler::ResultCondition, GPRReg character, RegExpSymbol&&, PCRelativeReach, uint8_t bit);

    RelocationRecorder m_recorder;
    PCRelativeReach m_reach;
    BuildContext* m_context;
    Vector<RegExpSymbol> m_symbols;
    HashMap<uint32_t, Vector<uint32_t, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_symbolsByHash;
    std::shared_ptr<bool> m_returnAddressesAreInReach;
};

RegExpCodeOrFailureReason compileSharedRoutine(const Yarr::SharedRoutine&, const AssemblerPolicy&, BuildContext*);

// Takes the arguments of a body and answers JITCodeFailure, as a body does that gives a match up.
RegExpCodeOrFailureReason compileFailureRoutine(const AssemblerPolicy&, BuildContext*);

JS_EXPORT_PRIVATE size_t maxPrivateCopySize(const RegExpSymbol&);

struct CompiledRegExp {
    size_t codeSize() const;

    RegExpRecordForm form { RegExpRecordForm::NoMetadata };
    std::optional<RegExpDeclineReason> reason { };
    // For 8-bit and for 16-bit subjects. Without the second, a 16-bit subject enters the failure routine, and the pattern is compiled at run time.
    std::array<std::unique_ptr<RegExpCode>, 2> code { };
};

enum class RegExpCodeFor16BitSubjects : uint8_t {
    Always,
    WhereWideCharactersCanBeInMatchedSubject,
};

struct RegExpCompilerInputs {
    StringView pattern;
    OptionSet<Yarr::Flags> flags;
    const RegExpSummary& summary;
    const AssemblerPolicy& assemblerPolicy;
    BuildContext* context;
    RegExpCodeFor16BitSubjects codeFor16BitSubjects { RegExpCodeFor16BitSubjects::Always };
};

JS_EXPORT_PRIVATE CompiledRegExp compileRegExp(const RegExpCompilerInputs&);

struct CompiledRegExps {
    uint32_t numberWithForm(RegExpRecordForm) const;

    Vector<CompiledRegExp> results;
    uint32_t numberWithUnicodePropertyLookup { 0 };
};

JS_EXPORT_PRIVATE CompiledRegExps compileRegExps(const Typed::Program&, const RegExpRequests&, BuildContext&);

struct RegExpSectionItem {
    enum class Kind : uint8_t {
        Body,
        DuplicateBody,
        Routine,
        Table,
    };

    Kind kind;
    uint32_t offset;
    uint32_t size;
    uint32_t regExp;
};

struct RegExpSection {
    static constexpr size_t alignment = 4 * KB;

    Vector<uint8_t> bytes;
    // A multiple of alignment. All code lies in front of it. Behind it lie tables alone.
    uint32_t dataOffset { 0 };
    Vector<std::array<uint32_t, 2>> entries;
    Vector<RegExpSectionItem> items;
};

JS_EXPORT_PRIVATE std::optional<RegExpSection> layOutRegExpSection(std::span<const CompiledRegExp>, const AssemblerPolicy&, const CodeTemplates&, BuildContext*);

struct RegExpRecordInputs {
    const RegExpRequests& requests;
    const CompiledRegExps& compiled;
    const RegExpSection& section;
};

// The match stub stores the start and the end of a match there for a RegExp without a Code record too: an atom, also one created at run time.
inline constexpr uint32_t regExpOutputSizeWithoutSubpatterns = 2;

struct RegExpSections {
    Vector<uint8_t> records;
    Vector<uint8_t> hashes;
    Vector<uint8_t> details;
    uint32_t maxOutputSize { regExpOutputSizeWithoutSubpatterns };
};

JS_EXPORT_PRIVATE RegExpSections encodeRegExpRecords(const RegExpRecordInputs&, BuildContext&);

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
