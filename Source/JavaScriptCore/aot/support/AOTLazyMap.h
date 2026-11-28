/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <memory>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

template<typename Map>
class LazyMap {
public:
    auto ensure(const auto& key, NOESCAPE const auto& make)
    {
        if (!m_map)
            m_map = makeUnique<Map>();
        return m_map->ensure(key, make);
    }

private:
    std::unique_ptr<Map> m_map;
};

} } // namespace JSC::AOT
