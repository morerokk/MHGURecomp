// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library.
#include "gamelib.h"

extern const uint32_t main_fn_addrs[], sdk_fn_addrs[], main_fn_count, sdk_fn_count;
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];

// (compiled with the generated code's flags: what they let the compiler use. Data, not a function: reading it runs
// none of the library's code.)
GAMELIB_EXPORT const uint32_t mhgugame_cpu_needs = 0
#ifdef __SSE4_2__
    | GAMELIB_CPU_SSE42
#endif
#ifdef __POPCNT__
    | GAMELIB_CPU_POPCNT
#endif
#ifdef __AVX__
    | GAMELIB_CPU_AVX
#endif
#ifdef __AVX2__
    | GAMELIB_CPU_AVX2
#endif
#ifdef __FMA__
    | GAMELIB_CPU_FMA
#endif
#ifdef __BMI__
    | GAMELIB_CPU_BMI
#endif
#ifdef __BMI2__
    | GAMELIB_CPU_BMI2
#endif
#ifdef __F16C__
    | GAMELIB_CPU_F16C
#endif
#ifdef __AVX512F__
    | GAMELIB_CPU_AVX512
#endif
    ;

GAMELIB_EXPORT const GameLibTables *mhgugame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI,  GAMELIB_ID,   main_fn_addrs,  sdk_fn_addrs,
                                    main_fn_ptrs, sdk_fn_ptrs,  &main_fn_count, &sdk_fn_count};
    return &t;
}
