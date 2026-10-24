/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringConstantNumbering.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTVector.h"
#include "SmallStrings.h"

namespace JSC { namespace AOT {

static bool hasCellInVM(std::span<const uint8_t> text)
{
    constexpr size_t maxEncodedSizeOfSingleCharacterString = 2;
    if (text.size() > maxEncodedSizeOfSingleCharacterString)
        return false;
    Vector<char16_t> units = Typed::decodeProgramString(text);
    return units.isEmpty() || (units.size() == 1 && units[0] <= maxSingleCharacterString);
}

static BitVector findStringsToNumber(const Typed::Program& program, const StringUses& uses)
{
    BitVector needsNumber(program.strings.size());
    needsNumber.merge(uses.isConstantInTables);
    for (size_t string : uses.isCreatedRepeatedly) {
        if (uses.isConstant.get(string) && !hasCellInVM(program.strings[StringID(string)]))
            needsNumber.quickSet(string);
    }
    return needsNumber;
}

StringConstantNumbering numberStringConstants(const Typed::Program& program, const StringUses& uses, BuildContext& context)
{
    BitVector needsNumber = findStringsToNumber(program, uses);

    StringConstantNumbering numbering;
    numbering.m_numbersPlusOne.fill(0, program.strings.size());
    numbering.m_strings.reserveInitialCapacity(needsNumber.bitCount());
    auto numberIfNeeded = [&](size_t string) {
        if (!needsNumber.quickGet(string) || numbering.m_numbersPlusOne[string])
            return;
        numbering.m_strings.append(StringID(string));
        numbering.m_numbersPlusOne[string] = numbering.count();
    };
    for (uint32_t function : uses.functionsInSourceOrder) {
        for (uint32_t string : uses.namedByFunction[function])
            numberIfNeeded(string);
    }
    for (size_t string : needsNumber)
        numberIfNeeded(string);

    context.statistics().count(StatisticID::StringConstantsNumbered, numbering.count());
    return numbering;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
