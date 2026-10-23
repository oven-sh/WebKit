/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeDescription.h"

#if ENABLE(AOT)

#include "AOTRuntimeFactValues.h"
#include <cstring>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral namesOfFacts[] = {
#define AOT_RUNTIME_FACT(name, value) name ""_s,
#include "AOTRuntimeFacts.def"
#undef AOT_RUNTIME_FACT
};
static_assert(std::size(namesOfFacts) == numberOfRuntimeFacts);

static std::optional<RuntimeDifference> findDifferenceFromThisTable(const BuiltinTableDescription& there)
{
    struct Fact {
        ASCIILiteral name;
        uint32_t BuiltinTableDescription::* value;
    };
    static constexpr Fact facts[] = {
        { "the names of the built-ins in the order of their numbers"_s, &BuiltinTableDescription::hashOfBuiltinTokens },
        { "the number of built-ins"_s, &BuiltinTableDescription::numberOfBuiltins },
        { "the number of overloads of built-ins"_s, &BuiltinTableDescription::numberOfOverloads },
        { "the number of types of built-ins"_s, &BuiltinTableDescription::numberOfTypes },
        { "the number of classes of built-ins"_s, &BuiltinTableDescription::numberOfClasses },
        { "the number of slots of built-ins"_s, &BuiltinTableDescription::numberOfSlots },
        { "the number of layouts of built-ins"_s, &BuiltinTableDescription::numberOfLayouts },
        { "the number of modules of the run time"_s, &BuiltinTableDescription::numberOfModules },
        { "the number of functions of the run time"_s, &BuiltinTableDescription::numberOfFunctions },
    };
    if (there.size != sizeof(there))
        return RuntimeDifference { };
    for (const Fact& fact : facts) {
        if (there.*fact.value != describedBuiltinTable.*fact.value)
            return RuntimeDifference { fact.name, describedBuiltinTable.*fact.value, there.*fact.value };
    }
    return std::nullopt;
}

std::optional<RuntimeDifference> findDifferenceFromThisRuntime(std::span<const uint8_t> other)
{
    if (BuiltinTableDescription table; other.size() >= sizeof(table)) {
        memcpy(&table, other.data(), sizeof(table));
        if (table.magic == BuiltinTableDescription::expectedMagic)
            return findDifferenceFromThisTable(table);
    }

    const RuntimeDescription& here = describedRuntime.header;
    RuntimeDescription there;
    if (other.size() < sizeof(there))
        return RuntimeDifference { };
    memcpy(&there, other.data(), sizeof(there));
    if (there.magic != RuntimeDescription::expectedMagic || there.size > other.size() || there.size != sizeof(there) + there.numberOfFacts * sizeof(uint32_t))
        return RuntimeDifference { };

    if (there.architecture != here.architecture)
        return RuntimeDifference { "the architecture"_s, here.architecture, there.architecture };
    if (there.operatingSystem != here.operatingSystem)
        return RuntimeDifference { "the operating system"_s, static_cast<uint64_t>(here.operatingSystem), static_cast<uint64_t>(there.operatingSystem) };
    if (there.imageForm > RuntimeDescription::ImageForm::PositionIndependent)
        return RuntimeDifference { "the form of the image"_s, static_cast<uint64_t>(here.imageForm), static_cast<uint64_t>(there.imageForm) };
    if (there.cpuFeatures != here.cpuFeatures)
        return RuntimeDifference { "the set of CPU features that the build requires"_s, here.cpuFeatures, there.cpuFeatures };
    if (there.checks != here.checks)
        return RuntimeDifference { "assertions and sanitizers"_s, here.checks, there.checks };
    if (there.numberOfFacts != here.numberOfFacts)
        return RuntimeDifference { "the number of facts in the description"_s, here.numberOfFacts, there.numberOfFacts };
    for (unsigned index = 0; index < numberOfRuntimeFacts; ++index) {
        uint32_t fact;
        memcpy(&fact, other.data() + sizeof(there) + index * sizeof(fact), sizeof(fact));
        if (fact != describedRuntime.facts[index])
            return RuntimeDifference { namesOfFacts[index], describedRuntime.facts[index], fact };
    }
    return std::nullopt;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
