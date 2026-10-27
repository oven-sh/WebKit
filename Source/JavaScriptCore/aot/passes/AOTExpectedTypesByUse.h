/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include "AOTVector.h"
#include <ranges>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// What the uses of a value say about it, for the tests that are about to be planned.
class ExpectedTypesByUse {
    WTF_MAKE_NONCOPYABLE(ExpectedTypesByUse);
public:
    explicit ExpectedTypesByUse(Function& function)
        : m_function(function)
    {
    }

    bool isEmpty() const { return m_replacedTypes.isEmpty(); }

    void expect(ValueID id, TypeID expected)
    {
        Value& value = m_function.edit(id);
        m_replacedTypes.append({ id, value.expected().predicted() });
        value.setExpected(ExpectedType(expected));
    }

    // The names that a value is asked for are all that speaks for the layouts expected of it: nobody has checked that, and no analysis has seen them arrive.
    void didGuessFromNames(ValueID id) { m_valuesGuessedFromNames.set(id.index()); }
    bool isGuessedFromNames(ValueID id) const { return m_valuesGuessedFromNames.get(id.index()); }

    void takeBack()
    {
        for (auto [id, replaced] : m_replacedTypes | std::views::reverse)
            m_function.edit(id, ValueEdit::Type).setExpected(ExpectedType(replaced));
        m_replacedTypes.clear();
    }

private:
    Function& m_function;
    Vector<std::pair<ValueID, TypeID>, 8> m_replacedTypes;
    BitVector m_valuesGuessedFromNames;
};

// Answers whether the declared type of an entry parameter changed.
bool expectLayoutsByNames(FunctionPassContext&, ExpectedTypesByUse&);

bool expectsReceiverOfCalledMethod(FunctionPassContext&, BlockID, const Value& call);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
