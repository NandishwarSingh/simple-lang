#include <cstdio>
#include <string>
#include <unordered_map>

int main() {
    std::unordered_map<long long, long long> a;
    for (long long i = 0; i < 10000000; i++) a[(i * 2654435761LL) % 4000037] = i;
    long long suma = 0;
    for (long long i = 0; i < 10000000; i++) {
        long long k = (i * 7919) % 4000037;
        auto it = a.find(k);
        if (it != a.end()) suma += it->second;
    }
    for (long long i = 0; i < 1000000; i++) a.erase((i * 31) % 4000037);
    suma += (long long)a.size() * 17;

    std::unordered_map<std::string, long long> wc;
    for (long long i = 0; i < 1500000; i++) {
        std::string w = "w" + std::to_string((i * 131) % 9973);
        auto it = wc.find(w);
        if (it != wc.end()) it->second++;
        else wc.emplace(std::move(w), 1);
    }
    long long sumb = 0;
    for (long long i = 0; i < 9973; i++) {
        auto it = wc.find("w" + std::to_string(i));
        if (it != wc.end()) sumb += it->second * (i + 1);
    }
    long long sumc = 0;
    for (auto& kv : wc) sumc += kv.second;

    printf("%lld\n", suma + sumb * 3 + sumc);
    return 0;
}
