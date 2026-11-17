#!/usr/bin/env ruby
# What offlineasm makes of an instruction between beginSigningLinkRegister and endSigningLinkRegister.

$: << File.expand_path("../../../Source/JavaScriptCore/offlineasm", __dir__)
require "arm64"

SIGN, AUTHENTICATE, STRIP, LOAD = "hint #27", "hint #31", "hint #7", "ldrb wzr, [x30]"
def atAddress(address, middle) ["stp x16, x17, [sp, #-16]!", *address, *middle, "ldp x16, x17, [sp], #16"] end
def add(register, from, distance) ["add #{register}, #{from}, ##{distance}"] end
def stored(address) atAddress(address, ["mov x17, x30", "hint #10", "stur x17, [x16, #-8]"]) end
def loaded(address) atAddress(address, ["ldur x17, [x16, #-8]", "hint #14", "mov x30, x17"]) end

expected = {}
["ret", "br x30", "blr lr", "cbz x30, 1f", "cbgt x30, x9, 1f", "pacga x9, x10, x11", "cmp x9, x30", "subs xzr, x30, x9", "adr x30, 1f",
    "movk x30, #1, lsl #16", "add x30, x30, #4", "ubfx x30, x30, #0, #48", "csel x30, x30, xzr, eq", "ldr w9, [x30, #4]",
    "ldr x9, [x10, x30]", "ld1 {v0.2d}, [x30], #16", "prfm pldl1keep, [x30]", "stp x9, x10, [sp, #-16]!", "hint #34", "hint #symbol", "1:"].each { | text | expected[text] = [text] }
["hint #7", "hint 27", "hint #0x1f // AUTIBSP", "pacibsp", "AUTIB x9, x10", "xpaclri", LOAD].each { | text | expected[text] = [] }
["stp x29, x30, [sp, #-16]!", "stp xzr, lr, [sp, #-0x10]!", "stur x30, [sp, #-8]", "str w30, [x9]"].each { | text | expected[text] = [SIGN, text, STRIP] }
["ldp x29, x30, [sp], #16", "ldr x30, [sp], 8", "ldr x30, [x30, #8]", "ldp x9, x30, [x30]", "ldapur x30, [x30, #8]", "ldr w30, [x9]", "ld1 {v0.2d}, [x30], x9"].each { | text | expected[text] = [text, AUTHENTICATE, LOAD] }
["mov x9, x30", "mov x30, x9", "mov x30, sp", "mov x30, ip0", "fmov x30, d0", "ins v0.d[1], x30", "mrs x30, tpidr_el0", "msr tpidr_el0, x30", "ldr x30, 1f", "ldar x30, [x9]",
    "stlxr w30, x9, [x10]", "casal x30, x9, [x10]", "ldadd x30, x9, [x10]", "ld1 {v0.2d}, [x9], x30", "incp x30, p0.b", "movt x30, zt0[8]", "gcspopm x30"].each { | text | expected[text] = [SIGN, text, AUTHENTICATE, LOAD] }
expected.merge!({
    "1: stp x29, x30, [sp, #-16]!" => ["1:", SIGN, "stp x29, x30, [sp, #-16]!", STRIP],
    "1 : stur x30, [sp, #-8]" => ["1 :", SIGN, "stur x30, [sp, #-8]", STRIP],
    'stur\tx30, [sp, #-8]' => [SIGN, "stur x30, [sp, #-8]", STRIP],
    'nop; stur x30, [sp, #-8] // a; b\n/* c; */ ldr x30, [sp], #8' => ["nop", SIGN, "stur x30, [sp, #-8]", STRIP, "ldr x30, [sp], #8", AUTHENTICATE, LOAD],
    "str x30, [x9, #32752]" => ["str xzr, [x9, #32752]", *stored(["add x16, x9, #7, lsl #12", "add x16, x16, #4088"])],
    "stur x30, [x17, #-256]" => ["stur xzr, [x17, #-256]", *stored(["sub x16, x17, #248"])],
    "str x30, [sp, #-16]!" => ["str xzr, [sp, #-16]!", *stored(add("x16", "sp", 24))],
    "str x30, [x9], #16" => ["str xzr, [x9], #16", *stored(["sub x16, x9, #8"])],
    "str x30, [sp, w9, sxtw #3]" => ["str xzr, [sp, w9, sxtw #3]", *stored(["add x16, sp, w9, sxtw #3", *add("x16", "x16", 24)])],
    "stp x30, x30, [x9]" => ["stp xzr, xzr, [x9]", *stored(add("x16", "x9", 8)), *stored(add("x16", "x9", 16))],
    "ldr x30, [x29, #8]" => [*loaded(add("x16", "x29", 16)), LOAD],
    "ldr x30, [x9, #-16]!" => [*loaded(["sub x16, x9, #8"]), "sub x9, x9, #16", LOAD],
    "ldp x29, x30, [x29, #0]" => [*loaded(add("x16", "x29", 16)), "ldp x29, xzr, [x29, #0]", LOAD],
    "ldp x30, x9, [sp], #16" => [*loaded(add("x16", "sp", 24)), "ldp xzr, x9, [sp], #16", LOAD],
    "ldp x30, xzr, [sp], #16" => [*loaded(add("x16", "sp", 24)), *add("sp", "sp", 16), LOAD],
})

failures = expected.reject { | text, result | arm64SigningLinkRegister(text) == result }
failures.each { | text, result | puts "FAIL #{text}\n  expected #{result.join("; ")}\n  got      #{arm64SigningLinkRegister(text).join("; ")}" }
puts "offlineasm: #{expected.size - failures.size} passed, #{failures.size} failed"
exit failures.empty?
