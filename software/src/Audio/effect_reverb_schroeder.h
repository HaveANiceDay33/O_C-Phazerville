#pragma once
#include <AudioStream.h>
#include <arm_math.h>   // Teensy core
#include <imxrt.h>

// ---- Schroeder/Moorer Reverb (wet only) ----
// Parallel comb filters -> series allpass filters
// Tunable decay time (RT60) in seconds

class AudioEffectReverbSchroeder : public AudioStream {
public:
  AudioEffectReverbSchroeder() : AudioStream(1, inputQueueArray) {
    setDecayTime(2.5f);   // default ~2.5s
    setDamping(0.5f);     // 0 = bright, 1 = dark
    reset();
  }

  // Set decay time in seconds (approximate RT60)
  void setDecayTime(float seconds) {
    if (seconds < 0.1f) seconds = 0.1f;
    if (seconds > 20.0f) seconds = 20.0f;

    float avgDelay = 0.0f;
    for (int i = 0; i < COMB_COUNT; i++) avgDelay += combLen[i];
    avgDelay /= COMB_COUNT;

    float delaySec = avgDelay / 44100.0f;
    float g = expf((-3.0f * delaySec) / seconds); // feedback coefficient

    __disable_irq();
    decayFeedback = g;
    __enable_irq();
  }

  // 0..1 where 1 = strong high-frequency damping
  void setDamping(float d) {
    if (d < 0.0f) d = 0.0f;
    if (d > 0.99f) d = 0.99f;
    __disable_irq();
    damp1 = d;
    damp2 = 1.0f - d;
    __enable_irq();
  }

  void reset() {
    __disable_irq();
    for (int i = 0; i < COMB_COUNT; ++i) {
      combIdx[i] = 0;
      combStore[i] = 0.0f;
      memset(combBuf[i], 0, sizeof(combBuf[i]));
    }
    for (int i = 0; i < ALLPASS_COUNT; ++i) {
      apIdx[i] = 0;
      memset(apBuf[i], 0, sizeof(apBuf[i]));
    }
    __enable_irq();
  }

  virtual void update(void) override {
    audio_block_t *in = receiveReadOnly(0);
    if (!in) return;

    audio_block_t *out = allocate();
    if (!out) {
      release(in);
      return;
    }

    float localDamp1 = damp1;
    float localDamp2 = damp2;
    float feedback   = decayFeedback;

    for (int n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {
      float x = in->data[n] * (1.0f / 32768.0f);

      // ---- Parallel combs ----
      float combSum = 0.0f;
      for (int i = 0; i < COMB_COUNT; ++i) {
        float y = combBuf[i][combIdx[i]];
        combStore[i] = (combStore[i] * localDamp2) + (y * localDamp1);
        float write = x + feedback * combStore[i];
        combBuf[i][combIdx[i]] = write;
        combIdx[i]++; if (combIdx[i] >= combLen[i]) combIdx[i] = 0;
        combSum += y;
      }

      // ---- Series allpasses ----
      float apOut = combSum;
      for (int i = 0; i < ALLPASS_COUNT; ++i) {
        float bufOut = apBuf[i][apIdx[i]];
        float z = apOut + (-AP_GAIN * bufOut);
        apBuf[i][apIdx[i]] = z;
        apIdx[i]++; if (apIdx[i] >= apLen[i]) apIdx[i] = 0;
        apOut = bufOut + z * AP_GAIN;
      }

      float y = apOut; // wet only

      int32_t s = (int32_t)(y * 32767.0f);
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      out->data[n] = (int16_t)s;
    }

    transmit(out);
    release(out);
    release(in);
  }

private:
  audio_block_t *inputQueueArray[1];

  // Tunables
  volatile float decayFeedback = 0.85f; // per-comb feedback
  volatile float damp1 = 0.5f, damp2 = 0.5f;

  // Delay line layout
  static constexpr int sr = 44100;
  static constexpr int COMB_COUNT = 4;
  static constexpr int combLenConst[COMB_COUNT] = {
    int(0.0297f * sr + 0.5f),
    int(0.0371f * sr + 0.5f),
    int(0.0411f * sr + 0.5f),
    int(0.0437f * sr + 0.5f)
  };
  static constexpr int ALLPASS_COUNT = 2;
  static constexpr int apLenConst[ALLPASS_COUNT] = {
    int(0.0050f * sr + 0.5f),
    int(0.0017f * sr + 0.5f)
  };
  static constexpr float AP_GAIN = 0.5f;

  int combLen[COMB_COUNT] = { combLenConst[0], combLenConst[1], combLenConst[2], combLenConst[3] };
  int apLen[ALLPASS_COUNT] = { apLenConst[0], apLenConst[1] };

  static constexpr int COMB_MAX = combLenConst[3];
  static constexpr int AP_MAX   = apLenConst[0];

  float combBuf[COMB_COUNT][COMB_MAX] __attribute__((aligned(4)));
  float apBuf[ALLPASS_COUNT][AP_MAX]  __attribute__((aligned(4)));

  volatile uint16_t combIdx[COMB_COUNT] = {0,0,0,0};
  volatile uint16_t apIdx[ALLPASS_COUNT] = {0,0};
  float combStore[COMB_COUNT] = {0,0,0,0};
};
