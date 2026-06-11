// r4: offline dump of KeyAtomStringCache bucket (hash % 512) for every key the
// butterfly-stress workload resolves through the cache. Replicates
// HashTranslatorCharBuffer<Latin1Character>: StringHasher::computeHashAndMaskTop8Bits
// == RapidHash::computeHashAndMaskTop8Bits (same headers as the engine).
#include <wtf/text/StringHasherInlines.h>
#include <cstdio>
#include <string>
#include <vector>
int main()
{
    std::vector<std::string> keys;
    for (int p = 0; p < 24; ++p) keys.push_back("p" + std::to_string(p));
    for (int i = 0; i < 60; ++i) keys.push_back("late" + std::to_string(i));
    for (int i = 0; i < 5; ++i) keys.push_back("fromSlot" + std::to_string(i));
    keys.push_back("tid"); keys.push_back("serial"); keys.push_back("indexed");
    keys.push_back("count"); keys.push_back("hits");
    std::vector<std::vector<std::string>> buckets(512);
    for (auto& k : keys) {
        std::span<const uint8_t> s(reinterpret_cast<const uint8_t*>(k.data()), k.size());
        unsigned h = WTF::StringHasher::computeHashAndMaskTop8Bits(s);
        unsigned b = h % 512;
        printf("%-12s hash=0x%08x bucket=%u\n", k.c_str(), h, b);
        buckets[b].push_back(k);
    }
    printf("\n== COLLIDING BUCKETS ==\n");
    int ncoll = 0;
    for (unsigned b = 0; b < 512; ++b) {
        if (buckets[b].size() > 1) {
            ++ncoll;
            printf("bucket %u:", b);
            for (auto& k : buckets[b]) printf(" %s", k.c_str());
            printf("\n");
        }
    }
    printf("colliding buckets: %d\n", ncoll);
    return 0;
}
