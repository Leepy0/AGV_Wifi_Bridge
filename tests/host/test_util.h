#pragma once
// 호스트 시험 공용: 실패해도 계속 돌고 마지막에 실패 수로 종료 코드를 정한다
#include <stdio.h>

static int g_fail, g_pass;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b); \
    if (va_ == vb_) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s == %s (0x%llX != 0x%llX)\n", __FILE__, __LINE__, #a, #b, va_, vb_); } \
} while (0)

static int test_summary(const char *name)
{
    printf("%s: %d passed, %d failed\n", name, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
