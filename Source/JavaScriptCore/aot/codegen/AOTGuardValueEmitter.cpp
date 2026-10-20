/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardValueEmitter.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static_assert(sizeof(InstanceHeader::shapeEpoch) == sizeof(int64_t));

GuardValueEmitter::GuardValueEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void GuardValueEmitter::generateGetShapeEpoch(const ValueContext& value)
{
    define(value, loadField<Integer>(instance(), m_heaps.AOTInstance_shapeEpoch));
}

void GuardValueEmitter::generateAllGuards(const ValueContext&)
{
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
