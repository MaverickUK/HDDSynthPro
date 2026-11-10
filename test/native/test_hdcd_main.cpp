// test_hdcd on its own, built by test/run_native_tests.sh with
// hdcd_device.h's candidate fixes on (HDDPRO_HDCD_FAST_WRITES): they must
// leave everything the host sees as it is, so the same tests pass both ways.
#include <cstdio>

int failures = 0;
int checks = 0;

void runHdcdTests();

int main() {
    runHdcdTests();
    std::printf("\nhdcd with its fast writes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
