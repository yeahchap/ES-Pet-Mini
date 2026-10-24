// PlatformIO 7 merges per-component C/C++ flags. Build ST's C++-compatible C
// source inside a C++ translation unit to avoid leaking language-only flags.
// Keep the original vendor source and its license unmodified.
#ifdef ES_PET_ULTIMATE_XIAO
#include "vendor/lsm6ds3/lsm6ds3_reg.c"
#endif
