#pragma once
#include <cmath>

// Homografias 3x3 (linha-maior) para o warp de 4 pontos (corner pin).
// Cantos sempre pela ordem: 0 = topo-esq, 1 = topo-dir, 2 = baixo-dir, 3 = baixo-esq.
namespace ps {

struct Mat3 { double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; };

// quadrado unitário (0,0)(1,0)(1,1)(0,1) → quadrilátero q (Heckbert)
inline Mat3 squareToQuad(const float q[4][2]) {
    double x0 = q[0][0], y0 = q[0][1], x1 = q[1][0], y1 = q[1][1];
    double x2 = q[2][0], y2 = q[2][1], x3 = q[3][0], y3 = q[3][1];
    double dx1 = x1 - x2, dx2 = x3 - x2, dx3 = x0 - x1 + x2 - x3;
    double dy1 = y1 - y2, dy2 = y3 - y2, dy3 = y0 - y1 + y2 - y3;
    Mat3 r;
    double a, b, c, d, e, f, g, h;
    if (std::fabs(dx3) < 1e-9 && std::fabs(dy3) < 1e-9) {
        a = x1 - x0; b = x3 - x0; c = x0;
        d = y1 - y0; e = y3 - y0; f = y0;
        g = 0; h = 0;
    } else {
        double det = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(det) < 1e-12) det = 1e-12;
        g = (dx3 * dy2 - dx2 * dy3) / det;
        h = (dx1 * dy3 - dx3 * dy1) / det;
        a = x1 - x0 + g * x1; b = x3 - x0 + h * x3; c = x0;
        d = y1 - y0 + g * y1; e = y3 - y0 + h * y3; f = y0;
    }
    double v[9] = {a, b, c, d, e, f, g, h, 1};
    for (int i = 0; i < 9; ++i) r.m[i] = v[i];
    return r;
}

inline Mat3 inverse(const Mat3& A) {
    const double* m = A.m;
    double c0 = m[4] * m[8] - m[5] * m[7];
    double c1 = m[5] * m[6] - m[3] * m[8];
    double c2 = m[3] * m[7] - m[4] * m[6];
    double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (std::fabs(det) < 1e-15) det = 1e-15;
    double id = 1.0 / det;
    Mat3 r;
    r.m[0] = c0 * id; r.m[1] = (m[2] * m[7] - m[1] * m[8]) * id; r.m[2] = (m[1] * m[5] - m[2] * m[4]) * id;
    r.m[3] = c1 * id; r.m[4] = (m[0] * m[8] - m[2] * m[6]) * id; r.m[5] = (m[2] * m[3] - m[0] * m[5]) * id;
    r.m[6] = c2 * id; r.m[7] = (m[1] * m[6] - m[0] * m[7]) * id; r.m[8] = (m[0] * m[4] - m[1] * m[3]) * id;
    return r;
}

inline void apply(const Mat3& H, double x, double y, double& ox, double& oy) {
    const double* m = H.m;
    double w = m[6] * x + m[7] * y + m[8];
    if (std::fabs(w) < 1e-15) w = 1e-15;
    ox = (m[0] * x + m[1] * y + m[2]) / w;
    oy = (m[3] * x + m[4] * y + m[5]) / w;
}

// quadrilátero convexo e não torcido? (o warp só é válido nesse caso)
inline bool quadIsConvex(const float q[4][2]) {
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const float* a = q[i]; const float* b = q[(i + 1) % 4]; const float* c = q[(i + 2) % 4];
        double cr = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
        int s = cr > 1e-9 ? 1 : cr < -1e-9 ? -1 : 0;
        if (!s) continue;
        if (!sign) sign = s; else if (s != sign) return false;
    }
    return true;
}

} // namespace ps
