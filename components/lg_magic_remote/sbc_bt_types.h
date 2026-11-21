// Minimal stand-in for Bluedroid's stack/bt_types.h, so the SBC encoder copied from ESP-IDF
// (components/bt/host/bluedroid/external/sbc/encoder, Apache-2.0) builds without Bluedroid (we use NimBLE).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE (!FALSE)
#endif
#ifndef SBC_ENC_INCLUDED
#define SBC_ENC_INCLUDED TRUE
#endif
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int8_t INT8;
typedef int16_t INT16;
typedef int32_t INT32;
typedef uint8_t BOOLEAN;
#define APPL_TRACE_EVENT(...) do {} while (0)
