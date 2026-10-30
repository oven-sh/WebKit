/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIDPrefix.h"
#include "AOTIR.h"
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/CommaPrinter.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

enum class PrintedFields : uint8_t { All, WireOnly };

void printProgram(PrintStream&, const Program&);
void printProgram(PrintStream&, const Program&, PrintedFields);
void printFunction(PrintStream&, const Program&, FunctionID, const Function&);
void printValue(PrintStream&, const Program&, const Function&, ValueID);
void printType(PrintStream&, const Program&, const Type&);
void printType(PrintStream&, const Program&, TypeID);
void printString(PrintStream&, const Program&, StringID);
void printPropertyKey(PrintStream&, const Program&, PropertyKeyID);

class IRPrinter {
    WTF_MAKE_NONCOPYABLE(IRPrinter);
public:
    IRPrinter(PrintStream&, const Program&);
    IRPrinter(PrintStream&, const Program&, PrintedFields);

    PrintStream& out() { return m_out; }
    const Program& program() const { return m_program; }
    bool printsEngineFields() const { return m_printedFields == PrintedFields::All; }

    void printProgram();
    void printFunction(FunctionID, const Function&);
    void printBody(const Function&);
    void printValue(const Function&, ValueID);
    void printTerminator(const Terminator&);
    void printType(const Type&);

    void print(ValueID id) { printID(IDPrefix::Value, id.index()); }
    void print(BlockID id) { printID(IDPrefix::Block, id.index()); }
    void print(FunctionID id) { printID(IDPrefix::Function, id.index()); }
    void print(LayoutID id) { printID(IDPrefix::Layout, id.index()); }
    void print(VariableID id) { printID(IDPrefix::Variable, id.index()); }
    void print(ModuleID id) { printID(IDPrefix::Module, id.index()); }
    void print(ModuleFailureID id) { printID(IDPrefix::ModuleFailure, id.index()); }
    void print(TypeSiteID id) { printID(IDPrefix::TypeSite, id.index()); }
    void print(EvalScopeID id) { printID(IDPrefix::EvalScope, id.index()); }
    void print(ExpectedResultID id) { printID(IDPrefix::ExpectedResult, id.index()); }
    void print(DataID id) { printID(IDPrefix::Data, id.index()); }
    void print(StringID);
    void print(PropertyKeyID);
    void print(TypeID);
    void print(ExpectedType expected) { print(expected.predicted()); }
    void print(LayoutSetID);
    void print(CalleeSetID);
    template<typename Tag> void print(ID<Tag> id) { m_out.print(id.index()); }

    void print(BuiltinID);
    void print(BuiltinClassID);
    void print(BuiltinName);
    void print(BuiltinLayoutID);
    void print(BuiltinSlotID);
    void print(BuiltinOverloadID);
    void print(RuntimeModuleID);
    void print(RuntimeFunctionID);

    void print(bool value) { m_out.print(value ? "true"_s : "false"_s); }
    void print(uint32_t value) { m_out.print(value); }
    void print(uint64_t value) { m_out.print(value); }
    void print(int64_t value) { m_out.print(value); }
    void print(double);
    void print(std::span<const uint8_t> bytes);
    template<typename E> requires std::is_enum_v<E> void print(E);
    template<typename F> void print(OptionSet<F>);

    void beginField(ASCIILiteral name);
    void beginRecord();
    void endRecord();
    void beginLine(ASCIILiteral keyword);
    void beginLine(ASCIILiteral keyword, IDPrefix, uint32_t index);
    void endLine();

    template<typename T> void field(ASCIILiteral name, const T& value);
    template<typename Range, typename Functor> void list(const Range&, NOESCAPE const Functor& printElement);

private:
    void printID(IDPrefix, uint32_t index);
    void printInvalid(ASCIILiteral kind, uint64_t number);
    void printQuoted(std::span<const uint8_t>);

    void printBlock(const Function&, BlockID);
    void printBlockHead(const Function&, BlockID, const Block&);
    void printEngineValueFields(const Value&);
    void printTarget(ASCIILiteral name, const Target&);
    void printArguments(std::span<const ValueID>);

    void printPrimitives(const Type&, CommaPrinter& bar);
    void printStringLiterals(const Type&, CommaPrinter& bar);
    void printSymbolLiterals(const Type&, CommaPrinter& bar);
    void printObjectParts(const Type&, CommaPrinter& bar);
    void printClassPart(const Type&, const ClassPart&);

    PrintStream& m_out;
    const Program& m_program;
    PrintedFields m_printedFields { PrintedFields::All };
    bool m_needsBlankBeforeField { true };
};

template<typename E>
    requires std::is_enum_v<E>
void IRPrinter::print(E enumerator)
{
    auto number = static_cast<uint64_t>(enumerator);
    if (number >= numberOfEnumerators<E>) {
        printInvalid("enumerator"_s, number);
        return;
    }
    m_out.print(nameOf(enumerator));
}

template<typename F>
void IRPrinter::print(OptionSet<F> flags)
{
    m_out.print("["_s);
    CommaPrinter comma;
    for (F flag : flags) {
        m_out.print(comma);
        unsigned bit = WTF::ctz(static_cast<uint32_t>(flag));
        if (bit >= numberOfEnumerators<F>)
            printInvalid("flag"_s, bit);
        else
            m_out.print(nameOf(flag));
    }
    m_out.print("]"_s);
}

template<typename T>
void IRPrinter::field(ASCIILiteral name, const T& value)
{
    beginField(name);
    print(value);
}

template<typename Range, typename Functor>
void IRPrinter::list(const Range& range, NOESCAPE const Functor& printElement)
{
    m_out.print("["_s);
    CommaPrinter comma;
    for (const auto& element : range) {
        m_out.print(comma);
        printElement(element);
    }
    m_out.print("]"_s);
}

// The name of a runtime function in IR text. Null for an id that is not in the table.
StringView tokenOf(const BuiltinTable&, RuntimeFunctionID);
RuntimeFunctionID runtimeFunctionFromToken(const BuiltinTable&, StringView);

inline bool needsEscape(uint8_t byte)
{
    return byte < 0x20 || byte >= 0x7f || byte == '"' || byte == '\\';
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaPrinter.h"

#endif // ENABLE(AOT)
