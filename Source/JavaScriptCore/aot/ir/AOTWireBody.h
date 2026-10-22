/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <memory>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Reader;

struct WireBodyHead {
    FunctionID function;
    const FunctionRecord& record;
    // The parameters of blocks are values too.
    uint32_t numberOfValues;
    uint32_t numberOfBlocks;
};

struct WireBlockHead {
    BlockID block;
    std::span<const ValueID> parameters;
    BlockRole role;
    BlockFrequency frequency;
    ValueID subject;
    const Target* handler;
    uint32_t numberOfValues;
};

// A body as the front end wrote it: blocks in order, values in the order of their numbers. A record is valid during the call that hands it over.
// Nothing has set the type, the expected type or the representation of a value.
class WireBodyVisitor {
public:
    virtual ~WireBodyVisitor() = default;
    virtual void beginBody(const WireBodyHead&) = 0;
    virtual void beginBlock(const WireBlockHead&) = 0;
    virtual void visitValue(ValueID, const Value&) = 0;
    virtual void visitTerminator(BlockID, const Terminator&) = 0;
    virtual void endBody() = 0;
};

// The bodies of a program that has been read, in the words of the front end. The bytes and the program outlive it.
class WireBodies {
    WTF_MAKE_NONCOPYABLE(WireBodies);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(WireBodies);
public:
    explicit WireBodies(std::unique_ptr<Reader>&&);
    JS_EXPORT_PRIVATE ~WireBodies();

    // The engine makes functions of its own, which have no words.
    JS_EXPORT_PRIVATE bool hasBody(FunctionID) const;
    JS_EXPORT_PRIVATE void walk(FunctionID, WireBodyVisitor&) const;
    JS_EXPORT_PRIVATE OpcodeSet operationsOf(FunctionID) const;

private:
    std::unique_ptr<Reader> m_reader;
};

struct ProgramWithWireBodies {
    std::unique_ptr<Program> program;
    std::unique_ptr<WireBodies> bodies;
};

// Both are null where Program::read() answers null.
JS_EXPORT_PRIVATE ProgramWithWireBodies readProgramKeepingWireBodies(std::span<const uint8_t> bytes, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
