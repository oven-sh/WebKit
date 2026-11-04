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

// A PQ-tree: all the orders of the leaves 0 .. n - 1 that keep each set taken so far side by side.
class ConsecutiveSets {
    WTF_MAKE_NONCOPYABLE(ConsecutiveSets);
public:
    JS_EXPORT_PRIVATE explicit ConsecutiveSets(uint32_t numberOfLeaves);
    JS_EXPORT_PRIVATE ~ConsecutiveSets();

    // True: from now on every order keeps these leaves side by side. False: no order does so and keeps the sets taken before, and nothing has changed.
    // The cost goes by the number of leaves given and the nodes above them alone, not by the size of the tree.
    JS_EXPORT_PRIVATE bool tryKeepTogether(std::span<const uint32_t> leaves);
    // One of the orders: wherever the tree leaves a choice, the part that holds the leaf with the lowest key comes first. The keys differ from one another.
    JS_EXPORT_PRIVATE Vector<uint32_t> order(std::span<const uint64_t> keyOfLeaf) const;

private:
    struct Tree;
    std::unique_ptr<Tree> m_tree;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
