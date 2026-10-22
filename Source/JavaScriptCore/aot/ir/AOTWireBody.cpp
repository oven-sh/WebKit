/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWireBody.h"

#if ENABLE(AOT)

#include "AOTReaderInternal.h"

namespace JSC { namespace AOT { namespace Typed {

WireBodies::WireBodies(std::unique_ptr<Reader>&& reader)
    : m_reader(WTF::move(reader))
{
}

WireBodies::~WireBodies() = default;

bool WireBodies::hasBody(FunctionID id) const
{
    auto functions = m_reader->sections().functions;
    return id.index() < functions.size() && functions[id.index()].body.blockCount;
}

void WireBodies::walk(FunctionID id, WireBodyVisitor& visitor) const
{
    RELEASE_ASSERT(hasBody(id));
    const Wire::Function& wire = m_reader->sections().functions[id.index()];
    RecordChecker checker(*m_reader, InternalError { .kind = InternalErrorKind::Unknown, .function = id, .count = id.index() });
    std::span<const uint32_t> code = checker.code(wire.body.code);
    Function decoded;
    Failure failure = BodyDecoder(*m_reader, id, wire.body, code, decoded).run();
    RELEASE_ASSERT(!failure && !checker.hasFailed());

    visitor.beginBody({ id, m_reader->program().functions[id.index()], static_cast<uint32_t>(decoded.values.size()), static_cast<uint32_t>(decoded.blocks.size()) });
    for (uint32_t index = 0; index < decoded.blocks.size(); ++index) {
        BlockID blockID(index);
        const Block& block = decoded.at(blockID);
        visitor.beginBlock({ blockID, block.parameters.span(), block.role, block.frequency, block.subject, block.handler ? &*block.handler : nullptr, static_cast<uint32_t>(block.values.size()) });
        for (ValueID value : block.values)
            visitor.visitValue(value, decoded.at(value));
        visitor.visitTerminator(blockID, block.terminator);
    }
    visitor.endBody();
}

OpcodeSet WireBodies::operationsOf(FunctionID id) const
{
    class Collector final : public WireBodyVisitor {
    public:
        void beginBody(const WireBodyHead&) final { }
        void beginBlock(const WireBlockHead&) final { }
        void visitValue(ValueID, const Value& value) final { operations.add(value.opcode); }
        void visitTerminator(BlockID, const Terminator&) final { }
        void endBody() final { }

        OpcodeSet operations;
    };
    Collector collector;
    walk(id, collector);
    return collector.operations;
}

ProgramWithWireBodies readProgramKeepingWireBodies(std::span<const uint8_t> bytes, BuildContext& build)
{
    auto program = makeUnique<Program>(BuiltinTable::singleton());
    program->borrowBuffer(bytes);
    auto reader = makeUniqueWithoutFastMallocCheck<Reader>(*program, IRForm::AsRead, build);
    if (!reader->run())
        return { };
    program->copyBorrowedLineStarts(build.parallelLoops());
    program->borrowBuffer({ });
    return { WTF::move(program), makeUnique<WireBodies>(WTF::move(reader)) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
