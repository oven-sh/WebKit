//@ requireOptions("--useDollarVM=1")

// The IntlDateTimeFormatImpl cache is keyed on locale/style options, so a
// default-options formatter constructed after a time-zone change must not
// reuse the impl (and its resolved default time zone) built beforehand.

function currentTimeZone() {
    return new Intl.DateTimeFormat().resolvedOptions().timeZone;
}

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

$vm.setHostTimeZone("Etc/UTC");
shouldBe(currentTimeZone(), "UTC"); // primes the impl cache

$vm.setHostTimeZone("America/Anchorage");
shouldBe(currentTimeZone(), "America/Anchorage");

$vm.setHostTimeZone("Asia/Tokyo");
shouldBe(currentTimeZone(), "Asia/Tokyo");
