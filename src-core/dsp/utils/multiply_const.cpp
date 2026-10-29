#include "multiply_const.h"
#include <type_traits>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        MultiplyConstBlock<T>::MultiplyConstBlock()
            : BlockSimple<T, T>(                            //
                  "multiply_const" + getShortTypeName<T>(), //
                  {{"in", getTypeSampleType<T>()}},         //
                  {{"out", getTypeSampleType<T>()}})
        {
        }

        template <typename T>
        MultiplyConstBlock<T>::~MultiplyConstBlock()
        {
        }

        template <typename T>
        uint32_t MultiplyConstBlock<T>::process(T *input, uint32_t nsamples, T *output)
        {
            if constexpr (std::is_same_v<T, float>)
            {
                volk_32f_s32f_multiply_32f(output, input, mult_const, nsamples);
            }
            else
            {
                volk_32fc_s32fc_multiply_32fc((lv_32fc_t *)output, (const lv_32fc_t *)input, complex_t(mult_const, 0.0f), nsamples);
            }

            return nsamples;
        }

        template class MultiplyConstBlock<float>;
        template class MultiplyConstBlock<complex_t>;
    } // namespace ndsp
} // namespace satdump
