#include "snr_estimator.h"
#include <algorithm>
#include <cmath>

M2M4SNREstimator::M2M4SNREstimator(float alpha)
{
    d_y1 = 0;
    d_y2 = 0;
    d_signal = 0;
    d_noise = 0;
    d_alpha = alpha;
    d_beta = 1.0 - alpha;
}

void M2M4SNREstimator::update(complex_t *input, int size)
{
    for (int i = 0; i < size; i++)
    {
        std::complex<float> c = (std::complex<float>)input[i];
        double mag2 = (double)c.real() * c.real() + (double)c.imag() * c.imag();

        double y1 = mag2;
        double y2 = mag2 * mag2;

        d_y1 = d_alpha * y1 + d_beta * d_y1;
        d_y2 = d_alpha * y2 + d_beta * d_y2;
    }

    if (d_y1 != d_y1)
        d_y1 = 0;
    if (d_y2 != d_y2)
        d_y2 = 0;
}

float M2M4SNREstimator::snr()
{
    double y1_2 = d_y1 * d_y1;
    double disc = 2.0 * y1_2 - d_y2;

    if (disc < 0.0)
        disc = 0.0;

    double root = sqrt(disc);
    d_signal = root;
    d_noise = d_y1 - root;

    if (d_noise <= 0.0)
        d_noise = 1e-12;

    return std::max<float>(0.0f, 10.0f * log10f((float)(d_signal / d_noise)));
}

float M2M4SNREstimator::signal() { return 10.0f * log10f((float)d_signal); }

float M2M4SNREstimator::noise() { return 10.0f * log10f((float)d_noise); }

EVMSNREstimator::EVMSNREstimator(int order, float alpha)
{
    d_order = order;
    d_pwr = 0;
    d_pwr_re = 0;
    d_pwr_im = 0;
    d_err = 0;
    d_signal = 0;
    d_noise = 0;
    d_alpha = alpha;
    d_beta = 1.0 - alpha;
}

void EVMSNREstimator::update(complex_t *input, int size)
{
    for (int i = 0; i < size; i++)
    {
        double re = input[i].real;
        double im = input[i].imag;

        double pwr = re * re + im * im;
        d_pwr = d_alpha * pwr + d_beta * d_pwr;
        d_pwr_re = d_alpha * (re * re) + d_beta * d_pwr_re;
        d_pwr_im = d_alpha * (im * im) + d_beta * d_pwr_im;

        double err_pow;

        if (d_order <= 4)
        {
            // BPSK / QPSK / OQPSK: per-component hard decision.
            // Each component independently carries ±a (binary antipodal).
            // Reference amplitude = sqrt(E[component^2]).
            // This is rotation-invariant: works for both axis and
            // diagonal constellations, and for OQPSK I/Q staggering.
            double a_re = sqrt(d_pwr_re);
            double a_im = sqrt(d_pwr_im);

            double ideal_re = (re >= 0.0) ? a_re : -a_re;
            double ideal_im = (im >= 0.0) ? a_im : -a_im;

            double err_re = re - ideal_re;
            double err_im = im - ideal_im;
            err_pow = err_re * err_re + err_im * err_im;
        }
        else
        {
            // 8PSK: use angle-based nearest point.
            // Try both possible grid rotations (offset 0 and step/2)
            // and pick whichever gives less error per sample.
            double A = sqrt(d_pwr);
            double step = 2.0 * M_PI / (double)d_order;
            double angle = atan2(im, re);

            double k0 = round(angle / step);
            double ideal0 = k0 * step;
            double k1 = round((angle - step * 0.5) / step);
            double ideal1 = k1 * step + step * 0.5;

            double d0_re = re - A * cos(ideal0);
            double d0_im = im - A * sin(ideal0);
            double d1_re = re - A * cos(ideal1);
            double d1_im = im - A * sin(ideal1);

            double e0 = d0_re * d0_re + d0_im * d0_im;
            double e1 = d1_re * d1_re + d1_im * d1_im;
            err_pow = (e0 < e1) ? e0 : e1;
        }

        d_err = d_alpha * err_pow + d_beta * d_err;
    }

    if (d_pwr != d_pwr)
        d_pwr = 0;
    if (d_err != d_err)
        d_err = 0;
    if (d_pwr_re != d_pwr_re)
        d_pwr_re = 0;
    if (d_pwr_im != d_pwr_im)
        d_pwr_im = 0;
}

float EVMSNREstimator::snr()
{
    double ps = d_pwr - d_err;
    if (ps < 0.0)
        ps = 0.0;

    double pn = d_err;
    if (pn <= 0.0)
        pn = 1e-12;

    d_signal = ps;
    d_noise = pn;

    return std::max<float>(0.0f, 10.0f * log10f((float)(ps / pn)));
}

float EVMSNREstimator::signal() { return 10.0f * log10f((float)d_signal); }
float EVMSNREstimator::noise() { return 10.0f * log10f((float)d_noise); }
