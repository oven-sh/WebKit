/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThinSummary.h"

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;
struct LinkTables;
struct LinkType;

void comeAliveForUnions(LinkFlow&, FunctionID);
void writeTablesOfUnions(LinkFlow&, LinkTables&);
void notePrototypesLaidAboveOtherObjects(LinkFlow&, const LinkType& above);
TypeID typeOfElementsOfTemplateObject(const BodyValue& made);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
