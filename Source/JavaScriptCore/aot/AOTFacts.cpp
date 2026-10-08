/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFacts.h"

#if ENABLE(AOT)

#include "JSCInlines.h"
#include "Options.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Facts);

Facts* Facts::s_shared;

void Facts::load(VM& vm, std::span<const uint8_t> bytes)
{
    delete std::exchange(s_shared, nullptr);
    if (bytes.empty())
        return;
    RELEASE_ASSERT(bytes.size() >= 12 && !memcmp(bytes.data(), "BFCT", 4));
    auto facts = makeUnique<Facts>();
    size_t numberOfWords = bytes.size() / 4;
    facts->m_words.grow(numberOfWords);
    memcpy(facts->m_words.mutableSpan().data(), bytes.data(), numberOfWords * 4);
    auto& words = facts->m_words;
    size_t at = 1;
    auto word = [&] {
        RELEASE_ASSERT(at < words.size());
        return words[at++];
    };
    auto skip = [&](size_t count) {
        RELEASE_ASSERT(count <= words.size() - at);
        at += count;
    };
    RELEASE_ASSERT_WITH_MESSAGE(word() == 2, "The facts are of another version");
    facts->m_flags = word();
    Vector<std::pair<uint32_t, uint32_t>> names;
    for (uint32_t count = word(); count--;) {
        uint32_t offset = word();
        names.append({ offset, word() });
    }
    for (uint32_t count = word(); count--;) {
        facts->m_types.append(at);
        skip(1);
        skip(word());
        skip(word());
    }
    for (uint32_t count = word(); count--;) {
        facts->m_shapes.append(at);
        skip(6);
        skip(3 * static_cast<size_t>(word()));
        skip(word());
    }
    for (uint32_t count = word(); count--;) {
        facts->m_functions.append(at);
        skip(4);
        skip(word());
    }
    for (uint32_t count = word(); count--;) {
        auto kind = static_cast<SiteKind>(word());
        uint32_t number = word();
        facts->m_sites.append({ kind, number, word() });
    }
    size_t sizeOfNames = word();
    RELEASE_ASSERT(at * 4 + sizeOfNames == bytes.size());
    auto text = bytes.subspan(at * 4);
    for (auto [offset, length] : names) {
        RELEASE_ASSERT(offset <= text.size() && length <= text.size() - offset);
        bool isNameOfSymbol = length && text[offset] == 0xfe;
        if (!length || isNameOfSymbol) {
            facts->m_names.append(Identifier());
            continue;
        }
        facts->m_names.append(Identifier::fromString(vm, WTF::String::fromUTF8(text.subspan(offset, length))));
        facts->m_numbersOfNames.add(facts->m_names.last().impl(), facts->m_names.size() - 1);
    }
    for (uint32_t number = 1; number < facts->m_shapes.size(); ++number) {
        auto shape = facts->shape(number);
        if (shape.kind == ShapeKind::Instance && shape.of)
            facts->m_shapesOfInstances.set(shape.of, number);
    }
    s_shared = facts.release();
}

void Facts::noteFunction(UnlinkedFunctionExecutable* executable, StringView source, unsigned parametersStart)
{
    static constexpr unsigned lengthOfTag = 7;
    if (!s_shared || parametersStart < lengthOfTag || parametersStart > source.length() || source[parametersStart - lengthOfTag] != 1)
        return;
    uint32_t tag = 0;
    for (unsigned i = parametersStart - lengthOfTag + 1; i < parametersStart; ++i) {
        char16_t character = source[i];
        unsigned digit;
        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (character >= 'A' && character <= 'Z')
            digit = character - 'A' + 10;
        else if (character >= 'a' && character <= 'z')
            digit = character - 'a' + 36;
        else if (character == '_')
            digit = 62;
        else if (character == '$')
            digit = 63;
        else
            return;
        tag = tag << 6 | digit;
    }
    Site site = s_shared->site(tag);
    if (site.kind != SiteKind::Function)
        return;
    Locker locker { s_shared->m_lock };
    s_shared->m_numbersOfFunctions.set(executable, site.number);
    s_shared->m_executables.add(site.number, Vector<UnlinkedFunctionExecutable*, 1> { }).iterator->value.append(executable);
}

Vector<UnlinkedFunctionExecutable*, 1> Facts::executablesOf(uint32_t function) const
{
    Locker locker { m_lock };
    return m_executables.get(function);
}

void Facts::noteCode(UnlinkedCodeBlock* codeBlock, UnlinkedFunctionExecutable* executable)
{
    if (!s_shared)
        return;
    Locker locker { s_shared->m_lock };
    auto it = s_shared->m_numbersOfFunctions.find(executable);
    if (it != s_shared->m_numbersOfFunctions.end())
        s_shared->m_numbersOfFunctionsWithCode.set(codeBlock, it->value);
}

std::optional<uint32_t> Facts::numberOfFunctionWithCode(UnlinkedCodeBlock* codeBlock) const
{
    Locker locker { m_lock };
    auto it = m_numbersOfFunctionsWithCode.find(codeBlock);
    if (it == m_numbersOfFunctionsWithCode.end())
        return std::nullopt;
    return it->value;
}

std::optional<uint32_t> Facts::numberOf(UnlinkedFunctionExecutable* executable) const
{
    Locker locker { m_lock };
    auto it = m_numbersOfFunctions.find(executable);
    if (it == m_numbersOfFunctions.end())
        return std::nullopt;
    return it->value;
}

void Facts::noteFieldInitializer(UnlinkedCodeBlock* initializer, uint32_t typeOfThis, uint32_t function, std::span<UnlinkedCodeBlock* const> constructors) const
{
    Locker locker { m_lock };
    m_typesOfThisInFieldInitializers.set(initializer, typeOfThis);
    for (UnlinkedCodeBlock* constructor : constructors) {
        if (constructor)
            m_fieldInitializers.set(constructor, function);
    }
}

std::optional<uint32_t> Facts::typeOfThisInFieldInitializer(UnlinkedCodeBlock* codeBlock) const
{
    Locker locker { m_lock };
    auto it = m_typesOfThisInFieldInitializers.find(codeBlock);
    if (it == m_typesOfThisInFieldInitializers.end())
        return std::nullopt;
    return it->value;
}

uint32_t Facts::fieldInitializerCalledBy(UnlinkedCodeBlock* constructor) const
{
    Locker locker { m_lock };
    return m_fieldInitializers.get(constructor);
}

auto Facts::type(uint32_t number) const -> TypeFact
{
    if (number >= m_types.size())
        return { static_cast<uint32_t>(IsUnknown), { }, { } };
    auto words = m_words.span().subspan(m_types[number]);
    uint32_t strings = words[1];
    uint32_t shapes = words[2 + strings];
    return { words[0], words.subspan(2, strings), words.subspan(3 + strings, shapes) };
}

auto Facts::shape(uint32_t number) const -> Shape
{
    if (!number || number >= m_shapes.size())
        return { };
    auto words = m_words.span().subspan(m_shapes[number]);
    uint32_t fields = words[6];
    static_assert(sizeof(Field) == 12);
    auto* firstField = reinterpret_cast<const Field*>(words.data() + 7);
    uint32_t slots = words[7 + 3 * fields];
    return { static_cast<ShapeKind>(words[0]), words[1], words[2], words[3], words[4], words[5], std::span { firstField, fields }, words.subspan(8 + 3 * fields, slots) };
}

auto Facts::function(uint32_t number) const -> Function
{
    if (number >= m_functions.size())
        return { };
    auto words = m_words.span().subspan(m_functions[number]);
    return { words[0], words[1], words[2], words[3], words.subspan(5, words[4]) };
}

auto Facts::field(uint32_t shapeNumber, UniquedStringImpl* name) const -> const Field*
{
    auto it = m_numbersOfNames.find(name);
    if (it == m_numbersOfNames.end())
        return nullptr;
    for (auto& field : shape(shapeNumber).fields) {
        if (field.name == it->value)
            return &field;
    }
    return nullptr;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
