/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramIdentifiers.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTImage.h"
#include "BuiltinNames.h"
#include "JSCInlines.h"
#include <algorithm>
#include <wtf/Atomics.h>
#include <wtf/MathExtras.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringView.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/SymbolRegistry.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramIdentifiers);

static size_t sizeOfIdentifierTable(uint32_t count)
{
    return roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(count, 1) * sizeof(UniquedStringImpl*));
}

Identifier enginePrivateName(VM& vm, const String& text)
{
    if (PrivateSymbolImpl* symbol = vm.propertyNames->builtinNames().lookUpPrivateName(text))
        return Identifier::fromUid(vm, symbol);
    Ref<PrivateSymbolImpl> symbol = PrivateSymbolImpl::create(*text.impl());
    Identifier name = Identifier::fromUid(vm, symbol.ptr());
    vm.propertyNames->appendExternalName(Identifier::fromString(vm, text), name);
    SUPPRESS_UNCOUNTED_LOCAL auto* leaked = &symbol.leakRef();
    UNUSED_VARIABLE(leaked);
    return name;
}

ProgramIdentifiers::ProgramIdentifiers(VM& vm, const Image& image)
    : m_vm(vm)
    , m_image(image)
    , m_count(image.text().numberOfIdentifiers())
    , m_identifiers(static_cast<UniquedStringImpl**>(OSAllocator::reserveAndCommit(sizeOfIdentifierTable(m_count), OSAllocator::FastMallocPages)))
    , m_numbersByHash(image.programTables().identifiersByHash())
{
}

ProgramIdentifiers::~ProgramIdentifiers()
{
    for (UniquedStringImpl* uid : std::span { m_identifiers, m_count }) {
        if (uid)
            uid->deref();
    }
    OSAllocator::decommitAndRelease(m_identifiers, sizeOfIdentifierTable(m_count));
}

void ProgramIdentifiers::createEngineSymbols()
{
    for (uint32_t number : m_image.programTables().identifiersOfEngineSymbols())
        identifier(number);
}

NEVER_INLINE const Identifier& ProgramIdentifiers::identifierSlowCase(uint32_t number)
{
    Identifier created = create(number);
    if (created.isNull()) {
        RELEASE_ASSERT(!number);
        return *reinterpret_cast<const Identifier*>(&m_identifiers[0]);
    }
    UniquedStringImpl* uid = &Ref { *created.impl() }.leakRef();
    if (uid->isSymbol()) {
        Locker locker { m_symbolsLock };
        m_numbersOfSymbols.add(uid, number);
    }
    WTF::storeStoreFence();
    m_identifiers[number] = uid;
    return *reinterpret_cast<const Identifier*>(&m_identifiers[number]);
}

Identifier ProgramIdentifiers::create(uint32_t number) const
{
    const ImageText& text = m_image.text();
    const ImageIdentifier& record = text.identifier(number);
    StringID string = record.text();
    switch (record.kind()) {
    case PropertyKeyKind::Text:
        if (!string)
            return Identifier();
        return Identifier::fromString(m_vm, text.createAtom(string));
    case PropertyKeyKind::EnginePrivateName:
        return enginePrivateName(m_vm, text.createString(string));
    case PropertyKeyKind::PrivateName:
        return Identifier::fromUid(PrivateSymbolImpl::create(*text.createString(string).impl()).get());
    case PropertyKeyKind::RegisteredSymbol:
        return Identifier::fromUid(m_vm.symbolRegistry().symbolForKey(text.createString(string)).get());
    case PropertyKeyKind::WellKnownSymbol: {
        const BuiltinTable& builtins = BuiltinTable::singleton();
        BuiltinName name = builtins.nameOf(static_cast<WellKnownSymbol>(record.hashOrSymbol()));
        RELEASE_ASSERT(name);
        return builtins.identifierOf(m_vm, name);
    }
    case PropertyKeyKind::CreatedSymbol:
        if (!string)
            return Identifier::fromUid(SymbolImpl::createNullSymbol().get());
        return Identifier::fromUid(SymbolImpl::create(*text.createString(string).impl()).get());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const Identifier* ProgramIdentifiers::tryGetIdentifierConcurrently(uint32_t number) const
{
    RELEASE_ASSERT(number < m_count);
    if (!m_identifiers[number])
        return nullptr;
    WTF::loadLoadFence();
    return reinterpret_cast<const Identifier*>(&m_identifiers[number]);
}

std::optional<uint32_t> ProgramIdentifiers::numberOf(UniquedStringImpl* uid) const
{
    if (!uid->isSymbol())
        return numberOfText(*uid);
    Locker locker { m_symbolsLock };
    auto found = m_numbersOfSymbols.find(uid);
    if (found == m_numbersOfSymbols.end())
        return std::nullopt;
    return found->value;
}

std::optional<uint32_t> ProgramIdentifiers::numberOfText(const UniquedStringImpl& uid) const
{
    if (m_numbersByHash.empty())
        return std::nullopt;
    const ImageText& text = m_image.text();
    uint32_t mask = static_cast<uint32_t>(m_numbersByHash.size() - 1);
    unsigned hash = uid.existingHash();
    for (uint32_t slot = hash & mask; uint32_t entry = m_numbersByHash[slot]; slot = (slot + 1) & mask) {
        uint32_t number = entry - 1;
        if (UniquedStringImpl* created = m_identifiers[number]) {
            if (created == &uid)
                return number;
            continue;
        }
        const ImageIdentifier& record = text.identifier(number);
        if (record.hashOrSymbol() == hash && text.equals(record.text(), StringView(uid)))
            return number;
    }
    return std::nullopt;
}

String ProgramIdentifiers::textOf(const Image& image, uint32_t number)
{
    const ImageText& text = image.text();
    const ImageIdentifier& record = text.identifier(number);
    if (record.kind() != PropertyKeyKind::Text || !record.text())
        return String();
    return text.createAtom(record.text());
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
