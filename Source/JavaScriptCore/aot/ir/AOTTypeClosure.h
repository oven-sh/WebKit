/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

bool includesData(const Program&, const Type&, const DataNode&);
void expectFieldsFilledWithOtherData(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
