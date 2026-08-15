import { shouldBe } from "./resources/assert.js";
import { value as first } from "./code-cache-key-compares-source/first.js";
import { value as second } from "./code-cache-key-compares-source/second.js";

// first.js and second.js are the same length and their source text has the
// same StringImpl hash (24 bits of RapidHash, 0xd438ab), so they produce equal
// SourceCodeKey hashes. SourceCodeKey::operator== has to fall back to comparing
// the source text, otherwise the CodeCache hands second.js the
// UnlinkedModuleProgramCodeBlock compiled for first.js and second.js silently
// evaluates first.js's code. Both files must stay byte-for-byte as they are
// (no comments, no whitespace changes) or they stop colliding; a replacement
// pair can be found by hashing candidate sources with
// StringHasher::computeHashAndMaskTop8Bits until two of them match.
shouldBe(first, 10001215);
shouldBe(second, 10006987);
