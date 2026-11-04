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

// The value that the data describes is one of the type.
bool includesData(TypeReader, const Type&, const DataNode&);
// A field that a piece of data fills with something outside enforceableTypeOf() its declared type is only expected from then on. WireTypeReader::finishObjects() calls it.
void expectFieldsFilledWithOtherData(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
