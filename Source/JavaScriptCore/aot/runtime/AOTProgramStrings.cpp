/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramStrings.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTPrefixCode.h"
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
    , m_constantCells(ensureConstantCellTable(state), InstanceLayout::stringCellCardShift)
{
    ConstantCellWords& constants = m_instance.runtime.constants;
    VM& vm = *m_instance.vm;
    constants.stringAllocator = subspaceFor<JSString>(vm)->allocatorFor(sizeof(JSString), AllocatorForMode::EnsureAllocator).localAllocator();
    RELEASE_ASSERT(static_cast<LocalAllocator*>(constants.stringAllocator)->cellSize() == sizeof(JSString));
    constants.cardsOfStringCells = m_constantCells.cards();
    static std::atomic<bool> hasFirstInstance;
    if (size_t numberOfListedWords = m_image.section<uint32_t>(ImageSection::LongStrings).size(); numberOfListedWords && !hasFirstInstance.exchange(true)) {
        size_t sizeOfStringHeaders = roundUpToMultipleOf(pageSize(), numberOfListedWords * sizeof(ImageStringHeader));
        constants.nextStringHeader = static_cast<uint8_t*>(reserveGuardedPages(sizeOfStringHeaders, OSAllocator::UnknownUsage));
        constants.endOfStringHeaders = constants.nextStringHeader + sizeOfStringHeaders;
    }
    m_constantCells.setCardsAreWrittenWithoutRecord();
    if (m_usesWideReferences)
        constants.wideKeyCells = state.tableArena.allocate<JSCell*>(m_tables.keyCellIdentifiers().size());
    else {
        constants.keyCells = state.tableArena.allocate<uint32_t>(m_tables.keyCellIdentifiers().size());
        constants.functionNameCache = state.tableArena.allocate<uint64_t>(ConstantCellWords::numberOfCachedFunctionNames);
        state.caches->addClearedEachCollection("names of functions"_s, asWritableBytes(std::span { constants.functionNameCache, ConstantCellWords::numberOfCachedFunctionNames }));
    }
    constants.bigIntCells = state.tableArena.allocate<JSCell*>(m_tables.bigIntConstants().size());
    state.caches->add(CacheKind::StrongRoots, "string constants"_s, *this, RootVolatility::GreyedByExecution);
}

JSString* ProgramStrings::string(VM& vm, ImageStringWord word, Made made) const
{
    DeferGCForAWhile deferGC(vm);
    const ImageText& text = m_image.text();
    if (word.isCoded())
        return jsString(vm, text.createString(word));
    if (StringImpl* header = text.tryGetHeader(word))
        return jsOwnedString(vm, String { *header });
    StringView characters = text.view(word);
    if (characters.isEmpty())
        return jsEmptyString(vm);
    if (characters.length() == 1) {
        if (char16_t character = characters.codeUnitAt(0); character <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(character);
    }
    if (made == Made::Once && word.hasListedHash()) {
        if (StringImpl* header = tryCreateHeader(characters, text.hash(word)))
            return jsOwnedString(vm, String { *header });
    }
    String value = word.isAtom() ? text.createAtom(word) : text.createString(word);
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


JSString* ProgramStrings::writtenNameOfFunction(VM& vm, FunctionID function)
{
    uint32_t number = function.index() - m_tables.closureFunctions.first;
    uint64_t* cached = !m_usesWideReferences && m_tables.hasClosureNumber(number) ? &m_instance.runtime.constants.functionNameCache[number % ConstantCellWords::numberOfCachedFunctionNames] : nullptr;
    if (cached && static_cast<uint32_t>(*cached) == number + 1)
        return uncheckedDowncast<JSString>(MarkedBlockCage::decode(m_instance.cellCageBase, static_cast<uint32_t>(*cached >> 32)));
    std::optional<uint32_t> name = m_tables.nameOfFunction(function.index());
    JSString* result = !name || !m_image.text().hasText(StringID(*name)) ? jsEmptyString(vm) : string(vm, StringID(*name));
    if (cached && MarkedBlockCage::baseOf(result) == m_instance.cellCageBase)
        *cached = static_cast<uint64_t>(MarkedBlockCage::encode(m_instance.cellCageBase, result)) << 32 | (number + 1);
    return result;
}

JSCell* ProgramStrings::keyCellIfCreated(uint32_t number) const
{
    RELEASE_ASSERT(number < m_tables.keyCellIdentifiers().size());
    const ConstantCellWords& constants = m_instance.runtime.constants;
    if (m_usesWideReferences)
        return constants.wideKeyCells[number];
    uint32_t word = constants.keyCells[number];
    if (!word)
        return nullptr;
    return MarkedBlockCage::decode(m_instance.cellCageBase, word);
}

JSCell* ProgramStrings::keyCell(VM& vm, uint32_t number)
{
    if (JSCell* cell = keyCellIfCreated(number))
        return cell;
    DeferGCForAWhile deferGC(vm);
    const Identifier& name = identifiersOf(m_instance).identifier(m_tables.keyCellIdentifiers()[number]);
    RELEASE_ASSERT(!name.isNull());
    JSCell* created;
    if (auto* symbol = dynamicDowncast<SymbolImpl>(name.impl()))
        created = Symbol::create(vm, *symbol);
    else
        created = jsOwnedString(vm, name.string());
    m_state.immortalCells->add(created);
    const ConstantCellWords& constants = m_instance.runtime.constants;
    if (m_usesWideReferences)
        constants.wideKeyCells[number] = created;
    else
        constants.keyCells[number] = MarkedBlockCage::encode(m_instance.cellCageBase, created);
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
            identifiersOf(m_instance).identifier(name);
    };
    for (uint32_t shape = 1; shape <= m_tables.numberOfShapes; ++shape) {
        if (!m_tables.layouts()[shape])
            continue;
        if (shape >= m_tables.firstEnvironmentShape) {
            for (const VariableData& variable : m_tables.environmentForShape(shape).describedVariables()) {
                if (!m_hasNamedVariablesForHeapSnapshots && variable.name != VariableData::noName)
                    identifiers.nameForHeapSnapshots(&variable, AtomString(text(m_tables, StringID(variable.string()))));
            }
            continue;
        }
        for (const FieldData& field : m_tables.layoutForShape(shape).fields())
            ensureName(field.name, field.slot);
    }
    if (!m_hasNamedVariablesForHeapSnapshots) {
        bool endsWhereClosureNumberEnds = forEachNamedCapture(decodeVarintBytes(m_tables.namedCaptures()).span(), [&](const NamedCaptureData& capture) {
            identifiers.nameForHeapSnapshots(capture.closureNumber, capture.slot, AtomString(text(m_tables, StringID(capture.name))), capture.isVariableOfModule);
        });
        RELEASE_ASSERT(endsWhereClosureNumberEnds);
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
