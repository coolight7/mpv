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
#include <stdint.h>
#include <time.h>

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

  // Load a lavfi sine wave as audio source
  reload_file("/home/mpv/sine.wav");

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

            // Verify all values are in range [0, 255]
            for (int i = 0; i < (int)ba->size; i++) {
              if (bytes[i] > 255) {
                fail("audio-fft value out of range: %d at index %d\n", bytes[i],
                     i);
              }
            }

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
  reload_file("/home/mpv/sine.wav");

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
    fail("mpv_get_property audio-fft: invalid data (size=%zu, expected=%d)\n",
         ba.size, fft_size);

  printf("Got FFT data via mpv_get_property: size=%zu\n", ba.size);

  printf("audio-fft read_property test passed!\n");
}

static void test_audio_fft_no_observers(void) {
  reload_file("/home/mpv/sine.wav");

  // Wait for playback to finish without observing the property
  wait_for_playback();

  printf("audio-fft no observers test passed!\n");
}

static void test_audio_fft_rate(void) {
  mpv_observe_property(ctx, 0, "audio-fft", MPV_FORMAT_BYTE_ARRAY);

  reload_file("/home/mpv/sine.wav");

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

  if (actual_rate < expected_rate * 0.7 || actual_rate > expected_rate * 1.5)
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

  const char *fmt = "================ TEST: %s ================\n";

  printf(fmt, "test_audio_fft_get_property");
  test_audio_fft_get_property();

  printf(fmt, "test_audio_fft_read_property");
  test_audio_fft_read_property();

  printf(fmt, "test_audio_fft_no_observers");
  test_audio_fft_no_observers();

  printf(fmt, "test_audio_fft_rate");
  test_audio_fft_rate();

  printf("================ SHUTDOWN ================\n");

  command_string("quit");
  while (wrap_wait_event()->event_id != MPV_EVENT_SHUTDOWN) {
  }

  return 0;
}