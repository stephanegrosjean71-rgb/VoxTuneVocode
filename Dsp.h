#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vx
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float polyBlep (float t, float dt)
{
    if (t < dt)       { t /= dt;            return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}

//==============================================================================
// Gammes pour l'autotune
constexpr int kNumScales = 9;

inline const char* scaleName (int i)
{
    static const char* n[kNumScales] = { "Chromatic", "Major", "Natural Minor", "Harmonic Minor",
                                         "Pentatonic Major", "Pentatonic Minor", "Blues", "Dorian", "Mixolydian" };
    return n[juce::jlimit (0, kNumScales - 1, i)];
}

inline uint16_t scaleMask (int i)
{
    static const std::vector<std::vector<int>> degrees = {
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 },
        { 0, 2, 3, 5, 7, 8, 11 }, { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 }, { 0, 3, 5, 6, 7, 10 },
        { 0, 2, 3, 5, 7, 9, 10 }, { 0, 2, 4, 5, 7, 9, 10 } };
    uint16_t m = 0;
    for (int d : degrees[(size_t) juce::jlimit (0, kNumScales - 1, i)])
        m = (uint16_t) (m | (1u << d));
    return m;
}

//==============================================================================
// Filtre SVF TPT, sortie passe-bande à gain unité au centre
struct Svf
{
    float ic1 = 0.f, ic2 = 0.f, a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 1.f;

    void set (float fc, float fs, float q)
    {
        fc = juce::jlimit (20.f, 0.45f * fs, fc);
        const float g = std::tan (kPi * fc / fs);
        k = 1.f / q;
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void copyCoeffs (const Svf& o) { a1 = o.a1; a2 = o.a2; a3 = o.a3; k = o.k; }

    float bp (float v0)
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        return k * v1;
    }
};

//==============================================================================
// AUTOTUNE : détection de hauteur YIN + correction par PSOLA (les formants sont conservés)
class AutoTuner
{
public:
    void prepare (double sampleRate)
    {
        fs = (float) sampleRate;
        latency = (int) (0.04f * fs);
        const float fs2 = fs * 0.5f;
        tauMin = std::max (2, (int) (fs2 / 1000.f));
        tauMax = (int) std::ceil (fs2 / 100.f);

        inBuf.fill (0.f);
        outBuf.fill (0.f);
        decBuf.fill (0.f);
        lin.assign ((size_t) (kWin + tauMax + 2), 0.f);
        diff.assign ((size_t) (tauMax + 2), 0.f);
        cmnd.assign ((size_t) (tauMax + 2), 1.f);

        now = 0; decW = 0; decPhase = false; decPrev = 0.f; hopCounter = 0;
        voiced = false; unvoicedCount = 0; periodIn = 0.f; f0 = 0.f;
        f0Hist[0] = f0Hist[1] = f0Hist[2] = 0.f;
        shift = 0.f; fOut = 0.f; havePrev = false; prevTarget = 0;
        markPos = (double) (now - latency); cPrev = now - latency; wasActive = false;
    }

    int getLatency() const { return latency; }
    bool isVoiced() const { return voiced && fOut > 0.f; }
    float correctedFreq() const { return fOut; }

    void setParams (bool on, int keyIdx, int scaleIdx, float speed, float amt, float refHz)
    {
        enabled = on;
        key = keyIdx;
        mask = scaleMask (scaleIdx);
        speedMs = speed;
        amount = amt;
        refA = refHz;
    }

    float process (float x)
    {
        inBuf[(size_t) (now & kMask)] = x;

        // décimation par 2 pour la détection
        if (decPhase)
        {
            decBuf[(size_t) (decW & kDecMask)] = 0.5f * (decPrev + x);
            ++decW;
        }
        decPrev = x;
        decPhase = ! decPhase;

        if (++hopCounter >= kHop)
        {
            hopCounter = 0;
            detect();
        }

        const int64_t readT = now - latency;
        float y;

        if (! enabled)
        {
            y = inBuf[(size_t) (readT & kMask)];
            markPos = (double) readT;
            cPrev = readT;
            wasActive = false;
        }
        else
        {
            if (! wasActive)
            {
                markPos = (double) readT;
                cPrev = readT;
                wasActive = true;
            }

            const float unvoicedWin = (float) (int) (0.005f * fs);

            for (;;)
            {
                float spacing, wh, tp = 0.f;
                if (voiced && fOut > 0.f && periodIn > 0.f)
                {
                    tp = periodIn;
                    spacing = std::min (std::max (fs / fOut, tp / 1.42f), tp / 0.7f);
                    wh = std::max (tp, spacing);
                }
                else
                {
                    spacing = wh = unvoicedWin;
                }

                if (markPos > (double) readT + (double) wh)
                    break;

                placeGrain (readT, spacing, wh, tp);
                markPos += (double) spacing;
            }

            const size_t idx = (size_t) (readT & kMask);
            y = outBuf[idx];
            outBuf[idx] = 0.f;
        }

        ++now;
        return y;
    }

private:
    static constexpr int kRing = 16384, kMask = kRing - 1;
    static constexpr int kDecRing = 2048, kDecMask = kDecRing - 1;
    static constexpr int kWin = 512;
    static constexpr int kHop = 256;

    void placeGrain (int64_t readT, float spacing, float wh, float tp)
    {
        int64_t c;
        if (tp > 0.f)
        {
            const double steps = std::max (0.0, std::round ((markPos - (double) cPrev) / (double) tp));
            const int64_t cand = cPrev + (int64_t) std::llround (steps * (double) tp);
            const int q = std::max (1, (int) (tp * 0.25f));
            int64_t best = cand;
            float mv = -1e9f;
            for (int j = -q; j <= q; ++j)
            {
                const float v = inBuf[(size_t) ((cand + j) & kMask)];
                if (v > mv) { mv = v; best = cand + j; }
            }
            c = best;
        }
        else
        {
            c = (int64_t) std::llround (markPos);
        }

        cPrev = c;

        const int wi = (int) wh;
        if (wi < 4 || c + wi >= now)
            return; // pas encore assez d'entrée disponible

        const float scale = (tp > 0.f) ? spacing / wh : 1.f;
        const int64_t o = (int64_t) std::llround (markPos);
        const float kk = kPi / (float) wi;

        for (int j = -wi; j < wi; ++j)
        {
            const int64_t oi = o + j;
            if (oi < readT)
                continue;
            const float w = 0.5f * (1.f + std::cos (kk * (float) j));
            outBuf[(size_t) (oi & kMask)] += inBuf[(size_t) ((c + j) & kMask)] * w * scale;
        }
    }

    void detect()
    {
        const int total = kWin + tauMax;
        for (int i = 0; i < total; ++i)
            lin[(size_t) i] = decBuf[(size_t) ((decW - total + i) & kDecMask)];

        float e = 0.f;
        for (int i = 0; i < kWin; ++i)
        {
            const float v = lin[(size_t) (tauMax + i)];
            e += v * v;
        }
        const float rms = std::sqrt (e / (float) kWin);

        bool found = false;
        float periodDec = 0.f;

        if (rms > 0.004f)
        {
            diff[0] = 0.f;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                float s = 0.f;
                const float* a = &lin[(size_t) tauMax];
                const float* b = &lin[(size_t) (tauMax - tau)];
                for (int j = 0; j < kWin; ++j)
                {
                    const float d = a[j] - b[j];
                    s += d * d;
                }
                diff[(size_t) tau] = s;
            }

            cmnd[0] = 1.f;
            float run = 0.f;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                run += diff[(size_t) tau];
                cmnd[(size_t) tau] = run > 1e-9f ? diff[(size_t) tau] * (float) tau / run : 1.f;
            }

            int best = -1;
            for (int tau = tauMin; tau < tauMax; ++tau)
            {
                if (cmnd[(size_t) tau] < 0.15f)
                {
                    while (tau + 1 < tauMax && cmnd[(size_t) (tau + 1)] < cmnd[(size_t) tau])
                        ++tau;
                    best = tau;
                    break;
                }
            }

            if (best < 0)
            {
                float mn = 1e9f;
                int bi = -1;
                for (int tau = tauMin; tau < tauMax; ++tau)
                    if (cmnd[(size_t) tau] < mn) { mn = cmnd[(size_t) tau]; bi = tau; }
                if (mn < 0.3f)
                    best = bi;
            }

            if (best > 0)
            {
                const float a = cmnd[(size_t) (best - 1)], b = cmnd[(size_t) best], c = cmnd[(size_t) (best + 1)];
                const float den = a - 2.f * b + c;
                float sh = (std::fabs (den) > 1e-9f) ? 0.5f * (a - c) / den : 0.f;
                sh = juce::jlimit (-1.f, 1.f, sh);
                periodDec = (float) best + sh;
                found = true;
            }
        }

        if (found)
        {
            unvoicedCount = 0;
            const float f = fs / (2.f * periodDec);
            f0Hist[2] = f0Hist[1];
            f0Hist[1] = f0Hist[0];
            f0Hist[0] = f;

            const float a = f0Hist[0], b = f0Hist[1], c = f0Hist[2];
            f0 = (b <= 0.f || c <= 0.f) ? a : std::max (std::min (a, b), std::min (std::max (a, b), c));
            periodIn = fs / f0;

            const bool onset = ! voiced;
            voiced = true;
            updateTarget (onset);
        }
        else if (++unvoicedCount >= 2)
        {
            voiced = false;
            havePrev = false;
            f0Hist[0] = f0Hist[1] = f0Hist[2] = 0.f;
        }
    }

    bool allowed (int n) const
    {
        const int pc = (((n - key) % 12) + 12) % 12;
        return ((mask >> pc) & 1u) != 0;
    }

    void updateTarget (bool onset)
    {
        const float midi = 69.f + 12.f * std::log2 (f0 / refA);
        const int rounded = (int) std::floor (midi + 0.5f);

        int bestN = rounded;
        float bestD = 1e9f;
        for (int k = -7; k <= 7; ++k)
        {
            const int n = rounded + k;
            if (! allowed (n))
                continue;
            const float d = std::fabs ((float) n - midi);
            if (d < bestD) { bestD = d; bestN = n; }
        }

        // petite hystérésis pour éviter les sauts de note à la frontière
        if (havePrev && allowed (prevTarget) && std::fabs (midi - (float) prevTarget) < 0.62f)
            bestN = prevTarget;
        prevTarget = bestN;
        havePrev = true;

        const float eff = enabled ? amount : 0.f;
        const float desired = ((float) bestN - midi) * eff;
        const float hopSec = (float) kHop / fs;
        const float alpha = (speedMs <= 0.5f || onset) ? 1.f : 1.f - std::exp (-hopSec / (speedMs * 0.001f));
        shift += (desired - shift) * alpha;
        fOut = f0 * std::pow (2.f, shift / 12.f);
    }

    float fs = 44100.f;
    int latency = 1764, tauMin = 22, tauMax = 221;

    std::array<float, kRing> inBuf {}, outBuf {};
    std::array<float, kDecRing> decBuf {};
    std::vector<float> lin, diff, cmnd;

    int64_t now = 0, decW = 0, cPrev = 0;
    double markPos = 0.0;
    bool decPhase = false, wasActive = false;
    float decPrev = 0.f;
    int hopCounter = 0, unvoicedCount = 0;

    bool voiced = false, havePrev = false, enabled = true;
    int prevTarget = 0, key = 0;
    uint16_t mask = 0x0FFF;
    float periodIn = 0.f, f0 = 0.f, f0Hist[3] {}, shift = 0.f, fOut = 0.f;
    float speedMs = 20.f, amount = 1.f, refA = 440.f;
};

//==============================================================================
// PORTEUSE : synthé (saw/carré anti-aliasé + quinte/octave + sub + souffle)
class Carrier
{
public:
    static constexpr int numVoices = 6;

    struct Params
    {
        float wave = 0.3f, noise = 0.25f, sub = 0.25f, harmony = 0.3f;
        float widthCents = 12.f, glideMs = 25.f, release = 0.25f;
    };

    void prepare (double sr)
    {
        fs = (float) sr;
        for (auto& v : voices) v = Voice {};
        autoV = Voice {};
        setParams (prm);
    }

    void setParams (const Params& p)
    {
        prm = p;
        rUp = std::pow (2.f, p.widthCents / 2400.f);
        rDown = 1.f / rUp;
        glideCoef = 1.f - std::exp (-1.f / (std::max (1.f, p.glideMs) * 0.001f * fs));
        atkCoef = 1.f - std::exp (-1.f / (0.008f * fs));
        relCoef = 1.f - std::exp (-1.f / (std::max (0.02f, p.release) * fs * 0.25f));
    }

    void noteOn (int note, float)
    {
        Voice* t = nullptr;
        for (auto& v : voices) if (v.active && v.note == note) { t = &v; break; }
        if (t == nullptr) for (auto& v : voices) if (! v.active) { t = &v; break; }
        if (t == nullptr)
        {
            t = &voices[0];
            for (auto& v : voices) if (v.env < t->env) t = &v;
        }
        if (! t->active)
        {
            *t = Voice {};
            t->phL = rng.nextFloat(); t->phR = rng.nextFloat();
        }
        t->active = true; t->gate = true; t->note = note;
    }

    void noteOff (int note)
    {
        for (auto& v : voices) if (v.active && v.note == note) v.gate = false;
    }

    void allOff()
    {
        for (auto& v : voices) v.gate = false;
    }

    void process (bool autoMode, float autoFreq, bool autoGate, float& L, float& R)
    {
        float sL = 0.f, sR = 0.f;

        if (autoMode)
        {
            renderVoice (autoV, autoGate, autoFreq, sL, sR);
        }
        else
        {
            for (auto& v : voices)
            {
                if (! v.active) continue;
                renderVoice (v, v.gate, 440.f * std::pow (2.f, (float) (v.note - 69) / 12.f), sL, sR);
                if (! v.gate && v.env < 1e-4f) v.active = false;
            }
        }

        L = sL + prm.noise * 0.25f * (rng.nextFloat() * 2.f - 1.f);
        R = sR + prm.noise * 0.25f * (rng.nextFloat() * 2.f - 1.f);
    }

private:
    struct Voice
    {
        bool active = false, gate = false;
        int note = 60;
        float freq = 0.f, env = 0.f;
        float phL = 0.f, phR = 0.f, ph5L = 0.f, ph5R = 0.f, ph8L = 0.f, ph8R = 0.f, phSub = 0.f;
    };

    float osc (float& ph, float inc) const
    {
        const float s1 = 2.f * ph - 1.f - polyBlep (ph, inc);
        float p2 = ph + 0.5f;
        if (p2 >= 1.f) p2 -= 1.f;
        const float s2 = 2.f * p2 - 1.f - polyBlep (p2, inc);
        ph += inc;
        if (ph >= 1.f) ph -= 1.f;
        return (1.f - prm.wave) * s1 + prm.wave * (s1 - s2) * 0.8f;
    }

    void renderVoice (Voice& v, bool gate, float targetFreq, float& L, float& R)
    {
        const bool ok = gate && targetFreq > 20.f;
        if (targetFreq > 20.f)
        {
            if (v.freq <= 0.f) v.freq = targetFreq;
            v.freq += (targetFreq - v.freq) * glideCoef;
        }

        if (ok) v.env += (1.f - v.env) * atkCoef;
        else    v.env -= v.env * relCoef;

        if (v.env < 1e-5f || v.freq <= 0.f)
            return;

        const float incL = std::min (0.45f, v.freq * rDown / fs);
        const float incR = std::min (0.45f, v.freq * rUp / fs);

        float l = osc (v.phL, incL);
        float r = osc (v.phR, incR);
        l += prm.harmony * (0.6f * osc (v.ph5L, std::min (0.45f, incL * 1.4983071f)) + 0.5f * osc (v.ph8L, std::min (0.45f, incL * 2.f)));
        r += prm.harmony * (0.6f * osc (v.ph5R, std::min (0.45f, incR * 1.4983071f)) + 0.5f * osc (v.ph8R, std::min (0.45f, incR * 2.f)));

        const float sub = std::sin (kTwoPi * v.phSub) * prm.sub;
        v.phSub += v.freq * 0.5f / fs;
        if (v.phSub >= 1.f) v.phSub -= 1.f;

        L += (l + sub) * v.env * 0.25f;
        R += (r + sub) * v.env * 0.25f;
    }

    float fs = 44100.f, rUp = 1.f, rDown = 1.f, glideCoef = 0.01f, atkCoef = 0.01f, relCoef = 0.001f;
    Params prm;
    juce::Random rng { 0x77aa1234 };
    std::array<Voice, numVoices> voices;
    Voice autoV;
};

//==============================================================================
// VOCODEUR : 16 bandes, filtres 4e ordre, décalage de formants sur la porteuse
class Vocoder
{
public:
    static constexpr int N = 16;
    static constexpr float kMakeup = 8.f;

    void prepare (double sr)
    {
        fs = (float) sr;
        for (auto& b : bands) b = Band {};
        setParams (4.f, 0.f, 4.f, 40.f, 0.5f);
    }

    void setParams (float q, float formantSemis, float attackMs, float releaseMs, float sibilance)
    {
        const float ratio = std::pow (2.f, formantSemis / 12.f);
        for (int i = 0; i < N; ++i)
        {
            auto& b = bands[(size_t) i];
            const float f = 100.f * std::pow (90.f, (float) i / (float) (N - 1));

            b.modA.set (f, fs, q);
            b.modB.copyCoeffs (b.modA);

            b.carLA.set (f * ratio, fs, q);
            b.carLB.copyCoeffs (b.carLA);
            b.carRA.copyCoeffs (b.carLA);
            b.carRB.copyCoeffs (b.carLA);

            const float tilt = 0.7f + 1.6f * (float) i / (float) (N - 1);
            const float sib = (i >= 12) ? 1.f + sibilance * 0.5f * (float) (i - 11) : 1.f;
            b.gain = tilt * sib;
        }
        aCoef = 1.f - std::exp (-1.f / (std::max (0.1f, attackMs) * 0.001f * fs));
        rCoef = 1.f - std::exp (-1.f / (std::max (1.f, releaseMs) * 0.001f * fs));
    }

    void process (float m, float cL, float cR, float& oL, float& oR)
    {
        float sL = 0.f, sR = 0.f;
        for (auto& b : bands)
        {
            const float mb = b.modB.bp (b.modA.bp (m));
            const float r = std::fabs (mb);
            b.env += (r > b.env ? aCoef : rCoef) * (r - b.env);

            const float g = b.env * b.gain;
            sL += b.carLB.bp (b.carLA.bp (cL)) * g;
            sR += b.carRB.bp (b.carRA.bp (cR)) * g;
        }
        oL = sL * kMakeup;
        oR = sR * kMakeup;
    }

private:
    struct Band
    {
        Svf modA, modB, carLA, carLB, carRA, carRB;
        float env = 0.f, gain = 1.f;
    };

    float fs = 44100.f, aCoef = 0.01f, rCoef = 0.001f;
    std::array<Band, N> bands;
};

} // namespace vx
