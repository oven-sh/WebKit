export default function* () {
    yield new Error().stack;
}
