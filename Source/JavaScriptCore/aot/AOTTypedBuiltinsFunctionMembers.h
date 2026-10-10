/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    // Function.prototype.bind, if that is the name.
    BuiltinID bindIfNamed(StringID) const;
    // `f.bind(thisArg, ..)`. `method`: what was read from `f` under the name, if the read is a value of its own. Nothing: it is no such call, or there is no code in place for it.
    std::optional<Operand> lowerBind(ValueID function, StringID, std::optional<Boxed> method, const Arguments&, const Type& result);
