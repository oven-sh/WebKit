/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ImmutableIntrinsics.h"

#include "BuiltinExecutables.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include <wtf/Lock.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImmutableIntrinsics);

static std::atomic<const ImmutableIntrinsics*> s_shared;

const ImmutableIntrinsics* ImmutableIntrinsics::shared()
{
    return s_shared.load(std::memory_order_acquire);
}

void ImmutableIntrinsics::ensureShared(VM& vm)
{
    if (!vm.useImmutableIntrinsics || shared())
        return;
    JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    RELEASE_ASSERT(shared());
}

static uint64_t keyFor(unsigned holder, const StringImpl& name)
{
    return static_cast<uint64_t>(holder + 1) << 32 | name.hash();
}

unsigned ImmutableIntrinsics::find(unsigned holder, const StringImpl& name) const
{
    if (name.isSymbol())
        return 0;
    auto it = m_numbers.find(keyFor(holder, name));
    if (it == m_numbers.end() || !WTF::equal(m_entries[it->value].name.impl(), &name))
        return 0;
    return it->value;
}

Vector<EncodedJSValue> ImmutableIntrinsics::describe(JSGlobalObject* globalObject, std::span<const ASCIILiteral> variableNames)
{
    VM& vm = globalObject->vm();
    auto description = std::unique_ptr<ImmutableIntrinsics>(new ImmutableIntrinsics);
    Vector<EncodedJSValue> values;
    UncheckedKeyHashMap<JSCell*, uint16_t> numberOfCell;

    auto add = [&](unsigned holder, const StringImpl& name, JSValue value) {
        RELEASE_ASSERT(values.size() < maximumCount);
        uint16_t number = values.size();
        Entry entry;
        entry.name = String(name.isolatedCopy());
        entry.holder = holder;
        entry.canonical = number;
        if (value.isCell()) {
            entry.isCell = true;
            entry.type = value.asCell()->type();
            entry.canonical = numberOfCell.add(value.asCell(), number).iterator->value;
            if (auto* function = dynamicDowncast<JSFunction>(value.asCell()); function && function->isBuiltinFunction()) {
                if (auto index = vm.builtinExecutables()->indexOf(function->jsExecutable()->unlinkedExecutable()))
                    entry.builtinCode = *index + 1;
            } else if (function)
                entry.isHostFunction = function->isHostFunction();
        } else
            entry.primitive = JSValue::encode(value);
        if (number && !description->m_numbers.add(keyFor(holder, name), number).isNewEntry)
            description->m_numbers.set(keyFor(holder, name), 0);
        description->m_hash = WTF::pairIntHash(description->m_hash, WTF::pairIntHash(name.hash(), holder << 16 | entry.canonical)) + entry.type + entry.isCell + entry.isHostFunction;
        description->m_entries.append(WTF::move(entry));
        values.append(JSValue::encode(value));
    };

    add(0, *StringImpl::empty(), globalObject);
    for (ASCIILiteral name : variableNames) {
        Identifier identifier = Identifier::fromString(vm, name);
        add(ImmutableIntrinsics::globalObject, *identifier.impl(), globalObject->getDirect(vm, identifier));
    }
    for (unsigned number = 1; number < values.size(); ++number) {
        JSObject* object = JSValue::decode(values[number]).getObject();
        if (!object || description->m_entries[number].canonical != number || !object->structure()->inheritorsMayOverrideReadOnlyProperties())
            continue;
        object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& property) {
            constexpr unsigned fixed = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
            if (!property.key()->isSymbol() && (property.attributes() & fixed) == fixed && !(property.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue))
                add(number, *property.key(), object->getDirect(property.offset()));
            return true;
        });
    }

    static Lock lock;
    Locker locker { lock };
    if (const ImmutableIntrinsics* first = shared())
        RELEASE_ASSERT(first->count() == description->count() && first->hash() == description->hash());
    else
        s_shared.store(description.release(), std::memory_order_release);
    return values;
}

} // namespace JSC
