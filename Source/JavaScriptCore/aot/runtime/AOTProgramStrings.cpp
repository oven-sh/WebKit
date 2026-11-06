/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramStrings.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTLiteralStringDigest.h"
#include "AOTProgramIdentifiers.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "FastMarkAppender.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "LocalAllocator.h"
#include "MarkedBlockCage.h"
#include "Symbol.h"
#include "TopExceptionScope.h"
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramStrings);

static std::span<const uint8_t> ensureConstantCellTable(State& state)
{
    Instance& instance = state.instance;
    size_t count = state.tables.instanceSizes.numberOfStringCells;
    if (!instance.options().useWideReferences())
        return { reinterpret_cast<const uint8_t*>(&instance) + InstanceLayout::offsetOfStringCell(0), count * InstanceLayout::sizeOfStringCell };
    instance.runtime.constants.wideStringCells = state.tableArena.allocate<RememberedSlot<JSCell*>>(count);
    return { reinterpret_cast<const uint8_t*>(instance.runtime.constants.wideStringCells), count * sizeof(JSCell*) };
}

ProgramStrings::ProgramStrings(State& state)
    : m_instance(state.instance)
    , m_state(state)
    , m_image(*state.instance.vm->aotState().image)
    , m_tables(state.tables)
    , m_usesWideReferences(state.instance.options().useWideReferences())
    , m_numberOfLiteralStrings(static_cast<uint32_t>(state.tables.literalStrings().size()))
    , m_constantCells(ensureConstantCellTable(state), InstanceLayout::stringCellCardShift)
{
    ConstantCellWords& constants = m_instance.runtime.constants;
    VM& vm = *m_instance.vm;
    constants.stringAllocator = subspaceFor<JSString>(vm)->allocatorFor(sizeof(JSString), AllocatorForMode::EnsureAllocator).localAllocator();
    RELEASE_ASSERT(static_cast<LocalAllocator*>(constants.stringAllocator)->cellSize() == sizeof(JSString));
    constants.cardsOfStringCells = m_constantCells.cards();
    static std::atomic<bool> hasFirstInstance;
    if (size_t numberOfListedWords = m_image.section<uint32_t>(ImageSection::LongStrings).size(); numberOfListedWords && !hasFirstInstance.exchange(true)) {
        // A site makes at most one header: the cell of a numbered constant is kept, and a site without a number runs at most once. A listed string has a word or more.
        constants.sizeOfStringHeaders = roundUpToMultipleOf(pageSize(), numberOfListedWords * sizeof(ImageStringHeader));
        constants.startOfStringHeaders = static_cast<uint8_t*>(reserveGuardedPages(constants.sizeOfStringHeaders, OSAllocator::UnknownUsage));
        constants.nextStringHeader = constants.startOfStringHeaders;
        constants.endOfStringHeaders = constants.startOfStringHeaders + constants.sizeOfStringHeaders;
    }
    m_constantCells.setCardsAreWrittenWithoutRecord();
    if (m_usesWideReferences) {
        constants.wideLiteralStrings = state.tableArena.allocate<JSCell*>(m_tables.literalStrings().size());
        constants.wideKeyCells = state.tableArena.allocate<JSCell*>(m_tables.keyCellIdentifiers().size());
    } else {
        constants.literalStrings = state.tableArena.allocate<uint32_t>(m_tables.literalStrings().size());
        constants.keyCells = state.tableArena.allocate<uint32_t>(m_tables.keyCellIdentifiers().size());
        constants.functionNameCache = state.tableArena.allocate<uint64_t>(ConstantCellWords::numberOfCachedFunctionNames);
        state.caches->addClearedEachCollection("names of functions"_s, asWritableBytes(std::span { constants.functionNameCache, ConstantCellWords::numberOfCachedFunctionNames }));
    }
    const ImageText& text = m_image.text();
    bool nameWithoutEntryIsEmpty = m_image.isObfuscated() || !text.numberOfStrings() || !text.hasText(StringID(0u)) || text.view(StringID(0u)).isEmpty();
    constants.nameOfFunctionsWithoutEntry = nameWithoutEntryIsEmpty ? state.instance.vm->smallStrings.emptyString() : nullptr;
    constants.literalStringCache = state.tableArena.allocate<uint64_t>(1 << literalStringCacheIndexBits);
    constants.bigIntCells = state.tableArena.allocate<JSCell*>(m_tables.bigIntConstants().size());
    state.caches->add(CacheKind::StrongRoots, "string constants"_s, *this, RootVolatility::GreyedByExecution);
}

JSString* ProgramStrings::string(VM& vm, StringID string, Made made) const
{
    DeferGCForAWhile deferGC(vm);
    const ImageText& text = m_image.text();
    if (StringImpl* header = text.tryGetHeader(string))
        return jsOwnedString(vm, String { *header });
    StringView characters = text.view(string);
    if (characters.isEmpty())
        return jsEmptyString(vm);
    if (characters.length() == 1) {
        if (char16_t character = characters.codeUnitAt(0); character <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(character);
    }
    if (made == Made::Once && text.hasListedHash(string)) {
        if (StringImpl* header = tryCreateHeader(characters, text.hash(string)))
            return jsOwnedString(vm, String { *header });
    }
    String value = text.isAtom(string) ? text.createAtom(string) : text.createString(string);
    if (Image::isBackedByStaticData(*value.impl()))
        return jsOwnedString(vm, value);
    return jsString(vm, value);
}

StringImpl* ProgramStrings::tryCreateHeader(StringView charactersInImage, unsigned hash) const
{
    ConstantCellWords& constants = m_instance.runtime.constants;
    if (static_cast<size_t>(constants.endOfStringHeaders - constants.nextStringHeader) < sizeof(ImageStringHeader))
        return nullptr;
    auto* header = reinterpret_cast<ImageStringHeader*>(constants.nextStringHeader);
    constants.nextStringHeader += sizeof(ImageStringHeader);
    uint64_t characters = charactersInImage.is8Bit() ? std::bit_cast<uint64_t>(charactersInImage.span8().data()) : std::bit_cast<uint64_t>(charactersInImage.span16().data());
    *header = ImageStringHeader::create(characters, charactersInImage.length(), hash, !charactersInImage.is8Bit(), StringImpl::StringNormal);
    return reinterpret_cast<StringImpl*>(header);
}

JSString* ProgramStrings::constantIfCreated(uint32_t number) const
{
    RELEASE_ASSERT(number < m_tables.instanceSizes.numberOfStringCells);
    if (m_usesWideReferences)
        return uncheckedDowncast<JSString>(m_instance.runtime.constants.wideStringCells[number].get());
    uint32_t word = m_instance.stringCell(number).get();
    if (!word)
        return nullptr;
    return uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_instance.cellCageBase, word));
}

JSString* ProgramStrings::constant(VM& vm, uint32_t number)
{
    if (JSString* cell = constantIfCreated(number))
        return cell;
    JSString* cell = string(vm, m_image.text().stringConstant(number), Made::Once);
    Locker locker { m_state.caches->lock() };
    if (m_usesWideReferences)
        m_instance.runtime.constants.wideStringCells[number].set(m_constantCells, cell);
    else
        m_instance.stringCell(number).set(m_constantCells, MarkedBlockCage::encode(m_instance.cellCageBase, cell));
    return cell;
}

JSString* ProgramStrings::createConstant(VM& vm, uint32_t stringIndex)
{
    RELEASE_ASSERT(stringIndex < m_image.text().numberOfStrings());
    return string(vm, StringID(stringIndex), Made::Once);
}

// ProgramTables::nameOfFunction(). Nothing for a function without a name, and for one that the compiler made: stretches of those share a name.
static std::optional<uint32_t> positionOfFunctionName(const ProgramTables& tables, FunctionID function)
{
    uint32_t source = tables.sourceFunctionOf(function.index());
    if (source >= tables.numberOfFunctions() - tables.functionSources().size())
        return std::nullopt;
    const uint32_t* group = tables.functionNameGroups().data() + source / 32 * 2;
    if (!(group[0] >> source % 32 & 1))
        return std::nullopt;
    return group[1] + std::popcount(group[0] & ((1u << source % 32) - 1));
}

JSString* ProgramStrings::writtenNameOfFunction(VM& vm, FunctionID function)
{
    StringID name(m_tables.nameOfFunction(function.index()));
    if (m_image.isObfuscated() || !m_image.text().hasText(name))
        return jsEmptyString(vm);
    std::optional<uint32_t> position = m_usesWideReferences ? std::nullopt : positionOfFunctionName(m_tables, function);
    return position ? functionNameAt(vm, *position) : string(vm, name);
}

JSString* ProgramStrings::functionNameAt(VM& vm, uint32_t position)
{
    RELEASE_ASSERT(!m_usesWideReferences && position < m_tables.functionNames().size());
    uint64_t& cached = m_instance.runtime.constants.functionNameCache[position % ConstantCellWords::numberOfCachedFunctionNames];
    if (static_cast<uint32_t>(cached) == position + 1)
        return uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_instance.cellCageBase, static_cast<uint32_t>(cached >> 32)));
    JSString* created = string(vm, StringID(m_tables.functionNames()[position]));
    cached = static_cast<uint64_t>(MarkedBlockCage::encode(m_instance.cellCageBase, created)) << 32 | (position + 1);
    return created;
}

JSCell* ProgramStrings::immortalCellAt(const uint32_t* narrowCells, JSCell* const* wideCells, uint32_t number) const
{
    if (m_usesWideReferences)
        return wideCells[number];
    uint32_t word = narrowCells[number];
    if (!word)
        return nullptr;
    return MarkedBlockCage::decode(m_instance.cellCageBase, word);
}

// The collector does not read these tables, so the cell has its root before compiled code can find it.
void ProgramStrings::setImmortalCellAt(uint32_t* narrowCells, JSCell** wideCells, uint32_t number, JSCell* cell) const
{
    m_state.immortalCells->add(cell);
    if (m_usesWideReferences) {
        wideCells[number] = cell;
        return;
    }
    narrowCells[number] = MarkedBlockCage::encode(m_instance.cellCageBase, cell);
}

JSString* ProgramStrings::literalStringIfCreated(uint32_t number) const
{
    RELEASE_ASSERT(number < m_numberOfLiteralStrings);
    const ConstantCellWords& constants = m_instance.runtime.constants;
    return uncheckedDowncast<JSString>(immortalCellAt(constants.literalStrings, constants.wideLiteralStrings, number));
}

JSString* ProgramStrings::stringForLiteralStringWord(uint64_t word) const
{
    ASSERT(isLiteralStringWord(word));
    uint32_t number = literalStringIndex(static_cast<uint32_t>(word));
    if (JSString* cell = literalStringIfCreated(number)) [[likely]]
        return cell;
    return createLiteralString(number);
}

NEVER_INLINE JSString* ProgramStrings::createLiteralString(uint32_t number) const
{
    RELEASE_ASSERT_WITH_MESSAGE(m_image.text().hasText(StringID(m_tables.literalStrings()[number])), "AOT: a value is a literal string that the program spells nowhere, and no string from outside was that string");
    JSString* created = string(*m_instance.vm, StringID(m_tables.literalStrings()[number]));
    const ConstantCellWords& constants = m_instance.runtime.constants;
    setImmortalCellAt(constants.literalStrings, constants.wideLiteralStrings, number, created);
    return created;
}

// The first string from outside that has the digest lends its text to the instance. No value is the literal string before that, so nobody asks for the text earlier.
bool ProgramStrings::isTextOfLiteralStringWithoutText(uint32_t number, StringView text) const
{
    if (JSString* lent = literalStringIfCreated(number))
        return text == StringView(*lent->tryGetValueImpl());
    std::span<const LiteralStringDigest> digests = m_tables.literalStringDigests();
    auto found = std::ranges::lower_bound(digests, number, { }, &LiteralStringDigest::number);
    RELEASE_ASSERT(found != digests.end() && found->number == number);
    if (found->length != text.length() || found->digest != digestOfLiteralString(text))
        return false;
    VM& vm = *m_instance.vm;
    DeferGCForAWhile deferGC(vm);
    const ConstantCellWords& constants = m_instance.runtime.constants;
    setImmortalCellAt(constants.literalStrings, constants.wideLiteralStrings, number, jsString(vm, text.toString()));
    return true;
}

std::optional<uint32_t> ProgramStrings::findLiteralString(unsigned hash, StringView text) const
{
    std::span<const uint32_t> table = m_tables.literalStringTable();
    if (table.empty())
        return std::nullopt;
    uint32_t numberOfSlots = static_cast<uint32_t>(table.size());
    static_assert(StringHasher::maskHash == (1u << LiteralStringTable::hashBits) - 1);
    for (uint32_t position = LiteralStringTable::firstSlot(hash, numberOfSlots);; position = position + 1 == numberOfSlots ? 0 : position + 1) {
        uint32_t entry = table[position];
        if (!entry)
            return std::nullopt;
        uint32_t number = LiteralStringTable::numberOf(entry);
        if (entry != LiteralStringTable::entryFor(number, hash))
            continue;
        StringID string(m_tables.literalStrings()[number]);
        if (m_image.text().hasText(string) ? m_image.text().equals(string, text) : isTextOfLiteralStringWithoutText(number, text))
            return number;
    }
}

std::optional<uint32_t> ProgramStrings::findLiteralString(JSString* string)
{
    unsigned length = string->length();
    std::span<const LiteralStringLimits> limits = m_tables.literalStringLimits();
    if (limits.empty() || length > limits[0].maxLength)
        return std::nullopt;
    if (StringImpl* impl = string->tryGetValueImpl()) {
        if (std::optional<uint32_t> word = m_image.text().literalStringWordOfHeader(*impl))
            return *word ? std::optional<uint32_t>(literalStringIndex(*word)) : std::nullopt;
        // The literal strings have their headers in the image.
        const ConstantCellWords& constants = m_instance.runtime.constants;
        if (std::bit_cast<uintptr_t>(impl) - std::bit_cast<uintptr_t>(constants.startOfStringHeaders) < constants.sizeOfStringHeaders)
            return std::nullopt;
        static_assert(sizeof(StringImpl) >= 1u << literalStringCacheImplShift);
        uintptr_t address = std::bit_cast<uintptr_t>(impl);
        uint64_t cacheIndex = literalStringCacheIndex(address);
        uint64_t& cached = m_instance.runtime.constants.literalStringCache[cacheIndex];
        if (cached >> literalStringCacheWordBits == address >> literalStringCacheImplShift) {
            uint32_t word = static_cast<uint32_t>(cached & ((1u << literalStringCacheWordBits) - 1)) << literalStringCacheWordShift;
            return word ? std::optional<uint32_t>(literalStringIndex(word)) : std::nullopt;
        }
        std::optional<uint32_t> number = findLiteralString(impl->hash(), StringView(*impl));
        m_literalStringCacheKeys[cacheIndex] = impl;
        cached = literalStringCacheEntry(address, number ? literalStringWord(*number) : 0);
        return number;
    }
    return withCharactersOf(string, [&](StringView text) {
        return findLiteralString(text.hash(), text);
    });
}

uint64_t ProgramStrings::literalStringWordFor(JSString* string)
{
    if (!string->length())
        return emptyLiteralStringWord;
    if (std::optional<uint32_t> number = findLiteralString(string))
        return literalStringWord(*number);
    return std::bit_cast<uint64_t>(string);
}

JSCell* ProgramStrings::keyCellIfCreated(uint32_t number) const
{
    RELEASE_ASSERT(number < m_tables.keyCellIdentifiers().size());
    const ConstantCellWords& constants = m_instance.runtime.constants;
    return immortalCellAt(constants.keyCells, constants.wideKeyCells, number);
}

JSCell* ProgramStrings::keyCell(VM& vm, uint32_t number)
{
    if (JSCell* cell = keyCellIfCreated(number))
        return cell;
    DeferGCForAWhile deferGC(vm);
    const Identifier& name = vm.aotState().identifiers->identifier(m_tables.keyCellIdentifiers()[number]);
    RELEASE_ASSERT(!name.isNull());
    JSCell* created;
    if (auto* symbol = dynamicDowncast<SymbolImpl>(name.impl()))
        created = Symbol::create(vm, *symbol);
    else
        created = jsOwnedString(vm, name.string());
    const ConstantCellWords& constants = m_instance.runtime.constants;
    setImmortalCellAt(constants.keyCells, constants.wideKeyCells, number, created);
    return created;
}

JSValue ProgramStrings::bigIntConstant(JSGlobalObject* globalObject, uint32_t number)
{
    std::span<const uint32_t> constants = m_tables.bigIntConstants();
    RELEASE_ASSERT(number < constants.size());
    JSCell*& cell = m_instance.runtime.constants.bigIntCells[number];
    if (cell)
        return cell;
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferTermination(vm);
    MarkedBlockCage::InfallibleAllocationScope infallibleAllocation(vm.heap.cage());
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue result = JSBigInt::parseInt(globalObject, m_image.text().view(StringID(constants[number])), JSBigInt::ErrorParseMode::ThrowExceptions);
    RELEASE_ASSERT(result);
    catchScope.assertNoException();
    if (result.isCell()) {
        m_state.immortalCells->add(result.asCell());
        cell = result.asCell();
    }
    return result;
}

void ProgramStrings::willAnalyzeHeap(VM& vm)
{
    ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
    auto ensureName = [&](uint32_t name, SlotData slot) {
        if (name && traitsOf(slot.representation()).holdsCell)
            identifiers.identifier(name);
    };
    for (uint32_t shape = 1; shape <= m_tables.numberOfShapes; ++shape) {
        if (!m_tables.layouts()[shape])
            continue;
        if (shape >= m_tables.firstEnvironmentShape) {
            for (const VariableData& variable : m_tables.environmentForShape(shape).describedVariables()) {
                if (!m_hasNamedVariablesForHeapSnapshots && variable.name != VariableData::noName)
                    identifiers.nameForHeapSnapshots(&variable, AtomString(text(m_tables, StringID(variable.name))));
            }
            continue;
        }
        for (const FieldData& field : m_tables.layoutForShape(shape).fields())
            ensureName(field.name, field.slot);
    }
    if (!m_hasNamedVariablesForHeapSnapshots) {
        bool endsWhereAFunctionEnds = forEachNamedCapture(m_tables.namedCaptures(), [&](const NamedCaptureData& capture) {
            identifiers.nameForHeapSnapshots(m_tables.closureNumberOf(capture.function), capture.slot, AtomString(text(m_tables, capture.nameString())));
        });
        RELEASE_ASSERT(endsWhereAFunctionEnds);
    }
    m_hasNamedVariablesForHeapSnapshots = true;
}

template<typename Appender>
ALWAYS_INLINE void ProgramStrings::visitRegion(Appender& appender, size_t begin, size_t end)
{
    size_t count = m_tables.instanceSizes.numberOfStringCells;
    if (m_usesWideReferences) {
        RememberedSlotSet::forEachEntryInRegion(std::span { m_instance.runtime.constants.wideStringCells, count }, begin, end, [&](RememberedSlot<JSCell*>& slot) ALWAYS_INLINE_LAMBDA {
            if (JSCell* cell = slot.get())
                appender.appendUnbarriered(cell);
        });
        return;
    }
    RememberedSlotSet::forEachEntryInRegion(std::span { &m_instance.stringCell(0), count }, begin, end, [&](RememberedSlot<uint32_t>& slot) ALWAYS_INLINE_LAMBDA {
        if (uint32_t index = slot.get())
            appender.appendUnbarriered(MarkedBlockCage::decode(m_instance.cellCageBase, index));
    });
}

template<typename Visitor>
void ProgramStrings::visitRoots(Visitor& visitor)
{
    bool isDeferred = m_constantCells.tryDeferFullVisit(visitor, [](SlotVisitor& visitor, void* owner, size_t begin, size_t end) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            static_cast<ProgramStrings*>(owner)->visitRegion(appender, begin, end);
        });
    }, this);
    if (isDeferred)
        return;
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        m_constantCells.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) ALWAYS_INLINE_LAMBDA {
            visitRegion(appender, begin, end);
        });
    });
}

template void ProgramStrings::visitRoots(AbstractSlotVisitor&);
template void ProgramStrings::visitRoots(SlotVisitor&);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
