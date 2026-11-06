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

static void testStubsReadConstantsAndCopiesBeforeReplacingThem()
{
    unsigned numberOfUnreadValues = 0;
    for (const StubOptions& options : stubOptionVariants()) {
        // Without the fast path of an allocation nobody reads what is laid out for it.
        if (options.forceGCSlowPaths)
            continue;
        StubBlob blob = buildEveryStub(options);
        std::span<const uint8_t> code = blob.code.bytes.span();
        Vector<StubPlacement> placementsByOffset = blob.placements;
        std::ranges::stable_sort(placementsByOffset, { }, &StubPlacement::offset);

        for (const UnreadValue& value : findUnreadConstantsAndCopies(code)) {
            const StubPlacement& stub = findStubAt(placementsByOffset, value.offset);
            ++numberOfUnreadValues;
            dataLog("x", static_cast<unsigned>(value.reg), " is written and on every path written again before it is read in ");
            dump(WTF::dataFile(), stub.key);
            dataLogLn(", which starts at ", stub.offset, ":");
            printInstructions(WTF::dataFile(), code, value.offset, value.offset + 2 * sizeof(uint32_t));
        }
    }
    AOT_TEST_CHECK(!numberOfUnreadValues);
}

static void testCheckerFollowsValuesOverBranches()
{
    constexpr GPRReg x0 = ARM64Registers::x0;
    auto find = [](std::initializer_list<uint32_t> words) {
        return findUnreadConstantsAndCopies(asByteSpan(std::span(words)));
    };
    auto finds = [&](std::initializer_list<uint32_t> words, std::initializer_list<UnreadValue> expected) {
        return std::ranges::equal(find(words), expected);
    };

    // mov w0, #0 . b +8 . orr w0, wzr, #1 . mov x0, x26 . cbnz w0, +8 . ret . ret
    AOT_TEST_CHECK(finds({ 0x52800000, 0x14000002, 0x320003E0, 0xAA1A03E0, 0x35000040, 0xD65F03C0, 0xD65F03C0 }, { { x0, 0 }, { x0, 8 } }));
    // mov x0, x1 . mov x0, x2 . ret
    AOT_TEST_CHECK(finds({ 0xAA0103E0, 0xAA0203E0, 0xD65F03C0 }, { { x0, 0 } }));
    // Both paths write it again: mov w0, #0 . cbz x1, +12 . mov x0, x2 . ret . mov x0, x3 . ret
    AOT_TEST_CHECK(finds({ 0x52800000, 0xB4000061, 0xAA0203E0, 0xD65F03C0, 0xAA0303E0, 0xD65F03C0 }, { { x0, 0 } }));

    // mov w13, #0 . b +8 . orr w13, wzr, #1 . mov x0, x26 . cbnz w13, +8 . ret . ret
    AOT_TEST_CHECK(find({ 0x5280000D, 0x14000002, 0x320003ED, 0xAA1A03E0, 0x3500004D, 0xD65F03C0, 0xD65F03C0 }).isEmpty());
    // One path reads it: mov w0, #0 . cbz x1, +12 . mov x0, x2 . ret . str x0, [x1] . ret
    AOT_TEST_CHECK(find({ 0x52800000, 0xB4000061, 0xAA0203E0, 0xD65F03C0, 0xF9000020, 0xD65F03C0 }).isEmpty());
    // A call may read it: mov w0, #0 . blr x11 . mov x0, x2 . ret
    AOT_TEST_CHECK(find({ 0x52800000, 0xD63F0160, 0xAA0203E0, 0xD65F03C0 }).isEmpty());
    // So may what a jump out of the code reaches: mov w0, #0 . b +4096
    AOT_TEST_CHECK(find({ 0x52800000, 0x14000400 }).isEmpty());
    // A loop reads it in its next turn: mov w0, #0 . add x1, x1, x0 . mov w0, #1 . cbnz x1, -8 . ret
    AOT_TEST_CHECK(find({ 0x52800000, 0x8B000021, 0x52800020, 0xB5FFFFC1, 0xD65F03C0 }).isEmpty());
    // What is loaded or computed is not looked at: ldr x0, [x1] . add x0, x1, #1 . mov x0, x2 . ret
    AOT_TEST_CHECK(find({ 0xF9400020, 0x91000420, 0xAA0203E0, 0xD65F03C0 }).isEmpty());
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
    // ldp x15, x16, [sp, #120] . ldur x17, [sp, #136] . ldur x17, [x26, #40] . cmp sp, x17
    AOT_TEST_CHECK(findConflicts({ 0xA947C3EF, 0xF84883F1, 0xF8428351, 0xEB3163FF }).isEmpty());
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
        { "stubs/scratch-registers/stubs-read-constants-and-copies-before-replacing-them"_s, testStubsReadConstantsAndCopiesBeforeReplacingThem },
        { "stubs/scratch-registers/checker-follows-values-over-branches"_s, testCheckerFollowsValuesOverBranches },
    };
    return tests;
#else
    return { };
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
