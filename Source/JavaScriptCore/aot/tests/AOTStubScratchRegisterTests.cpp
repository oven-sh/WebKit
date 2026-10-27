/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTScratchRegisterChecker.h"
#include "AOTStubBlob.h"
#include "AOTStubTestSupport.h"
#include <algorithm>
#include <initializer_list>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

#if ENABLE(AOT_SCRATCH_REGISTER_CHECKER)

using Kind = ScratchRegisterConflict::Kind;

static StubBlob buildEveryStub(const StubOptions& options)
{
    BuildContext context(makeDefaultCompileOptions());
    StubCatalog catalog(options, StubPolicy { });
    WTF::BitSet<numberOfEntries> everyEntry;
    everyEntry.setAll();
    auto blob = buildStubBlob(context, catalog, stubKeysWithLinkage(StubLinkage::CalledByCode).span(), everyEntry);
    AOT_TEST_CHECK(blob.has_value() && !context.diagnostics().hasErrors());
    return WTF::move(*blob);
}

static const StubPlacement& findStubAt(const Vector<StubPlacement>& placementsByOffset, uint32_t offset)
{
    auto next = std::ranges::upper_bound(placementsByOffset, offset, { }, &StubPlacement::offset);
    AOT_TEST_CHECK(next != placementsByOffset.begin());
    return *std::prev(next);
}

static void testStubsReadScratchValuesBeforeReplacingThem()
{
    unsigned numberOfConflicts = 0;
    for (const StubOptions& options : stubOptionVariants()) {
        StubBlob blob = buildEveryStub(options);
        std::span<const uint8_t> code = blob.code.bytes.span();
        Vector<StubPlacement> placementsByOffset = blob.placements;
        std::ranges::stable_sort(placementsByOffset, { }, &StubPlacement::offset);
        Vector<uint32_t> entryOffsets = placementsByOffset.map([](const StubPlacement& placement) {
            return placement.offset;
        });

        for (const ScratchRegisterConflict& conflict : findScratchRegisterConflicts(code, entryOffsets)) {
            const StubPlacement& stub = findStubAt(placementsByOffset, conflict.offset);
            ++numberOfConflicts;
            bool isReplaced = conflict.kind == Kind::ReplacedUnread;
            dataLog("x", static_cast<unsigned>(conflict.reg), isReplaced ? " is written twice and not read in between" : " is two operands of one instruction", " in ");
            dump(WTF::dataFile(), stub.key);
            dataLogLn(", which starts at ", stub.offset, ":");
            printInstructions(WTF::dataFile(), code, isReplaced ? conflict.offsetOfWrite : conflict.offset, conflict.offset + 2 * sizeof(uint32_t));
        }
    }
    AOT_TEST_CHECK(!numberOfConflicts);
}

static Vector<ScratchRegisterConflict> findConflicts(std::initializer_list<uint32_t> words, std::initializer_list<uint32_t> entryOffsets = { })
{
    return findScratchRegisterConflicts(asByteSpan(std::span(words)), std::span(entryOffsets));
}

static void testCheckerReportsConflicts()
{
    constexpr GPRReg x16 = ARM64Registers::x16;
    constexpr GPRReg x17 = ARM64Registers::x17;
    auto finds = [](std::initializer_list<uint32_t> words, std::initializer_list<ScratchRegisterConflict> expected) {
        return std::ranges::equal(findConflicts(words), expected);
    };

    // ldur x16, [x3, #32] . mov x16, #-11 . tst x16, x16 . b.ne
    AOT_TEST_CHECK(finds({ 0xF8420070, 0x92800150, 0xEA10021F, 0x54000001 }, { { Kind::ReplacedUnread, x16, 4, 0 } }));
    // sub x16, sp, #1, lsl #12 . mov x16, sp . cmp x16, x16
    AOT_TEST_CHECK(finds({ 0xD14007F0, 0x910003F0, 0xEB10021F }, { { Kind::ReplacedUnread, x16, 4, 0 }, { Kind::ReadAsTwoOperands, x16, 8 } }));
    // ldr x17, [x0] . ldr x17, [x1]
    AOT_TEST_CHECK(finds({ 0xF9400011, 0xF9400031 }, { { Kind::ReplacedUnread, x17, 4, 0 } }));
    // ldp x16, x17, [x0] . mov x16, #1 . add x0, x16, x17
    AOT_TEST_CHECK(finds({ 0xA9404410, 0xD2800030, 0x8B110200 }, { { Kind::ReplacedUnread, x16, 4, 0 } }));
    // ldr x16, [sp, #8] . mov x16, #-11 . tst x16, x16
    AOT_TEST_CHECK(finds({ 0xF94007F0, 0x92800150, 0xEA10021F }, { { Kind::ReplacedUnread, x16, 4, 0 } }));
    // mov x16, #1 . ldp x16, x17, [sp, #136]
    AOT_TEST_CHECK(finds({ 0xD2800030, 0xA948C7F0 }, { { Kind::ReplacedUnread, x16, 4, 0 } }));
    // ldr w16, [x0] . mov w16, #1
    AOT_TEST_CHECK(finds({ 0xB9400010, 0x52800030 }, { { Kind::ReplacedUnread, x16, 4, 0 } }));
    // mov x16, #1 . nop . mov x16, #1
    AOT_TEST_CHECK(finds({ 0xD2800030, 0xD503201F, 0xD2800030 }, { { Kind::ReplacedUnread, x16, 8, 0 } }));
    // str x16, [x16]
    AOT_TEST_CHECK(finds({ 0xF9000210 }, { { Kind::ReadAsTwoOperands, x16, 0 } }));
    // add x0, x17, x17
    AOT_TEST_CHECK(finds({ 0x8B110220 }, { { Kind::ReadAsTwoOperands, x17, 0 } }));
}

static void testCheckerAcceptsOrdinaryCode()
{
    // ldur x16, [x3, #32] . mov x17, #-11 . tst x16, x17 . b.ne
    AOT_TEST_CHECK(findConflicts({ 0xF8420070, 0x92800151, 0xEA11021F, 0x54000001 }).isEmpty());
    // ldur x16, [x3, #32] . tst x16, x16 . b.ne
    AOT_TEST_CHECK(findConflicts({ 0xF8420070, 0xEA10021F, 0x54000001 }).isEmpty());
    // mov x16, #1 . movk x16, #2, lsl #16 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xD2800030, 0xF2A00050, 0xF9000010 }).isEmpty());
    // mov x16, #1 . ldr x16, [x16] . add x16, x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xD2800030, 0xF9400210, 0x91000610, 0xF9000010 }).isEmpty());
    // mov x16, #1 . ldr x0, [x16, #8]! . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xD2800030, 0xF8408E00, 0xF9000010 }).isEmpty());
    // ldp x16, x17, [x0] . stp x16, x17, [x0] . ldp x16, x17, [x0] . add x0, x16, x17
    AOT_TEST_CHECK(findConflicts({ 0xA9404410, 0xA9004410, 0xA9404410, 0x8B110200 }).isEmpty());
    // ldp x16, x17, [sp, #136] . add sp, sp, #352 . orr w16, wzr, #0x7fff . cmp w9, w16 . ldr x17, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xA948C7F0, 0x910583FF, 0x32003BF0, 0x6B10013F, 0xF9400011 }).isEmpty());
    // ldp x16, x17, [sp], #16 . mov x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xA8C147F0, 0xD2800030, 0xF9000010 }).isEmpty());
    // stxr w16, x1, [x2] . cbnz w16, -4 . mov x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xC8107C41, 0x35FFFFF0, 0xD2800030, 0xF9000010 }).isEmpty());

    // The other path may read the first value: mov x16, #1 . cbz x0, +8 . mov x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xD2800030, 0xB4000040, 0xD2800030, 0xF9000010 }).isEmpty());
    // mov x16, #1 . bl +8 . mov x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xD2800030, 0x94000002, 0xD2800030, 0xF9000010 }).isEmpty());
    // mov x16, #1 . blr x16 . mov x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xD2800030, 0xD63F0200, 0xD2800030, 0xF9000010 }).isEmpty());
    // The second write is a branch target: cbz x0, +8 . mov x16, #1 . mov x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xB4000040, 0xD2800030, 0xD2800030, 0xF9000010 }).isEmpty());
    // The second write is an entry: mov x16, #1 . mov x16, #1 . str x16, [x0]
    AOT_TEST_CHECK(findConflicts({ 0xD2800030, 0xD2800030, 0xF9000010 }, { 4 }).isEmpty());
    AOT_TEST_CHECK(!findConflicts({ 0xD2800030, 0xD2800030, 0xF9000010 }).isEmpty());
}

#endif // ENABLE(AOT_SCRATCH_REGISTER_CHECKER)

std::span<const NativeTest> stubScratchRegisterTests()
{
#if ENABLE(AOT_SCRATCH_REGISTER_CHECKER)
    static constexpr NativeTest tests[] = {
        { "stubs/scratch-registers/stubs-read-values-before-replacing-them"_s, testStubsReadScratchValuesBeforeReplacingThem },
        { "stubs/scratch-registers/checker-reports-conflicts"_s, testCheckerReportsConflicts },
        { "stubs/scratch-registers/checker-accepts-ordinary-code"_s, testCheckerAcceptsOrdinaryCode },
    };
    return tests;
#else
    return { };
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
