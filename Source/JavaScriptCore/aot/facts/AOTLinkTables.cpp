/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkTables.h"

#if ENABLE(AOT)

#include "AOTFamilies.h"

namespace JSC { namespace AOT { namespace Typed {

LinkTables::LinkTables() = default;
LinkTables::LinkTables(LinkTables&&) = default;
LinkTables& LinkTables::operator=(LinkTables&&) = default;
LinkTables::~LinkTables() = default;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
