/* Tiny assert harness for the host tests. No external framework. */
#pragma once

#include <stdbool.h>
#include <stdio.h>

extern unsigned long g_checks;
extern unsigned long g_check_failures;
extern bool g_test_failed;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        g_checks++;                                                                   \
        if(!(cond)) {                                                                 \
            g_check_failures++;                                                       \
            g_test_failed = true;                                                     \
            fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while(0)

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        unsigned long check_a_ = (unsigned long)(a);                                     \
        unsigned long check_b_ = (unsigned long)(b);                                     \
        g_checks++;                                                                      \
        if(check_a_ != check_b_) {                                                       \
            g_check_failures++;                                                          \
            g_test_failed = true;                                                        \
            fprintf(                                                                     \
                stderr,                                                                  \
                "  %s:%d: CHECK_EQ failed: %s == %s (%lu != %lu)\n",                     \
                __FILE__,                                                                \
                __LINE__,                                                                \
                #a,                                                                      \
                #b,                                                                      \
                check_a_,                                                                \
                check_b_);                                                               \
        }                                                                                \
    } while(0)

typedef void (*TestFn)(void);
void run_test(const char* name, TestFn fn);
#define RUN(fn) run_test(#fn, fn)

/* suites */
void suite_registry(void);
void suite_engine(void);
void suite_status(const char* fixtures_dir);
