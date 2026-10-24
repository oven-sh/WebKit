/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONKeyOrder.h"
#include "AOTJSONSource.h"
#include "AOTJSONValueParser.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "JSCJSValue.h"
#include "Structure.h"
#include <array>
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

class CacheRegistry;
class ObjectBuilderPlan;
struct Instance;

class JSONRuntime {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(JSONRuntime);
    WTF_MAKE_NONCOPYABLE(JSONRuntime);
public:
    JSONRuntime(Instance&, CacheRegistry&);
    ~JSONRuntime();

    bool open(JSValue textOrBytes, void* stackPointer);
    bool open(StringView text, void* stackPointer);
    bool open(std::span<const uint8_t> utf8, void* stackPointer);
    bool close(const uint8_t* cursor);
    void abandon();
    void dropSourcesBelow(void* stackPointer);

    void clear();

    Instance& instance() const { return m_instance; }
    const StringImpl* wideText() const { return m_wideText.get(); }
    JSONKeyOrderLists& keyOrderLists() { return m_keyOrderLists; }
    size_t numberOfSuspendedSources() const { return m_suspendedSources.size(); }
    size_t sourceCapacity() const { return m_sourceCapacity; }

    struct RecordPlan {
        const ObjectBuilderPlan* plan { nullptr };
        Structure* structure { nullptr };
    };

    JSONValueParser::LearnedNames& learnedNames();

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

private:
    struct CachedRecordPlan {
        uint32_t ordinaryStructureID { 0 };
        RecordPlan recordPlan;
    };
    struct RecordPlanForNames {
        Vector<RefPtr<UniquedStringImpl>, 8> names;
        std::unique_ptr<ObjectBuilderPlan> plan;
    };
    static constexpr size_t recordPlanCacheSize = 128;
    AOT_CLASS_THRESHOLD(size_t, maxRecordPlans, 256);
    using RecordPlanCache = std::array<CachedRecordPlan, recordPlanCacheSize>;
    static constexpr size_t recordPlanCacheIndex(uint32_t structureID) { return (structureID >> 4) % recordPlanCacheSize; }

    RecordPlan findRecordPlan(VM&, Structure*);
    const ObjectBuilderPlan* findOrCreateRecordPlan(VM&, Structure*);
    std::unique_ptr<ObjectBuilderPlan> createRecordPlan(VM&, std::span<UniquedStringImpl* const> names);

    uint8_t* begin(void* stackPointer, size_t length);
    uint8_t* beginBesideOpenSource(void* stackPointer, size_t length);
    uint8_t* beginIn(uint8_t* start, void* stackPointer, size_t length, bool isNested);
    void end();
    void suspend();
    void resume();
    uint8_t* reserve(size_t size);
    uint8_t* replace(size_t size);
    void freeCurrent();
    void freeAll();

    bool openLatin1(std::span<const Latin1Character>, void* stackPointer);
    bool open16Bit(std::span<const char16_t>, void* stackPointer);
    bool openUTF8(std::span<const uint8_t>, void* stackPointer);

    Instance& m_instance;
    bool m_hasParsedRecords { false };
    std::unique_ptr<RecordPlanCache> m_recordPlanCache;
    std::unique_ptr<JSONValueParser::LearnedNames> m_learnedNames;
    Vector<RecordPlanForNames> m_recordPlans;
    Vector<JSONSource, 2> m_suspendedSources;
    RefPtr<StringImpl> m_wideText;
    size_t m_sourceCapacity { 0 };
    JSONKeyOrderLists m_keyOrderLists;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
