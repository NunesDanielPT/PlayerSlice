#pragma once
// Descodificador LTC (SMPTE 12M, código bifase) — C++ puro, sem dependências.
// Recebe amostras de áudio mono (float) e devolve frames de timecode completos.
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace ps {

struct LtcFrame {
    int h = 0, m = 0, s = 0, f = 0;
    bool drop = false;
    double sampleEnd = 0;   // posição (em amostras) em que o frame terminou de chegar
    float fpsEst = 0;       // fps estimados pela velocidade dos bits
};

class LtcDecoder {
public:
    explicit LtcDecoder(double sampleRate = 48000) { reset(sampleRate); }

    void reset(double sampleRate) {
        m_sr = sampleRate;
        m_period = sampleRate / (25.0 * 80.0);   // duração de 1 bit (palpite inicial: 25 fps)
        m_level = 0; m_sign = 0; m_lastCross = -1; m_halfPending = false;
        m_bits[0] = m_bits[1] = 0; m_bitCount = 0; m_pos = 0; m_peak = 0;
    }

    float peak() const { return m_peak; }

    // processa n amostras; chama onFrame(LtcFrame) por cada frame descodificado
    template <class F>
    void process(const float* x, int n, F&& onFrame) {
        for (int i = 0; i < n; ++i, ++m_pos) {
            float v = x[i];
            float a = std::fabs(v);
            m_level = std::max(a, m_level * 0.9995f);          // envolvente
            m_peak = std::max(a, m_peak * 0.9999f);
            float th = std::max(0.004f, m_level * 0.15f);       // histerese
            int sg = m_sign;
            if (v > th) sg = 1; else if (v < -th) sg = -1;
            if (sg != 0 && sg != m_sign) {
                if (m_sign != 0 && m_lastCross >= 0) edge((double)(m_pos - m_lastCross), onFrame);
                m_sign = sg;
                m_lastCross = m_pos;
            }
        }
    }

private:
    template <class F>
    void edge(double iv, F&& onFrame) {
        // intervalos impossíveis (ruído ou silêncio) → recomeçar
        const double minBit = m_sr / (35.0 * 80.0) * 0.6, maxBit = m_sr / (20.0 * 80.0) * 1.6;
        if (iv < minBit * 0.5 || iv > maxBit) { m_halfPending = false; return; }
        if (iv > m_period * 0.75) {
            // meio-período longo = bit 0
            m_period = m_period * 0.9 + iv * 0.1;
            m_halfPending = false;
            pushBit(0, onFrame);
        } else {
            m_period = m_period * 0.9 + iv * 2.0 * 0.1;
            if (m_halfPending) { m_halfPending = false; pushBit(1, onFrame); }
            else m_halfPending = true;
        }
        m_period = std::clamp(m_period, minBit, maxBit);
    }

    template <class F>
    void pushBit(int b, F&& onFrame) {
        // janela de 80 bits: m_bits[0] = bits mais antigos (64), m_bits[1] = últimos 16
        m_bits[0] = (m_bits[0] >> 1) | ((uint64_t)(m_bits[1] & 1) << 63);
        m_bits[1] = (uint16_t)((m_bits[1] >> 1) | (b << 15));
        if (m_bitCount < 80) m_bitCount++;
        // palavra de sincronismo (bits 64..79 em ordem de chegada): 0011 1111 1111 1101
        // no registo (bit mais antigo no LSB) fica 0xBFFC
        if (m_bitCount >= 80 && m_bits[1] == 0xBFFC) {
            uint64_t d = m_bits[0];
            auto bits = [&](int start, int n) { return (int)((d >> start) & ((1u << n) - 1)); };
            LtcFrame fr;
            fr.f = bits(0, 4) + bits(8, 2) * 10;
            fr.drop = bits(10, 1) != 0;
            fr.s = bits(16, 4) + bits(24, 3) * 10;
            fr.m = bits(32, 4) + bits(40, 3) * 10;
            fr.h = bits(48, 4) + bits(56, 2) * 10;
            fr.sampleEnd = (double)m_pos;
            fr.fpsEst = (float)(m_sr / (m_period * 80.0));
            if (fr.f < 30 && fr.s < 60 && fr.m < 60 && fr.h < 24) onFrame(fr);
            m_bitCount = 0;
        }
    }

    double   m_sr = 48000, m_period = 24;
    float    m_level = 0, m_peak = 0;
    int      m_sign = 0;
    int64_t  m_lastCross = -1, m_pos = 0;
    bool     m_halfPending = false;
    uint64_t m_bits[2] = {0, 0};
    int      m_bitCount = 0;
};

} // namespace ps
