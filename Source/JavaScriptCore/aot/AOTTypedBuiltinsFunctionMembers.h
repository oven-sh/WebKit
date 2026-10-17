/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    BuiltinID bindIfNamed(StringID) const;
    std::optional<Operand> lowerBind(ValueID function, StringID, std::optional<Boxed> method, const Arguments&, const Type& result);
