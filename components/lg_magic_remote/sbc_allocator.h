// Stand-in for Bluedroid's osi/allocator.h (see ../README.md).
#pragma once
#include <stdlib.h>
#define osi_malloc(n) malloc(n)
#define osi_free(p) free(p)
