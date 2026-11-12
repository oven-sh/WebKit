/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkTables.h"

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;

Vector<LinkedReceiverAsMade> storesThatFreeListsOfFields(const LinkFlow&, const LinkTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
