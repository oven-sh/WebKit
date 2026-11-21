//@ memoryHog!
//@ runDefault("--useRegExpImageMode=1")

if (/~~[^]{1073741824}/.exec("ሴ".repeat(2 ** 30 + 64)) !== null)
    throw new Error("matched");
