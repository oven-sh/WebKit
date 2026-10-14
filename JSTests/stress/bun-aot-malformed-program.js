//@ requireOptions("--useDollarVM=1")
// Bun: bytes that are no program of the front end are refused: no image is made of them and the dump says why. Nothing crashes.
if (typeof $vm.buildAOTImageOfTypedProgram === "function") {
    let seed = 1;
    const random = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) >>> 8;
    const inputs = [new Uint8Array(0), new Uint8Array(1), new Uint8Array(64), new Uint8Array(4096).fill(255)];
    for (let length = 1; length <= 1 << 16; length *= 4) {
        const bytes = new Uint8Array(length);
        for (let i = 0; i < length; i++)
            bytes[i] = random();
        inputs.push(bytes);
    }
    for (const bytes of inputs) {
        const image = $vm.buildAOTImageOfTypedProgram("0;", bytes);
        if (image !== null)
            throw new Error("an image was made of " + bytes.length + " bytes that are no program");
        let message = null;
        try {
            $vm.dumpAOTTypedProgram(bytes, "read");
        } catch (error) {
            message = String(error.message);
        }
        if (!message)
            throw new Error("the dump of " + bytes.length + " bytes that are no program said nothing");
    }
}
