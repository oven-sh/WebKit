/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2008-2023 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "JSObject.h"
#include <wtf/Atomics.h>

namespace JSC {

class DateInstance final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell* cell)
    {
        static_cast<DateInstance*>(cell)->DateInstance::~DateInstance();
    }

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.dateInstanceSpace();
    }

    static DateInstance* create(VM& vm, Structure* structure, double date)
    {
        DateInstance* instance = new (NotNull, allocateCell<DateInstance>(vm)) DateInstance(vm, structure);
        instance->finishCreation(vm, date);
        return instance;
    }

    static DateInstance* create(VM& vm, Structure* structure)
    {
        DateInstance* instance = new (NotNull, allocateCell<DateInstance>(vm)) DateInstance(vm, structure);
        instance->finishCreation(vm);
        return instance;
    }

    // With useJSThreads, m_internalNumber can be written by one thread (e.g. Date.prototype.setTime)
    // while another thread reads it (e.g. gregorianDateTime{UTC} cache validation). The spec
    // (SPEC-ungil.md) does not bless torn doubles here, so route all accesses through relaxed
    // 64-bit atomic loads/stores of the same storage word. The field itself stays a plain double:
    // layout, offsetOfInternalNumber() JIT users, and flag-off codegen are unchanged (relaxed
    // 64-bit load/store compiles to the same plain move on all supported targets).
    double internalNumber() const
    {
        return std::bit_cast<double>(std::bit_cast<const WTF::Atomic<uint64_t>*>(&m_internalNumber)->loadRelaxed());
    }
    void setInternalNumber(double value)
    {
        std::bit_cast<WTF::Atomic<uint64_t>*>(&m_internalNumber)->storeRelaxed(std::bit_cast<uint64_t>(value));
    }

    DECLARE_EXPORT_INFO;

    // Snapshot-taking overloads (SPEC-ungil §N.3 residue): an operation that pairs the
    // GregorianDateTime with anything else derived from the internal time value (e.g.
    // toISOString's ms-of-second) must load internalNumber() exactly ONCE and pass the
    // snapshot here, so the conversion cannot re-fetch a concurrently-updated value.
    const GregorianDateTime* gregorianDateTime(DateCache& cache, double milli) const
    {
        if (m_data && m_data->m_gregorianDateTimeCachedForMS == milli)
            return &m_data->m_cachedGregorianDateTime;
        return calculateGregorianDateTime(cache, milli);
    }

    const GregorianDateTime* gregorianDateTime(DateCache& cache) const
    {
        return gregorianDateTime(cache, internalNumber());
    }

    const GregorianDateTime* gregorianDateTimeUTC(DateCache& cache, double milli) const
    {
        if (m_data && m_data->m_gregorianDateTimeUTCCachedForMS == milli)
            return &m_data->m_cachedGregorianDateTimeUTC;
        return calculateGregorianDateTimeUTC(cache, milli);
    }

    const GregorianDateTime* gregorianDateTimeUTC(DateCache& cache) const
    {
        return gregorianDateTimeUTC(cache, internalNumber());
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static constexpr ptrdiff_t offsetOfInternalNumber() { return OBJECT_OFFSETOF(DateInstance, m_internalNumber); }
    static constexpr ptrdiff_t offsetOfData() { return OBJECT_OFFSETOF(DateInstance, m_data); }

private:
    JS_EXPORT_PRIVATE DateInstance(VM&, Structure*);

    DECLARE_DEFAULT_FINISH_CREATION;
    JS_EXPORT_PRIVATE void finishCreation(VM&, double);
    JS_EXPORT_PRIVATE const GregorianDateTime* calculateGregorianDateTime(DateCache&, double milli) const;
    JS_EXPORT_PRIVATE const GregorianDateTime* calculateGregorianDateTimeUTC(DateCache&, double milli) const;

    double m_internalNumber { PNaN };
    mutable RefPtr<DateInstanceData> m_data;
};

} // namespace JSC
