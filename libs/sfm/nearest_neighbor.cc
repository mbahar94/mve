/*
 * Nearest Neighbor implementation using exhaustive search.
 * Written by Simon Fuhrmann and Stepan Konrad.
 *
 * A helpful SSE/MMX overview.
 * Taken from: http://www.linuxjournal.com/content/
 *         ... introduction-gcc-compiler-intrinsics-vector-processing
 *
 * Compiler Options:
 * - X86/MMX/SSE1/2/3   -mfpmath=sse -mmmx -msse -msse2 -msse3
 * - ARM Neon           -mfpu=neon -mfloat-abi=softfp
 * - Freescale Altivec  -maltivec -mabi=altivec
 *
 * Include Files:
 * - arm_neon.h      ARM Neon types & intrinsics
 * - altivec.h       Freescale Altivec types & intrinsics
 * - mmintrin.h      X86 MMX
 * - xmmintrin.h     X86 SSE1
 * - emmintrin.h     X86 SSE2
 * - pmmintrin.h     X86 SSE3
 *
 * MMX/SSE Data Types:
 * - MMX:  __m64 64 bits of integers.
 * - SSE1: __m128 128 bits: four single precision floats.
 * - SSE2: __m128i 128 bits of packed integers, __m128d 128 bits: two doubles.
 *
 * Macros to check for availability:
 * - X86 MMX            __MMX__
 * - X86 SSE            __SSE__
 * - X86 SSE2           __SSE2__
 * - X86 SSE3           __SSE3__
 * - altivec functions  __VEC__
 * - neon functions     __ARM_NEON__
 */

#include <limits>
#include <iostream>
#include <emmintrin.h> // SSE2
#include <pmmintrin.h> // SSE3

#include "sfm/nearest_neighbor.h"

SFM_NAMESPACE_BEGIN

template <>
void
NearestNeighbor<short>::find (short const* query,
    NearestNeighbor<short>::Result* result) const
{
    /* Result distances are shamelessly misused to store inner products. */
    result->dist_1st_best = -std::numeric_limits<short>::max();
    result->dist_2nd_best = -std::numeric_limits<short>::max();
    result->index_1st_best = 0;
    result->index_2nd_best = 0;

#if ENABLE_SSE2_NN_SEARCH && defined(__SSE2__)
    /*
     * SSE inner product implementation.
     * Note that query and result should be 16 byte aligned.
     * Otherwise loading and storing values into/from registers is slow.
     * The dimension size must be divisible by 8, each __m128i register
     * can load 8 shorts = 16 bytes = 128 bit.
     */

    /* Using a constant number reduces computation time by about 1/3. */
    int const dim_8 = this->dimensions / 8;
    __m128i const* descr_ptr = reinterpret_cast<__m128i const*>(this->elements);
    for (int descr_iter = 0; descr_iter < this->num_elements; ++descr_iter)
    {
        /* Compute dot product between query and candidate. */
        __m128i const* query_ptr = reinterpret_cast<__m128i const*>(query);
        __m128i reg_result = _mm_set1_epi16(0);
        for (int i = 0; i < dim_8; ++i, ++query_ptr, ++descr_ptr)
        {
            __m128i reg_query = _mm_load_si128(query_ptr);
            __m128i reg_subject = _mm_load_si128(descr_ptr);
            reg_result = _mm_add_epi16(reg_result,
                _mm_mullo_epi16(reg_query, reg_subject));
        }
        short tmp[8];
        _mm_store_si128(reinterpret_cast<__m128i*>(tmp), reg_result);
        int inner_product = tmp[0] + tmp[1] + tmp[2] + tmp[3]
            + tmp[4] + tmp[5] + tmp[6] + tmp[7];

        /* Check if new largest inner product has been found. */
        if (inner_product > result->dist_2nd_best)
        {
            if (inner_product > result->dist_1st_best)
            {
                result->index_2nd_best = result->index_1st_best;
                result->dist_2nd_best = result->dist_1st_best;
                result->index_1st_best = descr_iter;
                result->dist_1st_best = inner_product;
            }
            else
            {
                result->index_2nd_best = descr_iter;
                result->dist_2nd_best = inner_product;
            }
        }
    }
#else
    short const* descr_ptr = this->elements;
    for (int i = 0; i < this->num_elements; ++i)
    {
        int inner_product = 0;
        for (int i = 0; i < this->dimensions; ++i, ++descr_ptr)
            inner_product += query[i] * *descr_ptr;

        /* Check if new largest inner product has been found. */
        if (inner_product > result->dist_2nd_best)
        {
            if (inner_product > result->dist_1st_best)
            {
                result->index_2nd_best = result->index_1st_best;
                result->dist_2nd_best = result->dist_1st_best;
                result->index_1st_best = i;
                result->dist_1st_best = inner_product;
            }
            else
            {
                result->index_2nd_best = i;
                result->dist_2nd_best = inner_product;
            }
        }
    }
#endif

    /*
     * Compute actual square distances.
     * The distance with signed 'char' vectors is: 2 * 127^2 - 2 * <Q, Ci>.
     * Unfortunately, the maximum distance is (2*127)^2, which does not fit
     * in a short. Therefore, the distance is clapmed at 127^2.
     */
    result->dist_1st_best = std::min(16129, std::max(0, (int)result->dist_1st_best));
    result->dist_2nd_best = std::min(16129, std::max(0, (int)result->dist_2nd_best));
    result->dist_1st_best = 32258 - 2 * result->dist_1st_best;
    result->dist_2nd_best = 32258 - 2 * result->dist_2nd_best;
}

template <>
void
NearestNeighbor<float>::find (float const* query,
    NearestNeighbor<float>::Result* result) const
{
    /* Result distances are shamelessly misused to store inner products. */
    result->dist_1st_best = -std::numeric_limits<float>::max();
    result->dist_2nd_best = -std::numeric_limits<float>::max();
    result->index_1st_best = 0;
    result->index_2nd_best = 0;

#if ENABLE_SSE3_NN_SEARCH && defined(__SSE3__)
    /*
     * SSE inner product implementation.
     * Note that query and result should be 16 byte aligned.
     * Otherwise loading and storing values into/from registers is slow.
     * The dimension size must be divisible by 4, each __m128 register
     * can load 4 floats = 16 bytes = 128 bit.
     */

    __m128 const* descr_ptr = reinterpret_cast<__m128 const*>(this->elements);
    int const dim_4 = this->dimensions / 4;
    for (int descr_iter = 0; descr_iter < this->num_elements; ++descr_iter)
    {
        /* Compute dot product between query and candidate. */
        __m128 const* query_ptr = reinterpret_cast<__m128 const*>(query);
        __m128 sum = _mm_setzero_ps();
        for (int i = 0; i < dim_4; ++i, ++query_ptr, ++descr_ptr)
            sum = _mm_add_ps(sum, _mm_mul_ps(*query_ptr, *descr_ptr));
        sum = _mm_hadd_ps(sum, sum);
        sum = _mm_hadd_ps(sum, sum);

        /* Check if new largest inner product has been found. */
        float inner_product = _mm_cvtss_f32(sum);
        if (inner_product > result->dist_2nd_best)
        {
            if (inner_product > result->dist_1st_best)
            {
                result->index_2nd_best = result->index_1st_best;
                result->dist_2nd_best = result->dist_1st_best;
                result->index_1st_best = descr_iter;
                result->dist_1st_best = inner_product;
            }
            else
            {
                result->index_2nd_best = descr_iter;
                result->dist_2nd_best = inner_product;
            }
        }
    }
#else
    float const* descr_ptr = this->elements;
    for (int i = 0; i < this->num_elements; ++i)
    {
        float inner_product = 0.0f;
        for (int j = 0; j < this->dimensions; ++j, ++descr_ptr)
            inner_product += query[j] * *descr_ptr;

        /* Check if new largest inner product has been found. */
        if (inner_product > result->dist_2nd_best)
        {
            if (inner_product > result->dist_1st_best)
            {
                result->index_2nd_best = result->index_1st_best;
                result->dist_2nd_best = result->dist_1st_best;
                result->index_1st_best = i;
                result->dist_1st_best = inner_product;
            }
            else
            {
                result->index_2nd_best = i;
                result->dist_2nd_best = inner_product;
            }
        }
    }
#endif

    /*
     * Compute actual (square) distances.
     */
    result->dist_1st_best = std::min(1.0f, std::max(0.0f,
        2.0f - 2.0f * result->dist_1st_best));
    result->dist_2nd_best = std::min(1.0f, std::max(0.0f,
        2.0f - 2.0f * result->dist_2nd_best));
}

SFM_NAMESPACE_END
