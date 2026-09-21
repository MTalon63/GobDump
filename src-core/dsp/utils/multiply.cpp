#include "multiply.h"
#include <type_traits>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        MultiplyBlock<T>::MultiplyBlock()
            : BlockSimpleMulti<T, T, 2, 1>(                                             //
                  "multiply_" + getShortTypeName<T>(),                                  //
                  {{"in 1", getTypeSampleType<T>()}, {"in 2", getTypeSampleType<T>()}}, //
                  {{"out", getTypeSampleType<T>()}})
        {
        }

        template <typename T>
        MultiplyBlock<T>::~MultiplyBlock()
        {
        }

        template <typename T>
        void MultiplyBlock<T>::process(T **input, uint32_t *nsamples, T **output, uint32_t *nsamples_out)
        {
            if (nsamples[0] != nsamples[1])
            {
                nsamples_out[0] = 0;
                return;
            }

            if constexpr (std::is_same_v<T, float>)
            {
                volk_32f_x2_multiply_32f(output[0], input[0], input[1], nsamples[0]);
            }
            else
            {
                volk_32fc_x2_multiply_32fc((lv_32fc_t *)output[0], (const lv_32fc_t *)input[0], (const lv_32fc_t *)input[1], nsamples[0]);
            }

            nsamples_out[0] = nsamples[0];
        }
        template class MultiplyBlock<float>;
        template class MultiplyBlock<complex_t>;
    } // namespace ndsp
} // namespace satdump
