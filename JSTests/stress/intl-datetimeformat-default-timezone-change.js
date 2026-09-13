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

// A time-zone change takes effect the next time the VM is entered, so each check runs in its own task.
$vm.setHostTimeZone("Etc/UTC");
setTimeout(() => {
    shouldBe(currentTimeZone(), "UTC"); // primes the impl cache

    $vm.setHostTimeZone("America/Anchorage");
    setTimeout(() => {
        shouldBe(currentTimeZone(), "America/Anchorage");

        $vm.setHostTimeZone("Asia/Tokyo");
        setTimeout(() => {
            shouldBe(currentTimeZone(), "Asia/Tokyo");
        }, 0);
    }, 0);
}, 0);
