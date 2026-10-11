#pragma once
#include <stdio.h>
#include <stdlib.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define DONE(name) do { printf("%s: %s\n", name, g_fail ? "FAILED" : "ok"); return g_fail ? 1 : 0; } while (0)
