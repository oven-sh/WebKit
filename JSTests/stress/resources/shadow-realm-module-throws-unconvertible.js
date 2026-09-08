globalThis.ran = false;

throw {
    toString() {
        globalThis.ran = true;
        throw globalThis;
    }
};

export const value = 1;
