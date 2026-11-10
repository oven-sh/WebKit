/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "JSExportMacros.h"
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class ConsecutiveSets {
    WTF_MAKE_NONCOPYABLE(ConsecutiveSets);
public:
    JS_EXPORT_PRIVATE explicit ConsecutiveSets(uint32_t numberOfLeaves);
    JS_EXPORT_PRIVATE ~ConsecutiveSets();

    JS_EXPORT_PRIVATE bool tryKeepTogether(std::span<const uint32_t> leaves);
    JS_EXPORT_PRIVATE bool tryKeepNestedSetTogether(std::span<const uint32_t> leaves);
    JS_EXPORT_PRIVATE Vector<uint32_t> order(std::span<const uint64_t> keyOfLeaf) const;

private:
    struct Tree;
    std::unique_ptr<Tree> m_tree;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
