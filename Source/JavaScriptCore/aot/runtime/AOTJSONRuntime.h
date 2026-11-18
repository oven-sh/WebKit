/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONSource.h"
#include "AOTJSONValueParser.h"
#include "AOTJSONWords.h"
#include "AOTLayoutData.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "Allocator.h"
#include "JSCJSValue.h"
#include "Structure.h"
#include "WeakGCMap.h"
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

class CacheRegistry;
class CompiledStructure;
class ObjectBuilderPlan;
struct Instance;

class JSONRuntime {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(JSONRuntime);
    WTF_MAKE_NONCOPYABLE(JSONRuntime);
public:
    JSONRuntime(Instance&, CacheRegistry&, ZeroedTableArena&);
    ~JSONRuntime();

    bool open(StringView text, JSONTextMemory, void* stackPointer);
    void abandon();

    void clear();

    const StringImpl* wideText() const { return m_wideText.get(); }

    JSONValueParser::LearnedNames& learnedNames();
    JSONValueParser::LearnedStrings& learnedStrings();
    JSONValueParser::RepeatedValues& repeatedValues();

    struct RecordPlan {
        const ObjectBuilderPlan* plan { nullptr };
        Structure* structure { nullptr };
    };
    bool hasParsedRecords() const { return m_hasParsedRecords; }
    ALWAYS_INLINE RecordPlan recordPlanFor(VM& vm, Structure* ordinaryStructure)
    {
        uint32_t id = ordinaryStructure->id().bits();
        if (m_recordPlanCache) [[likely]] {
            const CachedRecordPlan& cached = (*m_recordPlanCache)[recordPlanCacheIndex(id)];
            if (cached.ordinaryStructureID == id) [[likely]]
                return cached.recordPlan;
        }
        return findRecordPlan(vm, ordinaryStructure);
    }
    struct RecordBesideNames {
        const ObjectBuilderPlan* plan { nullptr };
        Structure* structure { nullptr };
        unsigned numberOfNamesInRecord { 0 };
        Structure* structureOfNamesBeyondRecord { nullptr };
        CompiledStructure* structureWithNamesBeyondRecord { nullptr };
    };
    RecordBesideNames* recordBesideNamesFor(Structure* ordinaryStructure)
    {
        uint32_t id = ordinaryStructure->id().bits();
        CachedRecordPlan& cached = (*m_recordPlanCache)[recordPlanCacheIndex(id)];
        return cached.ordinaryStructureID == id && cached.besideNames.plan ? &cached.besideNames : nullptr;
    }

private:
    struct CachedRecordPlan {
        uint32_t ordinaryStructureID { 0 };
        RecordPlan recordPlan;
        RecordBesideNames besideNames;
    };
    struct RecordPlanForNames {
        Vector<RefPtr<UniquedStringImpl>, 8> names;
        std::unique_ptr<ObjectBuilderPlan> plan;
        uint32_t numberOfCollectionsWhenLastFound { 0 };
        uint64_t numberOfTimesAskedWhenLastFound { 0 };
    };
    static constexpr size_t recordPlanCacheSize = 128;
    static constexpr uint32_t fewestRecordPlansLookedThrough = 256;
    using RecordPlanCache = std::array<CachedRecordPlan, recordPlanCacheSize>;
    static constexpr size_t recordPlanCacheIndex(uint32_t structureID) { return (structureID >> 4) % recordPlanCacheSize; }

    RecordPlan findRecordPlan(VM&, Structure*);
    struct RecordPlanFound {
        const ObjectBuilderPlan* plan { nullptr };
        unsigned numberOfNamesInRecord { 0 };
        bool hasNamesBeyondRecord { false };
    };
    RecordPlanFound findOrCreateRecordPlan(VM&, Structure*);
    void forgetRecordPlansNotFoundLately(VM&);

    uint8_t* begin(void* stackPointer, size_t length);
    void beginIn(const uint8_t* start, void* stackPointer, size_t length);
    void end();
    uint8_t* reserve(size_t size);
    uint8_t* replace(size_t size);
    void freeCurrent();

    bool tryOpenInPlace(std::span<const Latin1Character>, void* stackPointer);
    bool openLatin1(std::span<const Latin1Character>, void* stackPointer);
    bool open16Bit(std::span<const char16_t>, void* stackPointer);

    Instance& m_instance;
    bool m_hasParsedRecords { false };
    MadeInArena<RecordPlanCache> m_recordPlanCache;
    MadeInArena<JSONValueParser::LearnedNames> m_learnedNames;
    MadeInArena<JSONValueParser::LearnedStrings> m_learnedStrings;
    MadeInArena<JSONValueParser::RepeatedValues> m_repeatedValues;
    HashMap<uint32_t, Vector<RecordPlanForNames, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_recordPlans;
    uint64_t m_numberOfTimesAskedForRecordPlan { 0 };
    uint64_t m_numberOfTimesAskedWhenPlansWereForgotten { 0 };
    uint32_t m_numberOfCollections { 0 };
    uint32_t m_numberOfRecordPlans { 0 };
    uint32_t m_mostRecordPlansHeld { 0 };
    std::unique_ptr<WeakGCMap<Structure*, Structure>> m_structuresThatNoPlanFits;
    RefPtr<StringImpl> m_wideText;
    uint8_t* m_sourceCopy { nullptr };
    size_t m_sourceCapacity { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
