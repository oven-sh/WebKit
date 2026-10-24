/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestCommand.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

std::span<const TestCommand> effectsTestCommands();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
