/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeLayout.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTImageBuilder.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

enum class FormOfLinkTarget : uint8_t { PlaceInCode, Number, DistanceFromInstance };

struct ValuesOfLinkedMention {
    enum class State : uint8_t { NotAsked, HasValue, HasNone };

    std::array<uint32_t, 2> values;
    std::array<State, 2> states;
};
static_assert(!static_cast<uint8_t>(ValuesOfLinkedMention::State::NotAsked));

struct PieceOfLinkedCode {
    ASCIILiteral name { };
    const CompiledCode* function { nullptr };
    uint32_t position { 0 };
    size_t start;
    std::span<const uint8_t> bytes;
    std::span<const Relocation> relocations;
    std::span<const StubKey> stubKeys;
    size_t sharedCodeCopy;
    std::span<const VeneerTarget> veneers;
    size_t firstVeneer;
    std::span<ValuesOfLinkedMention> mentions { };
    const FarInstanceWords* farInstanceWords { nullptr };
};

class StubOffsetTable {
public:
    explicit StubOffsetTable(std::span<const StubPlacement> placements)
    {
        size_t numberOfSlots = std::bit_ceil(placements.size() * 2 + 1);
        m_shift = 64 - std::countr_zero(numberOfSlots);
        m_slots.fill(StubPlacement { StubKey(), 0 }, numberOfSlots);
        for (const StubPlacement& placement : placements) {
            RELEASE_ASSERT(placement.key != StubKey());
            size_t slot = slotOf(placement.key);
            while (m_slots[slot].key != StubKey())
                slot = (slot + 1) & (numberOfSlots - 1);
            m_slots[slot] = placement;
        }
    }

    std::optional<uint32_t> find(StubKey key) const
    {
        for (size_t slot = slotOf(key);; slot = (slot + 1) & (m_slots.size() - 1)) {
            const StubPlacement& placement = m_slots[slot];
            if (placement.key == key)
                return key != StubKey() ? std::optional(placement.offset) : std::nullopt;
            if (placement.key == StubKey())
                return std::nullopt;
        }
    }

private:
    size_t slotOf(StubKey key) const { return static_cast<size_t>(key.bits() * 0x9e3779b97f4a7c15ULL >> m_shift); }

    Vector<StubPlacement> m_slots;
    unsigned m_shift { 0 };
};

struct LinkedBodySizes {
    uint64_t bodies { 0 };
    uint64_t farInstanceWords { 0 };
    uint64_t veneers { 0 };
    uint64_t padding { 0 };
};

} // anonymous namespace

static constexpr FormOfLinkTarget formOfLinkTarget(RelocationKind kind)
{
    switch (kind) {
    case RelocationKind::Call:
    case RelocationKind::Jump:
    case RelocationKind::ShortJump:
    case RelocationKind::ConditionalJump:
    case RelocationKind::ShortConditionalJump:
    case RelocationKind::ConditionalJumpThenNop:
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
    case RelocationKind::InvertedJumpThenJump:
    case RelocationKind::AddressNear:
    case RelocationKind::AddressFar:
    case RelocationKind::Load64:
    case RelocationKind::Distance32:
        return FormOfLinkTarget::PlaceInCode;
    case RelocationKind::Number32:
    case RelocationKind::NumberWord:
        return FormOfLinkTarget::Number;
    case RelocationKind::InstanceOffset:
    case RelocationKind::InstanceAccess:
        return FormOfLinkTarget::DistanceFromInstance;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr FormOfLinkTarget formOfLinkTarget(SymbolSpace space)
{
    switch (space) {
    case SymbolSpace::Function:
    case SymbolSpace::Stub:
    case SymbolSpace::ArgumentThunk:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::RegExp:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return FormOfLinkTarget::PlaceInCode;
    case SymbolSpace::Number:
        return FormOfLinkTarget::Number;
    case SymbolSpace::InstanceWord:
        return FormOfLinkTarget::DistanceFromInstance;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

namespace {

struct CodeLinker {
    size_t sizeOfVeneer() const { return inputs.templates.sizeOfVeneer(); }
    FunctionID functionOf(const PieceOfLinkedCode& piece) const { return piece.function ? piece.function->unit.function : FunctionID(); }

    static ASCIILiteral nameOfNumberSpace(NumberSpace space)
    {
        switch (space) {
#define AOT_NAME_OF_NUMBER_SPACE(name) \
        case NumberSpace::name: \
            return #name ""_s;
        AOT_NAME_OF_NUMBER_SPACE(Name)
        AOT_NAME_OF_NUMBER_SPACE(NameOfText)
        AOT_NAME_OF_NUMBER_SPACE(String)
        AOT_NAME_OF_NUMBER_SPACE(StringCell)
        AOT_NAME_OF_NUMBER_SPACE(KeyCell)
        AOT_NAME_OF_NUMBER_SPACE(BuiltinCell)
        AOT_NAME_OF_NUMBER_SPACE(BigInt)
        AOT_NAME_OF_NUMBER_SPACE(GlobalBinding)
        AOT_NAME_OF_NUMBER_SPACE(SiteCache)
        AOT_NAME_OF_NUMBER_SPACE(RequireEdge)
        AOT_NAME_OF_NUMBER_SPACE(Variable)
        AOT_NAME_OF_NUMBER_SPACE(CardOfVariable)
        AOT_NAME_OF_NUMBER_SPACE(CardOfWord)
        AOT_NAME_OF_NUMBER_SPACE(ModuleValue)
        AOT_NAME_OF_NUMBER_SPACE(ModuleNamespace)
        AOT_NAME_OF_NUMBER_SPACE(ModuleBuiltinCell)
        AOT_NAME_OF_NUMBER_SPACE(DataStorage)
        AOT_NAME_OF_NUMBER_SPACE(DataElements)
        AOT_NAME_OF_NUMBER_SPACE(RegExpLiteralCell)
        AOT_NAME_OF_NUMBER_SPACE(TemplateObjectCell)
        AOT_NAME_OF_NUMBER_SPACE(ArrayProfile)
        AOT_NAME_OF_NUMBER_SPACE(ReadGroupCache)
        AOT_NAME_OF_NUMBER_SPACE(EnvironmentHandedBack)
        AOT_NAME_OF_NUMBER_SPACE(ClassGuard)
        AOT_NAME_OF_NUMBER_SPACE(ClassRank)
        AOT_NAME_OF_NUMBER_SPACE(HeirLookedAt)
        AOT_NAME_OF_NUMBER_SPACE(ClosureFunction)
        AOT_NAME_OF_NUMBER_SPACE(ClosureCell)
        AOT_NAME_OF_NUMBER_SPACE(Allocation)
        AOT_NAME_OF_NUMBER_SPACE(Request)
        AOT_NAME_OF_NUMBER_SPACE(PropertyRun)
        AOT_NAME_OF_NUMBER_SPACE(ShownString)
        AOT_NAME_OF_NUMBER_SPACE(AtomOfString)
        AOT_NAME_OF_NUMBER_SPACE(Type)
        AOT_NAME_OF_NUMBER_SPACE(MethodSelector)
        AOT_NAME_OF_NUMBER_SPACE(RegExp)
#undef AOT_NAME_OF_NUMBER_SPACE
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void write(size_t offset, std::span<const uint8_t> bytes) const
    {
        if (bytes.empty())
            return;
        RELEASE_ASSERT(isSumSmallerThanOrEqual(offset, bytes.size(), code.size()));
        memcpySpan(code.subspan(offset), bytes);
    }

    size_t pad(size_t begin, size_t end) const
    {
        RELEASE_ASSERT_WITH_MESSAGE(begin <= end && end <= layout.functionsEnd, "AOT: two pieces of code overlap, or one lies behind the end of the functions");
        inputs.templates.fillWithBreakpoints(code.subspan(begin, end - begin));
        return end - begin;
    }

    size_t startOfWhatFollows(size_t end, size_t nextBody) const
    {
        size_t start = nextBody < layout.bodyStarts.size() ? layout.bodyStarts[nextBody] : layout.functionsEnd;
        for (uint32_t copy : layout.sharedCodeCopies) {
            if (copy >= end)
                start = std::min<size_t>(start, copy);
        }
        return start;
    }

    COLD NEVER_INLINE std::nullopt_t fail(const PieceOfLinkedCode& piece, const Relocation& relocation, StringView why) const
    {
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::RelocationNotApplied,
            .function = functionOf(piece),
            .count = relocation.offset,
            .detail = makeString("at offset "_s, relocation.offset, " of the "_s, piece.function ? nameOf(piece.function->unit.role) : piece.name, ": "_s, why),
        });
        return std::nullopt;
    }

    NEVER_INLINE void failForMentionWithoutValue(const PieceOfLinkedCode& piece, const Relocation& relocation, NumberSpace space, bool asInstanceWord) const
    {
        fail(piece, relocation, makeString("what it mentions in NumberSpace::"_s, nameOfNumberSpace(space), asInstanceWord ? " has no word of the instance"_s : " has no number"_s));
    }

    ALWAYS_INLINE std::optional<uint32_t> valueOfMention(const PieceOfLinkedCode& piece, const Relocation& relocation, bool asInstanceWord) const
    {
        using State = ValuesOfLinkedMention::State;
        if (relocation.symbol >= piece.mentions.size())
            return fail(piece, relocation, "the code has no such mention"_s);
        ValuesOfLinkedMention& known = piece.mentions[relocation.symbol];
        if (known.states[asInstanceWord] == State::NotAsked) {
            const CodeBody& body = piece.function->body;
            const Mention& mention = body.mentions[relocation.symbol];
            std::optional<uint32_t> value = numbers.valueOf(mention, body.mentionWords.span(), asInstanceWord);
            known.states[asInstanceWord] = value ? State::HasValue : State::HasNone;
            known.values[asInstanceWord] = value.value_or(0);
            if (!value)
                failForMentionWithoutValue(piece, relocation, mention.space, asInstanceWord);
        }
        if (known.states[asInstanceWord] == State::HasNone)
            return std::nullopt;
        return known.values[asInstanceWord];
    }

    std::optional<uint32_t> recordOfMentionedRegExp(const PieceOfLinkedCode& piece, const Relocation& relocation) const
    {
        std::span<const Mention> mentions = piece.function->body.mentions.span();
        if (relocation.symbol >= mentions.size() || mentions[relocation.symbol].space != NumberSpace::RegExp || mentions[relocation.symbol].addend)
            return fail(piece, relocation, "its symbol is no mention of a regular expression alone"_s);
        return valueOfMention(piece, relocation, false);
    }

    std::optional<size_t> placeOf(const VeneerTarget& target) const
    {
        switch (target.space) {
        case SymbolSpace::Function: {
            std::optional<uint32_t> entryOffset = inputs.functions[target.index].body.offsetOfEntry(target.entry);
            if (!entryOffset)
                return std::nullopt;
            return static_cast<size_t>(layout.bodyStarts[target.index]) + *entryOffset;
        }
        case SymbolSpace::RegExp: {
            const auto& entries = inputs.regExpSection.entries;
            if (target.index >= entries.size() || target.entry >= entries[target.index].size())
                return std::nullopt;
            return static_cast<size_t>(layout.regExpCodeOffset) + entries[target.index][target.entry];
        }
        case SymbolSpace::Stub:
        case SymbolSpace::ArgumentThunk:
        case SymbolSpace::Constant:
        case SymbolSpace::JumpTable:
        case SymbolSpace::Number:
        case SymbolSpace::InstanceWord:
        case SymbolSpace::WebAssembly:
        case SymbolSpace::FFI:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    String describeMissingEntry(const VeneerTarget& target) const
    {
        if (target.space != SymbolSpace::Function)
            return makeString("there is no entry "_s, static_cast<unsigned>(target.entry), " of regular expression "_s, target.index);
        CodeUnit unit = inputs.functions[target.index].unit;
        return makeString("the "_s, nameOf(unit.role), " of function f"_s, unit.function.index(), " has no entry "_s, static_cast<unsigned>(target.entry));
    }

    std::optional<size_t> placeOrVeneerOf(const PieceOfLinkedCode& piece, const Relocation& relocation, const VeneerTarget& target) const
    {
        std::optional<size_t> place = placeOf(target);
        if (!place)
            return fail(piece, relocation, describeMissingEntry(target));
        if (!transfersControl(relocation.kind))
            return place;
        int64_t distance = static_cast<int64_t>(*place) - static_cast<int64_t>(piece.start + relocation.offset);
        if (std::abs(distance) < MacroAssembler::relocationReach(relocation.kind))
            return place;
        auto veneer = std::ranges::lower_bound(piece.veneers, target, comesFirstAmongVeneers);
        if (veneer == piece.veneers.end() || *veneer != target)
            return place;
        return piece.firstVeneer + (veneer - piece.veneers.begin()) * sizeOfVeneer();
    }

    std::optional<size_t> resolve(const PieceOfLinkedCode& piece, const Relocation& relocation) const
    {
        if (formOfLinkTarget(relocation.kind) != formOfLinkTarget(relocation.space))
            return fail(piece, relocation, "its kind does not fit the space of its symbol"_s);

        const SharedCode& sharedCode = inputs.sharedCode;
        const CodeBody* body = piece.function ? &piece.function->body : nullptr;
        uint32_t symbol = relocation.symbol;
        switch (relocation.space) {
        case SymbolSpace::Function: {
            if (!body || symbol >= body->callees.size())
                return fail(piece, relocation, "the code lists no such callee"_s);
            OptionalIndex callee = inputs.callees.at(piece.position, symbol);
            if (!callee)
                return fail(piece, relocation, "the callee has no code"_s);
            return placeOrVeneerOf(piece, relocation, { SymbolSpace::Function, relocation.entry, *callee });
        }
        case SymbolSpace::Stub: {
            if (symbol >= piece.stubKeys.size())
                return fail(piece, relocation, "the code lists no such stub"_s);
            if (std::optional<uint32_t> offset = stubOffsets.find(piece.stubKeys[symbol]))
                return piece.sharedCodeCopy + *offset - sizeOfHeadOfStub(relocation.entry);
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::CallToStubNotGenerated, .function = functionOf(piece), .count = relocation.offset });
            return std::nullopt;
        }
        case SymbolSpace::ArgumentThunk:
            if (!body || symbol >= body->argumentMoves.size())
                return fail(piece, relocation, "the code lists no such argument moves"_s);
            return piece.sharedCodeCopy + sharedCode.argumentThunksOffset + sharedCode.thunkOffset(piece.position, symbol);
        case SymbolSpace::Constant:
        case SymbolSpace::JumpTable: {
            if (!body)
                return fail(piece, relocation, "shared code has no constants and no jump tables"_s);
            const Vector<uint32_t, 0>& offsets = (relocation.space == SymbolSpace::Constant ? constants.constantOffsetsByBody : constants.jumpTableOffsetsByBody)[piece.position];
            if (symbol >= offsets.size())
                return fail(piece, relocation, "the code has no such constant or jump table"_s);
            return static_cast<size_t>(layout.constantsOffset) + offsets[symbol];
        }
        case SymbolSpace::Number:
        case SymbolSpace::InstanceWord: {
            std::optional<uint32_t> value = valueOfMention(piece, relocation, relocation.space == SymbolSpace::InstanceWord);
            if (!value)
                return std::nullopt;
            return static_cast<size_t>(*value);
        }
        case SymbolSpace::RegExp: {
            std::optional<uint32_t> record = body ? recordOfMentionedRegExp(piece, relocation) : std::optional(symbol);
            if (!record)
                return std::nullopt;
            return placeOrVeneerOf(piece, relocation, { SymbolSpace::RegExp, relocation.entry, *record });
        }
        case SymbolSpace::WebAssembly:
        case SymbolSpace::FFI:
            return fail(piece, relocation, "neither functions nor shared code name symbols of this space"_s);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    [[nodiscard]] bool applyToFarInstanceWord(const PieceOfLinkedCode& piece, const Relocation& relocation, int32_t distance, uint32_t& numberOfDetoursTaken) const
    {
        const FarInstanceWords* words = piece.farInstanceWords;
        if (!words)
            return false;
        size_t firstDistance = piece.start + FarInstanceWords::offsetBehind(piece.bytes.size());
        if (words->takesDetour(piece.bytes.size(), relocation.offset)) {
            if (numberOfDetoursTaken >= words->numberOfDetours)
                return false;
            size_t detour = firstDistance + words->sizeOfDistances() + numberOfDetoursTaken++ * sizeOfDetourToFarInstanceWord;
            return applyRelocationThroughDetour(code, piece.start, relocation, detour, distance);
        }
        auto found = std::ranges::lower_bound(words->distances, distance);
        if (found == words->distances.end() || *found != distance)
            return false;
        return applyRelocationReadingDistance(code, piece.start, relocation, firstDistance + (found - words->distances.begin()) * sizeof(int32_t));
    }

    uint64_t link(const PieceOfLinkedCode& piece) const
    {
        write(piece.start, piece.bytes);
        uint64_t numberApplied = 0;
        uint32_t numberOfDetoursTaken = 0;
        for (const Relocation& relocation : piece.relocations) {
            size_t size = MacroAssembler::relocationSize(relocation.kind);
            if (!size || !isSumSmallerThanOrEqual<size_t>(relocation.offset, size, piece.bytes.size())) {
                fail(piece, relocation, "it does not lie within the code"_s);
                continue;
            }
            std::optional<size_t> target = resolve(piece, relocation);
            if (!target)
                continue;
            int32_t distance = static_cast<int32_t>(static_cast<uint32_t>(*target));
            bool isApplied;
            if (relocation.space == SymbolSpace::InstanceWord && isFarInstanceWord(distance))
                isApplied = applyToFarInstanceWord(piece, relocation, distance, numberOfDetoursTaken);
            else {
                isApplied = withKindAsConstant(relocation.kind, [&](auto kind) ALWAYS_INLINE_LAMBDA {
                    [[clang::always_inline]] return MacroAssembler::applyRelocation(kind, code, piece.start + relocation.offset, *target);
                });
            }
            if (!isApplied) {
                fail(piece, relocation, "its target is out of reach"_s);
                continue;
            }
            ++numberApplied;
        }
        return piece.relocations.size() - numberApplied;
    }

    [[nodiscard]] bool writeVeneer(FunctionID function, size_t at, size_t target) const
    {
        const RelocatableCode& veneer = inputs.templates.veneer;
        write(at, veneer.bytes.span());
        if (applyRelocation(code, at, veneer.relocations[0], target))
            return true;
        reportLinkFailure(LinkFailure::OutOfPCRelativeReach, function, context);
        return false;
    }

    uint64_t writeVeneers(FunctionID function, size_t firstVeneer, std::span<const VeneerTarget> targets) const
    {
        ASSERT(std::ranges::is_sorted(targets, comesFirstAmongVeneers));
        uint64_t numberNotWritten = 0;
        for (size_t index = 0; index < targets.size(); ++index) {
            if (std::optional<size_t> place = placeOf(targets[index])) {
                numberNotWritten += !writeVeneer(function, firstVeneer + index * sizeOfVeneer(), *place);
                continue;
            }
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = function, .detail = makeString("a veneer has no target: "_s, describeMissingEntry(targets[index])) });
            ++numberNotWritten;
        }
        return numberNotWritten;
    }

    uint64_t linkSharedCode(size_t copy) const
    {
        const SharedCode& sharedCode = inputs.sharedCode;
        const RelocatableCode& stubs = sharedCode.blob.code;
        const RelocatableCode& thunks = sharedCode.argumentThunks.code;
        std::span<const VeneerTarget> veneers = layout.sharedCodeVeneers.span();
        size_t firstVeneer = copy + sharedCode.veneersOffset;
        size_t end = firstVeneer + veneers.size() * sizeOfVeneer();
        RELEASE_ASSERT(end <= copy + layout.sharedCodeSize);

        auto linkPart = [&](ASCIILiteral name, size_t start, const RelocatableCode& part, std::span<const StubKey> stubKeys) {
            return link({
                .name = name,
                .start = start,
                .bytes = part.bytes.span(),
                .relocations = part.relocations.span(),
                .stubKeys = stubKeys,
                .sharedCodeCopy = copy,
                .veneers = veneers,
                .firstVeneer = firstVeneer,
            });
        };

        uint64_t numberNotApplied = linkPart("stubs"_s, copy, stubs, { });
        if (size_t firstCopy = layout.sharedCodeCopies[0]; copy != firstCopy) {
            for (const StubEntryRange& entry : sharedCode.blob.entriesInFrontOfSingleReturnSites) {
                RELEASE_ASSERT(entry.offset + sizeOfVeneer() <= entry.end);
                numberNotApplied += !writeVeneer({ }, copy + entry.offset, firstCopy + entry.offset);
            }
        }
        size_t endOfParts = copy + stubs.bytes.size();
        if (!thunks.bytes.isEmpty()) {
            size_t startOfThunks = copy + sharedCode.argumentThunksOffset;
            pad(endOfParts, startOfThunks);
            numberNotApplied += linkPart("argument thunks"_s, startOfThunks, thunks, sharedCode.argumentThunks.stubKeys.span());
            endOfParts = startOfThunks + thunks.bytes.size();
        }
        pad(endOfParts, firstVeneer);
        numberNotApplied += writeVeneers({ }, firstVeneer, veneers);
        Statistics& statistics = context.statistics();
        statistics.count(StatisticID::SharedCodeBytesEmitted, firstVeneer - copy);
        statistics.count(StatisticID::VeneerBytesEmitted, end - firstVeneer);
        statistics.count(StatisticID::PaddingBytesBetweenFunctions, pad(end, startOfWhatFollows(end, std::ranges::lower_bound(layout.bodyStarts, end) - layout.bodyStarts.begin())));
        return numberNotApplied;
    }

    void writeJumpTables(uint32_t position) const
    {
        const CompiledCode& function = inputs.functions[position];
        const CodeBody& body = function.body;
        const Vector<uint32_t, 0>& tableOffsets = constants.jumpTableOffsetsByBody[position];
        const auto& jumpTables = body.rareLists->jumpTables;
        RELEASE_ASSERT(tableOffsets.size() == jumpTables.size());
        int64_t bodyStart = layout.bodyStarts[position];
        for (size_t index = 0; index < tableOffsets.size(); ++index) {
            std::span<const uint32_t> targets = jumpTables[index].targets.span();
            size_t table = static_cast<size_t>(layout.constantsOffset) + tableOffsets[index];
            RELEASE_ASSERT(isSumSmallerThanOrEqual(table, targets.size_bytes(), code.size()));
            std::span<uint8_t> words = code.subspan(table, targets.size_bytes());
            for (uint32_t target : targets) {
                int64_t distance = bodyStart + target - static_cast<int64_t>(table);
                if (target > body.bytes.size() || !isRepresentableAs<int32_t>(distance)) {
                    context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = function.unit.function, .count = target, .detail = makeString("jump table "_s, index, " of the "_s, nameOf(function.unit.role), " has the target "_s, target) });
                    return;
                }
                WTF::unalignedStore<int32_t>(words.data(), static_cast<int32_t>(distance));
                words = words.subspan(sizeof(int32_t));
            }
        }
    }

    uint64_t linkBody(uint32_t position, Vector<ValuesOfLinkedMention, 64>& mentions, LinkedBodySizes& sizes) const
    {
        const CompiledCode& function = inputs.functions[position];
        const CodeBody& body = function.body;
        std::span<const VeneerTarget> veneers = layout.veneersByBody[position].span();
        size_t start = layout.bodyStarts[position];
        size_t end = start + body.bytes.size();
        const FarInstanceWords* farInstanceWords = inputs.sizeOfFarInstanceWords(position) ? &inputs.farInstanceWords[position] : nullptr;
        size_t firstVeneer = layout.firstVeneerOffset(position, body.bytes.size(), inputs.sizeOfFarInstanceWords(position));
        mentions.resize(body.mentions.size());
        zeroSpan(mentions.mutableSpan());

        uint64_t numberNotApplied = link({
            .function = &function,
            .position = position,
            .start = start,
            .bytes = body.bytes.span(),
            .relocations = body.relocations.span(),
            .stubKeys = body.stubKeys.span(),
            .sharedCodeCopy = layout.sharedCodeCopyByBody[position],
            .veneers = veneers,
            .firstVeneer = firstVeneer,
            .mentions = mentions.mutableSpan(),
            .farInstanceWords = farInstanceWords,
        });
        sizes.bodies += body.bytes.size();
        if (farInstanceWords) {
            size_t firstDistance = start + FarInstanceWords::offsetBehind(body.bytes.size());
            sizes.padding += pad(end, firstDistance);
            write(firstDistance, asByteSpan(farInstanceWords->distances.span()));
            end = firstDistance + farInstanceWords->size();
            sizes.farInstanceWords += farInstanceWords->size();
        }
        if (!veneers.empty()) {
            sizes.padding += pad(end, firstVeneer);
            numberNotApplied += writeVeneers(function.unit.function, firstVeneer, veneers);
            end = firstVeneer + veneers.size() * sizeOfVeneer();
            sizes.veneers += veneers.size() * sizeOfVeneer();
        }
        sizes.padding += pad(end, startOfWhatFollows(end, position + 1));
        writeJumpTables(position);
        return numberNotApplied;
    }

    const CodeLayoutInputs& inputs;
    const CodeLayout& layout;
    const ConstantPool& constants;
    const ImageNumbers& numbers;
    std::span<uint8_t> code;
    BuildContext& context;
    const StubOffsetTable& stubOffsets;
};

} // anonymous namespace

void linkCode(const CodeLinkerInputs& inputs, std::span<uint8_t> code, BuildContext& context)
{
    constexpr size_t bytesOfConstantsInChunk = MB;
    constexpr uint32_t bodiesInChunk = 64;
    const ParallelLoops& loops = context.parallelLoops();
    const CodeLayout& layout = inputs.layout;
    const uint32_t numberOfBodies = safeCast<uint32_t>(inputs.layoutInputs.functions.size());
    RELEASE_ASSERT(code.size() == layout.codeSize && layout.functionsEnd <= code.size() && !layout.sharedCodeCopies.isEmpty());
    RELEASE_ASSERT(layout.bodyStarts.size() == numberOfBodies && layout.sharedCodeCopyByBody.size() == numberOfBodies && layout.veneersByBody.size() == numberOfBodies);
    RELEASE_ASSERT(inputs.constants.constantOffsetsByBody.size() == numberOfBodies && inputs.constants.jumpTableOffsetsByBody.size() == numberOfBodies);
    RELEASE_ASSERT(inputs.layoutInputs.callees.firstCallee.size() == static_cast<size_t>(numberOfBodies) + 1);
    StubOffsetTable stubOffsets(inputs.layoutInputs.sharedCode.blob.placements.span());
    CodeLinker linker { inputs.layoutInputs, layout, inputs.constants, inputs.numbers, code, context, stubOffsets };

    std::span<const uint8_t> pool = inputs.constants.bytes.span();
    forEachIndexInParallel(loops, static_cast<unsigned>((pool.size() + bytesOfConstantsInChunk - 1) / bytesOfConstantsInChunk), [&](unsigned chunk) {
        size_t first = chunk * bytesOfConstantsInChunk;
        linker.write(layout.constantsOffset + first, pool.subspan(first, std::min(bytesOfConstantsInChunk, pool.size() - first)));
    });
    Statistics& statistics = context.statistics();
    statistics.count(StatisticID::PaddingBytesBetweenFunctions, linker.pad(0, linker.startOfWhatFollows(0, 0)));

    const unsigned numberOfCopies = static_cast<unsigned>(layout.sharedCodeCopies.size());
    const unsigned numberOfChunks = (numberOfBodies + bodiesInChunk - 1) / bodiesInChunk;
    std::atomic<uint64_t> numberNotApplied { 0 };
    forEachIndexInParallel(loops, numberOfCopies + numberOfChunks, [&](unsigned index) {
        if (index < numberOfCopies) {
            numberNotApplied.fetch_add(linker.linkSharedCode(layout.sharedCodeCopies[index]), std::memory_order_relaxed);
            return;
        }
        uint32_t first = (index - numberOfCopies) * bodiesInChunk;
        Vector<ValuesOfLinkedMention, 64> mentions;
        LinkedBodySizes sizes;
        uint64_t numberNotAppliedInChunk = 0;
        for (uint32_t position = first; position < std::min(first + bodiesInChunk, numberOfBodies); ++position)
            numberNotAppliedInChunk += linker.linkBody(position, mentions, sizes);
        numberNotApplied.fetch_add(numberNotAppliedInChunk, std::memory_order_relaxed);
        statistics.count(StatisticID::FunctionBytesEmitted, sizes.bodies);
        statistics.count(StatisticID::PaddingBytesBetweenFunctions, sizes.padding);
        if (sizes.farInstanceWords)
            statistics.count(StatisticID::FarInstanceWordBytesEmitted, sizes.farInstanceWords);
        if (sizes.veneers)
            statistics.count(StatisticID::VeneerBytesEmitted, sizes.veneers);
    });

    if (uint64_t count = numberNotApplied.load(std::memory_order_relaxed); count && !context.diagnostics().hasErrors())
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = clampTo<uint32_t>(count), .detail = makeString(count, " links were not made, and none was reported"_s) });
    statistics.count(StatisticID::CodeBytesEmitted, layout.codeSize);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
