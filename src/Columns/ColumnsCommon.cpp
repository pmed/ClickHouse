#ifdef __SSE2__
    #include <emmintrin.h>
#endif

#include <Columns/IColumn.h>
#include <Columns/ColumnVector.h>
#include <Common/typeid_cast.h>
#include <Common/HashTable/HashSet.h>
#include "ColumnsCommon.h"


namespace DB
{

#if defined(__SSE2__) && defined(__POPCNT__)
/// Transform 64-byte mask to 64-bit mask.
static UInt64 toBits64(const Int8 * bytes64)
{
    static const __m128i zero16 = _mm_setzero_si128();
    UInt64 res =
        static_cast<UInt64>(_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes64)), zero16)))
        | (static_cast<UInt64>(_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes64 + 16)), zero16))) << 16)
        | (static_cast<UInt64>(_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes64 + 32)), zero16))) << 32)
        | (static_cast<UInt64>(_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes64 + 48)), zero16))) << 48);

    return ~res;
}
#endif

size_t countBytesInFilter(const UInt8 * filt, size_t sz)
{
    size_t count = 0;

    /** NOTE: In theory, `filt` should only contain zeros and ones.
      * But, just in case, here the condition > 0 (to signed bytes) is used.
      * It would be better to use != 0, then this does not allow SSE2.
      */

    const Int8 * pos = reinterpret_cast<const Int8 *>(filt);
    const Int8 * end = pos + sz;

#if defined(__SSE2__) && defined(__POPCNT__)
    const Int8 * end64 = pos + sz / 64 * 64;

    for (; pos < end64; pos += 64)
        count += __builtin_popcountll(toBits64(pos));

    /// TODO Add duff device for tail?
#endif

    for (; pos < end; ++pos)
        count += *pos != 0;

    return count;
}

size_t countBytesInFilter(const IColumn::Filter & filt)
{
    return countBytesInFilter(filt.data(), filt.size());
}

size_t countBytesInFilterWithNull(const IColumn::Filter & filt, const UInt8 * null_map)
{
    size_t count = 0;

    /** NOTE: In theory, `filt` should only contain zeros and ones.
      * But, just in case, here the condition > 0 (to signed bytes) is used.
      * It would be better to use != 0, then this does not allow SSE2.
      */

    const Int8 * pos = reinterpret_cast<const Int8 *>(filt.data());
    const Int8 * pos2 = reinterpret_cast<const Int8 *>(null_map);
    const Int8 * end = pos + filt.size();

#if defined(__SSE2__) && defined(__POPCNT__)
    const Int8 * end64 = pos + filt.size() / 64 * 64;

    for (; pos < end64; pos += 64, pos2 += 64)
        count += __builtin_popcountll(toBits64(pos) & ~toBits64(pos2));

        /// TODO Add duff device for tail?
#endif

    for (; pos < end; ++pos, ++pos2)
        count += (*pos & ~*pos2) != 0;

    return count;
}

std::vector<size_t> countColumnsSizeInSelector(IColumn::ColumnIndex num_columns, const IColumn::Selector & selector)
{
    std::vector<size_t> counts(num_columns);
    for (auto idx : selector)
        ++counts[idx];

    return counts;
}

bool memoryIsByte(const void * data, size_t size, uint8_t byte)
{
    if (size == 0)
        return true;
    const auto * ptr = reinterpret_cast<const uint8_t *>(data);
    return *ptr == byte && memcmp(ptr, ptr + 1, size - 1) == 0;
}

bool memoryIsZero(const void * data, size_t size)
{
    return memoryIsByte(data, size, 0x0);
}

namespace ErrorCodes
{
    extern const int SIZES_OF_COLUMNS_DOESNT_MATCH;
}


namespace
{
    /// Implementation details of filterArraysImpl function, used as template parameter.
    /// Allow to build or not to build offsets array.

    struct ResultOffsetsBuilder
    {
        IColumn::Offsets & res_offsets;
        IColumn::Offset current_src_offset = 0;

        explicit ResultOffsetsBuilder(IColumn::Offsets * res_offsets_) : res_offsets(*res_offsets_) {}

        void reserve(ssize_t result_size_hint, size_t src_size)
        {
            res_offsets.reserve(result_size_hint > 0 ? result_size_hint : src_size);
        }

        void insertOne(size_t array_size)
        {
            current_src_offset += array_size;
            res_offsets.push_back(current_src_offset);
        }

        template <size_t SIMD_BYTES>
        void insertChunk(
            const IColumn::Offset * src_offsets_pos,
            bool first,
            IColumn::Offset chunk_offset,
            size_t chunk_size)
        {
            const auto offsets_size_old = res_offsets.size();
            res_offsets.resize(offsets_size_old + SIMD_BYTES);
            memcpy(&res_offsets[offsets_size_old], src_offsets_pos, SIMD_BYTES * sizeof(IColumn::Offset));

            if (!first)
            {
                /// difference between current and actual offset
                const auto diff_offset = chunk_offset - current_src_offset;

                if (diff_offset > 0)
                {
                    auto * res_offsets_pos = &res_offsets[offsets_size_old];

                    /// adjust offsets
                    for (size_t i = 0; i < SIMD_BYTES; ++i)
                        res_offsets_pos[i] -= diff_offset;
                }
            }
            current_src_offset += chunk_size;
        }
    };

    struct NoResultOffsetsBuilder
    {
        explicit NoResultOffsetsBuilder(IColumn::Offsets *) {}
        void reserve(ssize_t, size_t) {}
        void insertOne(size_t) {}

        template <size_t SIMD_BYTES>
        void insertChunk(
            const IColumn::Offset *,
            bool,
            IColumn::Offset,
            size_t)
        {
        }
    };


    template <typename T, typename ResultOffsetsBuilder>
    void filterArraysImplGeneric(
        const PaddedPODArray<T> & src_elems, const IColumn::Offsets & src_offsets,
        PaddedPODArray<T> & res_elems, IColumn::Offsets * res_offsets,
        const IColumn::Filter & filt, ssize_t result_size_hint)
    {
        const size_t size = src_offsets.size();
        if (size != filt.size())
            throw Exception("Size of filter doesn't match size of column.", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

        ResultOffsetsBuilder result_offsets_builder(res_offsets);

        if (result_size_hint)
        {
            result_offsets_builder.reserve(result_size_hint, size);

            if (result_size_hint < 0)
                res_elems.reserve(src_elems.size());
            else if (result_size_hint < 1000000000 && src_elems.size() < 1000000000)    /// Avoid overflow.
                res_elems.reserve((result_size_hint * src_elems.size() + size - 1) / size);
        }

        const UInt8 * filt_pos = filt.data();
        const auto * filt_end = filt_pos + size;

        const auto * offsets_pos = src_offsets.data();
        const auto * offsets_begin = offsets_pos;

        /// copy array ending at *end_offset_ptr
        const auto copy_array = [&] (const IColumn::Offset * offset_ptr)
        {
            const auto arr_offset = offset_ptr == offsets_begin ? 0 : offset_ptr[-1];
            const auto arr_size = *offset_ptr - arr_offset;

            result_offsets_builder.insertOne(arr_size);

            const auto elems_size_old = res_elems.size();
            res_elems.resize(elems_size_old + arr_size);
            memcpy(&res_elems[elems_size_old], &src_elems[arr_offset], arr_size * sizeof(T));
        };

    #if defined(__AVX512F__) && defined(__AVX512BW__)
        const __m512i zero_vec = _mm512_setzero_epi32();
        static constexpr size_t SIMD_BYTES = 64;
        const auto * filt_end_aligned = filt_pos + size / SIMD_BYTES * SIMD_BYTES;

        while (filt_pos < filt_end_aligned)
        {
            uint64_t mask = _mm512_cmp_epi8_mask(_mm512_loadu_si512(reinterpret_cast<const __m512i *>(filt_pos)), zero_vec, _MM_CMPINT_GT);

            if (mask == 0xffffffffffffffff)
            {
                /// SIMD_BYTES consecutive rows pass the filter
                const auto first = offsets_pos == offsets_begin;

                const auto chunk_offset = first ? 0 : offsets_pos[-1];
                const auto chunk_size = offsets_pos[SIMD_BYTES - 1] - chunk_offset;

                result_offsets_builder.template insertChunk<SIMD_BYTES>(offsets_pos, first, chunk_offset, chunk_size);

                /// copy elements for SIMD_BYTES arrays at once
                const auto elems_size_old = res_elems.size();
                res_elems.resize(elems_size_old + chunk_size);
                memcpy(&res_elems[elems_size_old], &src_elems[chunk_offset], chunk_size * sizeof(T));
            }
            else
            {
                while (mask)
                {
                    size_t index = __builtin_ctzll(mask);
                    copy_array(offsets_pos + index);
                #ifdef __BMI__
                    mask = _blsr_u64(mask);
                #else
                    mask = mask & (mask-1);
                #endif
                }
            }

            filt_pos += SIMD_BYTES;
            offsets_pos += SIMD_BYTES;
        }
    #elif defined(__AVX__) && defined(__AVX2__)
        const __m256i zero_vec = _mm256_setzero_si256();
        static constexpr size_t SIMD_BYTES = 32;
        const auto * filt_end_aligned = filt_pos + size / SIMD_BYTES * SIMD_BYTES;

        while (filt_pos < filt_end_aligned)
        {
            uint32_t mask = _mm256_movemask_epi8(_mm256_cmpgt_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(filt_pos)), zero_vec));

            if (mask == 0xffffffff)
            {
                /// SIMD_BYTES consecutive rows pass the filter
                const auto first = offsets_pos == offsets_begin;

                const auto chunk_offset = first ? 0 : offsets_pos[-1];
                const auto chunk_size = offsets_pos[SIMD_BYTES - 1] - chunk_offset;

                result_offsets_builder.template insertChunk<SIMD_BYTES>(offsets_pos, first, chunk_offset, chunk_size);

                /// copy elements for SIMD_BYTES arrays at once
                const auto elems_size_old = res_elems.size();
                res_elems.resize(elems_size_old + chunk_size);
                memcpy(&res_elems[elems_size_old], &src_elems[chunk_offset], chunk_size * sizeof(T));
            }
            else
            {
                while (mask)
                {
                    size_t index = __builtin_ctz(mask);
                    copy_array(offsets_pos + index);
                #ifdef __BMI__
                    mask = _blsr_u32(mask);
                #else
                    mask = mask & (mask-1);
                #endif
                }
            }

            filt_pos += SIMD_BYTES;
            offsets_pos += SIMD_BYTES;
        }
    #elif defined(__SSE2__)
        const __m128i zero_vec = _mm_setzero_si128();
        static constexpr size_t SIMD_BYTES = 16;
        const auto * filt_end_aligned = filt_pos + size / SIMD_BYTES * SIMD_BYTES;

        while (filt_pos < filt_end_aligned)
        {
            UInt16 mask = _mm_movemask_epi8(_mm_cmpeq_epi8(
                _mm_loadu_si128(reinterpret_cast<const __m128i *>(filt_pos)),
                zero_vec));
            mask = ~mask;

            if (mask == 0xffff)
            {
                /// SIMD_BYTES consecutive rows pass the filter
                const auto first = offsets_pos == offsets_begin;

                const auto chunk_offset = first ? 0 : offsets_pos[-1];
                const auto chunk_size = offsets_pos[SIMD_BYTES - 1] - chunk_offset;

                result_offsets_builder.template insertChunk<SIMD_BYTES>(offsets_pos, first, chunk_offset, chunk_size);

                /// copy elements for SIMD_BYTES arrays at once
                const auto elems_size_old = res_elems.size();
                res_elems.resize(elems_size_old + chunk_size);
                memcpy(&res_elems[elems_size_old], &src_elems[chunk_offset], chunk_size * sizeof(T));
            }
            else
            {
                while (mask)
                {
                    size_t index = __builtin_ctz(mask);
                    copy_array(offsets_pos + index);
                    mask = mask & (mask - 1);
                }
            }

            filt_pos += SIMD_BYTES;
            offsets_pos += SIMD_BYTES;
        }
    #endif

        while (filt_pos < filt_end)
        {
            if (*filt_pos)
                copy_array(offsets_pos);

            ++filt_pos;
            ++offsets_pos;
        }
    }
}


template <typename T>
void filterArraysImpl(
    const PaddedPODArray<T> & src_elems, const IColumn::Offsets & src_offsets,
    PaddedPODArray<T> & res_elems, IColumn::Offsets & res_offsets,
    const IColumn::Filter & filt, ssize_t result_size_hint)
{
    return filterArraysImplGeneric<T, ResultOffsetsBuilder>(src_elems, src_offsets, res_elems, &res_offsets, filt, result_size_hint);
}

template <typename T>
void filterArraysImplOnlyData(
    const PaddedPODArray<T> & src_elems, const IColumn::Offsets & src_offsets,
    PaddedPODArray<T> & res_elems,
    const IColumn::Filter & filt, ssize_t result_size_hint)
{
    return filterArraysImplGeneric<T, NoResultOffsetsBuilder>(src_elems, src_offsets, res_elems, nullptr, filt, result_size_hint);
}


/// Explicit instantiations - not to place the implementation of the function above in the header file.
#define INSTANTIATE(TYPE) \
template void filterArraysImpl<TYPE>( \
    const PaddedPODArray<TYPE> &, const IColumn::Offsets &, \
    PaddedPODArray<TYPE> &, IColumn::Offsets &, \
    const IColumn::Filter &, ssize_t); \
template void filterArraysImplOnlyData<TYPE>( \
    const PaddedPODArray<TYPE> &, const IColumn::Offsets &, \
    PaddedPODArray<TYPE> &, \
    const IColumn::Filter &, ssize_t);

INSTANTIATE(UInt8)
INSTANTIATE(UInt16)
INSTANTIATE(UInt32)
INSTANTIATE(UInt64)
INSTANTIATE(Int8)
INSTANTIATE(Int16)
INSTANTIATE(Int32)
INSTANTIATE(Int64)
INSTANTIATE(Float32)
INSTANTIATE(Float64)

#undef INSTANTIATE

namespace detail
{
    template <typename T>
    const PaddedPODArray<T> * getIndexesData(const IColumn & indexes)
    {
        auto * column = typeid_cast<const ColumnVector<T> *>(&indexes);
        if (column)
            return &column->getData();

        return nullptr;
    }

    template const PaddedPODArray<UInt8> * getIndexesData<UInt8>(const IColumn & indexes);
    template const PaddedPODArray<UInt16> * getIndexesData<UInt16>(const IColumn & indexes);
    template const PaddedPODArray<UInt32> * getIndexesData<UInt32>(const IColumn & indexes);
    template const PaddedPODArray<UInt64> * getIndexesData<UInt64>(const IColumn & indexes);
}

size_t getLimitForPermutation(size_t column_size, size_t perm_size, size_t limit)
{
    if (limit == 0)
        limit = column_size;
    else
        limit = std::min(column_size, limit);

    if (perm_size < limit)
        throw Exception(ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH,
            "Size of permutation ({}) is less than required ({})", perm_size, limit);

    return limit;
}


}
