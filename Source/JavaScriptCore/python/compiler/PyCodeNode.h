/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Nodes.h"
#include <wtf/Function.h>

namespace JSC {

// What a BytecodeGenerator generates code for when the code is Python: a module, a class body, or a function.
class PythonCodeNode final : public ScopeNode {
public:
    using Emitter = Function<void(BytecodeGenerator&)>;

    PythonCodeNode(ParserArena& arena, const SourceCode& source, unsigned parameterCount, bool isGeneratorBody, Emitter&& emitter)
        : ScopeNode(arena, locationOf(source), locationOf(source), source, nullptr, { }, { }, { }, NoFeatures, StrictModeLexicallyScopedFeature, NoInnerArrowFunctionFeatures)
        , m_parameterCount(parameterCount)
        , m_isGeneratorBody(isGeneratorBody)
        , m_emitter(WTF::move(emitter))
    {
    }

    unsigned parameterCount() const { return m_parameterCount; } // Without `this`.
    bool isGeneratorBody() const { return m_isGeneratorBody; }

private:
    static JSTokenLocation locationOf(const SourceCode& source)
    {
        JSTokenLocation location;
        location.startOffset = source.startOffset();
        location.endOffset = source.endOffset();
        return location;
    }

    void emitBytecode(BytecodeGenerator& generator, RegisterID* = nullptr) final { m_emitter(generator); }

    unsigned m_parameterCount;
    bool m_isGeneratorBody;
    Emitter m_emitter;
};

} // namespace JSC
