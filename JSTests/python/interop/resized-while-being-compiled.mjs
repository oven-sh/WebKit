// The optimizing compilers put the length of a typed array, and where its elements are, into the code that they make, if the array is one that the code always uses. They go by nothing being able to change those
// until the array has an ArrayBuffer, which it is given when the code is put to use. Python can: a bytearray is a Uint8Array, and it is made longer and shorter where it is. If that happens while the code is being
// made, on another thread, the code is not to be used.
import * as m from "./resized-while-being-compiled.py";

let wrong = 0;
let refused = 0;
for (let round = 0; round < 400; ++round) {
    // A global of its own each time, with a variable that is written once, which is what makes the array something that the code always uses.
    const g = runString("var view; function last() { return view[view.length - 1]; } function at(i) { return view[i]; } function length() { return view.length; }");
    const a = m.make(2000 + round, 1);
    g.view = a;
    let size = a.length;
    let value = 1;
    for (let i = 0; i < 6000; ++i) {
        if (g.length() !== size || g.last() !== value || g.at(size >> 1) === undefined || g.at(size) !== undefined) {
            ++wrong;
            break;
        }
        if (!(i % 7)) {
            try {
                // Once the code is in use the array has an ArrayBuffer, and then Python may not.
                switch ((i / 7) % 3) {
                case 0:
                    value = (value % 200) + 1;
                    m.grow(a, 3000, value);
                    size += 3000;
                    break;
                case 1:
                    m.drop(a, 1500);
                    size -= 1500;
                    break;
                case 2:
                    m.shrink(a, size - 1400);
                    size -= 1400;
                    break;
                }
            } catch (e) {
                ++refused;
                break;
            }
        }
    }
}
// Whether the code ever came into use, which is when Python is refused, depends on how the engine was started.
print("wrong:", wrong);
