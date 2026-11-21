//@ skip if $memoryLimited
//@ runDefault

// A Latin-1 string of 1 << 30 or more characters cannot be upconverted to
// UTF-16: the Vector<char16_t> behind StringView::upconvertedCharacters()
// cannot hold that many characters. When the collator has to hand ICU the
// UTF-16 form, compareStrings must throw instead of crashing.

function shouldThrowOutOfMemory(func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof RangeError) || error.message !== "Out of memory")
        throw new Error("expected RangeError: Out of memory, got " + error);
}

// One flat 8-bit string. The other operand is 16-bit so that none of the
// 8-bit fast paths apply and the comparison needs the UTF-16 upconversion.
const huge = "a".repeat(1 << 30);
const sixteenBit = "\u3042";

shouldThrowOutOfMemory(() => huge.localeCompare(sixteenBit));
shouldThrowOutOfMemory(() => sixteenBit.localeCompare(huge));
shouldThrowOutOfMemory(() => new Intl.Collator().compare(huge, sixteenBit));
shouldThrowOutOfMemory(() => new Intl.Collator("en", { sensitivity: "base" }).compare(sixteenBit, huge));
