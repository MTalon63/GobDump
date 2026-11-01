#include "swap_reader.h"
#include "image/io.h"
#include "image/jpeg_utils.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace proba
{
    namespace swap
    {
        namespace
        {
            constexpr uint8_t IMAGE_MARKER = 0x01;
            constexpr int IMAGE_HEADER_LEN = 8;
            constexpr int METADATA_END = 92;
            constexpr int STREAM_START = 88;
            constexpr int CRC_LEN = 2;
            constexpr int PACKET_LEN = 65536;
            constexpr int DATA_PER_PACKET = PACKET_LEN - 6 - IMAGE_HEADER_LEN - CRC_LEN;
            constexpr uint16_t SEQUENCE_MODULO = 1 << 14;
            constexpr uint32_t MAX_STREAM_SIZE = 64 * 1024 * 1024;

            SWAPReader::ImageHeader parseHeader(const std::vector<uint8_t> &payload)
            {
                SWAPReader::ImageHeader header;
                if (payload.size() < METADATA_END)
                    return header;

                header.end = proba2::readBE(payload, 1, 6);
                header.start = proba2::readBE(payload, 8, 6);
                header.obs_mode = payload[22];
                header.lang_rot = proba2::readBEDouble(&payload[23]);
                header.pav_rot0 = proba2::readBEDouble(&payload[31]);
                header.pav_rot1 = proba2::readBEDouble(&payload[39]);
                header.capture_mode = payload[47];
                header.exposure = (uint32_t)proba2::readBE(payload, 48, 4);
                header.compressed_size = (uint32_t)proba2::readBE(payload, 76, 4);
                header.jpeg_length = (uint32_t)proba2::readBE(payload, 88, 4);

                header.valid = header.compressed_size <= MAX_STREAM_SIZE && header.jpeg_length > 0 && header.jpeg_length <= MAX_STREAM_SIZE && header.packetCount() > 0 &&
                               header.packetCount() <= 1024;
                return header;
            }

            constexpr double CDELT1 = 3.1646941;
            constexpr double CDELT2 = 3.1760467;
            constexpr double MOUNT_OFFSET = -0.3;
            constexpr double LIMB_ABOVE_PHOTOSPHERE = 2.0;
            constexpr int LIMB_ANGLES = 720;
            constexpr int RING_SEARCH = 64;

            constexpr double PI = 3.14159265358979323846;

            // Grey level -> detector DN, the archive's level 0 values.
            constexpr uint16_t RECODE_TABLE[256] = {
                10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
                26, 27, 29, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65, 68,
                71, 74, 77, 80, 84, 89, 94, 99, 104, 109, 114, 119, 124, 129, 134, 139,
                144, 149, 154, 159, 164, 169, 174, 179, 185, 192, 199, 206, 213, 220, 227, 234,
                241, 248, 255, 262, 269, 276, 283, 291, 300, 309, 318, 327, 336, 345, 354, 363,
                372, 381, 390, 399, 408, 417, 426, 435, 444, 453, 462, 471, 480, 490, 501, 512,
                523, 534, 545, 556, 567, 578, 589, 600, 611, 622, 633, 644, 656, 669, 682, 695,
                708, 721, 734, 747, 760, 773, 786, 799, 812, 825, 838, 851, 864, 877, 890, 903,
                916, 930, 945, 960, 975, 990, 1005, 1020, 1035, 1050, 1065, 1080, 1095, 1110, 1125, 1140,
                1155, 1170, 1186, 1203, 1220, 1237, 1254, 1271, 1288, 1305, 1322, 1339, 1356, 1373, 1390, 1407,
                1424, 1441, 1458, 1475, 1493, 1512, 1531, 1550, 1569, 1588, 1607, 1626, 1645, 1664, 1683, 1702,
                1721, 1740, 1759, 1778, 1797, 1816, 1836, 1857, 1878, 1899, 1920, 1941, 1962, 1983, 2004, 2025,
                2046, 2067, 2088, 2109, 2130, 2151, 2172, 2193, 2214, 2235, 2257, 2280, 2303, 2326, 2349, 2372,
                2395, 2418, 2441, 2464, 2487, 2510, 2533, 2556, 2579, 2603, 2628, 2653, 2678, 2703, 2728, 2753,
                2778, 2803, 2828, 2853, 2878, 2903, 2928, 2953, 2978, 3003, 3028, 3053, 3078, 3103, 3128, 3154,
                3181, 3208, 3235, 3262, 3289, 3316, 3343, 3370, 3397, 3424, 3451, 3478, 3505, 3532, 3560, 3588,
            };

            struct Matrix3
            {
                double m[3][3];
            };

            Matrix3 matrixMultiply(const Matrix3 &a, const Matrix3 &b)
            {
                Matrix3 r;
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++)
                        r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
                return r;
            }

            Matrix3 matrixIdentity()
            {
                Matrix3 r = {};
                r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
                return r;
            }

            Matrix3 matrixTranslate(double x, double y)
            {
                Matrix3 r = matrixIdentity();
                r.m[0][2] = x;
                r.m[1][2] = y;
                return r;
            }

            Matrix3 matrixRotate(double degrees)
            {
                double c = std::cos(degrees * PI / 180.0), s = std::sin(degrees * PI / 180.0);
                Matrix3 r = matrixIdentity();
                r.m[0][0] = c;
                r.m[0][1] = -s;
                r.m[1][0] = s;
                r.m[1][1] = c;
                return r;
            }

            void matrixApply(const Matrix3 &m, double x, double y, double &out_x, double &out_y)
            {
                out_x = m.m[0][0] * x + m.m[0][1] * y + m.m[0][2];
                out_y = m.m[1][0] * x + m.m[1][1] * y + m.m[1][2];
            }

            Matrix3 matrixInverse(const Matrix3 &m)
            {
                double a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
                double d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
                double g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];
                double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);

                Matrix3 r = {};
                if (std::abs(det) < 1e-12)
                    return r;
                double inv = 1.0 / det;
                r.m[0][0] = (e * i - f * h) * inv;
                r.m[0][1] = (c * h - b * i) * inv;
                r.m[0][2] = (b * f - c * e) * inv;
                r.m[1][0] = (f * g - d * i) * inv;
                r.m[1][1] = (a * i - c * g) * inv;
                r.m[1][2] = (c * d - a * f) * inv;
                r.m[2][0] = (d * h - e * g) * inv;
                r.m[2][1] = (b * g - a * h) * inv;
                r.m[2][2] = (a * e - b * d) * inv;
                return r;
            }

            void keysWeights(double f, double w[4])
            {
                constexpr double a = -0.5;
                double f2 = f * f, f3 = f2 * f;
                w[0] = a * (f3 - 2 * f2 + f);
                w[1] = (a + 2) * f3 - (a + 3) * f2 + 1;
                w[2] = -(a + 2) * f3 + (2 * a + 3) * f2 - a * f;
                w[3] = a * (f2 - f3);
            }

            double bilinear(const std::vector<float> &image, int width, int height, double x, double y)
            {
                int x0 = std::clamp((int)std::floor(x), 0, width - 2);
                int y0 = std::clamp((int)std::floor(y), 0, height - 2);
                double fx = x - x0, fy = y - y0;

                auto at = [&](int xx, int yy) { return (double)image[(size_t)yy * width + xx]; };
                return at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
            }

            double cubicInterpolate(const std::vector<float> &image, int width, int height, double x, double y)
            {
                if (!(x >= 0 && x < width && y >= 0 && y < height))
                    return 0.0;

                int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
                double wx[4], wy[4];
                keysWeights(x - x0, wx);
                keysWeights(y - y0, wy);

                double sum = 0;
                for (int j = 0; j < 4; j++)
                {
                    int yy = std::clamp(y0 + j - 1, 0, height - 1);
                    double across = 0;
                    for (int i = 0; i < 4; i++)
                    {
                        int xx = std::clamp(x0 + i - 1, 0, width - 1);
                        across += wx[i] * image[(size_t)yy * width + xx];
                    }
                    sum += wy[j] * across;
                }
                return sum;
            }

            struct LimbFit
            {
                double x = 0, y = 0, radius = 0, agreeing = 0;
            };

            bool ringCenter(const std::vector<float> &image, int width, int height, double radius, double &out_x, double &out_y)
            {
                int center_x = width / 2, center_y = height / 2;
                std::vector<std::pair<int, int>> ring;
                int extent = (int)std::ceil(radius + 4);
                for (int dy = -extent; dy <= extent; dy++)
                    for (int dx = -extent; dx <= extent; dx++)
                        if (std::abs(std::hypot((double)dx, (double)dy) - radius) < 3.0)
                            ring.push_back({dx, dy});
                if (ring.empty())
                    return false;

                auto score = [&](int dx, int dy)
                {
                    double sum = 0;
                    int ox = center_x + dx, oy = center_y + dy;
                    for (auto &point : ring)
                    {
                        int x = ox + point.first, y = oy + point.second;
                        if (x >= 0 && y >= 0 && x < width && y < height)
                            sum += image[(size_t)y * width + x];
                    }
                    return sum;
                };

                int best_dx = 0, best_dy = 0;
                double best = -1;
                for (int dy = -RING_SEARCH; dy <= RING_SEARCH; dy += 2)
                    for (int dx = -RING_SEARCH; dx <= RING_SEARCH; dx += 2)
                    {
                        double value = score(dx, dy);
                        if (value > best)
                        {
                            best = value;
                            best_dx = dx;
                            best_dy = dy;
                        }
                    }

                for (int dy = best_dy - 2; dy <= best_dy + 2; dy++)
                    for (int dx = best_dx - 2; dx <= best_dx + 2; dx++)
                    {
                        double value = score(dx, dy);
                        if (value > best)
                        {
                            best = value;
                            best_dx = dx;
                            best_dy = dy;
                        }
                    }

                out_x = center_x + best_dx;
                out_y = center_y + best_dy;
                return true;
            }

            bool limbCircle(const std::vector<float> &image, int width, int height, double x, double y, double radius, LimbFit &fit)
            {
                double rho_min = radius * 0.97, rho_max = radius * 1.03;
                constexpr double rho_step = 0.25;
                int rho_steps = (int)std::ceil((rho_max - rho_min) / rho_step);

                std::vector<double> px, py;
                for (int angle = 0; angle < LIMB_ANGLES; angle++)
                {
                    double theta = 2.0 * PI * angle / LIMB_ANGLES;
                    double ct = std::cos(theta), st = std::sin(theta);
                    double best = -1, best_x = 0, best_y = 0;
                    bool inside = true;
                    for (int k = 0; k < rho_steps; k++)
                    {
                        double r = rho_min + rho_step * k;
                        double sx = x + ct * r, sy = y + st * r;
                        double value = bilinear(image, width, height, sx, sy);
                        if (value > best)
                        {
                            best = value;
                            best_x = sx;
                            best_y = sy;
                        }
                        if (!(sx >= 0 && sx < width - 1 && sy >= 0 && sy < height - 1))
                            inside = false;
                    }
                    if (inside)
                    {
                        px.push_back(best_x);
                        py.push_back(best_y);
                    }
                }

                if (px.size() < 3)
                    return false;

                std::vector<char> keep(px.size(), 1);
                double cx = x, cy = y, fitted = radius;

                for (int iteration = 0; iteration < 3; iteration++)
                {
                    int count = 0;
                    for (char k : keep)
                        count += k;
                    if (count < 3)
                        return false;

                    double a00 = 0, a01 = 0, a02 = 0, a11 = 0, a12 = 0, a22 = 0;
                    double b0 = 0, b1 = 0, b2 = 0;
                    for (size_t i = 0; i < px.size(); i++)
                    {
                        if (!keep[i])
                            continue;
                        double X = 2 * px[i], Y = 2 * py[i], R = px[i] * px[i] + py[i] * py[i];
                        a00 += X * X;
                        a01 += X * Y;
                        a02 += X;
                        a11 += Y * Y;
                        a12 += Y;
                        a22 += 1;
                        b0 += X * R;
                        b1 += Y * R;
                        b2 += R;
                    }

                    double det = a00 * (a11 * a22 - a12 * a12) - a01 * (a01 * a22 - a12 * a02) + a02 * (a01 * a12 - a11 * a02);
                    if (std::abs(det) < 1e-12)
                        return false;

                    cx = (b0 * (a11 * a22 - a12 * a12) - a01 * (b1 * a22 - a12 * b2) + a02 * (b1 * a12 - a11 * b2)) / det;
                    cy = (a00 * (b1 * a22 - a12 * b2) - b0 * (a01 * a22 - a12 * a02) + a02 * (a01 * b2 - b1 * a02)) / det;
                    double c = (a00 * (a11 * b2 - b1 * a12) - a01 * (a01 * b2 - b1 * a02) + b0 * (a01 * a12 - a11 * a02)) / det;
                    fitted = std::sqrt(std::max(0.0, c + cx * cx + cy * cy));

                    double sum = 0, sum2 = 0;
                    std::vector<double> residual(px.size());
                    for (size_t i = 0; i < px.size(); i++)
                    {
                        residual[i] = std::hypot(px[i] - cx, py[i] - cy) - fitted;
                        if (keep[i])
                        {
                            sum += residual[i];
                            sum2 += residual[i] * residual[i];
                        }
                    }
                    double mean = sum / count;
                    double spread = std::sqrt(std::max(0.0, sum2 / count - mean * mean));

                    for (size_t i = 0; i < px.size(); i++)
                        keep[i] = std::abs(residual[i]) < 2.5 * spread + 0.5;
                }

                int count = 0;
                for (char k : keep)
                    count += k;

                fit.x = cx;
                fit.y = cy;
                fit.radius = fitted;
                fit.agreeing = (double)count / LIMB_ANGLES;
                return true;
            }

            bool findSunCenter(const std::vector<float> &dn, int width, int height, double photosphere, LimbFit &fit)
            {
                double x = width / 2.0, y = height / 2.0;
                ringCenter(dn, width, height, photosphere, x, y);

                LimbFit current;
                for (int iteration = 0; iteration < 2; iteration++)
                {
                    if (!limbCircle(dn, width, height, x, y, photosphere, current))
                        return false;
                    x = current.x;
                    y = current.y;
                }

                fit = current;
                return std::abs(fit.radius - (photosphere + LIMB_ABOVE_PHOTOSPHERE)) < 4.0 && fit.agreeing >= 0.5;
            }
        }

        int SWAPReader::ImageHeader::packetCount() const
        {
            uint64_t stream = (uint64_t)(STREAM_START - IMAGE_HEADER_LEN) + compressed_size;
            return (int)((stream + DATA_PER_PACKET - 1) / DATA_PER_PACKET);
        }

        int SWAPReader::ImageHeader::packetLength(int index) const
        {
            int count = packetCount();
            if (index < count - 1)
                return PACKET_LEN;

            int64_t last_data = (int64_t)(STREAM_START - IMAGE_HEADER_LEN) + (int64_t)compressed_size - (int64_t)DATA_PER_PACKET * (count - 1);
            if (last_data < 0 || last_data > DATA_PER_PACKET)
                return -1;
            return 6 + IMAGE_HEADER_LEN + (int)last_data + CRC_LEN;
        }

        SWAPReader::SWAPReader(std::string &outputfolder, proba2::GPSClock &gps_clock, bool align, bool gif_animation) : clock(gps_clock), align_sun(align), make_gif(gif_animation)
        {
            count = 0;
            output_folder = outputfolder;
        }

        void SWAPReader::work(ccsds::CCSDSPacket &packet)
        {
            const auto &payload = packet.payload;

            if (payload.size() <= METADATA_END || payload[0] != IMAGE_MARKER)
                return;

            uint64_t end = proba2::readBE(payload, 1, 6);
            if (end == 0)
                return;

            auto it = currentOuts.find(end);
            if (it == currentOuts.end())
            {
                it = currentOuts.insert({end, WIP_Swap()}).first;
                count++;
                logger->debug("SWAP: new image, end of acquisition stamp %llu", (unsigned long long)end);
            }

            auto &image = it->second;
            uint16_t sequence = packet.header.packet_sequence_count;
            bool crc_ok = proba2::crc16OK(packet);

            auto existing = image.packets.find(sequence);
            if (existing == image.packets.end() || (crc_ok && !existing->second.crc_ok))
                image.packets[sequence] = Packet{payload, crc_ok};
        }

        bool SWAPReader::findFirstPacket(const WIP_Swap &image, uint16_t &sequence, const Packet *&packet) const
        {
            const Packet *fallback = nullptr;
            uint16_t fallback_sequence = 0;

            for (auto &entry : image.packets)
            {
                const Packet &candidate = entry.second;
                if (candidate.data.size() <= METADATA_END + 1)
                    continue;
                if (!(candidate.data[METADATA_END] == 0xFF && candidate.data[METADATA_END + 1] == 0xD8))
                    continue;

                if (candidate.crc_ok)
                {
                    sequence = entry.first;
                    packet = &candidate;
                    return true;
                }
                if (fallback == nullptr)
                {
                    fallback = &candidate;
                    fallback_sequence = entry.first;
                }
            }

            if (fallback != nullptr)
            {
                sequence = fallback_sequence;
                packet = fallback;
                return true;
            }
            return false;
        }

        bool SWAPReader::assemble(const WIP_Swap &image, bool keep_bit_errors, Assembled &out) const
        {
            uint16_t first_sequence = 0;
            const Packet *first = nullptr;
            if (!findFirstPacket(image, first_sequence, first))
                return false;

            out.header = parseHeader(first->data);
            if (!out.header.valid)
                return false;

            out.header_ok = first->crc_ok;
            out.packets_expected = out.header.packetCount();

            std::vector<uint8_t> stream;
            stream.reserve((size_t)out.packets_expected * DATA_PER_PACKET);
            int bad_crc = 0;
            int used = 0;

            for (int index = 0; index < out.packets_expected; index++)
            {
                uint16_t sequence = (uint16_t)((first_sequence + index) % SEQUENCE_MODULO);
                auto it = image.packets.find(sequence);
                if (it == image.packets.end())
                    break;

                int expected_length = out.header.packetLength(index);
                if (expected_length < 0 || (int)(6 + it->second.data.size()) != expected_length)
                    break;

                if (!it->second.crc_ok)
                {
                    if (!keep_bit_errors)
                        break;
                    bad_crc++;
                }

                const auto &payload = it->second.data;
                size_t start = (index == 0) ? METADATA_END : IMAGE_HEADER_LEN;
                if (payload.size() < start + CRC_LEN)
                    break;

                stream.insert(stream.end(), payload.begin() + start, payload.end() - CRC_LEN);
                used++;
            }

            out.packets = used;
            out.bad_crc = bad_crc;

            bool complete = used == out.packets_expected;
            if (complete && out.header.jpeg_length <= stream.size() && out.header.jpeg_length >= 2 &&
                stream[out.header.jpeg_length - 2] == 0xFF && stream[out.header.jpeg_length - 1] == 0xD9)
            {
                out.jpeg.assign(stream.begin(), stream.begin() + out.header.jpeg_length);
            }
            else
            {
                out.jpeg = stream;

                out.jpeg.push_back(0xFF);
                out.jpeg.push_back(0xD9);
            }

            return !out.jpeg.empty();
        }

        double SWAPReader::imageQuality(image::Image &img)
        {
            if (img.size() == 0 || img.depth() != 8)
                return 0;

            size_t n = img.size();
            double sum = 0, sum2 = 0;
            for (size_t i = 0; i < n; i++)
            {
                double v = img.get(i);
                sum += v;
                sum2 += v * v;
            }
            double mean = sum / n;
            double variance = sum2 / n - mean * mean;
            return variance > 0 ? std::sqrt(variance) : 0;
        }

        int SWAPReader::rollDegrees(double lang_rot)
        {

            if (!(lang_rot >= -1.0 && lang_rot <= 1.0))
                return -1;

            constexpr double RAD_TO_DEG = 180.0 / 3.14159265358979323846;
            double roll = 2.0 * std::asin(lang_rot) * RAD_TO_DEG;
            double nearest = std::round(roll / 90.0) * 90.0;
            if (std::abs(roll - nearest) > 0.01)
                return -1;

            return ((int)std::lround(roll) % 360 + 360) % 360;
        }

        int SWAPReader::northUpSteps(double lang_rot)
        {
            int roll = rollDegrees(lang_rot);
            if (roll < 0)
                return -1;

            return ((270 - roll) / 90) % 4;
        }

        image::Image SWAPReader::rotate90(image::Image &img, int steps)
        {
            steps = ((steps % 4) + 4) % 4;
            if (steps == 0 || img.size() == 0)
                return img;

            image::Image current = img;
            for (int step = 0; step < steps; step++)
            {

                image::Image rotated(current.depth(), current.height(), current.width(), current.channels());
                for (int channel = 0; channel < current.channels(); channel++)
                    for (size_t y = 0; y < current.height(); y++)
                        for (size_t x = 0; x < current.width(); x++)
                            rotated.set(channel, y, current.width() - 1 - x, current.get(channel, x, y));
                current = std::move(rotated);
            }
            return current;
        }

        bool SWAPReader::alignImage(image::Image &img, const ImageHeader &header, double utc)
        {
            int width = (int)img.width(), height = (int)img.height();
            if (img.size() == 0 || img.depth() != 8 || img.channels() < 1 || width < 16 || height < 16)
                return false;

            int roll = rollDegrees(header.lang_rot);
            if (roll < 0)
                return false;

            // p2sw_imgcor works in FITS orientation: row 0 is the last JPEG row.
            std::vector<float> grey((size_t)width * height), dn((size_t)width * height);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                {
                    float value = (float)img.get(0, x, height - 1 - y);
                    grey[(size_t)y * width + x] = value;
                    int grey_level = std::clamp((int)std::lround(value), 0, 255);
                    dn[(size_t)y * width + x] = (float)RECODE_TABLE[grey_level];
                }

            double photosphere = proba2::sunAngularRadiusArcsec(utc) / CDELT1;
            LimbFit fit;
            if (!findSunCenter(dn, width, height, photosphere, fit))
            {
                logger->warn("SWAP: limb fit failed (sun %.1f,%.1f r=%.1f, %.0f%% of angles), not centring", fit.x, fit.y, fit.radius, fit.agreeing * 100.0);
                return false;
            }

            double solar_ep = proba2::solarEpDeg(utc);
            double crota = std::fmod(270.0 - roll + 360.0, 360.0);
            double cx = (width - 1.0) / 2.0, cy = (height - 1.0) / 2.0;

            // uncenter . solar_ep . translate . mount . roll . scale . center
            Matrix3 center = matrixTranslate(-cx, -cy);
            Matrix3 uncenter = matrixTranslate(cx, cy);
            Matrix3 scale = matrixIdentity();
            scale.m[1][1] = CDELT2 / CDELT1;
            Matrix3 roll_matrix = matrixMultiply(matrixRotate(MOUNT_OFFSET), matrixRotate(crota));
            Matrix3 chain = matrixMultiply(roll_matrix, matrixMultiply(scale, center));

            double moved_x, moved_y;
            matrixApply(matrixMultiply(uncenter, chain), fit.x, fit.y, moved_x, moved_y);
            Matrix3 translate = matrixTranslate(cx - moved_x, cy - moved_y);
            Matrix3 matrix = matrixMultiply(matrixMultiply(uncenter, matrixRotate(solar_ep)), matrixMultiply(translate, chain));
            Matrix3 inverse = matrixInverse(matrix);
            if (std::abs(inverse.m[2][2]) < 1e-12)
                return false;

            std::vector<float> aligned((size_t)width * height);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                {
                    double in_x, in_y;
                    matrixApply(inverse, (double)x, (double)y, in_x, in_y);
                    aligned[(size_t)y * width + x] = (float)cubicInterpolate(grey, width, height, in_x, in_y);
                }

            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    img.set(0, x, height - 1 - y, (int)std::lround(std::clamp((double)aligned[(size_t)y * width + x], 0.0, 255.0)));

            logger->info("SWAP: limb fit at (%.2f, %.2f), r=%.2f, %.0f%% of angles; roll %d, SOLAR_EP %.3f", fit.x, fit.y, fit.radius, fit.agreeing * 100.0, roll, solar_ep);
            return true;
        }

        std::string SWAPReader::uniqueName(const std::string &filename) const
        {
            if (!std::filesystem::exists(output_folder + "/" + filename + ".png"))
                return filename;

            int index = 0;
            while (std::filesystem::exists(output_folder + "/" + filename + "-" + std::to_string(index) + ".png"))
                index++;
            return filename + "-" + std::to_string(index);
        }

        void SWAPReader::save()
        {
            clock.finalize();

            for (auto &entry : currentOuts)
            {
                WIP_Swap &image = entry.second;
                Assembled assembled;
                bool assembled_ok = assemble(image, true, assembled) && !assembled.jpeg.empty();

                image::Image img;
                if (assembled_ok)
                {
                    img = image::decompress_jpeg(assembled.jpeg.data(), assembled.jpeg.size());

                    if (imageQuality(img) <= 1.0)
                    {
                        Assembled strict;
                        if (assemble(image, false, strict) && !strict.jpeg.empty())
                        {
                            auto strict_img = image::decompress_jpeg(strict.jpeg.data(), strict.jpeg.size());
                            if (imageQuality(strict_img) > 1.0)
                            {
                                assembled = std::move(strict);
                                img = std::move(strict_img);
                            }
                        }
                    }

                    if (imageQuality(img) <= 1.0)
                        assembled_ok = false;
                }

                uint64_t start_stamp = assembled.header.valid ? assembled.header.start : entry.first;
                double utc = clock.utcFromStamp((double)start_stamp);
                std::string name = "SWAP_" + proba2::formatUTC(utc, "%Y%m%dT%H%M%SZ");

                if (!assembled_ok)
                {
                    logger->warn("SWAP: image ending at stamp %llu has no usable JPEG, skipping", (unsigned long long)entry.first);
                    continue;
                }

                int steps = northUpSteps(assembled.header.lang_rot);
                bool aligned = false;
                if (align_sun)
                {
                    if (steps < 0)
                        logger->warn("SWAP: damaged LANG_ROT in " + name + ", not aligning");
                    else
                        aligned = alignImage(img, assembled.header, utc);
                    if (!aligned)
                        logger->warn("SWAP: " + name + " kept in the coarse orientation only");
                }

                if (!aligned)
                {
                    if (steps > 0)
                        img = rotate90(img, steps);
                }

                name = uniqueName(name);

                std::string output_path = output_folder + "/" + name + ".png";
                image::save_img(img, output_path);
                all_images.push_back(output_path);

                if (make_gif)
                {
                    if (!gif.isOpen())
                    {
                        std::string gif_path = output_folder + "/SWAP.gif";
                        if (gif.open(gif_path, (int)img.width(), (int)img.height(), GIFWriter::tintPalette()))
                            logger->info("SWAP: writing animation to " + gif_path);
                        else
                            logger->warn("SWAP: could not start the animation");
                    }
                    if (gif.isOpen())
                        gif.addFrame(img);
                }

                std::string orientation;
                if (aligned)
                    orientation = ", solar north up, sun centred";
                else if (steps > 0)
                    orientation = ", north-up " + std::to_string(steps * 90) + " deg CCW";
                else if (steps == 0)
                    orientation = ", already north-up";
                else
                    orientation = ", roll unknown";

                std::string state = assembled.complete() ? "complete" : "cut after " + std::to_string(assembled.packets) + " of " + std::to_string(assembled.packets_expected) + " packets";
                logger->info("SWAP: saved " + name + ".png (" + state + (assembled.bad_crc ? ", " + std::to_string(assembled.bad_crc) + " with bit errors" : "") +
                             (assembled.header_ok ? "" : ", header has bit errors") + orientation + ")");
            }

            if (gif.frameCount() > 0)
            {
                int frames = gif.frameCount();
                gif.close();
                logger->info("SWAP: animation saved, %d frames", frames);
            }
        }
    }
}
