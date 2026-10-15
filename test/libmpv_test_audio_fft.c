/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "libmpv_common.h"
#include <math.h>
#include <stdint.h>
#include <time.h>

// Tone files the tests play. They are written by write_tone_wav() below, so
// the test does not depend on a sample file in the repository.
// The frequencies are not multiples of 10 Hz on purpose: with a tone that
// repeats exactly every 1/audio-fft-rate seconds every frame would carry the
// same bytes, and mpv does not send a second event for a value that did not
// change.
static const char *tone_440_path = "audio_fft_tone_437.wav";
static const char *tone_5000_path = "audio_fft_tone_5003.wav";
static const double tone_440_hz = 437.0;
static const double tone_5000_hz = 5003.0;
static const char *noise_path = "audio_fft_noise.wav";
static const char *mark_path = "audio_fft_mark.wav";

static void wait_for_playback(void) {
  bool finished = false;
  while (!finished) {
    mpv_event *event = wrap_wait_event();
    if (event->event_id == MPV_EVENT_END_FILE)
      finished = true;
  }
}

static void test_audio_fft_get_property(void) {
  // Observe the audio-fft property
  mpv_observe_property(ctx, 0, "audio-fft", MPV_FORMAT_BYTE_ARRAY);

  int fft_size = 256;

  // Load the generated tone file as audio source
  reload_file(tone_440_path);

  bool got_fft_data = false;
  bool finished = false;
  int fft_count = 0;

  while (!finished) {
    mpv_event *event = wrap_wait_event();
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
      mpv_event_property *prop = event->data;
      if (strcmp(prop->name, "audio-fft") == 0) {
        if (prop->format == MPV_FORMAT_BYTE_ARRAY) {
          mpv_byte_array *ba = prop->data;
          if (ba && ba->data && ba->size == (size_t)fft_size) {
            uint8_t *bytes = (uint8_t *)ba->data;
            got_fft_data = true;
            fft_count++;

            // Check that we have some non-zero values
            bool has_nonzero = false;
            for (int i = 0; i < (int)ba->size; i++) {
              if (bytes[i] > 0) {
                has_nonzero = true;
                break;
              }
            }
            if (!has_nonzero && fft_count > 5) {
              fail("audio-fft data is all zeros after %d frames\n", fft_count);
            }

            printf("FFT frame %d: size=%zu, first 10 values: ", fft_count,
                   ba->size);
            for (int i = 0; i < 10 && i < (int)ba->size; i++)
              printf("%d ", bytes[i]);
            printf("\n");

            if (fft_count >= 10)
              finished = true;
          }
        }
      }
      break;
    }
    case MPV_EVENT_END_FILE:
      finished = true;
      break;
    }
  }

  mpv_unobserve_property(ctx, 0);

  if (!got_fft_data)
    fail("No audio-fft property change events received!\n");

  printf("Received %d FFT frames\n", fft_count);
  printf("audio-fft get_property test passed!\n");
}

static void test_audio_fft_read_property(void) {
  int64_t fft_size = 128;

  // Observe the property to trigger FFT computation
  mpv_set_property(ctx, "audio-fft-size", MPV_FORMAT_INT64, &fft_size);
  mpv_observe_property(ctx, 0, "audio-fft", MPV_FORMAT_BYTE_ARRAY);
  reload_file(tone_440_path);

  // Wait for a few FFT frames to be generated
  bool finished = false;
  bool got_observe_data = false;
  while (!finished) {
    mpv_event *event = wrap_wait_event();
    if (event->event_id == MPV_EVENT_END_FILE)
      finished = true;
    if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
      mpv_event_property *prop = event->data;
      if (strcmp(prop->name, "audio-fft") == 0 &&
          prop->format == MPV_FORMAT_BYTE_ARRAY) {
        mpv_byte_array *ba = prop->data;
        if (ba && ba->data && ba->size > 0) {
          got_observe_data = true;
          finished = true;
        }
      }
    }
  }

  mpv_unobserve_property(ctx, 0);

  if (!got_observe_data)
    fail("No FFT data via observe!\n");

  // Now test mpv_get_property with MPV_FORMAT_BYTE_ARRAY
  mpv_byte_array ba;
  int r = mpv_get_property(ctx, "audio-fft", MPV_FORMAT_BYTE_ARRAY, &ba);
  if (r < 0)
    fail("mpv_get_property audio-fft failed: %s\n", mpv_error_string(r));
  if (!ba.data || ba.size != (size_t)fft_size)
    fail("mpv_get_property audio-fft: invalid data (size=%zu, expected=%" PRId64
         ")\n",
         ba.size, fft_size);

  printf("Got FFT data via mpv_get_property: size=%zu\n", ba.size);

  printf("audio-fft read_property test passed!\n");
}

// Open a mono 16 bit WAV file and write its header. The test writes its own
// input, so it does not depend on a sample file in the repository.
static FILE *open_wav(const char *path, int samples) {
  const int rate = 44100;
  int data_size = samples * 2;
  unsigned char header[44] = {0};
  FILE *f = fopen(path, "wb");
  if (!f)
    fail("cannot write %s\n", path);
  memcpy(header, "RIFF", 4);
  unsigned int file_size = 36 + data_size;
  header[4] = file_size & 0xff;
  header[5] = (file_size >> 8) & 0xff;
  header[6] = (file_size >> 16) & 0xff;
  header[7] = (file_size >> 24) & 0xff;
  memcpy(header + 8, "WAVEfmt ", 8);
  header[16] = 16; // format chunk size
  header[20] = 1;  // PCM
  header[22] = 1;  // one channel
  header[24] = rate & 0xff;
  header[25] = (rate >> 8) & 0xff;
  header[26] = (rate >> 16) & 0xff;
  header[27] = (rate >> 24) & 0xff;
  unsigned int byte_rate = rate * 2;
  header[28] = byte_rate & 0xff;
  header[29] = (byte_rate >> 8) & 0xff;
  header[30] = (byte_rate >> 16) & 0xff;
  header[31] = (byte_rate >> 24) & 0xff;
  header[32] = 2;   // block align
  header[34] = 16;  // bits per sample
  memcpy(header + 36, "data", 4);
  header[40] = data_size & 0xff;
  header[41] = (data_size >> 8) & 0xff;
  header[42] = (data_size >> 16) & 0xff;
  header[43] = (data_size >> 24) & 0xff;
  if (fwrite(header, 1, sizeof(header), f) != sizeof(header))
    fail("cannot write %s\n", path);
  return f;
}

// Append one sample to a WAV file that open_wav() created.
static void write_sample(FILE *f, double value) {
  int sample = (int)(value * 32767.0);
  if (sample > 32767)
    sample = 32767;
  if (sample < -32768)
    sample = -32768;
  unsigned char bytes[2] = {sample & 0xff, (sample >> 8) & 0xff};
  if (fwrite(bytes, 1, 2, f) != 2)
    fail("cannot write the sample file\n");
}

static void write_tone_wav(const char *path, double freq, double seconds) {
  int samples = (int)(44100 * seconds);
  FILE *f = open_wav(path, samples);
  for (int i = 0; i < samples; i++)
    write_sample(f, 0.5 * sin(2.0 * M_PI * freq * i / 44100.0));
  fclose(f);
  printf("wrote tone file %s (%.0f Hz, %.1f s)\n", path, freq, seconds);
}

// Noise for the rate test: every frame of noise is different, so every
// published spectrum is a change and the client sees one event per frame.
static void write_noise_wav(const char *path, double seconds) {
  int samples = (int)(44100 * seconds);
  FILE *f = open_wav(path, samples);
  uint32_t state = 1;
  for (int i = 0; i < samples; i++) {
    state = state * 1103515245u + 12345u;
    write_sample(f, ((int)((state >> 16) & 0x7fff) - 16384) / 32768.0);
  }
  fclose(f);
  printf("wrote noise file %s (%.1f s)\n", path, seconds);
}

// Noise with a short 1 kHz burst at a known time. The noise makes every frame
// different, so every frame is published and the burst can be found by the
// time it shows up at.
//
// The noise is kept low (0.01) on purpose: a frame whose loudest band stays
// below the display gate keeps its plain levels, so the background of this
// file does not lift the 1 kHz band into the range the test looks for. A
// louder background would be put on the frame's own scale and could reach the
// burst level by itself.
static void write_mark_wav(const char *path, double seconds, double mark_time) {
  int samples = (int)(44100 * seconds);
  int mark_samples = (int)(44100 * 0.05);
  int mark_start = (int)(44100 * mark_time);
  FILE *f = open_wav(path, samples);
  uint32_t state = 1;
  for (int i = 0; i < samples; i++) {
    double value = 0.01;
    state = state * 1103515245u + 12345u;
    value *= ((int)((state >> 16) & 0x7fff) - 16384) / 32768.0;
    if (i >= mark_start && i < mark_start + mark_samples)
      value += 0.9 * sin(2.0 * M_PI * 1000.0 * (i - mark_start) / 44100.0);
    write_sample(f, value);
  }
  fclose(f);
  printf("wrote mark file %s (%.1f s, burst at %.1f s)\n", path, seconds,
         mark_time);
}

// What one run over a tone file measured.
struct spectrum_result {
  int frames;              // spectrum frames seen
  int average_peak;        // average column of the peak, frames after the first few
  int average_peak_value;  // average value of that peak column
  int max_high;     // largest value at or above high_from
  int max_low;      // largest value below low_to
  int high_from;    // first column that counts as "above"
  int low_to;       // first column that does not count as "below"
};

// Play a file and collect the published spectrum for about two seconds.
//
// The value is read with mpv_get_property() while the file plays instead of
// through property events: a pure tone produces almost the same frame every
// time, and mpv does not send an event for a value that did not change. The
// event path is covered by test_audio_fft_get_property().
static void run_spectrum(const char *path, int bin_count, int high_from,
                         int low_to, struct spectrum_result *result) {
  int64_t size = bin_count;
  mpv_set_property(ctx, "audio-fft-size", MPV_FORMAT_INT64, &size);

  *result = (struct spectrum_result){.high_from = high_from, .low_to = low_to};

  // The spectrum is only computed while someone observes the property; the
  // values themselves are read with mpv_get_property() below.
  mpv_observe_property(ctx, 0, "audio-fft", MPV_FORMAT_BYTE_ARRAY);
  reload_file(path);

  int frames = 0, peak_sum = 0, peak_sum_value = 0, peak_frames = 0;
  int max_high = 0, max_low = 0;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (1) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed =
        (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
    if (elapsed > 2.0)
      break;
    mpv_wait_event(ctx, 0.05);

    mpv_byte_array ba = {0};
    int r = mpv_get_property(ctx, "audio-fft", MPV_FORMAT_BYTE_ARRAY, &ba);
    if (r < 0 || !ba.data || !ba.size)
      continue;
    if (ba.size != (size_t)bin_count)
      fail("audio-fft: size %zu, expected %d\n", ba.size, bin_count);

    const uint8_t *bytes = ba.data;
    int peak = 0;
    for (int i = 1; i < bin_count; i++) {
      if (bytes[i] > bytes[peak])
        peak = i;
    }

    // skip the first reads, the sample buffer is not filled up at the start
    if (frames >= 3) {
      peak_sum += peak;
      peak_sum_value += bytes[peak];
      peak_frames++;
      for (int i = high_from; i < bin_count; i++) {
        if (bytes[i] > max_high)
          max_high = bytes[i];
      }
      for (int i = 0; i < low_to; i++) {
        if (bytes[i] > max_low)
          max_low = bytes[i];
      }
    }
    frames++;

    // the buffer of mpv_get_property() belongs to the caller
    mpv_free(ba.data);
  }

  mpv_unobserve_property(ctx, 0);

  if (peak_frames < 3)
    fail("audio-fft: only %d spectrum reads with data\n", peak_frames);

  result->frames = frames;
  result->average_peak = peak_sum / peak_frames;
  result->average_peak_value = peak_sum_value / peak_frames;
  result->max_high = max_high;
  result->max_low = max_low;
}

// The published bytes are display bands: the band of a frequency is fixed by
// the table in player/audio.c (30 Hz steps up to 3.84 kHz, then a fixed ratio
// per band up to 12 kHz and to 20 kHz), so a tone has to show up in the band
// its frequency falls into and nowhere else. Two tones are checked: a low one,
// and one that the earlier version (which averaged the frame down and only
// covered 2.5 kHz) folded back into the low bands.
//
// The bands are read after the display adjustment (see
// audio_fft_display_adjust()), which keeps the order of the bands and turns
// the loudest band of a frame into a value near the top; a range far below the
// loudest band stays at zero, so "no content in this range" is still visible.
// The contrast step in between moves the bands away from the middle of the
// height and leaves both ends in place, so it keeps those two properties.
static void test_audio_fft_spectrum(void) {
  const int bin_count = 256;
  const uint8_t floor_value = 8;

  // 437 Hz sits in the band [420, 450), everything from 840 Hz (band 28) up
  // has to stay at zero.
  struct spectrum_result low;
  run_spectrum(tone_440_path, bin_count, 28, 0, &low);
  printf("audio-fft %.0f Hz: %d frames, average peak band %d, expected 14, "
         "max above 840 Hz %d\n",
         tone_440_hz, low.frames, low.average_peak, low.max_high);
  if (abs(low.average_peak - 14) > 1)
    fail("audio-fft: %.0f Hz peak band %d, expected 14 (420..450 Hz)\n",
         tone_440_hz, low.average_peak);
  if (low.max_high > floor_value)
    fail("audio-fft: %.0f Hz has %d above 840 Hz, expected an empty range\n",
         tone_440_hz, low.max_high);

  // 5003 Hz sits in the band [4938, 5012). An implementation that folds (or
  // one that mirrors) puts it in the low bands instead.
  struct spectrum_result high;
  run_spectrum(tone_5000_path, bin_count, 162, 130, &high);
  printf("audio-fft %.0f Hz: %d frames, average peak band %d, expected 145, "
         "max below 4 kHz %d, max above 6.4 kHz %d\n",
         tone_5000_hz, high.frames, high.average_peak, high.max_low,
         high.max_high);
  if (abs(high.average_peak - 145) > 1)
    fail("audio-fft: %.0f Hz peak band %d, expected 145 (4938..5012 Hz)\n",
         tone_5000_hz, high.average_peak);
  if (high.max_low > floor_value)
    fail("audio-fft: %.0f Hz tone has %d below 4 kHz, expected the range to be "
         "empty\n",
         tone_5000_hz, high.max_low);
  if (high.max_high > floor_value)
    fail("audio-fft: %.0f Hz has %d above 6.4 kHz, expected an empty range\n",
         tone_5000_hz, high.max_high);

  printf("audio-fft spectrum test passed!\n");
}

// The picture describes the audio, not the volume. The samples are collected
// before the audio output applies its gain (volume, volume-gain, mute), and
// the frame is put on its own scale afterwards, so a quiet or muted player
// keeps showing the running spectrum and the published bytes stay the same as
// at full volume. Muting is the part that catches a capture point which moved
// behind the gain: the samples would drop to zero and the frame would be
// empty.
static void test_audio_fft_volume(void) {
  const int bin_count = 256;
  struct spectrum_result full, quiet, muted;

  set_property_string("volume", "100");
  run_spectrum(tone_5000_path, bin_count, 0, 0, &full);
  set_property_string("volume", "25");
  run_spectrum(tone_5000_path, bin_count, 0, 0, &quiet);
  set_property_string("mute", "yes");
  run_spectrum(tone_5000_path, bin_count, 0, 0, &muted);
  set_property_string("mute", "no");
  set_property_string("volume", "100");

  printf("audio-fft volume: peak value %d at volume 100, %d at volume 25, %d "
         "while muted\n",
         full.average_peak_value, quiet.average_peak_value,
         muted.average_peak_value);
  if (full.average_peak_value < 100)
    fail("audio-fft: no tone in the spectrum (peak value %d)\n",
         full.average_peak_value);
  if (abs(quiet.average_peak_value - full.average_peak_value) > 4)
    fail("audio-fft: the levels follow the volume (volume 100 gives %d, "
         "volume 25 gives %d)\n",
         full.average_peak_value, quiet.average_peak_value);
  if (abs(muted.average_peak_value - full.average_peak_value) > 4)
    fail("audio-fft: mute changed the levels (%d muted vs %d unmuted)\n",
         muted.average_peak_value, full.average_peak_value);

  printf("audio-fft volume test passed!\n");
}

static void test_audio_fft_no_observers(void) {
  reload_file(tone_440_path);

  // Wait for playback to finish without observing the property
  wait_for_playback();

  printf("audio-fft no observers test passed!\n");
}

// The spectrum follows the sound. The samples are collected where they are
// handed to the audio output, which runs ahead of the sound by the audio the
// output has queued, so the published frame is built from the samples around
// the position that is heard now. A burst at a known time in the file has to
// show up in the spectrum about that time after playback started; without
// that the burst would be early by the queued amount (about 0.6 seconds of
// playback with the null output of this test).
static void test_audio_fft_position(void) {
  const int bin_count = 256;
  const double mark_time = 1.0;
  const int mark_band = 33;   // 990 .. 1020 Hz
  const int mark_value = 120; // the burst is well above this, the background
                              // (low noise) stays below it even after the frame
                              // is put on its own scale

  mpv_observe_property(ctx, 0, "audio-fft", MPV_FORMAT_BYTE_ARRAY);
  reload_file(mark_path);

  struct timespec first = {0}, mark = {0};
  int frames = 0;
  bool finished = false;
  while (!finished) {
    mpv_event *event = mpv_wait_event(ctx, 0.05);
    if (event->event_id == MPV_EVENT_END_FILE)
      break;
    if (event->event_id != MPV_EVENT_PROPERTY_CHANGE)
      continue;
    mpv_event_property *prop = event->data;
    if (strcmp(prop->name, "audio-fft") != 0 ||
        prop->format != MPV_FORMAT_BYTE_ARRAY)
      continue;
    mpv_byte_array *ba = prop->data;
    if (!ba || !ba->data || ba->size != (size_t)bin_count)
      continue;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint8_t *bytes = ba->data;
    int band_max = 0;
    for (int i = mark_band - 2; i <= mark_band + 2; i++) {
      if (bytes[i] > band_max)
        band_max = bytes[i];
    }
    if (!frames)
      first = now;
    if (band_max >= mark_value && mark.tv_sec == 0)
      mark = now;
    frames++;
  }

  mpv_unobserve_property(ctx, 0);

  if (frames < 20)
    fail("audio-fft position: only %d frames published\n", frames);
  if (mark.tv_sec == 0)
    fail("audio-fft position: no 1 kHz burst seen in the spectrum\n");

  double elapsed = (mark.tv_sec - first.tv_sec) +
                   (mark.tv_nsec - first.tv_nsec) / 1e9;
  printf("audio-fft position: burst at %.1f s in the file, seen after %.2f s "
         "of playback (%d frames)\n",
         mark_time, elapsed, frames);
  if (fabs(elapsed - mark_time) > 0.25)
    fail("audio-fft position: burst at %.1f s appeared after %.2f s, the "
         "spectrum does not follow the playback position\n",
         mark_time, elapsed);

  printf("audio-fft position test passed!\n");
}

static void test_audio_fft_rate(void) {
  mpv_observe_property(ctx, 0, "audio-fft", MPV_FORMAT_BYTE_ARRAY);

  reload_file(noise_path);

  int fft_count = 0;
  bool finished = false;
  struct timespec first_ts = {0};
  struct timespec last_ts = {0};

  while (!finished) {
    mpv_event *event = wrap_wait_event();
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
      mpv_event_property *prop = event->data;
      if (strcmp(prop->name, "audio-fft") == 0) {
        if (prop->format == MPV_FORMAT_BYTE_ARRAY) {
          mpv_byte_array *ba = prop->data;
          if (ba && ba->data && ba->size > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (fft_count == 0)
              first_ts = now;
            last_ts = now;
            fft_count++;
            printf("FFT frame %d at timestamp %ld.%09ld\n", fft_count,
                   (long)now.tv_sec, now.tv_nsec);
          }
        }
      }
      break;
    }
    case MPV_EVENT_END_FILE:
      finished = true;
      break;
    }
  }

  mpv_unobserve_property(ctx, 0);

  if (fft_count < 3)
    fail("Not enough FFT frames to measure rate: got %d\n", fft_count);

  double elapsed = (last_ts.tv_sec - first_ts.tv_sec) +
                   (last_ts.tv_nsec - first_ts.tv_nsec) / 1e9;
  double actual_rate = (double)(fft_count - 1) / elapsed;
  double expected_rate = 10.0;

  printf("FFT frames: %d, elapsed: %.3f seconds, "
         "actual rate: %.1f Hz, expected rate: %.1f Hz\n",
         fft_count, elapsed, actual_rate, expected_rate);

  // Spectrum frames have to arrive at the rate from audio-fft-rate, without
  // extra events for values that did not change.
  if (actual_rate < expected_rate * 0.7 || actual_rate > expected_rate * 1.25)
    fail("audio-fft rate mismatch: expected ~%.1f Hz, got %.1f Hz\n",
         expected_rate, actual_rate);

  printf("audio-fft rate test passed!\n");
}

int main(void) {
  ctx = mpv_create();
  if (!ctx)
    return 1;

  atexit(exit_cleanup);

  // Set FFT options before initialization
  mpv_set_option_string(ctx, "audio-fft-rate", "10");
  mpv_set_option_string(ctx, "audio-fft-size", "256");

  initialize();

  write_tone_wav(tone_440_path, tone_440_hz, 3.0);
  write_tone_wav(tone_5000_path, tone_5000_hz, 3.0);
  write_noise_wav(noise_path, 3.0);
  write_mark_wav(mark_path, 3.0, 1.0);

  const char *fmt = "================ TEST: %s ================\n";

  printf(fmt, "test_audio_fft_get_property");
  test_audio_fft_get_property();

  printf(fmt, "test_audio_fft_read_property");
  test_audio_fft_read_property();

  printf(fmt, "test_audio_fft_spectrum");
  test_audio_fft_spectrum();

  printf(fmt, "test_audio_fft_volume");
  test_audio_fft_volume();

  printf(fmt, "test_audio_fft_no_observers");
  test_audio_fft_no_observers();

  printf(fmt, "test_audio_fft_position");
  test_audio_fft_position();

  printf(fmt, "test_audio_fft_rate");
  test_audio_fft_rate();

  printf("================ SHUTDOWN ================\n");

  command_string("quit");
  while (wrap_wait_event()->event_id != MPV_EVENT_SHUTDOWN) {
  }

  return 0;
}