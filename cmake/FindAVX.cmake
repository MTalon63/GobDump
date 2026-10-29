#.rst:
# FindAVX
# -------
#
# Finds AVX / AVX2 / AVX512 support: the compiler accepts the flags AND this CPU can run them.
# The probes are run, not just compiled; -mavx2 compiles on an AVX-only CPU and then SIGILLs.
#
# Sets plain variables <lang>_{AVX,AVX2,AVX512}_{FOUND,FLAGS} for lang in C, CXX.
# Cross-compiling probes nothing; opt in with -DSIMD_TRUST_CPU_AVX2=ON (etc).
#
# =============================================================================

INCLUDE(CheckCSourceCompiles)
INCLUDE(CheckCXXSourceCompiles)
INCLUDE(CheckCSourceRuns)
INCLUDE(CheckCXXSourceRuns)

OPTION(SIMD_TRUST_CPU_AVX "Enable AVX without probing the CPU (cross-compiling, target is known)" OFF)
OPTION(SIMD_TRUST_CPU_AVX2 "Enable AVX2+FMA without probing the CPU (cross-compiling, target is known)" OFF)
OPTION(SIMD_TRUST_CPU_AVX512 "Enable AVX512 without probing the CPU (cross-compiling, target is known)" OFF)

# argc in, volatile out, so the probed instruction really executes.

SET(AVX_CODE "
  #include <immintrin.h>

  volatile float avx_sink;

  int main(int argc, char **argv)
  {
    __m256 a = _mm256_set1_ps((float)argc);
    __m256 b = _mm256_mul_ps(a, a);
    avx_sink = _mm256_cvtss_f32(_mm256_add_ps(b, a));
    return 0;
  }
")

# -mfma is part of the flags we ship, so FMA must work too.
SET(AVX2_CODE "
  #include <immintrin.h>

  volatile int avx2_sink;
  volatile float fma_sink;

  int main(int argc, char **argv)
  {
    __m256i a = _mm256_set1_epi16((short)argc);
    a = _mm256_abs_epi16(a);
    avx2_sink = _mm256_extract_epi32(a, 0);

    __m256 x = _mm256_set1_ps((float)argc);
    fma_sink = _mm256_cvtss_f32(_mm256_fmadd_ps(x, x, x));
    return 0;
  }
")

SET(AVX512_CODE "
  #include <immintrin.h>

  volatile int avx512_sink;

  int main(int argc, char **argv)
  {
    __m512i a = _mm512_set1_epi32(argc);
    __m512i b = _mm512_abs_epi32(a);
    avx512_sink = (int)_mm512_cmp_epi32_mask(a, b, _MM_CMPINT_EQ);
    return 0;
  }
")

# Drop results cached by the old compile-only probes.
FOREACH(__SIMD_VAR
    C_AVX_FOUND C_AVX_FLAGS C_AVX2_FOUND C_AVX2_FLAGS C_AVX512_FOUND C_AVX512_FLAGS
    CXX_AVX_FOUND CXX_AVX_FLAGS CXX_AVX2_FOUND CXX_AVX2_FLAGS CXX_AVX512_FOUND CXX_AVX512_FLAGS)
  UNSET(${__SIMD_VAR} CACHE)
ENDFOREACH()
UNSET(__SIMD_VAR)

MACRO(CHECK_SSE lang type flags)
  SET(${lang}_${type}_FOUND FALSE)
  SET(${lang}_${type}_FLAGS "")
  SET(__FLAG_I 1)
  SET(CMAKE_REQUIRED_FLAGS_SAVE "${CMAKE_REQUIRED_FLAGS}")

  IF(CMAKE_CROSSCOMPILING AND NOT SIMD_TRUST_CPU_${type})
    MESSAGE(STATUS "${lang} ${type} : cross-compiling, CPU not probed - disabled (set SIMD_TRUST_CPU_${type}=ON if the target has it)")
  ELSE()
    FOREACH(__FLAG ${flags})
      IF(NOT ${lang}_${type}_FOUND)
        SET(CMAKE_REQUIRED_FLAGS "${__FLAG}")
        IF("${lang}" STREQUAL "CXX")
          IF(CMAKE_CROSSCOMPILING)
            CHECK_CXX_SOURCE_COMPILES("${${type}_CODE}" ${lang}_HAS_${type}_BUILD_${__FLAG_I})
            SET(__SIMD_OK "${${lang}_HAS_${type}_BUILD_${__FLAG_I}}")
          ELSE()
            CHECK_CXX_SOURCE_RUNS("${${type}_CODE}" ${lang}_HAS_${type}_RUN_${__FLAG_I})
            SET(__SIMD_OK "${${lang}_HAS_${type}_RUN_${__FLAG_I}}")
          ENDIF()
        ELSE()
          IF(CMAKE_CROSSCOMPILING)
            CHECK_C_SOURCE_COMPILES("${${type}_CODE}" ${lang}_HAS_${type}_BUILD_${__FLAG_I})
            SET(__SIMD_OK "${${lang}_HAS_${type}_BUILD_${__FLAG_I}}")
          ELSE()
            CHECK_C_SOURCE_RUNS("${${type}_CODE}" ${lang}_HAS_${type}_RUN_${__FLAG_I})
            SET(__SIMD_OK "${${lang}_HAS_${type}_RUN_${__FLAG_I}}")
          ENDIF()
        ENDIF()
        IF(__SIMD_OK)
          SET(${lang}_${type}_FOUND TRUE)
          SET(${lang}_${type}_FLAGS "${__FLAG}")
        ENDIF()
        MATH(EXPR __FLAG_I "${__FLAG_I}+1")
      ENDIF()
    ENDFOREACH()
    SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_SAVE}")

    IF(${lang}_${type}_FOUND)
      MESSAGE(STATUS "${lang} ${type} : enabled (flags = [${${lang}_${type}_FLAGS}])")
    ELSE()
      MESSAGE(STATUS "${lang} ${type} : disabled - compiler accepts the flags, but this CPU cannot execute them")
    ENDIF()
  ENDIF()
ENDMACRO()

CHECK_SSE(C "AVX" " ;-mavx;/arch:AVX")
CHECK_SSE(C "AVX2" " ;-mavx2 -mfma;/arch:AVX2")
CHECK_SSE(C "AVX512" " ;-mavx512f -mavx512dq -mavx512vl -mavx512bw -mfma;/arch:AVX512")

CHECK_SSE(CXX "AVX" " ;-mavx;/arch:AVX")
CHECK_SSE(CXX "AVX2" " ;-mavx2 -mfma;/arch:AVX2")
CHECK_SSE(CXX "AVX512" " ;-mavx512f -mavx512dq -mavx512vl -mavx512bw -mfma;/arch:AVX512")
