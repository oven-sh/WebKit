/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

String formatMessage(const UserDiagnostic&, const Typed::Program*);
String formatMessage(const InternalError&, const Typed::Program*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
