/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowPropertyLocations.h"

namespace JSC { namespace AOT { namespace Typed {

FlowOwnListing exportListingOf(const Program&, ModuleID);

FlowOwnProperty findNamespaceExport(const FlowWalkInputs&, FlowObjectID namespaceObject, PropertyKeyID);
FlowOwnListing forEachNamespaceExport(const FlowWalkInputs&, FlowObjectID namespaceObject, NOESCAPE const FlowOwnPropertyFunctor&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
