/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

@alwaysInline
function call(thisArgument)
{
    "use strict";

    // f.call(...) is compiled to a call of f and never gets here. Function.prototype.call.bind(f)(...) does.
    // The DFG inlines the bound function, and then this function with f as a constant |this|. It goes on to
    // inline f, or f's intrinsic, only if the call has a fixed number of arguments. There, all paths but one
    // are dead code. The DFG tier does not eliminate an arguments object, so this function has none.
    var argumentCount = @argumentCount();
    if (argumentCount === 2)
        return this.@call(thisArgument, @argument(1));
    if (argumentCount === 3)
        return this.@call(thisArgument, @argument(1), @argument(2));
    if (argumentCount <= 1)
        return this.@call(thisArgument);
    if (argumentCount === 4)
        return this.@call(thisArgument, @argument(1), @argument(2), @argument(3));

    return this.@apply(thisArgument, @argumentsFrom(1));
}

function apply(thisValue, argumentValues)
{
    "use strict";

    return this.@apply(thisValue, argumentValues);
}
