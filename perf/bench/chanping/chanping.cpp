#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

struct Slot {
    std::mutex m;
    std::condition_variable nf, ne;
    long long v = 0;
    int count = 0;

    void send(long long val) {
        std::unique_lock<std::mutex> lk(m);
        nf.wait(lk, [&] { return count == 0; });
        v = val;
        count = 1;
        ne.notify_one();
    }
    long long recv() {
        std::unique_lock<std::mutex> lk(m);
        ne.wait(lk, [&] { return count == 1; });
        long long val = v;
        count = 0;
        nf.notify_one();
        return val;
    }
};

Slot ping, pong;

int main() {
    std::thread t([] {
        for (;;) {
            long long v = ping.recv();
            if (v == -1) return;
            pong.send(v + 1);
        }
    });
    long long total = 0;
    for (long long i = 0; i < 100000; i++) {
        ping.send(i);
        total += pong.recv();
    }
    ping.send(-1);
    t.join();
    std::printf("%lld\n", total);
    return 0;
}
