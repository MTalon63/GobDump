#pragma once

#include "common/dsp/complex.h"

/* From GNU Radio, adapted */

//! \brief SNR Estimator using 2nd and 4th-order moments.
/*! \ingroup snr_blk
 *
 *  An SNR estimator for M-PSK signals that uses 2nd (M2) and 4th
 *  (M4) order moments. This estimator uses knowledge of the
 *  kurtosis of the signal (\f$k_a)\f$ and noise (\f$k_w\f$) to make its
 *  estimation. We use Beaulieu's approximations here to M-PSK
 *  signals and AWGN channels such that \f$k_a=1\f$ and \f$k_w=2\f$. These
 *  approximations significantly reduce the complexity of the
 *  calculations (and computations) required.
 *
 *  Reference:
 *  D. R. Pauluzzi and N. C. Beaulieu, "A comparison of SNR
 *  estimation techniques for the AWGN channel," IEEE
 *  Trans. Communications, Vol. 48, No. 10, pp. 1681-1691, 2000.
 */
class M2M4SNREstimator
{
private:
    double d_y1, d_y2; // widened: 4th moment loses precision fast in float
    double d_alpha, d_beta;
    double d_signal, d_noise;

public:
    /*! Constructor
     *
     *  Parameters:
     *  \param alpha: the update rate of internal running average
     *  calculations.
     */
    M2M4SNREstimator(float alpha = 0.001);
    ~M2M4SNREstimator() {}

    void update(complex_t *input, int size);
    float snr();
    float signal();
    float noise();
};

//! \brief Decision-directed SNR estimator using error-vector magnitude (EVM).
/*!
 *  Uses recovered, phase/timing-corrected M-PSK symbols. For each symbol,
 *  decides the nearest ideal constellation point (constant modulus, so only
 *  phase matters) and measures the direct error power against it. Requires
 *  post-recovery symbols (e.g. output of a Costas loop + clock recovery),
 *  NOT raw pre-demod IQ.
 */
class EVMSNREstimator
{
private:
    int d_order;         // 2 = BPSK, 4 = QPSK/OQPSK, 8 = 8PSK
    double d_pwr, d_err; // EMA of total power / error power
    double d_alpha, d_beta;
    double d_signal, d_noise;
    double d_step; // 2*PI / order, precomputed

public:
    EVMSNREstimator(int order = 4, float alpha = 0.001);
    ~EVMSNREstimator() {}

    void update(complex_t *input, int size);
    float snr();
    float signal();
    float noise();
};
