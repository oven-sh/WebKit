/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpCompiler.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTNumbering.h"
#include "AOTSectionLinker.h"
#include "AOTSectionWriter.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC::AOT {

static constexpr uint8_t numberOfRegExpCharSizes = 2;
static constexpr size_t regExpBodyAlignment = isX86_64() ? 1 : 4;
static constexpr size_t regExpTableAlignment = 8;
static constexpr size_t regExpRoutineAlignment = 16;
static constexpr size_t regExpSharedTableAlignment = 16;

static_assert(std::tuple_size_v<decltype(CompiledRegExp::code)> == numberOfRegExpCharSizes);
static_assert(!(regExpSharedTableAlignment % Yarr::sharedTableAlignment));
static_assert(defaultLimitValue(LimitID::RegExpSectionSize, hostArchitecture()) <= std::numeric_limits<uint32_t>::max());
static_assert(defaultLimitValue(LimitID::RegExpClassTableClusterSize, hostArchitecture()) <= Yarr::ClassTablePacker::defaultMaxClusterSize);

namespace {

template<typename T>
class RegExpValueNumbering {
public:
    uint32_t numberOf(const T& value)
    {
        Candidates& candidates = m_candidates.add(keyOf(value), Candidates()).iterator->value;
        if (std::optional<uint32_t> number = findAmong(candidates, value))
            return *number;
        uint32_t number = size();
        candidates.append(number);
        m_values.append(&value);
        return number;
    }

    std::optional<uint32_t> find(const T& value) const
    {
        auto candidates = m_candidates.find(keyOf(value));
        if (candidates == m_candidates.end())
            return std::nullopt;
        return findAmong(candidates->value, value);
    }

    const T& at(uint32_t number) const { return *m_values[number]; }
    uint32_t size() const { return static_cast<uint32_t>(m_values.size()); }

private:
    using Candidates = Vector<uint32_t, 1>;

    static uint32_t keyOf(const T& value) { return computeHash(value) >> 1; }

    std::optional<uint32_t> findAmong(const Candidates& candidates, const T& value) const
    {
        for (uint32_t candidate : candidates) {
            if (at(candidate) == value)
                return candidate;
        }
        return std::nullopt;
    }

    Vector<const T*> m_values;
    UncheckedKeyHashMap<uint32_t, Candidates, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_candidates;
};

struct RegExpUnit {
    const RegExpCode* code;
    Vector<uint32_t> symbols;

    friend bool operator==(const RegExpUnit& a, const RegExpUnit& b)
    {
        return a.code->reach == b.code->reach
            && a.code->body.bytes == b.code->body.bytes
            && std::ranges::equal(a.code->body.relocations, b.code->body.relocations, [&](const Relocation& left, const Relocation& right) {
                return left.offset == right.offset && left.kind == right.kind && a.symbols[left.symbol] == b.symbols[right.symbol];
            });
    }

    friend void add(Hasher& hasher, const RegExpUnit& unit)
    {
        add(hasher, StringHasher::computeHashAndMaskTop8Bits(unit.code->body.bytes.span()));
        for (const Relocation& relocation : unit.code->body.relocations)
            add(hasher, relocation.offset, relocation.kind, unit.symbols[relocation.symbol]);
    }
};

class RegExpSectionBuilder {
    WTF_MAKE_NONCOPYABLE(RegExpSectionBuilder);
public:
    RegExpSectionBuilder(std::span<const CompiledRegExp> regExps, const AssemblerPolicy& policy, const CodeTemplates& templates, BuildContext* context)
        : m_regExps(regExps)
        , m_policy(policy)
        , m_templates(templates)
        , m_context(context)
        , m_maxSize(context ? context->limit(LimitID::RegExpSectionSize) : defaultLimitValue(LimitID::RegExpSectionSize, hostArchitecture()))
        , m_maxClusterSize(context ? context->limit(LimitID::RegExpClassTableClusterSize) : Yarr::ClassTablePacker::defaultMaxClusterSize)
    {
    }

    std::optional<RegExpSection> build()
    {
        for (uint8_t charSizeIndex = 0; charSizeIndex < numberOfRegExpCharSizes; ++charSizeIndex) {
            for (uint32_t regExp = 0; regExp < m_regExps.size(); ++regExp) {
                if (m_regExps[regExp].form == RegExpRecordForm::Code && m_regExps[regExp].code[charSizeIndex])
                    m_bodies.append({ numberUnit(*m_regExps[regExp].code[charSizeIndex], regExp), regExp, charSizeIndex });
            }
        }
        if (!compileRoutines())
            return std::nullopt;

        addTablesAndRoutines();
        addBodies();
        if (!addFailureRoutine())
            return std::nullopt;

        auto findTarget = [&](uint32_t unit, const Relocation& relocation) -> std::optional<UnitTarget> {
            const Vector<UnitTarget>& targets = m_unitInfos[unit].targets;
            if (relocation.symbol >= targets.size())
                return std::nullopt;
            return targets[relocation.symbol];
        };
        UnitResolver resolve = findTarget;
        auto linked = linkSection({ m_units.span(), resolve, m_templates, m_maxSize, RegExpSection::alignment });
        if (!linked) {
            reportFailure(linked.error());
            return std::nullopt;
        }
        if (linked->numberOfVeneers) {
            reportFailure({ SectionLinkError::OutOfPCRelativeReach, 0, 0 });
            return std::nullopt;
        }
        return createSection(WTF::move(*linked));
    }

private:
    using Kind = RegExpSectionItem::Kind;

    struct Body {
        RegExpUnit unit;
        uint32_t regExp;
        uint8_t charSizeIndex;
        uint32_t number { 0 };
    };

    struct Routine {
        std::unique_ptr<RegExpCode> code;
        RegExpUnit unit;
    };

    struct SymbolState {
        uint32_t firstUser;
        std::optional<UnitTarget> target { };
        std::optional<uint32_t> routine { };
    };

    struct UnitInfo {
        Kind kind;
        uint32_t regExp;
        Vector<UnitTarget> targets;
    };

    RegExpUnit numberUnit(const RegExpCode& code, uint32_t regExp)
    {
        return { &code, code.symbols.map([&](const RegExpSymbol& symbol) {
            uint32_t number = m_symbols.numberOf(symbol);
            if (number == m_symbolStates.size())
                m_symbolStates.append({ regExp });
            return number;
        }) };
    }

    std::unique_ptr<RegExpCode> takeRoutine(RegExpCodeOrFailureReason&& code)
    {
        if (code)
            return WTF::move(*code);
        if (m_context)
            m_context->diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode });
        return nullptr;
    }

    [[nodiscard]] bool compileRoutines()
    {
        for (uint32_t symbol = 0; symbol < m_symbols.size(); ++symbol) {
            auto* routine = std::get_if<Yarr::SharedRoutine>(&m_symbols.at(symbol));
            if (!routine)
                continue;
            std::unique_ptr<RegExpCode> code = takeRoutine(compileSharedRoutine(*routine, m_policy, m_context));
            if (!code)
                return false;
            RegExpUnit unit = numberUnit(*code, m_symbolStates[symbol].firstUser);
            m_symbolStates[symbol].routine = static_cast<uint32_t>(m_routines.size());
            m_routines.append({ WTF::move(code), WTF::move(unit) });
        }
        return true;
    }

    uint32_t addUnit(Kind kind, uint32_t alignment, std::span<const uint8_t> bytes, uint32_t regExp, std::span<const Relocation> relocations = { })
    {
        m_units.append(LinkUnit { bytes, relocations, alignment, kind == Kind::Table });
        m_unitInfos.append(UnitInfo { kind, regExp, { } });
        return static_cast<uint32_t>(m_units.size() - 1);
    }

    uint32_t addUnit(Kind kind, uint32_t alignment, const RelocatableCode& code, uint32_t regExp)
    {
        return addUnit(kind, alignment, code.bytes.span(), regExp, code.relocations.span());
    }

    std::span<uint8_t> ownBytes(Vector<uint8_t>&& bytes)
    {
        m_ownedBytes.append(WTF::move(bytes));
        return m_ownedBytes.last().mutableSpan();
    }

    Vector<UnitTarget> targetsOf(const RegExpUnit& unit) const
    {
        return unit.symbols.map([&](uint32_t symbol) {
            RELEASE_ASSERT(m_symbolStates[symbol].target);
            return *m_symbolStates[symbol].target;
        });
    }

    const RegExpUnit& routineUnit(uint32_t symbol) const { return m_routines[*m_symbolStates[symbol].routine].unit; }

    void addTablesAndRoutines()
    {
        for (Yarr::SharedTableID id : Yarr::allSharedTableIDs) {
            std::optional<uint32_t> symbol = m_symbols.find(RegExpSymbol { id });
            if (!symbol)
                continue;
            SymbolState& state = m_symbolStates[*symbol];
            state.target = UnitTarget { addUnit(Kind::Table, regExpSharedTableAlignment, Yarr::sharedTable(id), state.firstUser) };
        }

        Vector<uint32_t> classTables;
        Vector<uint32_t> bitPlanes;
        Vector<uint32_t> routines;
        for (uint32_t symbol = 0; symbol < m_symbols.size(); ++symbol) {
            SymbolState& state = m_symbolStates[symbol];
            auto addTable = [&](std::span<const uint8_t> bytes) {
                state.target = UnitTarget { addUnit(Kind::Table, regExpTableAlignment, bytes, state.firstUser) };
            };
            WTF::switchOn(m_symbols.at(symbol),
                [&](Yarr::SharedTableID) { },
                [&](const PlainData& data) { addTable(data.bytes.span()); },
                [&](const Latin1BitPlane&) { bitPlanes.append(symbol); },
                [&](const Yarr::ClassTable&) { classTables.append(symbol); },
                [&](const Yarr::SharedRoutine&) {
                    state.target = UnitTarget { addUnit(Kind::Routine, regExpRoutineAlignment, routineUnit(symbol).code->body, state.firstUser) };
                    routines.append(symbol);
                },
                [&](const Literal64& literal) { addTable(ownBytes(Vector<uint8_t>(asByteSpan(literal.value)))); });
        }
        addClassTables(classTables.span());
        addBitPlanes(bitPlanes.span());

        for (uint32_t symbol : routines)
            m_unitInfos[m_symbolStates[symbol].target->unit].targets = targetsOf(routineUnit(symbol));
    }

    void addBodies()
    {
        RegExpValueNumbering<RegExpUnit> numbering;
        for (Body& body : m_bodies) {
            body.number = numbering.numberOf(body.unit);
            if (body.number < m_unitOfBody.size())
                continue;
            uint32_t unit = addUnit(Kind::Body, regExpBodyAlignment, body.unit.code->body, body.regExp);
            m_unitInfos[unit].targets = targetsOf(body.unit);
            m_unitOfBody.append(unit);
            if (body.charSizeIndex)
                m_sizeOf16BitBodies += body.unit.code->body.bytes.size();
        }
    }

    [[nodiscard]] bool addFailureRoutine()
    {
        auto withoutCode = std::ranges::find_if(m_regExps, [](const CompiledRegExp& regExp) {
            return regExp.form != RegExpRecordForm::Code || !regExp.code[0] || !regExp.code[1];
        });
        if (withoutCode == m_regExps.end())
            return true;
        m_failureRoutine = takeRoutine(compileFailureRoutine(m_policy, m_context));
        if (!m_failureRoutine)
            return false;
        m_failureRoutineUnit = addUnit(Kind::Routine, regExpRoutineAlignment, m_failureRoutine->body, static_cast<uint32_t>(withoutCode - m_regExps.begin()));
        return true;
    }

    void addClassTables(std::span<const uint32_t> symbols)
    {
        if (symbols.empty())
            return;
        Yarr::ClassTablePacker packer(m_maxClusterSize);
        Vector<size_t> tables(symbols.size(), [&](size_t index) {
            return packer.add(std::get<Yarr::ClassTable>(m_symbols.at(symbols[index])));
        });
        packer.pack();

        uint32_t unit = addUnit(Kind::Table, regExpTableAlignment, ownBytes(Vector<uint8_t>(packer.bytes())), m_symbolStates[symbols.front()].firstUser);
        for (size_t index = 0; index < symbols.size(); ++index)
            m_symbolStates[symbols[index]].target = UnitTarget { unit, packer.offsetOf(tables[index]) };
    }

    void addBitPlanes(std::span<const uint32_t> symbols)
    {
        struct ByteTable {
            uint32_t unit;
            std::span<uint8_t> bytes;
        };

        std::array<Vector<ByteTable>, Latin1BitPlane::planesPerByteTable> tablesWithFreeBit;
        for (uint32_t symbol : symbols) {
            const Latin1BitPlane& plane = std::get<Latin1BitPlane>(m_symbols.at(symbol));
            RELEASE_ASSERT(plane.bit < Latin1BitPlane::planesPerByteTable);
            if (tablesWithFreeBit[plane.bit].isEmpty()) {
                std::span<uint8_t> bytes = ownBytes(Vector<uint8_t>(FillWith { }, Yarr::CharacterClass::latin1TableSize, static_cast<uint8_t>(0)));
                ByteTable table { addUnit(Kind::Table, regExpTableAlignment, bytes, m_symbolStates[symbol].firstUser), bytes };
                for (Vector<ByteTable>& tables : tablesWithFreeBit)
                    tables.append(table);
            }
            ByteTable table = tablesWithFreeBit[plane.bit].takeLast();
            plane.members.forEachSetBit([&](size_t character) {
                table.bytes[character] |= 1 << plane.bit;
            });
            m_symbolStates[symbol].target = UnitTarget { table.unit };
        }
    }

    RegExpSection createSection(LinkedSection&& linked) const
    {
        RegExpSection section;
        auto addItem = [&](Kind kind, uint32_t unit, uint32_t offset, uint32_t regExp) {
            section.items.append({ kind, offset, static_cast<uint32_t>(m_units[unit].bytes.size()), regExp });
        };
        section.bytes = WTF::move(linked.bytes);
        section.dataOffset = linked.dataOffset;
        uint32_t entryWithoutCode = m_failureRoutineUnit ? linked.uniqueUnitOffsets[*m_failureRoutineUnit] : 0;
        section.entries.fill({ entryWithoutCode, entryWithoutCode }, m_regExps.size());
        uint32_t endOfCode = 0;
        for (uint32_t unit = 0; unit < m_units.size(); ++unit) {
            addItem(m_unitInfos[unit].kind, unit, linked.uniqueUnitOffsets[unit], m_unitInfos[unit].regExp);
            if (!m_units[unit].isData)
                endOfCode = std::max(endOfCode, linked.uniqueUnitOffsets[unit] + static_cast<uint32_t>(m_units[unit].bytes.size()));
        }
        RELEASE_ASSERT(endOfCode <= section.dataOffset && section.dataOffset <= section.bytes.size());
        if (m_context) {
            Statistics& statistics = m_context->statistics();
            statistics.count(StatisticID::RegExpCodeBytesEmitted, endOfCode);
            statistics.count(StatisticID::RegExp16BitCodeBytes, m_sizeOf16BitBodies);
            statistics.count(StatisticID::RegExpTableBytesEmitted, section.bytes.size() - section.dataOffset);
            statistics.count(StatisticID::PaddingBytesBetweenCodeParts, section.dataOffset - endOfCode);
        }

        BitVector seenBodies(m_unitOfBody.size());
        for (const Body& body : m_bodies) {
            uint32_t unit = m_unitOfBody[body.number];
            section.entries[body.regExp][body.charSizeIndex] = linked.uniqueUnitOffsets[unit];
            if (seenBodies.set(body.number))
                addItem(Kind::DuplicateBody, unit, linked.uniqueUnitOffsets[unit], body.regExp);
        }
        AOT::stableSortInLine(section.items, { }, &RegExpSectionItem::offset);
        return section;
    }

    void reportFailure(const SectionLinkFailure& failure)
    {
        if (!m_context)
            return;
        switch (failure.error) {
        case SectionLinkError::SectionTooLarge:
            m_context->reportLimitExceeded(LimitID::RegExpSectionSize, m_maxSize);
            return;
        case SectionLinkError::OutOfPCRelativeReach:
        case SectionLinkError::SymbolWithoutUnit:
            m_context->diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = failure.unit });
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::span<const CompiledRegExp> m_regExps;
    const AssemblerPolicy& m_policy;
    const CodeTemplates& m_templates;
    BuildContext* m_context;
    uint64_t m_maxSize;
    size_t m_maxClusterSize;

    RegExpValueNumbering<RegExpSymbol> m_symbols;
    Vector<SymbolState> m_symbolStates;
    Vector<Routine> m_routines;
    Vector<Body> m_bodies;
    Vector<uint32_t> m_unitOfBody;
    uint64_t m_sizeOf16BitBodies { 0 };

    Vector<LinkUnit> m_units;
    Vector<UnitInfo> m_unitInfos;
    Vector<Vector<uint8_t>> m_ownedBytes;
    std::unique_ptr<RegExpCode> m_failureRoutine;
    std::optional<uint32_t> m_failureRoutineUnit;
};

} // anonymous namespace

std::optional<RegExpSection> layOutRegExpSection(std::span<const CompiledRegExp> regExps, const AssemblerPolicy& policy, const CodeTemplates& templates, BuildContext* context)
{
    return RegExpSectionBuilder(regExps, policy, templates, context).build();
}

static uint32_t encodeRegExpNamedGroups(SectionWriter& details, const RegExpRequest& request, const RegExpSummary& summary, const StringNumbers& strings)
{
    Vector<uint32_t> words;
    for (StringID name : request.captureGroupNames)
        words.append(name ? strings.requiredNumberOf(name) + 1 : 0);
    for (const RegExpNamedGroupStrings& group : request.namedGroups) {
        words.append(strings.requiredNumberOf(group.name));
        words.append(static_cast<uint32_t>(group.parenIndices.size()));
        words.appendVector(group.parenIndices);
    }

    ImageRegExpNamedGroups header {
        .numDuplicateNamedCaptureGroups = summary.numDuplicateNamedCaptureGroups,
        .numberOfCaptureGroupNames = static_cast<uint32_t>(request.captureGroupNames.size()),
        .numberOfNamedGroups = static_cast<uint32_t>(request.namedGroups.size()),
        .numberOfWords = static_cast<uint32_t>(words.size()),
    };
    uint32_t offset = details.append(asByteSpan(header), ImageRegExpNamedGroups::alignment);
    details.append(asByteSpan(words.span()), alignof(uint32_t));
    return offset;
}

static void encodeRegExpMetadata(ImageRegExp& record, const RegExpRequest& request, const RegExpSummary& summary, const StringNumbers& strings, SectionWriter& details)
{
    record.numSubpatterns = summary.numSubpatterns;
    record.minimumSize = summary.minimumSize;
    record.rawSpecificPattern = static_cast<uint8_t>(summary.specificPattern);
    record.sourceIsPattern = summary.sourceIsPattern;
    record.hasNamedGroups = summary.hasNamedGroups();
    RELEASE_ASSERT(!record.hasNamedGroups || !record.hasCharacterSet());
    if (record.hasNamedGroups)
        record.details = encodeRegExpNamedGroups(details, request, summary, strings);
    if (record.hasCharacterSet()) {
        RELEASE_ASSERT(summary.characterSet);
        const Yarr::CharacterSetWithRanges& set = *summary.characterSet;
        ASSERT(!set.bits.firstWideRange);
        RELEASE_ASSERT(set.bits.wideRangeCount == set.wideRanges.size());
        ImageRegExpCharacterSet header { set.bits };
        record.details = details.append(asByteSpan(header), ImageRegExpCharacterSet::alignment);
        details.append(asByteSpan(set.wideRanges.span()), alignof(Yarr::CharacterRange));
    }
}

static ImageRegExp createRegExpRecord(const RegExpRequest& request, const CompiledRegExp& compiled, const RegExpSummary& summary, const std::array<uint32_t, 2>& entries, const StringNumbers& strings, SectionWriter& details)
{
    RegExpRecordForm form = compiled.form;
    ASSERT(request.flags == ImageRegExp::significantFlags(request.flags));
    RELEASE_ASSERT((form == RegExpRecordForm::NoMetadata) == (summary.metadata == RegExpMetadata::Unknown));
    ImageRegExp record { };
    record.pattern = strings.requiredNumberOf(request.pattern);
    record.rawFlags = request.flags.toRaw();
    record.rawForm = static_cast<uint8_t>(form);
    record.isLiteral = request.origin == RegExpRequestOrigin::Literal;

    switch (summary.metadata) {
    case RegExpMetadata::Unknown:
        break;
    case RegExpMetadata::Known:
        encodeRegExpMetadata(record, request, summary, strings, details);
        break;
    }

    switch (form) {
    case RegExpRecordForm::Code:
        record.codeFor8Bit = entries[0];
        record.codeFor16Bit = entries[1];
        record.hasCodeFor8BitSubjectsAlone = !compiled.code[1];
        RELEASE_ASSERT(!record.hasCodeFor8BitSubjectsAlone || !compiled.code[0] || summary.matchesOnlyWhole8BitSubjects);
        record.handsLongSearchesOver = std::ranges::any_of(compiled.code, [](auto& code) { return code && code->handsLongSearchesOver; });
        break;
    case RegExpRecordForm::Atom:
        RELEASE_ASSERT(request.atom);
        record.atom = strings.requiredNumberOf(request.atom);
        break;
    case RegExpRecordForm::NoCode:
    case RegExpRecordForm::NoMetadata:
        break;
    }
    return record;
}

static bool hasRegExpRecord(RegExpRequestOrigin origin, RegExpRecordForm form)
{
    switch (form) {
    case RegExpRecordForm::Code:
    case RegExpRecordForm::Atom:
        return true;
    case RegExpRecordForm::NoCode:
        return origin != RegExpRequestOrigin::Candidate;
    case RegExpRecordForm::NoMetadata:
        return origin == RegExpRequestOrigin::Literal;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RegExpSections encodeRegExpRecords(const RegExpRecordInputs& inputs, BuildContext& context)
{
    std::span<const RegExpRequest> requests = inputs.requests.all();
    RELEASE_ASSERT(requests.size() == inputs.compiled.results.size() && requests.size() == inputs.section.entries.size());

    Vector<ImageRegExp> records;
    Vector<ImageRegExpHash> hashes;
    SectionWriter details(context);
    uint32_t maxOutputSize = regExpOutputSizeWithoutSubpatterns;
    for (size_t index = 0; index < requests.size(); ++index) {
        const RegExpRequest& request = requests[index];
        const CompiledRegExp& compiled = inputs.compiled.results[index];
        if (!hasRegExpRecord(request.origin, compiled.form))
            continue;
        uint32_t recordNumber = static_cast<uint32_t>(records.size());
        RELEASE_ASSERT(request.origin != RegExpRequestOrigin::Literal || recordNumber == index);
        const RegExpSummary* summary = context.regExpSummaries().summaryIfComputed(request.pattern, request.flags);
        RELEASE_ASSERT(summary);

        records.append(createRegExpRecord(request, compiled, *summary, inputs.section.entries[index], inputs.strings, details));
        hashes.append(ImageRegExpHash { ImageRegExp::hashOf(summary->patternHash, request.flags), recordNumber });
        if (compiled.form == RegExpRecordForm::Code || compiled.form == RegExpRecordForm::NoCode)
            maxOutputSize = std::max(maxOutputSize, (summary->numSubpatterns + 1) * 2 + summary->numDuplicateNamedCaptureGroups);
    }
    if (details.didOverflow())
        return { };

    AOT::sort(hashes, { }, [](const ImageRegExpHash& entry) {
        return std::pair { entry.hash, entry.record };
    });
    return { bytesOf(records), bytesOf(hashes), details.takeBytes(), maxOutputSize };
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
