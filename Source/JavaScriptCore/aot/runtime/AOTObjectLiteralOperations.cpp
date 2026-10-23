/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeFunctions.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "PropertyRun.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const PropertyRun* propertyRunAt(Instance* instance, uint32_t number)
{
    const PropertyRun*& run = instance->runtime.literals.propertyRuns[number];
    if (run) [[likely]]
        return run;
    std::span<const uint32_t> row = instance->tables().propertyRunRow(number);
    RELEASE_ASSERT(row.size() == PropertyRunHeader::decode(row[0]).count + 1u);
    std::span<const uint32_t> numbers = row.subspan(1);
    Vector<UniquedStringImpl*, 16> names(numbers.size(), [&](size_t index) {
        return identifierAt(instance, numbers[index]).impl();
    });
    run = instance->vm->propertyRuns.intern(names.span());
    return run;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
