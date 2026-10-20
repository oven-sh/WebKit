/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramStrings.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImmortalCells.h"
#include "AOTInstance.h"
#include "AOTProgramIdentifiers.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "FastMarkAppender.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "Symbol.h"
#include "TopExceptionScope.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramStrings);

static constexpr unsigned constantCellRegionShift = 10;

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
    , m_constantCells(ensureConstantCellTable(state), constantCellRegionShift)
{
    ConstantCellWords& constants = m_instance.runtime.constants;
    constants.literalStrings = state.tableArena.allocate<JSCell*>(m_tables.literalStrings().size());
    constants.literalStringCache = state.tableArena.allocate<uint64_t>(1 << literalStringCacheIndexBits);
    constants.keyCells = state.tableArena.allocate<JSCell*>(m_image.text().numberOfIdentifiers());
    constants.bigIntCells = state.tableArena.allocate<JSCell*>(m_tables.bigIntConstants().size());
    state.caches->add(CacheKind::StrongRoots, "string constants"_s, *this, RootVolatility::GreyedByExecution);
}

String ProgramStrings::text(const Image& image, StringID string)
{
    return image.text().createString(string);
}

bool ProgramStrings::hasText(const Image& image, StringID string, StringView text)
{
    return image.text().equals(string, text);
}

AtomString ProgramStrings::atomOf(const Image& image, StringID string)
{
    return AtomString(image.text().createAtom(string));
}

String ProgramStrings::text(const ProgramTables&, StringID string)
{
    return Image::current()->text().createString(string);
}

StringView ProgramStrings::viewInImage(const ProgramTables&, StringID string)
{
    return Image::current()->text().view(string);
}

StringID ProgramStrings::identifierString(const ProgramTables&, uint32_t identifier)
{
    return Image::current()->text().identifier(identifier).text();
}

JSString* ProgramStrings::createString(VM& vm, StringID string) const
{
    DeferGCForAWhile deferGC(vm);
    const ImageText& text = m_image.text();
    StringView characters = text.view(string);
    if (characters.isEmpty())
        return jsEmptyString(vm);
    if (characters.length() == 1) {
        if (char16_t character = characters.codeUnitAt(0); character <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(character);
    }
    String value = text.isAtom(string) ? text.createAtom(string) : text.createString(string);
    if (Image::isBackedByStaticData(*value.impl()))
        return jsOwnedString(vm, value);
    return jsString(vm, value);
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
    JSString* cell = createString(vm, m_image.text().stringConstant(number));
    setConstant(number, cell);
    return cell;
}

void ProgramStrings::setConstant(uint32_t number, JSString* cell)
{
    Locker locker { m_state.caches->lock() };
    if (m_usesWideReferences) {
        m_instance.runtime.constants.wideStringCells[number].set(m_constantCells, cell);
        return;
    }
    m_instance.stringCell(number).set(m_constantCells, MarkedBlockCage::encode(m_instance.cellCageBase, cell));
}

JSString* ProgramStrings::createConstant(VM& vm, uint32_t stringIndex)
{
    RELEASE_ASSERT(stringIndex < m_image.text().numberOfStrings());
    return createString(vm, StringID(stringIndex));
}

JSString* ProgramStrings::string(VM& vm, StringID string)
{
    return createString(vm, string);
}

JSString* ProgramStrings::stringForLiteralStringWord(uint64_t word) const
{
    ASSERT(isLiteralStringWord(word));
    std::span<const uint32_t> literalStrings = m_tables.literalStrings();
    uint32_t number = literalStringIndex(static_cast<uint32_t>(word));
    RELEASE_ASSERT(number < literalStrings.size());
    JSCell*& cell = m_instance.runtime.constants.literalStrings[number];
    if (!cell) {
        JSString* created = createString(*m_instance.vm, StringID(literalStrings[number]));
        m_state.immortalCells->add(created);
        cell = created;
    }
    return uncheckedDowncast<JSString>(cell);
}

std::optional<uint32_t> ProgramStrings::findLiteralString(unsigned hash, StringView text) const
{
    std::span<const uint32_t> table = m_tables.literalStringTable();
    if (table.empty())
        return std::nullopt;
    uint32_t mask = static_cast<uint32_t>(table.size() - 1);
    for (uint32_t position = hash & mask;; position = (position + 1) & mask) {
        uint32_t entry = table[position];
        if (!entry)
            return std::nullopt;
        uint32_t number = LiteralStringTable::numberOf(entry);
        if (entry != LiteralStringTable::entryFor(number, hash))
            continue;
        if (m_image.text().equals(StringID(m_tables.literalStrings()[number]), text))
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
        std::optional<uint32_t> number = findLiteralString(impl->hash(), StringView(*impl));
        static_assert(sizeof(StringImpl) >= 1u << literalStringCacheImplShift);
        uintptr_t address = std::bit_cast<uintptr_t>(impl);
        uint64_t cacheIndex = literalStringCacheIndex(address);
        m_literalStringCacheKeys[cacheIndex] = impl;
        m_instance.runtime.constants.literalStringCache[cacheIndex] = literalStringCacheEntry(address, number ? literalStringWord(*number) : 0);
        return number;
    }
    if (string->isSubstring()) {
        auto characters = string->view(m_instance.globalObject);
        return findLiteralString(characters->hash(), characters.data);
    }
    if (string->is8Bit()) {
        Vector<Latin1Character, 64> characters(length);
        string->resolveToBuffer(characters.mutableSpan());
        StringView text { characters.span() };
        return findLiteralString(text.hash(), text);
    }
    Vector<char16_t, 64> characters(length);
    string->resolveToBuffer(characters.mutableSpan());
    StringView text { characters.span() };
    return findLiteralString(text.hash(), text);
}

uint64_t ProgramStrings::literalStringWordFor(JSString* string)
{
    if (!string->length())
        return emptyLiteralStringWord;
    if (std::optional<uint32_t> number = findLiteralString(string))
        return literalStringWord(*number);
    return std::bit_cast<uint64_t>(string);
}

JSCell* ProgramStrings::keyCell(VM& vm, uint32_t identifier)
{
    RELEASE_ASSERT(identifier < m_image.text().numberOfIdentifiers());
    JSCell*& cell = m_instance.runtime.constants.keyCells[identifier];
    if (cell)
        return cell;
    DeferGCForAWhile deferGC(vm);
    const Identifier& name = vm.aotState().identifiers->identifier(identifier);
    RELEASE_ASSERT(!name.isNull());
    JSCell* created;
    if (auto* symbol = dynamicDowncast<SymbolImpl>(name.impl())) {
        RELEASE_ASSERT(!symbol->isPrivate());
        created = Symbol::create(vm, *symbol);
    } else
        created = jsOwnedString(vm, name.string());
    m_state.immortalCells->add(created);
    cell = created;
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
            for (const VariableData& variable : m_tables.environmentForShape(shape).describedVariables())
                ensureName(variable.name, variable.slot);
            continue;
        }
        for (const FieldData& field : m_tables.layoutForShape(shape).fields())
            ensureName(field.name, field.slot);
    }
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
