/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <compare>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

// The value an operation hands on unchanged: the operand of a cast, of a request, of the operations that finish a literal or a class.
ValueID sameValueOperandOf(const Value&);
ValueID definitionThroughSameValues(const Function&, ValueID);

struct ClosureHolder {
    enum class Kind : uint8_t { Variable, Capture, EnvironmentField, Member, Getter, Setter };

    static ClosureHolder variable(VariableID variable) { return { Kind::Variable, variable.index(), 0 }; }
    static ClosureHolder capture(FunctionID function, uint32_t index) { return { Kind::Capture, function.index(), index }; }
    static ClosureHolder environmentField(LayoutID layout, uint32_t field) { return { Kind::EnvironmentField, layout.index(), field }; }
    static ClosureHolder member(Kind kind, LayoutID layout, ClassSide side, PropertyKeyID key) { return { kind, layout.index(), key.index(), side }; }

    PropertyKeyID key() const { return PropertyKeyID(index); }

    Kind kind { Kind::Variable };
    uint32_t owner { 0 };
    uint32_t index { 0 };
    ClassSide side { ClassSide::Prototype };

    friend bool operator==(const ClosureHolder&, const ClosureHolder&) = default;
    friend auto operator<=>(const ClosureHolder&, const ClosureHolder&) = default;
};

// The functions whose closures a variable, a capture, an environment field, a method, an accessor or a member of a literal is filled with, read off the operations that fill them.
class KnownClosures {
public:
    KnownClosures(const Program&, BuildContext&);

    std::span<const FunctionID> closuresIn(ClosureHolder) const LIFETIME_BOUND;
    std::span<const ClosureHolder> holdersOf(FunctionID) const LIFETIME_BOUND;
    // `class extends Base` with Base a parameter, a call, a built-in: before flow such a class may extend any class.
    std::span<const LayoutID> classesWithUnknownBase() const LIFETIME_BOUND { return m_classesWithUnknownBase.span(); }

private:
    Vector<ClosureHolder> m_holders;
    Vector<FunctionID> m_closures;
    Vector<FunctionID> m_functions;
    Vector<ClosureHolder> m_holdersByFunction;
    Vector<LayoutID> m_classesWithUnknownBase;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
