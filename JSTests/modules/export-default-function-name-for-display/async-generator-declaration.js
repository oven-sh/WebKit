export default async function* () {
    await null;
    yield new Error().stack;
}
