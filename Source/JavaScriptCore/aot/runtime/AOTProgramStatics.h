/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "RememberedSlot.h"
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class HeapAnalyzer;

namespace AOT {

struct State;

class ProgramStatics {
    WTF_MAKE_NONCOPYABLE(ProgramStatics);
    WTF_MAKE_TZONE_ALLOCATED(ProgramStatics);
public:
    static constexpr unsigned cardShift = InstanceLayout::cardShift;
    static constexpr uint8_t cardIsDirty = RememberedSlotSet::writtenCard;

    explicit ProgramStatics(State&);

    void putBack(uint32_t offset, uint32_t size);

    struct VariableWithViewMirror {
        void refresh();
        static void refreshViewsOfOneBuffer(std::span<VariableWithViewMirror>);

        EncodedJSValue variable;
        void* vector;
        uint64_t length;
        uint64_t lengthOfBuffer;
    };
    std::span<VariableWithViewMirror> viewMirrorVariables(const StaticsRange&);
    void refreshViewMirrors();
    void didWriteVariable(uint32_t offset);
    void didMoveStorageOf(JSCell* view);
    void didStoreRootInStatics(uint32_t offset) { m_slots.record(m_instance.statics() + offset); }
    void setCell(uint32_t offset, JSCell* cell) { at<RememberedSlot<JSCell*>>(offset).set(m_slots, cell); }

    template<typename T> T& at(uint32_t offset)
    {
        RELEASE_ASSERT(offset + sizeof(T) <= m_tables.instanceSizes.sizeOfStatics);
        return *reinterpret_cast<T*>(m_instance.statics() + offset);
    }

    static JSValue readVariable(Instance&, SlotData);
    static JSValue readVariableWithoutAllocating(const Instance&, SlotData);
    static bool isVariableEmpty(const Instance&, SlotData);
    static void writeVariable(Instance&, SlotData, JSValue);
    static void writeNamespace(Instance& instance, SlotData slot, JSValue value) { write(instance, slot, value); }
    static void emptyVariableWhoseValueHasMoved(Instance&, SlotData);

    template<typename Visitor> void visitRoots(Visitor&);

    void willAnalyzeHeap(VM&);
    JSCell* environmentOfModuleThatDefines(uint32_t closureNumber) const;
    JSCell* environmentOfModule(uint32_t module) const;
    void analyzeVariableOfModule(JSCell* standIn, JSCell* held, UniquedStringImpl* name, HeapAnalyzer&) const;
    JSCell* scopeInHeapSnapshotOf(JSCell* closure) const;
    void analyzeVariablesOfModule(JSCell* standIn, uint32_t module, HeapAnalyzer&) const;

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    std::span<const VariableData> namedVariablesOfModule(uint32_t module) const;
    JSCell* cellIn(SlotData) const;
    static void write(Instance&, SlotData, JSValue);

    Instance& m_instance;
    const ProgramTables& m_tables;
    RememberedSlotSet m_slots;
    Vector<VariableData> m_namedVariables;
    Vector<uint32_t> m_firstNamedVariablesOfModules;
    Vector<uint32_t> m_modulesInHeapSnapshots;
    Vector<uint32_t> m_placesOfModulesOfClosureNumbers;
    Structure* m_structureOfModuleEnvironments { nullptr };
    Vector<JSCell*> m_environmentsOfModules;
    Structure* m_structureOfScopesOfClosures { nullptr };
    struct ScopesOfClosures {
        void clear();

        VM* vm { nullptr };
        Vector<std::pair<JSCell*, JSCell*>> list;
        mutable UncheckedKeyHashSet<std::tuple<JSCell*, JSCell*, UniquedStringImpl*>> variablesShown;
    };
    ScopesOfClosures m_scopesOfClosures;
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
