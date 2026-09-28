/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "config.h"
#include "PythonASTDumper.h"

#include "JSBigInt.h"
#include "JSCInlines.h"
#include "PythonASTWalker.h"
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

namespace {

// As JSON. JSTests/python/resources/dump-ast.py says what form it is in.
class Dumper : public ASTWalker<Dumper> {
public:
    explicit Dumper(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
    {
    }

    String dump(Module& module)
    {
        walk(module);
        return m_out.toString();
    }

    void open(ASTClass astClass)
    {
        m_out.append("{\"_\":\""_s, descriptionOf(astClass).name, '"');
    }

    void name(ASCIILiteral name)
    {
        m_out.append(",\""_s, name, "\":"_s);
    }

    void close(const Node& node)
    {
        m_out.append(",\"@\":["_s, node.line, ',', node.column, ',', node.endLine, ',', node.endColumn, "]}"_s);
    }

    void close()
    {
        m_out.append('}');
    }

    void null() { m_out.append("null"_s); }
    void identifier(const Identifier& identifier) { string(identifier.string()); }
    void integer(int number) { m_out.append(number); }

    void singleton(ASTClass astClass)
    {
        open(astClass);
        close();
    }

    void openList(size_t)
    {
        m_out.append('[');
        m_isFirst = true;
    }

    void element()
    {
        if (!m_isFirst)
            m_out.append(',');
        m_isFirst = false;
    }

    void closeList()
    {
        m_out.append(']');
        m_isFirst = false;
    }

    bool canGoDeeper() { return true; }

    void string(StringView view)
    {
        m_out.append('"');
        for (char16_t c : view.codeUnits()) {
            switch (c) {
            case '"':
                m_out.append("\\\""_s);
                break;
            case '\\':
                m_out.append("\\\\"_s);
                break;
            case '\n':
                m_out.append("\\n"_s);
                break;
            case '\r':
                m_out.append("\\r"_s);
                break;
            case '\t':
                m_out.append("\\t"_s);
                break;
            case '\f':
                m_out.append("\\f"_s);
                break;
            case '\b':
                m_out.append("\\b"_s);
                break;
            default:
                if (c >= ' ' && c <= '~')
                    m_out.append(static_cast<Latin1Character>(c));
                else
                    m_out.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
                break;
            }
        }
        m_out.append('"');
    }

    void simpleConstant(ASCIILiteral type)
    {
        m_out.append("{\"c\":\""_s, type, "\"}"_s);
    }

    void constant(Constant::Type type)
    {
        switch (type) {
        case Constant::Type::None:
            return simpleConstant("None"_s);
        case Constant::Type::True:
            return simpleConstant("True"_s);
        case Constant::Type::False:
            return simpleConstant("False"_s);
        case Constant::Type::Ellipsis:
            return simpleConstant("Ellipsis"_s);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void constant(Constant& node)
    {
        switch (node.type) {
        case Constant::Type::Integer:
            m_out.append("{\"c\":\"int\",\"v\":\""_s, hex(node.integer, Lowercase), "\"}"_s);
            return;
        case Constant::Type::BigInteger: {
            VM& vm = m_globalObject->vm();
            JSValue bigInt = JSBigInt::parseInt(m_globalObject, vm, node.text->string(), node.radix, JSBigInt::ErrorParseMode::IgnoreExceptions);
            RELEASE_ASSERT(bigInt && bigInt.isHeapBigInt());
            m_out.append("{\"c\":\"int\",\"v\":\""_s, bigInt.asHeapBigInt()->toString(m_globalObject, 16), "\"}"_s);
            return;
        }
        case Constant::Type::Float:
            m_out.append("{\"c\":\"float\",\"v\":\""_s, std::bit_cast<uint64_t>(node.real), "\"}"_s);
            return;
        case Constant::Type::Imaginary:
            m_out.append("{\"c\":\"complex\",\"v\":\""_s, std::bit_cast<uint64_t>(node.real), "\"}"_s);
            return;
        case Constant::Type::String:
        case Constant::Type::Bytes:
            m_out.append("{\"c\":\""_s, node.type == Constant::Type::String ? "str"_s : "bytes"_s, "\",\"v\":"_s);
            string(node.text->string());
            m_out.append('}');
            return;
        default:
            constant(node.type);
            return;
        }
    }

private:
    JSGlobalObject* m_globalObject;
    StringBuilder m_out;
    bool m_isFirst { false };
};

} // anonymous namespace

String dumpAST(JSGlobalObject* globalObject, Module& module)
{
    return Dumper(globalObject).dump(module);
}

} } // namespace JSC::Python
