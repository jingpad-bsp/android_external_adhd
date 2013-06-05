// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <math.h>
#include "crossover.h"
#include "dsp_util.h"
#include "eq.h"

namespace {

/* Adds amplitude * sin(pi*freq*i + offset) to the data array. */
static void add_sine(float *data, size_t len, float freq, float offset,
                     float amplitude)
{
  for (size_t i = 0; i < len; i++)
    data[i] += amplitude * sinf((float)M_PI*freq*i + offset);
}

/* Calculates the magnitude at normalized frequency f. The output is
 * the result of DFT, multiplied by 2/len. */
static float magnitude_at(float *data, size_t len, float f)
{
  double re = 0, im = 0;
  f *= (float)M_PI;
  for (size_t i = 0; i < len; i++) {
    re += data[i] * cos(i * f);
    im += data[i] * sin(i * f);
  }
  return sqrt(re * re + im * im) * (2.0 / len);
}

TEST(InterleaveTest, All) {
  int16_t input[12] = {
    -32768, -32767, -32766, -2, -1, 0, 1, 2, 3, 32765, 32766, 32767
  };

  float answer[12] = {
    -1, -32766/32768.0f, -1/32768.0f, 1/32768.0f, 3/32768.0f, 32766/32768.0f,
    -32767/32768.0f, -2/32768.0f, 0, 2/32768.0f, 32765/32768.0f, 32767/32768.0f
  };

  float output[12];
  float *out_ptr[] = {output, output + 6};

  dsp_util_deinterleave(input, out_ptr, 2, 6);

  for (int i = 0 ; i < 12; i++) {
    EXPECT_EQ(answer[i], output[i]);
  }

  /* dsp_util_interleave() should round to nearest number. */
  for (int i = 0 ; i < 12; i += 2) {
    output[i] += 0.499 / 32768.0f;
    output[i + 1] -= 0.499 / 32768.0f;
  }

  int16_t output2[12];
  dsp_util_interleave(out_ptr, output2, 2, 6);
  for (int i = 0 ; i < 12; i++) {
    EXPECT_EQ(input[i], output2[i]);
  }
}

TEST(EqTest, All) {
  struct eq *eq;
  size_t len = 44100;
  float NQ = len / 2;
  float f_low = 10 / NQ;
  float f_mid = 100 / NQ;
  float f_high = 1000 / NQ;
  float *data = (float *)malloc(sizeof(float) * len);

  dsp_enable_flush_denormal_to_zero();
  /* low pass */
  memset(data, 0, sizeof(float) * len);
  add_sine(data, len, f_low, 0, 1);  // 10Hz sine, magnitude = 1
  EXPECT_FLOAT_EQ(1, magnitude_at(data, len, f_low));
  add_sine(data, len, f_high, 0, 1);  // 1000Hz sine, magnitude = 1
  EXPECT_FLOAT_EQ(1, magnitude_at(data, len, f_low));
  EXPECT_FLOAT_EQ(1, magnitude_at(data, len, f_high));

  eq = eq_new();
  EXPECT_EQ(0, eq_append_biquad(eq, BQ_LOWPASS, f_mid, 0, 0));
  eq_process(eq, data, len);
  EXPECT_NEAR(1, magnitude_at(data, len, f_low), 0.01);
  EXPECT_NEAR(0, magnitude_at(data, len, f_high), 0.01);
  eq_free(eq);

  /* high pass */
  memset(data, 0, sizeof(float) * len);
  add_sine(data, len, f_low, 0, 1);
  add_sine(data, len, f_high, 0, 1);

  eq = eq_new();
  EXPECT_EQ(0, eq_append_biquad(eq, BQ_HIGHPASS, f_mid, 0, 0));
  eq_process(eq, data, len);
  EXPECT_NEAR(0, magnitude_at(data, len, f_low), 0.01);
  EXPECT_NEAR(1, magnitude_at(data, len, f_high), 0.01);
  eq_free(eq);

  /* peaking */
  memset(data, 0, sizeof(float) * len);
  add_sine(data, len, f_low, 0, 1);
  add_sine(data, len, f_high, 0, 1);

  eq = eq_new();
  EXPECT_EQ(0, eq_append_biquad(eq, BQ_PEAKING, f_high, 5, 6)); // Q=5, 6dB gain
  eq_process(eq, data, len);
  EXPECT_NEAR(1, magnitude_at(data, len, f_low), 0.01);
  EXPECT_NEAR(2, magnitude_at(data, len, f_high), 0.01);
  eq_free(eq);

  free(data);

  /* Too many biquads */
  eq = eq_new();
  for (int i = 0; i < MAX_BIQUADS_PER_EQ; i++) {
    EXPECT_EQ(0, eq_append_biquad(eq, BQ_PEAKING, f_high, 5, 6));
  }
  EXPECT_EQ(-1, eq_append_biquad(eq, BQ_PEAKING, f_high, 5, 6));
  eq_free(eq);
}

TEST(CrossoverTest, All) {
  struct crossover xo;
  size_t len = 44100;
  float NQ = len / 2;
  float f0 = 62.5 / NQ;
  float f1 = 250 / NQ;
  float f2 = 1000 / NQ;
  float f3 = 4000 / NQ;
  float f4 = 16000 / NQ;
  float *data = (float *)malloc(sizeof(float) * len);
  float *data1 = (float *)malloc(sizeof(float) * len);
  float *data2 = (float *)malloc(sizeof(float) * len);

  dsp_enable_flush_denormal_to_zero();
  crossover_init(&xo, f1, f3);
  memset(data, 0, sizeof(float) * len);
  add_sine(data, len, f0, 0, 1);
  add_sine(data, len, f2, 0, 1);
  add_sine(data, len, f4, 0, 1);

  crossover_process(&xo, len, data, data1, data2);

  // low band
  EXPECT_NEAR(1, magnitude_at(data, len, f0), 0.01);
  EXPECT_NEAR(0, magnitude_at(data, len, f2), 0.01);
  EXPECT_NEAR(0, magnitude_at(data, len, f4), 0.01);

  // mid band
  EXPECT_NEAR(0, magnitude_at(data1, len, f0), 0.01);
  EXPECT_NEAR(1, magnitude_at(data1, len, f2), 0.01);
  EXPECT_NEAR(0, magnitude_at(data1, len, f4), 0.01);

  // high band
  EXPECT_NEAR(0, magnitude_at(data2, len, f0), 0.01);
  EXPECT_NEAR(0, magnitude_at(data2, len, f2), 0.01);
  EXPECT_NEAR(1, magnitude_at(data2, len, f4), 0.01);

  free(data);
  free(data1);
  free(data2);
}

}  //  namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}