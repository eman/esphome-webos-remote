#!/bin/sh
# Builds and runs the mSBC encoder host test.
set -e
cd "$(dirname "$0")"
C=../../components/lg_magic_remote
cc -std=c99 -O2 -Wall -I$C -o test_msbc test_msbc.c $C/sbc_analysis.c $C/sbc_dct.c $C/sbc_dct_coeffs.c \
  $C/sbc_enc_bit_alloc_mono.c $C/sbc_enc_bit_alloc_ste.c $C/sbc_enc_coeffs.c $C/sbc_encoder.c $C/sbc_packing.c -lm
./test_msbc "$@"
