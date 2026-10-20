/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeLayout.h"
#include "AOTConstantPool.h"
#include "AOTInstanceLayout.h"
#include <span>

namespace JSC { namespace AOT {

struct CodeLinkerInputs {
    const CodeLayoutInputs& layoutInputs;
    const CodeLayout& layout;
    const Outlining& outlining;
    const ConstantPool& constants;
    InstanceSizes instanceSizes;
};

void linkCode(const CodeLinkerInputs&, std::span<uint8_t> code, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
