#include "subtract.h"
#include <type_traits>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        SubtractBlock<T>::SubtractBlock()
            : BlockSimpleMulti<T, T, 2, 1>(                                             //
                  "subtract_" + getShortTypeName<T>(),                                  //
                  {{"in 1", getTypeSampleType<T>()}, {"in 2", getTypeSampleType<T>()}}, //
                  {{"out", getTypeSampleType<T>()}})
        {
        }

        template <typename T>
        SubtractBlock<T>::~SubtractBlock()
        {
        }

        template <typename T>
        void SubtractBlock<T>::process(T **input, uint32_t *nsamples, T **output, uint32_t *nsamples_out)
        {
            if (nsamples[0] != nsamples[1])
            {
                nsamples_out[0] = 0;
                return;
            }

            if constexpr (std::is_same_v<T, float>)
            {
                volk_32f_x2_subtract_32f(output[0], input[0], input[1], nsamples[0]);
            }
            else
            {
                // No volk_32fc_x2_subtract_32fc kernel exists in VOLK; complex_t stays scalar.
                for (uint32_t i = 0; i < nsamples[0]; i++)
                {
                    output[0][i] = input[0][i] - input[1][i];
                }
            }

            nsamples_out[0] = nsamples[0];
        }
        template class SubtractBlock<float>;
        template class SubtractBlock<complex_t>;
    } // namespace ndsp
} // namespace satdump
