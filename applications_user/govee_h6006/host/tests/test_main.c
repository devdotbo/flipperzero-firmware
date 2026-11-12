#include "harness.h"

#include <stdlib.h>

unsigned long g_checks = 0;
unsigned long g_check_failures = 0;
bool g_test_failed = false;

static unsigned g_tests = 0;
static unsigned g_failed_tests = 0;

void run_test(const char* name, TestFn fn) {
    g_test_failed = false;
    g_tests++;
    fn();
    if(g_test_failed) {
        g_failed_tests++;
        printf("FAIL %s\n", name);
    } else {
        printf("ok   %s\n", name);
    }
}

int main(int argc, char** argv) {
    const char* fixtures = argc > 1 ? argv[1] : NULL;
    suite_registry();
    suite_engine();
    suite_status(fixtures);
    printf(
        "SUMMARY: %u tests, %u failed, %lu checks, %lu check failures\n",
        g_tests,
        g_failed_tests,
        g_checks,
        g_check_failures);
    return g_failed_tests == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
