/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTDerivedFunctions.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <initializer_list>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

class DerivedFunctionRequests {
public:
    explicit DerivedFunctionRequests(FunctionPassContext& pass)
        : m_pass(&pass)
    {
    }

    explicit DerivedFunctionRequests(DerivedFunctionInputs& inputs)
        : m_functions(&inputs.derivedFunctions)
    {
    }

    FunctionID ensure(const DerivedFunctionKey&) const;
    DerivedFunctionKey keyOf(FunctionID provisional) const;

private:
    FunctionPassContext* m_pass { nullptr };
    DerivedFunctions* m_functions { nullptr };
};

struct EmissionInputs {
    explicit EmissionInputs(FunctionPassContext&);
    EmissionInputs(DerivedFunctionInputs&, FunctionID subject);

    AnalysisContext analysisContext() const { return { build, program, &flow, nullptr, function }; }

    const Program& program;
    const FlowResults& flow;
    TypeArena& types;
    StringArena& strings;
    BuildContext& build;
    DerivedFunctionRequests functions;
    FunctionID function;
    // The subject of a limit hit: `function` may be provisional, which means nothing outside the pass.
    FunctionID functionAsRead;
};

struct DerivedSignature {
    Vector<TypeID, 2> parameters;
    Vector<TypeID, 3> results;
};

struct DerivedFunctionHead {
    Function function;
    BlockID entry;
    Vector<ValueID, 2> parameters;
};

DerivedFunctionHead createDerivedFunction(const EmissionInputs&, FunctionRole, CreatorRecord&&, const DerivedSignature&);

class Emission {
    WTF_MAKE_NONCOPYABLE(Emission);
public:
    Emission(IRBuilder&, const EmissionInputs&);

    IRBuilder& builder() { return m_builder; }
    const EmissionInputs& inputs() const { return m_inputs; }
    const Program& program() const { return m_inputs.program; }
    const Function& function() const { return m_builder.function(); }
    const Type& typeOf(ValueID) const;

    TypeID outcomeType();
    TypeID cursorType();
    TypeID keyOrderType();
    TypeID stringBuilderType();

    ValueID appendOutcome(JSONOutcome);
    ValueID appendString(StringID);
    ValueID appendString(StringView text);
    ValueID appendNull();
    ValueID appendResultAt(ValueID results, uint32_t index);
    ValueID appendResults(std::initializer_list<ValueID>);
    void emitReturnOf(std::initializer_list<ValueID>);

    ValueID appendCallOf(FunctionID, std::initializer_list<ValueID> arguments);
    ValueID appendHelperCall(BuiltinID helper, std::initializer_list<ValueID> arguments);
    ValueID appendBuiltinCall(BuiltinOverloadID, ValueID receiver, std::span<const ValueID> arguments);

    struct TestedObject {
        ValueID object;
        ValueID guard;
    };
    TestedObject emitShapeTest(ValueID, std::span<const LayoutID>, BlockID notTaken, BlockFrequency whenTaken = BlockFrequency::Normal);
    ValueID emitKeyOrderTest(const TestedObject&, LayoutID, std::span<const uint32_t> fields, BlockID notTaken);
    ValueID appendFieldRead(const TestedObject&, LayoutID, uint32_t field);
    ValueID appendFieldReadOrUndefined(const TestedObject&, LayoutID, uint32_t field);

    void emitBranchOnOutcome(ValueID outcome, BlockID parsed, BlockID rejected, BlockID unknown);

    // The block that jumps to the named block takes that block's frequency, and the code goes on along the other edge.
    ValueID emitProvenBranch(ValueID test, BlockID notTaken, BlockFrequency whenTaken = BlockFrequency::Normal);
    ValueID emitTypeTest(ValueID, TypeID tested, BlockID notTaken);
    // Tests the parts of the field's type that a TypeTest can establish for this value: it proves no contents, storage or pristine state.
    ValueID emitRequireStoredType(ValueID, TypeID fieldType, BlockID notTaken);
    bool passesOnlyUndefinedIntoStoredType(const Type& value, TypeID fieldType);

    // What the type of a field proves about built-in classes and holds for a value because of what created the value.
    enum class ProofsByOrigin : uint8_t {
        None,
        AboutArrays,
        AboutEveryClass,
    };
    TypeID fieldTypeToTest(TypeID fieldType, ProofsByOrigin);
    // The result goes straight into the field: an assumption is checked against the position it feeds.
    ValueID emitRequireFieldType(ValueID, TypeID fieldType, ProofsByOrigin, BlockID notTaken);
    void emitJumpUnless(ValueID condition, BlockID otherwise);
    void emitJumpIf(ValueID condition, BlockID target);

    LayoutSetID layoutSetOf(std::span<const LayoutID>);
    bool fitsDeclaredType(ValueID, const DeclaredType&) const;

private:
    ValueID appendSlotRead(Opcode, const TestedObject&, LayoutID, uint32_t field);

    IRBuilder& m_builder;
    const EmissionInputs& m_inputs;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
