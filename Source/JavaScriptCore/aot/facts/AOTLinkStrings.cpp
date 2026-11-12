/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkStrings.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include <algorithm>
#include <wtf/ASCIICType.h>

namespace JSC { namespace AOT { namespace Typed {

bool keyThatBeginsSoMayNamePrototypeLink(const Program& program, const SummaryAccess& access)
{
    StringID beginningOfKey = access.beginningOfKey();
    if (!beginningOfKey)
        return true;
    std::span<const uint8_t> beginning = program.string(beginningOfKey);
    std::span<const uint8_t> nameOfLink = program.string(program.stringOf(BuiltinNames::__proto__));
    return beginning.size() <= nameOfLink.size() && std::ranges::equal(beginning, nameOfLink.first(beginning.size()));
}

bool KeyAffixes::mayMatch(std::span<const uint8_t> name) const
{
    return name.size() >= beginning.size() + ending.size() && std::ranges::equal(name.first(beginning.size()), beginning) && std::ranges::equal(name.last(ending.size()), ending);
}

enum class AffixSide : bool { Beginning, Ending };

static std::span<const uint8_t> affixOfValue(const Program& program, const BodyIndex& index, ValueID value, AffixSide side)
{
    while (value) {
        value = index.stripCasts(value);
        if (index.isParameter(value))
            return { };
        BodyValue definition = index.value(value);
        switch (definition.opcode()) {
        case Opcode::String: {
            std::span<const uint8_t> affix = program.string(definition.as<Opcode::String>().string());
            return std::ranges::all_of(affix, isASCII<uint8_t>) ? affix : std::span<const uint8_t>();
        }
        case Opcode::Add: {
            auto sum = definition.as<Opcode::Add>();
            value = side == AffixSide::Beginning ? sum.leftOperand() : sum.rightOperand();
            break;
        }
        case Opcode::Strcat: {
            std::span<const ValueID> parts = definition.as<Opcode::Strcat>().partsOperands();
            value = parts.empty() ? ValueID() : side == AffixSide::Beginning ? parts.front() : parts.back();
            break;
        }
        default:
            return { };
        }
    }
    return { };
}

KeyAffixes affixesOfKey(const Program& program, const BodyIndex& index, ValueID key)
{
    key = key ? index.stripCasts(key) : ValueID();
    if (!key || index.isParameter(key) || (index.opcodeOf(key) != Opcode::Add && index.opcodeOf(key) != Opcode::Strcat))
        return { };
    KeyAffixes affixes { affixOfValue(program, index, key, AffixSide::Beginning), affixOfValue(program, index, key, AffixSide::Ending) };
    bool mayBeIndex = (affixes.beginning.empty() || isASCIIDigit(affixes.beginning.front())) && (affixes.ending.empty() || isASCIIDigit(affixes.ending.back()));
    return mayBeIndex ? KeyAffixes() : affixes;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
