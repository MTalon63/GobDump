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
    d_step = 2.0 * M_PI / (double)order;
    d_pwr = 0;
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

        // Reference amplitude from the running power average
        double A = sqrt(d_pwr);

        // Nearest ideal constellation phase (constant modulus -> only phase matters)
        double angle = atan2(im, re);
        double k = round(angle / d_step);
        double ideal_angle = k * d_step;

        double ideal_re = A * cos(ideal_angle);
        double ideal_im = A * sin(ideal_angle);

        double err_re = re - ideal_re;
        double err_im = im - ideal_im;
        double err_pow = err_re * err_re + err_im * err_im;

        d_err = d_alpha * err_pow + d_beta * d_err;
    }

    if (d_pwr != d_pwr)
        d_pwr = 0;
    if (d_err != d_err)
        d_err = 0;
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
