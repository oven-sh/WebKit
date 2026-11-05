/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaRecords.h"
#include "AOTType.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

bool includesData(TypeReader, const Type&, const DataNode&);
void expectFieldsFilledWithOtherData(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
