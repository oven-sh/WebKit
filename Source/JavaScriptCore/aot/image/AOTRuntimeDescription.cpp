/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeDescription.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIdentifierTable.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTOperationData.h"
#include "AOTRegExpRecord.h"
#include "AOTRuntimeFactsOfLayoutsIncludes.h"
#include "AOTRuntimeTable.h"
#include "AOTStubBlob.h"
#include "AOTStubRegisters.h"
#include "FFIStubs.h"
#include <bit>
#include <cstring>
#include <iterator>

namespace JSC { namespace AOT {

static constexpr ASCIILiteral namesOfFacts[] = {
#define AOT_RUNTIME_FACT(name, value) name ""_s,
#include "AOTRuntimeFacts.def"
#undef AOT_RUNTIME_FACT
};
static constexpr unsigned numberOfRuntimeFacts = std::size(namesOfFacts);

struct DescribedRuntime {
    RuntimeDescription header;
    uint32_t facts[numberOfRuntimeFacts];
};
static_assert(offsetof(DescribedRuntime, facts) == sizeof(RuntimeDescription));

static constexpr uint32_t checkedRuntimeFact(uint64_t value)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(value <= UINT32_MAX);
    return static_cast<uint32_t>(value);
}

static constexpr DescribedRuntime thisRuntime = {
    {
        RuntimeDescription::expectedMagic,
        sizeof(RuntimeDescription) + sizeof(DescribedRuntime::facts),
        numberOfRuntimeFacts,
        CPUFeatureSet::buildTarget().bits(),
        static_cast<uint8_t>(hostArchitecture()),
#if OS(LINUX)
        RuntimeDescription::OperatingSystem::Linux,
#elif OS(DARWIN)
        RuntimeDescription::OperatingSystem::Darwin,
#elif OS(WINDOWS)
        RuntimeDescription::OperatingSystem::Windows,
#elif OS(FREEBSD)
        RuntimeDescription::OperatingSystem::FreeBSD,
#else
        RuntimeDescription::OperatingSystem::Other,
#endif
        static_cast<uint8_t>(imageFormOfThisRuntime()),
        (ASSERT_ENABLED ? RuntimeDescription::Assertions : 0) | (ASAN_ENABLED ? RuntimeDescription::AddressSanitizer : 0),
        imageFormatVersion,
    },
    {
#define AOT_RUNTIME_FACT(name, value) checkedRuntimeFact(value),
#include "AOTRuntimeFacts.def"
#undef AOT_RUNTIME_FACT
    },
};

// It lies beside imageStamp(), which every run time has, so that no linker leaves it out.
extern "C" AOT_IN_RUNTIME_DESCRIPTION_SECTION AOT_KEPT_BY_EVERY_LINKER constinit const DescribedRuntime describedRuntime = thisRuntime;

std::span<const uint8_t> descriptionOfThisRuntime()
{
    return { reinterpret_cast<const uint8_t*>(&describedRuntime), describedRuntime.header.size };
}

std::span<const ASCIILiteral> namesOfRuntimeFacts()
{
    return namesOfFacts;
}

static constexpr void mixIntoStamp(uint64_t& stamp, uint64_t value)
{
    stamp = (stamp ^ value) * 0x100000001b3ULL;
    stamp ^= stamp >> 29;
}

uint64_t imageStamp(CPUFeatureSet features, const std::array<uint8_t, 16>& identity, Wire::ImageForm form)
{
    static constexpr uint64_t stampOfFacts = [] {
        uint64_t stamp = imageFormatVersion;
        for (uint32_t fact : thisRuntime.facts)
            mixIntoStamp(stamp, fact);
        return stamp;
    }();
    uint64_t stamp = stampOfFacts;
    for (uint64_t word : std::bit_cast<std::array<uint64_t, 2>>(identity))
        mixIntoStamp(stamp, word);
    mixIntoStamp(stamp, static_cast<uint64_t>(hostArchitecture()) | static_cast<uint64_t>(features.bits()) << 8);
    if (form == Wire::ImageForm::PositionIndependent)
        mixIntoStamp(stamp, sizeof(ImageStringRecord));
    return stamp;
}

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
    if (there.imageForm > static_cast<uint8_t>(Wire::ImageForm::PositionIndependent))
        return RuntimeDifference { "the form of the image"_s, static_cast<uint64_t>(here.imageForm), static_cast<uint64_t>(there.imageForm) };
    if (there.imageFormatVersion != here.imageFormatVersion)
        return RuntimeDifference { "the version of the image format"_s, here.imageFormatVersion, there.imageFormatVersion };
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
