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

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "mpv_talloc.h"

#include "common/common.h"
#include "common/encode.h"
#include "common/msg.h"
#include "options/options.h"
#include "osdep/timer.h"

#include "audio/format.h"
#include "audio/out/ao.h"
#include "demux/demux.h"
#include "filters/f_async_queue.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"

#include "client.h"
#include "command.h"
#include "core.h"

enum {
  AD_OK = 0,
  AD_EOF = -2,
  AD_WAIT = -4,
};

static void ao_process(struct mp_filter *f);

// Drops the samples that were collected for the realtime spectrum (defined at
// the end of this file, next to the rest of the audio_fft_* functions).
static void audio_fft_reset_capture(struct MPContext *mpctx);
static void audio_fft_init_tables(struct MPContext *mpctx, int fft_size);

static void update_speed_filters(struct MPContext *mpctx) {
  struct ao_chain *ao_c = mpctx->ao_chain;
  if (!ao_c)
    return;

  double pitch = mpctx->opts->playback_pitch;
  double speed = mpctx->opts->playback_speed;
  double resample = mpctx->speed_factor_a;
  double drop = 1.0;

  if (!mpctx->opts->pitch_correction) {
    resample *= speed;
    speed = 1.0;
  }

  int video_sync =
      mpctx->display_sync_active ? mpctx->video_out->opts->video_sync : VS_NONE;
  switch (video_sync) {
  case VS_DISP_ADROP:
    drop *= speed * resample / pitch;
    resample = pitch;
    speed = 1.0;
    break;
  case VS_DISP_TEMPO:
    speed = mpctx->audio_speed / pitch;
    resample = pitch;
    break;
  default:
    resample *= pitch;
    speed /= pitch;
    break;
  }

  mp_output_chain_set_audio_speed(ao_c->filter, speed, resample, drop);
}

static int recreate_audio_filters(struct MPContext *mpctx) {
  struct ao_chain *ao_c = mpctx->ao_chain;
  mp_assert(ao_c);

  if (!mp_output_chain_update_filters(ao_c->filter, mpctx->opts->af_settings))
    goto fail;

  update_speed_filters(mpctx);

  mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

  return 0;

fail:
  MP_ERR(mpctx, "Audio filter initialized failed!\n");
  return -1;
}

int reinit_audio_filters(struct MPContext *mpctx) {
  struct ao_chain *ao_c = mpctx->ao_chain;
  if (!ao_c)
    return 0;

  double delay = mp_output_get_measured_total_delay(ao_c->filter);

  if (recreate_audio_filters(mpctx) < 0)
    return -1;

  double ndelay = mp_output_get_measured_total_delay(ao_c->filter);

  // Only force refresh if the amount of dropped buffered data is going to
  // cause "issues" for the A/V sync logic.
  if (mpctx->audio_status == STATUS_PLAYING && delay - ndelay >= 0.2)
    issue_refresh_seek(mpctx, MPSEEK_EXACT);
  return 1;
}

static double db_gain(double db) { return pow(10.0, db / 20.0); }

static float compute_replaygain(struct MPContext *mpctx) {
  struct MPOpts *opts = mpctx->opts;

  float rgain = 1.0;

  struct replaygain_data *rg = NULL;
  struct track *track = mpctx->current_track[0][STREAM_AUDIO];
  if (track)
    rg = track->stream->codec->replaygain_data;
  if (opts->rgain_mode && rg) {
    MP_VERBOSE(mpctx, "Replaygain: Track=%f/%f Album=%f/%f\n", rg->track_gain,
               rg->track_peak, rg->album_gain, rg->album_peak);

    float gain, peak;
    if (opts->rgain_mode == 1) {
      gain = rg->track_gain;
      peak = rg->track_peak;
    } else {
      gain = rg->album_gain;
      peak = rg->album_peak;
    }

    gain += opts->rgain_preamp;
    rgain = db_gain(gain);

    MP_VERBOSE(mpctx, "Applying replay-gain: %f\n", rgain);

    if (!opts->rgain_clip) { // clipping prevention
      rgain = MPMIN(rgain, 1.0 / peak);
      MP_VERBOSE(mpctx, "...with clipping prevention: %f\n", rgain);
    }
  } else if (opts->rgain_fallback) {
    rgain = db_gain(opts->rgain_fallback);
    MP_VERBOSE(mpctx, "Applying fallback gain: %f\n", rgain);
  }

  return rgain;
}

float audio_get_gain(struct MPContext *mpctx) {
  struct MPOpts *opts = mpctx->opts;
  float gain = MPMAX(opts->softvol_volume / 100.0, 0);
  gain = pow(gain, 3);
  gain *= compute_replaygain(mpctx);
  gain *= db_gain(opts->softvol_gain);
  if (opts->softvol_mute)
    gain = 0.0;
  return gain;
}

// Called when opts->softvol_volume or opts->softvol_mute were changed.
void audio_update_volume(struct MPContext *mpctx) {
  struct ao_chain *ao_c = mpctx->ao_chain;
  if (!ao_c || !ao_c->ao)
    return;

  float gain = audio_get_gain(mpctx);
  ao_set_gain(ao_c->ao, gain);
}

// Call this if opts->playback_speed or mpctx->speed_factor_* change.
void update_playback_speed(struct MPContext *mpctx) {
  mpctx->audio_speed = mpctx->opts->playback_speed * mpctx->speed_factor_a;
  mpctx->video_speed = mpctx->opts->playback_speed * mpctx->speed_factor_v;

  update_speed_filters(mpctx);
}

static bool has_video_track(struct MPContext *mpctx) {
  if (mpctx->vo_chain && mpctx->vo_chain->is_coverart)
    return false;

  for (int n = 0; n < mpctx->num_tracks; n++) {
    struct track *track = mpctx->tracks[n];
    if (track->type == STREAM_VIDEO && !track->attached_picture &&
        !track->image)
      return true;
  }

  return false;
}

static void ao_chain_reset_state(struct ao_chain *ao_c) {
  ao_c->last_out_pts = MP_NOPTS_VALUE;
  ao_c->out_eof = false;
  ao_c->start_pts_known = false;
  ao_c->start_pts = MP_NOPTS_VALUE;
  ao_c->untimed_throttle = false;
  ao_c->underrun = false;
  ao_c->delaying_audio_start = false;
}

void reset_audio_state(struct MPContext *mpctx) {
  if (mpctx->ao_chain) {
    ao_chain_reset_state(mpctx->ao_chain);
    struct track *t = mpctx->ao_chain->track;
    if (t && t->dec)
      mp_decoder_wrapper_set_play_dir(t->dec, mpctx->play_dir);
  }
  mpctx->audio_status = mpctx->ao_chain ? STATUS_SYNCING : STATUS_EOF;
  mpctx->delay = 0;
  mpctx->logged_async_diff = -1;
  // The collected spectrum samples belong to the old playback position.
  audio_fft_reset_capture(mpctx);
}

void uninit_audio_out(struct MPContext *mpctx) {
  struct ao_chain *ao_c = mpctx->ao_chain;
  if (ao_c) {
    ao_c->ao_queue = NULL;
    TA_FREEP(&ao_c->queue_filter);
    ao_c->ao = NULL;
  }
  if (mpctx->ao) {
    // Note: with gapless_audio, stop_play is not correctly set
    if ((mpctx->opts->gapless_audio || mpctx->stop_play == AT_END_OF_FILE) &&
        ao_is_playing(mpctx->ao) && !get_internal_paused(mpctx)) {
      MP_VERBOSE(mpctx, "draining left over audio\n");
      ao_drain(mpctx->ao);
    }
    ao_uninit(mpctx->ao);

    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);
  }
  mpctx->ao = NULL;
  TA_FREEP(&mpctx->ao_filter_fmt);
}

static void ao_chain_uninit(struct ao_chain *ao_c) {
  struct track *track = ao_c->track;
  if (track) {
    mp_assert(track->ao_c == ao_c);
    track->ao_c = NULL;
    if (ao_c->dec_src)
      mp_assert(track->dec->f->pins[0] == ao_c->dec_src);
    talloc_free(track->dec->f);
    track->dec = NULL;
  }

  if (ao_c->filter_src)
    mp_pin_disconnect(ao_c->filter_src);

  talloc_free(ao_c->filter->f);
  talloc_free(ao_c->ao_filter);
  talloc_free(ao_c);
}

void uninit_audio_chain(struct MPContext *mpctx) {
  if (mpctx->ao_chain) {
    ao_chain_uninit(mpctx->ao_chain);
    mpctx->ao_chain = NULL;

    mpctx->audio_status = STATUS_EOF;

    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);
  }

  audio_fft_uninit(mpctx);
}

static char *audio_config_to_str_buf(char *buf, size_t buf_sz, int rate,
                                     int format, struct mp_chmap channels) {
  char ch[128];
  mp_chmap_to_str_buf(ch, sizeof(ch), &channels);
  char *hr_ch = mp_chmap_to_str_hr(&channels);
  if (strcmp(hr_ch, ch) != 0)
    mp_snprintf_cat(ch, sizeof(ch), " (%s)", hr_ch);
  snprintf(buf, buf_sz, "%dHz %s %dch %s", rate, ch, channels.num,
           af_fmt_to_str(format));
  return buf;
}

// Decide whether on a format change, we should reinit the AO.
static bool keep_weak_gapless_format(struct mp_aframe *old,
                                     struct mp_aframe *new) {
  bool res = false;
  struct mp_aframe *new_mod = mp_aframe_new_ref(new);
  MP_HANDLE_OOM(new_mod);

  // If the sample formats are compatible (== libswresample generally can
  // convert them), keep the AO. On other changes, recreate it.

  int old_fmt = mp_aframe_get_format(old);
  int new_fmt = mp_aframe_get_format(new);

  if (af_format_conversion_score(old_fmt, new_fmt) == INT_MIN)
    goto done; // completely incompatible formats

  if (!mp_aframe_set_format(new_mod, old_fmt))
    goto done;

  res = mp_aframe_config_equals(old, new_mod);

done:
  talloc_free(new_mod);
  return res;
}

static void ao_chain_set_ao(struct ao_chain *ao_c, struct ao *ao) {
  if (ao_c->ao != ao) {
    mp_assert(!ao_c->ao);
    ao_c->ao = ao;
    ao_c->ao_queue = ao_get_queue(ao_c->ao);
    ao_c->queue_filter = mp_async_queue_create_filter(
        ao_c->ao_filter, MP_PIN_IN, ao_c->ao_queue);
    mp_async_queue_set_notifier(ao_c->queue_filter, ao_c->ao_filter);
    // Make sure filtering never stops with frames stuck in access filter.
    mp_filter_set_high_priority(ao_c->queue_filter, true);
    audio_update_volume(ao_c->mpctx);
  }

  if (ao_c->filter->ao_needs_update)
    mp_output_chain_set_ao(ao_c->filter, ao_c->ao);

  mp_filter_wakeup(ao_c->ao_filter);
}

static int reinit_audio_filters_and_output(struct MPContext *mpctx) {
  struct MPOpts *opts = mpctx->opts;
  struct ao_chain *ao_c = mpctx->ao_chain;
  mp_assert(ao_c);
  struct track *track = ao_c->track;

  mp_assert(ao_c->filter->ao_needs_update);

  // The "ideal" filter output format
  struct mp_aframe *out_fmt = mp_aframe_new_ref(ao_c->filter->output_aformat);
  MP_HANDLE_OOM(out_fmt);

  if (!mp_aframe_config_is_valid(out_fmt)) {
    talloc_free(out_fmt);
    goto init_error;
  }

  if (af_fmt_is_pcm(mp_aframe_get_format(out_fmt))) {
    if (opts->force_srate)
      mp_aframe_set_rate(out_fmt, opts->force_srate);
    if (opts->audio_output_format)
      mp_aframe_set_format(out_fmt, opts->audio_output_format);
    if (opts->audio_output_channels.num_chmaps == 1)
      mp_aframe_set_chmap(out_fmt, &opts->audio_output_channels.chmaps[0]);
  }

  // Weak gapless audio: if the filter output format is the same as the
  // previous one, keep the AO and don't reinit anything.
  // Strong gapless: always keep the AO
  if ((mpctx->ao_filter_fmt && mpctx->ao && opts->gapless_audio < 0 &&
       keep_weak_gapless_format(mpctx->ao_filter_fmt, out_fmt)) ||
      (mpctx->ao && opts->gapless_audio > 0)) {
    ao_chain_set_ao(ao_c, mpctx->ao);
    talloc_free(out_fmt);
    return 0;
  }

  // Format change during syncing. Force playback start early, then wait.
  if (ao_c->ao_queue && mp_async_queue_get_frames(ao_c->ao_queue) &&
      mpctx->audio_status == STATUS_SYNCING) {
    mpctx->audio_status = STATUS_READY;
    mp_wakeup_core(mpctx);
    talloc_free(out_fmt);
    return 0;
  }
  if (mpctx->audio_status == STATUS_READY) {
    talloc_free(out_fmt);
    return 0;
  }

  uninit_audio_out(mpctx);

  int out_rate = mp_aframe_get_rate(out_fmt);
  int out_format = mp_aframe_get_format(out_fmt);
  struct mp_chmap out_channels = {0};
  mp_aframe_get_chmap(out_fmt, &out_channels);

  int ao_flags = 0;
  bool spdif_fallback = af_fmt_is_spdif(out_format) && ao_c->spdif_passthrough;

  if (opts->ao_null_fallback && !spdif_fallback)
    ao_flags |= AO_INIT_NULL_FALLBACK;

  if (opts->audio_stream_silence)
    ao_flags |= AO_INIT_STREAM_SILENCE;

  if (opts->audio_exclusive)
    ao_flags |= AO_INIT_EXCLUSIVE;

  if (af_fmt_is_pcm(out_format)) {
    if (!opts->audio_output_channels.set ||
        opts->audio_output_channels.auto_safe)
      ao_flags |= AO_INIT_SAFE_MULTICHANNEL_ONLY;

    mp_chmap_sel_list(&out_channels, opts->audio_output_channels.chmaps,
                      opts->audio_output_channels.num_chmaps);
  }

  if (!has_video_track(mpctx))
    ao_flags |= AO_INIT_MEDIA_ROLE_MUSIC;

  mpctx->ao_filter_fmt = out_fmt;

  mpctx->ao =
      ao_init_best(mpctx->global, ao_flags, mp_wakeup_core_cb, mpctx,
                   mpctx->encode_lavc_ctx, out_rate, out_format, out_channels);

  int ao_rate = 0;
  int ao_format = 0;
  struct mp_chmap ao_channels = {0};
  if (mpctx->ao)
    ao_get_format(mpctx->ao, &ao_rate, &ao_format, &ao_channels);

  // Verify passthrough format was not changed.
  if (mpctx->ao && af_fmt_is_spdif(out_format)) {
    if (out_rate != ao_rate || out_format != ao_format ||
        !mp_chmap_equals(&out_channels, &ao_channels)) {
      MP_ERR(mpctx, "Passthrough format unsupported.\n");
      ao_uninit(mpctx->ao);
      mpctx->ao = NULL;
    }
  }

  if (!mpctx->ao) {
    // If spdif was used, try to fallback to PCM.
    if (spdif_fallback && ao_c->track && ao_c->track->dec) {
      MP_VERBOSE(mpctx, "Falling back to PCM output.\n");
      ao_c->spdif_passthrough = false;
      ao_c->spdif_failed = true;
      mp_decoder_wrapper_set_spdif_flag(ao_c->track->dec, false);
      if (!mp_decoder_wrapper_reinit(ao_c->track->dec))
        goto init_error;
      reset_audio_state(mpctx);
      mp_output_chain_reset_harder(ao_c->filter);
      mp_wakeup_core(mpctx); // reinit with new format next time
      return 0;
    }

    MP_ERR(mpctx, "Could not open/initialize audio device -> no sound.\n");
    mpctx->error_playing = MPV_ERROR_AO_INIT_FAILED;
    goto init_error;
  }

  char tmp[192];
  MP_INFO(mpctx, "AO: [%s] %s\n", ao_get_name(mpctx->ao),
          audio_config_to_str_buf(tmp, sizeof(tmp), ao_rate, ao_format,
                                  ao_channels));
  MP_VERBOSE(mpctx, "AO: Description: %s\n", ao_get_description(mpctx->ao));
  update_window_title(mpctx, true);

  ao_c->ao_resume_time =
      opts->audio_wait_open > 0 ? mp_time_sec() + opts->audio_wait_open : 0;

  bool eof = mpctx->audio_status == STATUS_EOF;
  ao_set_paused(mpctx->ao, get_internal_paused(mpctx), eof);

  ao_chain_set_ao(ao_c, mpctx->ao);

  audio_update_volume(mpctx);

  // Almost nonsensical hack to deal with certain format change scenarios.
  if (mpctx->audio_status == STATUS_PLAYING)
    ao_start(mpctx->ao);

  mp_wakeup_core(mpctx);
  mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

  return 0;

init_error:
  uninit_audio_chain(mpctx);
  uninit_audio_out(mpctx);
  error_on_track(mpctx, track);
  return -1;
}

int init_audio_decoder(struct MPContext *mpctx, struct track *track) {
  mp_assert(!track->dec);
  if (!track->stream)
    goto init_error;

  track->dec = mp_decoder_wrapper_create(mpctx->filter_root, track->stream);
  if (!track->dec)
    goto init_error;

  if (track->ao_c)
    mp_decoder_wrapper_set_spdif_flag(track->dec, true);

  if (!mp_decoder_wrapper_reinit(track->dec))
    goto init_error;

  return 1;

init_error:
  if (track->sink)
    mp_pin_disconnect(track->sink);
  track->sink = NULL;
  error_on_track(mpctx, track);
  return 0;
}

void reinit_audio_chain(struct MPContext *mpctx) {
  struct track *track = NULL;
  track = mpctx->current_track[0][STREAM_AUDIO];
  if (!track || !track->stream) {
    if (!mpctx->encode_lavc_ctx)
      uninit_audio_out(mpctx);
    error_on_track(mpctx, track);
    return;
  }
  reinit_audio_chain_src(mpctx, track);
}

static const struct mp_filter_info ao_filter = {
    .name = "ao",
    .process = ao_process,
};

// (track=NULL creates a blank chain, used for lavfi-complex)
void reinit_audio_chain_src(struct MPContext *mpctx, struct track *track) {
  mp_assert(!mpctx->ao_chain);

  mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

  struct ao_chain *ao_c = talloc_zero(NULL, struct ao_chain);
  mpctx->ao_chain = ao_c;
  ao_c->mpctx = mpctx;
  ao_c->log = mpctx->log;
  ao_c->filter =
      mp_output_chain_create(mpctx->filter_root, MP_OUTPUT_CHAIN_AUDIO);
  ao_c->spdif_passthrough = true;
  ao_c->last_out_pts = MP_NOPTS_VALUE;
  ao_c->delay = mpctx->opts->audio_delay;

  ao_c->ao_filter = mp_filter_create(mpctx->filter_root, &ao_filter);
  if (!ao_c->filter || !ao_c->ao_filter)
    goto init_error;
  ao_c->ao_filter->priv = ao_c;

  mp_filter_add_pin(ao_c->ao_filter, MP_PIN_IN, "in");
  mp_pin_connect(ao_c->ao_filter->pins[0], ao_c->filter->f->pins[1]);

  if (track) {
    ao_c->track = track;
    track->ao_c = ao_c;
    if (!init_audio_decoder(mpctx, track))
      goto init_error;
    ao_c->dec_src = track->dec->f->pins[0];
    mp_pin_connect(ao_c->filter->f->pins[0], ao_c->dec_src);
  }

  reset_audio_state(mpctx);

  if (recreate_audio_filters(mpctx) < 0)
    goto init_error;

  if (mpctx->ao)
    audio_update_volume(mpctx);

  mp_wakeup_core(mpctx);
  return;

init_error:
  uninit_audio_chain(mpctx);
  uninit_audio_out(mpctx);
  error_on_track(mpctx, track);
}

// Return pts value corresponding to the start point of audio written to the
// ao queue so far.
double written_audio_pts(struct MPContext *mpctx) {
  return mpctx->ao_chain ? mpctx->ao_chain->last_out_pts : MP_NOPTS_VALUE;
}

// Return pts value corresponding to currently playing audio adjusted for AO
// delay and playback speed.
double playing_audio_pts(struct MPContext *mpctx) {
  double pts = written_audio_pts(mpctx);
  if (pts == MP_NOPTS_VALUE || !mpctx->ao)
    return pts;
  return pts - mpctx->audio_speed * ao_get_delay(mpctx->ao);
}

// This garbage is needed for untimed AOs. These consume audio infinitely fast,
// so try keeping approximate A/V sync by blocking audio transfer as needed.
static void update_throttle(struct MPContext *mpctx) {
  struct ao_chain *ao_c = mpctx->ao_chain;
  bool new_throttle = mpctx->audio_status == STATUS_PLAYING &&
                      mpctx->delay > 0 && ao_c && ao_c->ao &&
                      ao_untimed(ao_c->ao) && mpctx->video_status != STATUS_EOF;
  if (ao_c && new_throttle != ao_c->untimed_throttle) {
    ao_c->untimed_throttle = new_throttle;
    mp_wakeup_core(mpctx);
    mp_filter_wakeup(ao_c->ao_filter);
  }
}

static void ao_process(struct mp_filter *f) {
  struct ao_chain *ao_c = f->priv;
  struct MPContext *mpctx = ao_c->mpctx;

  if (!ao_c->queue_filter) {
    // This will eventually lead to the creation of the AO + queue, due
    // to how f_output_chain and AO management works.
    mp_pin_out_request_data(f->ppins[0]);
    // Check for EOF with no data case, which is a mess because everything
    // hates us.
    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (frame.type == MP_FRAME_EOF) {
      MP_VERBOSE(mpctx, "got EOF with no data before it\n");
      ao_c->out_eof = true;
      mpctx->audio_status = STATUS_DRAINING;
      mp_wakeup_core(mpctx);
    } else if (frame.type) {
      mp_pin_out_unread(f->ppins[0], frame);
    }
    return;
  }

  // Due to mp_async_queue_set_notifier() this function is called when the
  // queue becomes full. This affects state changes in the normal playloop,
  // so wake it up. But avoid redundant wakeups during normal playback.
  if (mpctx->audio_status != STATUS_PLAYING &&
      mp_async_queue_is_full(ao_c->ao_queue))
    mp_wakeup_core(mpctx);

  if (mpctx->audio_status == STATUS_SYNCING && !ao_c->start_pts_known)
    return;

  if (ao_c->untimed_throttle)
    return;

  if (!mp_pin_can_transfer_data(ao_c->queue_filter->pins[0], f->ppins[0]))
    return;

  struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
  if (frame.type == MP_FRAME_AUDIO) {
    struct mp_aframe *af = frame.data;

    double endpts = get_play_end_pts(mpctx);
    if (endpts != MP_NOPTS_VALUE) {
      endpts *= mpctx->play_dir;
      // Avoid decoding and discarding the entire rest of the file.
      if (mp_aframe_get_pts(af) >= endpts) {
        mp_pin_out_unread(f->ppins[0], frame);
        if (!ao_c->out_eof) {
          ao_c->out_eof = true;
          mp_pin_in_write(ao_c->queue_filter->pins[0], MP_EOF_FRAME);
        }
        return;
      }
    }
    double startpts = mpctx->audio_status == STATUS_SYNCING ? ao_c->start_pts
                                                            : MP_NOPTS_VALUE;
    mp_aframe_clip_timestamps(af, startpts, endpts);

    int samples = mp_aframe_get_size(af);
    if (!samples) {
      mp_filter_internal_mark_progress(f);
      mp_frame_unref(&frame);
      return;
    }

    ao_c->out_eof = false;

    if (mpctx->audio_status == STATUS_DRAINING ||
        mpctx->audio_status == STATUS_EOF) {
      // If a new frame comes decoder/filter EOF, we should preferably
      // call get_sync_pts() again, which (at least in obscure situations)
      // may require us to wait a while until the sync PTS is known. Our
      // code sucks and can't deal with that, so jump through a hoop to
      // get things done in the correct order.
      mp_pin_out_unread(f->ppins[0], frame);
      ao_c->start_pts_known = false;
      mpctx->audio_status = STATUS_SYNCING;
      mp_wakeup_core(mpctx);
      MP_VERBOSE(mpctx, "new audio frame after EOF\n");
      return;
    }

    mpctx->shown_aframes += samples;
    double real_samplerate = mp_aframe_get_rate(af) / mpctx->audio_speed;
    if (mpctx->video_status != STATUS_EOF)
      mpctx->delay += samples / real_samplerate;
    ao_c->last_out_pts = mp_aframe_end_pts(af);
    update_throttle(mpctx);

    audio_fft_capture(mpctx, af);

    // Gapless case: the AO is still playing from previous file. It makes
    // no sense to wait, and in fact the "full queue" event we're waiting
    // for may never happen, so start immediately.
    // If the new audio starts "later" (big video sync offset), transfer
    // of data is stopped somewhere else.
    if (mpctx->audio_status == STATUS_SYNCING && ao_is_playing(ao_c->ao)) {
      mpctx->audio_status = STATUS_READY;
      mp_wakeup_core(mpctx);
      MP_VERBOSE(mpctx, "previous audio still playing; continuing\n");
    }

    mp_pin_in_write(ao_c->queue_filter->pins[0], frame);
  } else if (frame.type == MP_FRAME_EOF) {
    MP_VERBOSE(mpctx, "audio filter EOF\n");

    ao_c->out_eof = true;
    mp_wakeup_core(mpctx);

    mp_pin_in_write(ao_c->queue_filter->pins[0], frame);
    mp_filter_internal_mark_progress(f);
  } else {
    mp_frame_unref(&frame);
  }
}

void reload_audio_output(struct MPContext *mpctx) {
  if (!mpctx->ao)
    return;

  ao_reset(mpctx->ao);
  uninit_audio_out(mpctx);
  reinit_audio_filters(mpctx); // mostly to issue refresh seek

  struct ao_chain *ao_c = mpctx->ao_chain;

  if (ao_c) {
    reset_audio_state(mpctx);
    mp_output_chain_reset_harder(ao_c->filter);
  }

  // Whether we can use spdif might have changed. If we failed to use spdif
  // in the previous initialization, try it with spdif again (we'll fallback
  // to PCM again if necessary).
  if (ao_c && ao_c->track) {
    struct mp_decoder_wrapper *dec = ao_c->track->dec;
    if (dec && ao_c->spdif_failed) {
      ao_c->spdif_passthrough = true;
      ao_c->spdif_failed = false;
      mp_decoder_wrapper_set_spdif_flag(ao_c->track->dec, true);
      if (!mp_decoder_wrapper_reinit(dec)) {
        MP_ERR(mpctx, "Error reinitializing audio.\n");
        error_on_track(mpctx, ao_c->track);
      }
    }
  }

  mp_wakeup_core(mpctx);
}

// Returns audio start pts for seeking or video sync.
// Returns false if PTS is not known yet.
static bool get_sync_pts(struct MPContext *mpctx, double *pts) {
  struct MPOpts *opts = mpctx->opts;

  *pts = MP_NOPTS_VALUE;

  if (!opts->initial_audio_sync)
    return true;

  bool sync_to_video = mpctx->vo_chain && mpctx->video_status != STATUS_EOF &&
                       !mpctx->vo_chain->is_sparse;

  if (sync_to_video) {
    if (mpctx->video_status < STATUS_READY)
      return false; // wait until we know a video PTS
    if (mpctx->video_pts != MP_NOPTS_VALUE)
      *pts = mpctx->video_pts - opts->audio_delay;
  } else if (mpctx->hrseek_active) {
    *pts = mpctx->hrseek_pts;
  } else {
    // If audio-only is enabled mid-stream during playback, sync accordingly.
    *pts = mpctx->playback_pts;
  }

  return true;
}

// Look whether audio can be started yet - if audio has to start some time
// after video.
// Caller needs to ensure mpctx->restart_complete is OK
void audio_start_ao(struct MPContext *mpctx) {
  struct ao_chain *ao_c = mpctx->ao_chain;
  if (!ao_c || !ao_c->ao || mpctx->audio_status != STATUS_READY)
    return;
  double pts = MP_NOPTS_VALUE;
  if (!get_sync_pts(mpctx, &pts))
    return;
  double apts = playing_audio_pts(mpctx);
  if (pts != MP_NOPTS_VALUE && apts != MP_NOPTS_VALUE && pts < apts &&
      mpctx->video_status != STATUS_EOF) {
    double diff = (apts - pts) / mpctx->opts->playback_speed;
    if (!get_internal_paused(mpctx))
      mp_set_timeout(mpctx, diff);
    if (mpctx->logged_async_diff != diff) {
      MP_VERBOSE(mpctx, "delaying audio start %f vs. %f, diff=%f\n", apts, pts,
                 diff);
      mpctx->logged_async_diff = diff;
      ao_c->delaying_audio_start = true;
    }
    return;
  }

  MP_VERBOSE(mpctx, "starting audio playback\n");
  ao_c->delaying_audio_start = false;
  ao_start(ao_c->ao);
  mpctx->audio_status = STATUS_PLAYING;
  if (ao_c->out_eof) {
    mpctx->audio_status = STATUS_DRAINING;
    MP_VERBOSE(mpctx, "audio draining\n");
  }
  ao_c->underrun = false;
  mpctx->logged_async_diff = -1;
  mp_wakeup_core(mpctx);
}

void fill_audio_out_buffers(struct MPContext *mpctx) {
  struct MPOpts *opts = mpctx->opts;

  if (mpctx->ao && ao_query_and_reset_events(mpctx->ao, AO_EVENT_RELOAD))
    reload_audio_output(mpctx);

  update_throttle(mpctx);

  struct ao_chain *ao_c = mpctx->ao_chain;
  if (!ao_c)
    return;

  if (ao_c->filter->failed_output_conversion) {
    error_on_track(mpctx, ao_c->track);
    return;
  }

  if (ao_c->filter->ao_needs_update) {
    if (reinit_audio_filters_and_output(mpctx) < 0)
      return;
  }

  if (mpctx->vo_chain && ao_c->track && ao_c->track->dec &&
      mp_decoder_wrapper_get_pts_reset(ao_c->track->dec)) {
    MP_WARN(mpctx, "Reset playback due to audio timestamp reset.\n");
    reset_playback_state(mpctx);
    mp_wakeup_core(mpctx);
  }

  if (mpctx->audio_status == STATUS_SYNCING) {
    double pts;
    bool ok = get_sync_pts(mpctx, &pts);

    // If the AO is still playing from the previous file (due to gapless),
    // but if video is active, this may not work if audio starts later than
    // video, and gapless has no advantages anyway. So block doing anything
    // until the old audio is fully played.
    // (Buggy if AO underruns.)
    if (mpctx->ao && ao_is_playing(mpctx->ao) &&
        mpctx->video_status != STATUS_EOF) {
      MP_VERBOSE(mpctx, "blocked, waiting for old audio to play\n");
      ok = false;
    }

    if (ao_c->start_pts_known != ok || ao_c->start_pts != pts) {
      ao_c->start_pts_known = ok;
      ao_c->start_pts = pts;
      mp_filter_wakeup(ao_c->ao_filter);
    }

    if (ao_c->ao && mp_async_queue_is_full(ao_c->ao_queue)) {
      mpctx->audio_status = STATUS_READY;
      mp_wakeup_core(mpctx);
      MP_VERBOSE(mpctx, "audio ready\n");
    } else if (ao_c->out_eof) {
      // Force playback start early.
      mpctx->audio_status = STATUS_READY;
      mp_wakeup_core(mpctx);
      MP_VERBOSE(mpctx, "audio ready (and EOF)\n");
    }
  }

  if (ao_c->ao && !ao_is_playing(ao_c->ao) && !ao_c->underrun &&
      (mpctx->audio_status == STATUS_PLAYING ||
       mpctx->audio_status == STATUS_DRAINING)) {
    // Should be playing, but somehow isn't.

    if (ao_c->out_eof && !mp_async_queue_get_frames(ao_c->ao_queue)) {
      MP_VERBOSE(mpctx, "AO signaled EOF (while in state %s)\n",
                 mp_status_str(mpctx->audio_status));
      mpctx->audio_status = STATUS_EOF;
      mp_wakeup_core(mpctx);
      // stops untimed AOs, stops pull AOs from streaming silence
      ao_reset(ao_c->ao);
    } else {
      if (!ao_c->ao_underrun) {
        MP_WARN(mpctx, "Audio device underrun detected.\n");
        ao_c->ao_underrun = true;
        mp_wakeup_core(mpctx);
        ao_c->underrun = true;
      }

      // Wait until buffers are filled before recovering underrun.
      if (ao_c->out_eof || mp_async_queue_is_full(ao_c->ao_queue)) {
        MP_VERBOSE(mpctx, "restarting audio after underrun\n");
        ao_start(mpctx->ao_chain->ao);
        ao_c->ao_underrun = false;
        ao_c->underrun = false;
        mp_wakeup_core(mpctx);
      }
    }
  }

  if (mpctx->audio_status == STATUS_PLAYING && ao_c->out_eof) {
    mpctx->audio_status = STATUS_DRAINING;
    MP_VERBOSE(mpctx, "audio draining\n");
    mp_wakeup_core(mpctx);
  }

  if (mpctx->audio_status == STATUS_DRAINING) {
    // Wait until the AO has played all queued data. In the gapless case,
    // we trigger EOF immediately, and let it play asynchronously.
    if (!ao_c->ao || (!ao_is_playing(ao_c->ao) || opts->gapless_audio)) {
      MP_VERBOSE(mpctx, "audio EOF reached\n");
      mpctx->audio_status = STATUS_EOF;
      mp_wakeup_core(mpctx);
    }
  }

  if (mpctx->restart_complete)
    audio_start_ao(mpctx); // in case it got delayed
}

// Drop data queued for output, or which the AO is currently outputting.
void clear_audio_output_buffers(struct MPContext *mpctx) {
  if (mpctx->ao)
    ao_reset(mpctx->ao);
}

// ---------------------------------------------------------------------------
// Realtime audio spectrum for visualization ("audio-fft" property)
//
// The property publishes audio-fft-size bytes for the visualization to draw.
// A band is not a single FFT bin: band i covers the frequency range from
// AUDIO_FFT_BAND_EDGES[i] to AUDIO_FFT_BAND_EDGES[i + 1] Hz (see the table
// below), so with the defaults (rate 10, size 256) one frame covers 1/10 second
// and the bands tile 0 .. 20 kHz. The level of a band is the combined level of
// the FFT bins inside it, which keeps the value a pure tone produces the same
// as when the bin was published on its own.
//
// A band is first measured as a level (-80 dB .. 0 dB over 0 .. 255, the same
// scale the batch extraction of a whole file (mediaxx) publishes), and then
// put on the scale of the frame it belongs to (the loudest band of that frame
// is drawn near the top) because that is what the picture needs: the levels as
// they are make dense parts of a song a flat wall and quiet parts a low line
// (see audio_fft_display_adjust() below). The batch extraction keeps
// publishing plain levels, because the numbers there have to mean something.
//
// Each frame is transformed on its own: the newest window of samples that
// still fits into one frame (a power of two, 4096 samples at 44.1 kHz, which
// is 93 ms) is multiplied with a Hann window and transformed with a FFT of
// that length. The bins of that transform are rate/fft_size Hz wide (11 Hz at
// 44.1 kHz), which is what the bands are built from. An earlier version
// averaged the frame down to 2*size samples first and used those as the
// transform input; that only covered size * rate / rate_hz Hz (2.5 kHz with
// the defaults) and folded everything above it back into the low bands, so
// cymbals and other high content showed up in the wrong bands.
//
// Samples are collected from the audio frames that go to the audio output,
// before the audio output applies its gain (see ao_post_process_data(), which
// runs when the output takes a frame out of its queue), and that is also why
// this works for every source, not only local files (the batch extraction
// needs the file). Because that point runs ahead of the sound, the window is
// taken around the position that is played now: the audio the output has
// queued but not played yet is left out (see ao_get_delay(), the same delay
// playing_audio_pts() uses). Without that, the spectrum would lead the sound
// by the queued amount, about 0.4 seconds with the null output. The tables and
// buffers are built once per size and rate, so publishing a frame does not
// allocate memory or call trigonometric functions in a loop. The levels
// therefore describe the audio and not the volume: the volume, volume-gain and
// mute settings all change the sound after this point, so a muted player still
// shows the running spectrum, like the batch extraction of a file does (it
// does not know the volume either).
// ---------------------------------------------------------------------------

// The published bands, as their edges in Hz: band i covers
// [AUDIO_FFT_BAND_EDGES[i], AUDIO_FFT_BAND_EDGES[i + 1]).
//
// The layout follows what the picture needs and what the analysis window can
// still separate:
//   * 0 .. 3.84 kHz in fixed 30 Hz steps (128 bands, about three FFT bins per
//     band at 44.1 kHz and at 48 kHz), where music has most of its detail;
//   * 3.84 .. 12 kHz by a fixed ratio per band (77 bands, about 47 per octave);
//   * 12 .. 20 kHz by a fixed ratio per band (51 bands, about 69 per octave).
// A fixed number of Hz per band would either waste bands on the top (nothing
// up there moves) or make the low bands too narrow to separate (a band thinner
// than about three FFT bins only follows its neighbour). Bands per third:
// 12 percent of the bands cover 0 .. 1 kHz, 44 percent cover 1 .. 5 kHz,
// 42 percent cover 5 .. 20 kHz.
//
// Keep this table and the one in mediaxx
// (src/lib/analyse/audio_visualization.h) identical: the batch extraction and
// this property have to agree. The batch extraction publishes plain levels,
// this property publishes what the picture draws (see
// audio_fft_display_adjust() below).
static const int AUDIO_FFT_BAND_EDGES[] = {
    0,     30,    60,    90,    120,   150,   180,   210,   240,   270,   300,
    330,   360,   390,   420,   450,   480,   510,   540,   570,   600,   630,
    660,   690,   720,   750,   780,   810,   840,   870,   900,   930,   960,
    990,   1020,  1050,  1080,  1110,  1140,  1170,  1200,  1230,  1260,  1290,
    1320,  1350,  1380,  1410,  1440,  1470,  1500,  1530,  1560,  1590,  1620,
    1650,  1680,  1710,  1740,  1770,  1800,  1830,  1860,  1890,  1920,  1950,
    1980,  2010,  2040,  2070,  2100,  2130,  2160,  2190,  2220,  2250,  2280,
    2310,  2340,  2370,  2400,  2430,  2460,  2490,  2520,  2550,  2580,  2610,
    2640,  2670,  2700,  2730,  2760,  2790,  2820,  2850,  2880,  2910,  2940,
    2970,  3000,  3030,  3060,  3090,  3120,  3150,  3180,  3210,  3240,  3270,
    3300,  3330,  3360,  3390,  3420,  3450,  3480,  3510,  3540,  3570,  3600,
    3630,  3660,  3690,  3720,  3750,  3780,  3810,  3840,  3897,  3955,  4014,
    4074,  4135,  4197,  4259,  4323,  4387,  4452,  4519,  4586,  4655,  4724,
    4794,  4866,  4938,  5012,  5087,  5163,  5240,  5318,  5397,  5477,  5559,
    5642,  5726,  5811,  5898,  5986,  6075,  6166,  6258,  6351,  6446,  6542,
    6639,  6738,  6839,  6941,  7044,  7149,  7256,  7364,  7474,  7585,  7698,
    7813,  7929,  8048,  8167,  8289,  8413,  8538,  8666,  8795,  8926,  9059,
    9194,  9331,  9470,  9611,  9755,  9900,  10048, 10197, 10349, 10504, 10660,
    10819, 10980, 11144, 11310, 11479, 11650, 11824, 12000, 12121, 12243, 12366,
    12491, 12616, 12743, 12872, 13001, 13132, 13264, 13398, 13533, 13669, 13806,
    13945, 14086, 14228, 14371, 14515, 14662, 14809, 14958, 15109, 15261, 15415,
    15570, 15726, 15885, 16045, 16206, 16369, 16534, 16701, 16869, 17038, 17210,
    17383, 17558, 17735, 17913, 18094, 18276, 18460, 18646, 18833, 19023, 19215,
    19408, 19603, 19801, 20000};

#define AUDIO_FFT_BANDS (MP_ARRAY_SIZE(AUDIO_FFT_BAND_EDGES) - 1)

// Longest audio delay in the audio output that the capture buffer keeps room
// for, in seconds. The output was already given this much audio that is not
// heard yet, and the spectrum is taken from the samples around the position
// that is heard now. A playback device usually queues a few tenths of a
// second; the value only has to cover that.
#define AUDIO_FFT_DELAY_MAX 0.6

// The spectrum is built from the samples that go to the audio output; those
// belong to the playback position they were taken from.
static void audio_fft_reset_capture(struct MPContext *mpctx) {
  mpctx->audio_fft_buffer_pos = 0;
  mpctx->audio_fft_written = 0;
  mpctx->audio_fft_captured = 0;
}

static void audio_fft_free_capture(struct MPContext *mpctx) {
  talloc_free(mpctx->audio_fft_buffer);
  talloc_free(mpctx->audio_fft_frame);
  talloc_free(mpctx->audio_fft_output);
  mpctx->audio_fft_buffer = NULL;
  mpctx->audio_fft_frame = NULL;
  mpctx->audio_fft_output = NULL;
  mpctx->audio_fft_buffer_size = 0;
  mpctx->audio_fft_span = 0;
  mpctx->audio_fft_fft_size = 0;
  mpctx->audio_fft_sample_rate = 0;
  mpctx->audio_fft_output_size = 0;
  mpctx->audio_fft_output_valid = false;
  mpctx->audio_fft_next_time = 0;
  audio_fft_reset_capture(mpctx);
}

// Make sure the capture buffers fit the sample rate and the column count.
static void audio_fft_update_capture(struct MPContext *mpctx, int sample_rate,
                                     int bin_count) {
  int span = sample_rate / mpctx->opts->audio_fft_rate;
  if (span <= 0)
    return;

  // The transform length is the largest power of two that fits into one
  // frame, so the window never reaches further back than one frame and the
  // covered range (sample_rate/2) still includes the last band edge.
  int fft_size = 1;
  while (fft_size * 2 <= span)
    fft_size *= 2;
  if (fft_size < 64)
    fft_size = 64;

  if (mpctx->audio_fft_span == span && mpctx->audio_fft_fft_size == fft_size &&
      mpctx->audio_fft_sample_rate == sample_rate &&
      mpctx->audio_fft_output_size == bin_count)
    return;

  audio_fft_free_capture(mpctx);
  mpctx->audio_fft_span = span;
  mpctx->audio_fft_fft_size = fft_size;
  mpctx->audio_fft_sample_rate = sample_rate;
  // The buffer keeps two frames: one is published, the next one is being
  // filled, and the window of a frame sits inside a frame. The extra room
  // holds the audio the output was given but has not played yet, so the
  // window can be taken from the samples that are heard now.
  mpctx->audio_fft_buffer_size =
      span * 2 + (int)(AUDIO_FFT_DELAY_MAX * sample_rate) + 1;
  mpctx->audio_fft_buffer =
      talloc_array(mpctx, float, mpctx->audio_fft_buffer_size);
  mpctx->audio_fft_frame = talloc_array(mpctx, float, fft_size);
  mpctx->audio_fft_output = talloc_array(mpctx, uint8_t, bin_count);
  mpctx->audio_fft_output_size = bin_count;

  audio_fft_init_tables(mpctx, fft_size);
}

static void audio_fft_free_tables(struct MPContext *mpctx) {
  talloc_free(mpctx->audio_fft_bit_reverse);
  talloc_free(mpctx->audio_fft_twiddle);
  talloc_free(mpctx->audio_fft_window);
  talloc_free(mpctx->audio_fft_real);
  talloc_free(mpctx->audio_fft_imag);
  mpctx->audio_fft_bit_reverse = NULL;
  mpctx->audio_fft_twiddle = NULL;
  mpctx->audio_fft_window = NULL;
  mpctx->audio_fft_real = NULL;
  mpctx->audio_fft_imag = NULL;
  mpctx->audio_fft_bin_count = 0;
}

// Build the tables for the given transform length.
static void audio_fft_init_tables(struct MPContext *mpctx, int fft_size) {
  int n = fft_size;

  audio_fft_free_tables(mpctx);

  mpctx->audio_fft_bit_reverse = talloc_array(mpctx, int, n);
  mpctx->audio_fft_twiddle = talloc_array(mpctx, float, n);
  mpctx->audio_fft_window = talloc_array(mpctx, float, n);
  mpctx->audio_fft_real = talloc_array(mpctx, float, n);
  mpctx->audio_fft_imag = talloc_array(mpctx, float, n);

  // Bit reversal, one table lookup per sample instead of bit operations.
  int bits = 0;
  while ((1 << bits) < n)
    bits++;
  for (int i = 0; i < n; i++) {
    int reversed = 0;
    for (int b = 0; b < bits; b++) {
      if (i & (1 << b))
        reversed |= 1 << (bits - 1 - b);
    }
    mpctx->audio_fft_bit_reverse[i] = reversed;
  }

  // Twiddle factors w(k) = exp(-2*pi*i*k/n) for k < n/2. The stages of the
  // FFT pick their factors with a fixed step, so one table is enough.
  for (int k = 0; k < n / 2; k++) {
    double angle = 2.0 * M_PI * k / n;
    mpctx->audio_fft_twiddle[2 * k] = cos(angle);
    mpctx->audio_fft_twiddle[2 * k + 1] = -sin(angle);
  }

  // Hann window.
  for (int i = 0; i < n; i++) {
    mpctx->audio_fft_window[i] =
        0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1)));
  }

  // The transformed bins are sums over n samples, so a full scale sine
  // (amplitude 1) has a magnitude of n/2 and the DC part a magnitude of n.
  mpctx->audio_fft_mag_scale_dc = 1.0f / (float)n;
  mpctx->audio_fft_mag_scale_ac = 2.0f / (float)n;

  mpctx->audio_fft_bin_count = n / 2;
}

// In-place radix-2 FFT. audio_fft_real[] holds the input samples, fft_imag[]
// must be zero before the call; afterwards both hold the transformed values.
static void audio_fft_run(struct MPContext *mpctx, int n) {
  float *real = mpctx->audio_fft_real;
  float *imag = mpctx->audio_fft_imag;
  const int *bit_reverse = mpctx->audio_fft_bit_reverse;
  const float *twiddle = mpctx->audio_fft_twiddle;

  for (int i = 0; i < n; i++) {
    int j = bit_reverse[i];
    if (i < j) {
      float t = real[i];
      real[i] = real[j];
      real[j] = t;
      t = imag[i];
      imag[i] = imag[j];
      imag[j] = t;
    }
  }

  for (int len = 2; len <= n; len <<= 1) {
    int half = len / 2;
    int step = n / len; // step between two twiddle factors in the table
    for (int i = 0; i < n; i += len) {
      const float *w = twiddle;
      for (int j = 0; j < half; j++) {
        int idx1 = i + j;
        int idx2 = idx1 + half;
        float re = real[idx2];
        float im = imag[idx2];
        float t_re = w[0] * re - w[1] * im;
        float t_im = w[0] * im + w[1] * re;
        real[idx2] = real[idx1] - t_re;
        imag[idx2] = imag[idx1] - t_im;
        real[idx1] += t_re;
        imag[idx1] += t_im;
        w += 2 * step;
      }
    }
  }
}

// Apply the window to the newest frame and transform it.
static void audio_fft_analyze(struct MPContext *mpctx) {
  int n = mpctx->audio_fft_fft_size;
  const float *frame = mpctx->audio_fft_frame;
  const float *window = mpctx->audio_fft_window;
  float *real = mpctx->audio_fft_real;
  float *imag = mpctx->audio_fft_imag;

  for (int i = 0; i < n; i++) {
    real[i] = frame[i] * window[i];
    imag[i] = 0.0f;
  }

  audio_fft_run(mpctx, n);
}

// Level of one band as a byte between 0 and 255.
//
// The bins of the band are added as levels (their squares), each counted with
// the share of its width that falls into the band. Counting whole bins instead
// would make a band measure 3 or 4 bins depending on where its edges land on
// the bin grid: the level would step by about 1 dB between neighbouring bands,
// and the same band would be measured differently at 44.1 kHz and at 48 kHz.
// With the shares, every band of the table measures exactly its own frequency
// width, whatever the sample rate is.
//
// A sine keeps the level a single published bin had, because the band around it
// holds the three bins the window spreads a tone over.
static inline uint8_t audio_fft_band_to_byte(struct MPContext *mpctx, double f0,
                                             double f1, int sample_rate) {
  const float *real = mpctx->audio_fft_real;
  const float *imag = mpctx->audio_fft_imag;
  const float db_min = -80.0f;
  const float db_range = 80.0f;
  float scale_dc2 =
      mpctx->audio_fft_mag_scale_dc * mpctx->audio_fft_mag_scale_dc;
  float scale_ac2 =
      mpctx->audio_fft_mag_scale_ac * mpctx->audio_fft_mag_scale_ac;
  int usable = mpctx->audio_fft_bin_count; // bins the transform provides
  double bin_hz = (double)sample_rate / (double)mpctx->audio_fft_fft_size;
  int first = (int)floor(f0 / bin_hz) - 1;
  int last = (int)ceil(f1 / bin_hz) + 1;
  float sum = 0.0f;

  if (first < 0)
    first = 0;
  if (last > usable)
    last = usable;

  for (int i = first; i < last; i++) {
    // Bin i covers [i - 0.5, i + 0.5] bin widths; its share is the part of
    // that range which overlaps the band.
    double share =
        MPMIN((i + 0.5) * bin_hz, f1) - MPMAX((i - 0.5) * bin_hz, f0);
    if (share <= 0.0)
      continue;
    float scale2 = i ? scale_ac2 : scale_dc2;
    sum += (real[i] * real[i] + imag[i] * imag[i]) * scale2 *
           (float)(share / bin_hz);
  }

  float magnitude = sqrtf(sum);
  float db = 20.0f * log10f(magnitude + 1e-10f);
  float level = (db - db_min) / db_range;
  if (level < 0.0f)
    level = 0.0f;
  if (level > 1.0f)
    level = 1.0f;
  return (uint8_t)(level * 255.0f);
}

// Display adjustment of a published frame: what the visualization draws.
//
// The levels alone are hard to read: music uses only the upper part of the
// -80 dB range (about -70 .. -15 dB), so a quiet passage is a low line that
// fills a third of the height, and a dense part of a song (drums, vocals and
// instruments at the same time: every band carries energy, neighbouring bands
// differ by 30 .. 40 dB) is a wall that never reaches the floor. Neither looks
// like "several waves at different heights".
//
// A frame is therefore put on its own scale: the loudest band of that frame is
// drawn at AUDIO_FFT_DISPLAY_TOP, a band AUDIO_FFT_DISPLAY_WINDOW_DB below it
// is drawn at zero, and everything in between keeps its relative height. The
// order of the bands does not change, only how much of the drawing height is
// used. Measured on a real song this grows the differences between
// neighbouring bands by about 45 percent and the frame to frame movement by
// about 60 percent, and it flattens the difference between passages, which is
// what fills the picture while the music plays.
//
// The bands of one frame still sit close together after that (most of them
// land between 20 and 60 percent of the height, because only the loudest band
// reaches the top), so AUDIO_FFT_DISPLAY_CONTRAST pulls them apart once more:
// a band above the middle of the drawing height is moved up, one below the
// middle is moved down, and the top and the floor stay where they are. This
// keeps the average height of the picture about the same as without the extra
// step, unlike a stronger AUDIO_FFT_DISPLAY_GAMMA, which mostly makes the
// whole picture shorter. Measured on the same song, 2.5 grows the differences
// between neighbouring bands by another 40 percent and the frame to frame
// movement by another 50 percent.
//
// Frames whose loudest band stays below AUDIO_FFT_DISPLAY_GATE_LOW are left as
// they are (silence and very quiet parts keep looking quiet instead of being
// pumped up to the top), and between the two gate values the plain and the
// adjusted frame are mixed, so crossing the gate does not jump.
//
// AUDIO_FFT_DISPLAY_GAMMA is applied last (a gamma curve on the height), which
// lifts the middle of the range a little and keeps the very quiet frames low.
//
// The app and the plugins draw these bytes as they come; keep these constants
// in step with AudioWaveBaseConfig_c in the app (defDisplayWindowDb /
// defDisplayTopLevel / defDisplayGateLow / defDisplayGateHigh /
// defDisplayContrast / defDisplayGamma, used when a frame comes from the batch
// extraction instead of from this property).
#define AUDIO_FFT_DISPLAY_WINDOW_DB 50.0
#define AUDIO_FFT_DISPLAY_TOP 230.0
#define AUDIO_FFT_DISPLAY_GATE_LOW 64.0
#define AUDIO_FFT_DISPLAY_GATE_HIGH 96.0
#define AUDIO_FFT_DISPLAY_CONTRAST 1.0
#define AUDIO_FFT_DISPLAY_GAMMA 1.2

static void audio_fft_display_adjust(uint8_t *bytes, int count) {
  if (count < 1)
    return;

  int peak = 0;
  for (int i = 0; i < count; i++) {
    if (bytes[i] > peak)
      peak = bytes[i];
  }

  bool adjust = peak > (int)AUDIO_FFT_DISPLAY_GATE_LOW;
  double low = 0.0, span = 1.0, weight = 0.0;
  if (adjust) {
    low = (double)peak - AUDIO_FFT_DISPLAY_WINDOW_DB / 80.0 * 255.0;
    span = peak - low < 1.0 ? 1.0 : (double)peak - low;
    weight =
        peak >= AUDIO_FFT_DISPLAY_GATE_HIGH
            ? 1.0
            : ((double)peak - AUDIO_FFT_DISPLAY_GATE_LOW) /
                  (AUDIO_FFT_DISPLAY_GATE_HIGH - AUDIO_FFT_DISPLAY_GATE_LOW);
  }

  for (int i = 0; i < count; i++) {
    double value = (double)bytes[i];
    if (adjust) {
      // Place of the band between the bottom of the window (0) and the loudest
      // band (1). The contrast curve leaves both ends in place and pulls what
      // lies between them away from the middle.
      double place = (value - low) / span;
      if (place < 0.0)
        place = 0.0;
      if (place > 1.0)
        place = 1.0;
      double up = powf((float)place, (float)AUDIO_FFT_DISPLAY_CONTRAST);
      double down =
          powf((float)(1.0 - place), (float)AUDIO_FFT_DISPLAY_CONTRAST);
      double scaled = up / (up + down) * AUDIO_FFT_DISPLAY_TOP;
      value += (scaled - value) * weight;
      if (value < 0.0)
        value = 0.0;
      if (value > 255.0)
        value = 255.0;
    }
    double shown =
        powf((float)(value / 255.0), (float)AUDIO_FFT_DISPLAY_GAMMA) * 255.0;
    int out = (int)(shown + 0.5);
    bytes[i] = out < 0 ? 0 : (out > 255 ? 255 : (uint8_t)out);
  }
}

// How many bands are published: the option picks a prefix of the table.
static int audio_fft_band_count(struct MPContext *mpctx) {
  int count = mpctx->opts->audio_fft_size;
  return count > AUDIO_FFT_BANDS ? AUDIO_FFT_BANDS : count;
}

// Turn the transformed samples into the byte spectrum: one byte per band of
// AUDIO_FFT_BAND_EDGES, from 0 Hz up.
static void audio_fft_to_bytes(struct MPContext *mpctx, int bin_count) {
  uint8_t *output = mpctx->audio_fft_output;
  int sample_rate = mpctx->audio_fft_sample_rate;

  if (!bin_count || !mpctx->audio_fft_fft_size || sample_rate <= 0) {
    memset(output, 0, bin_count);
    return;
  }
  if (bin_count > AUDIO_FFT_BANDS)
    bin_count = AUDIO_FFT_BANDS;

  for (int i = 0; i < bin_count; i++) {
    output[i] = audio_fft_band_to_byte(mpctx, (double)AUDIO_FFT_BAND_EDGES[i],
                                       (double)AUDIO_FFT_BAND_EDGES[i + 1],
                                       sample_rate);
  }

  // The levels above are turned into what the picture draws.
  audio_fft_display_adjust(output, bin_count);
}

// Mix one audio frame down to mono and append it to the ring buffer.
void audio_fft_capture(struct MPContext *mpctx, struct mp_aframe *aframe) {
  if (!mpctx->opts->audio_fft_rate || !mpctx->opts->audio_fft_size)
    return;

  if (!mp_property_has_observers(mpctx, "audio-fft"))
    return;

  int samples = mp_aframe_get_size(aframe);
  int channels = mp_aframe_get_channels(aframe);
  int sample_rate = mp_aframe_get_rate(aframe);
  int format = mp_aframe_get_format(aframe);
  uint8_t **planes = mp_aframe_get_data_ro(aframe);

  if (samples <= 0 || channels <= 0 || sample_rate <= 0 || !planes ||
      !planes[0])
    return;
  if (!af_fmt_is_valid(format) ||
      (!af_fmt_is_float(format) && !af_fmt_is_int(format)))
    return;

  audio_fft_update_capture(mpctx, sample_rate, audio_fft_band_count(mpctx));
  if (!mpctx->audio_fft_buffer)
    return;

  bool planar = af_fmt_is_planar(format);
  bool is_float = af_fmt_is_float(format);
  bool is_unsigned = af_fmt_is_unsigned(format);
  int bytes = af_fmt_to_bytes(format);
  float scale = 1.0f / (float)channels;
  float int_scale = 1.0f / (float)(1 << (bytes * 8 - 1));

  float *buffer = mpctx->audio_fft_buffer;
  int size = mpctx->audio_fft_buffer_size;
  int pos = mpctx->audio_fft_buffer_pos;

  for (int s = 0; s < samples; s++) {
    float sum = 0.0f;

    for (int c = 0; c < channels; c++) {
      const uint8_t *ptr = planar
                               ? planes[c] + (size_t)s * bytes
                               : planes[0] + (size_t)(s * channels + c) * bytes;
      if (is_float) {
        if (bytes == 4) {
          float value;
          memcpy(&value, ptr, sizeof(value));
          sum += value;
        } else {
          double value;
          memcpy(&value, ptr, sizeof(value));
          sum += value;
        }
      } else if (is_unsigned) {
        // unsigned samples are centered around half of their value range
        int value = bytes == 1 ? ptr[0] : ptr[0] | (ptr[1] << 8);
        sum += ((float)value - (float)(1 << (bytes * 8 - 1))) * int_scale;
      } else {
        // signed samples, least significant byte first
        int32_t value = (int8_t)ptr[bytes - 1]; // sign extend
        for (int b = bytes - 2; b >= 0; b--)
          value = (value << 8) | ptr[b];
        sum += (float)value * int_scale;
      }
    }

    buffer[pos] = sum * scale;
    pos++;
    if (pos >= size)
      pos = 0;
  }

  mpctx->audio_fft_buffer_pos = pos;
  mpctx->audio_fft_written += samples;
  mpctx->audio_fft_captured += samples;
}

// Compute the spectrum from the collected samples. Called from the play loop,
// the rate is limited by the audio-fft-rate option.
void audio_fft_compute(struct MPContext *mpctx) {
  struct MPOpts *opts = mpctx->opts;

  if (!opts->audio_fft_rate || !opts->audio_fft_size)
    return;

  // Wait until a whole frame was captured (a frame is 1/audio-fft-rate
  // seconds long), and skip if nothing new arrived since the last spectrum
  // (this also covers paused playback).
  int span = mpctx->audio_fft_span;
  if (!mpctx->audio_fft_buffer || span <= 0 ||
      mpctx->audio_fft_written < span || !mpctx->audio_fft_captured) {
    return;
  }

  // One frame every 1/audio-fft-rate seconds. The deadline is carried over
  // from the previous frame instead of starting from "now", so the rate does
  // not drift with the granularity of the play loop (a frame per pass would
  // give a lower rate than configured). A deadline that fell behind (pause,
  // long gap) starts over instead of catching up in a burst.
  double interval = 1.0 / (double)opts->audio_fft_rate;
  double now = mp_time_sec();
  if (mpctx->audio_fft_next_time != 0 && now < mpctx->audio_fft_next_time) {
    return;
  }

  if (!mp_property_has_observers(mpctx, "audio-fft"))
    return;

  int bin_count = audio_fft_band_count(mpctx);
  int fft_size = mpctx->audio_fft_fft_size;

  // The audio output was already handed the audio of the next few tenths of a
  // second, which is not heard yet. Take the window from the position that is
  // heard now (playing_audio_pts() subtracts the same delay), otherwise the
  // spectrum would run ahead of the sound by that amount.
  int64_t delay = 0;
  if (mpctx->ao) {
    double delay_sec = ao_get_delay(mpctx->ao);
    if (delay_sec > 0)
      delay = (int64_t)(delay_sec * mpctx->audio_fft_sample_rate);
  }
  // never reach behind the samples the ring buffer still holds
  int64_t oldest = mpctx->audio_fft_buffer_size - fft_size;
  if (delay > oldest)
    delay = oldest;
  if (delay > mpctx->audio_fft_written - fft_size)
    delay = mpctx->audio_fft_written - fft_size;
  if (delay < 0)
    delay = 0;

  // Take the window out of the ring buffer.
  float *frame = mpctx->audio_fft_frame;
  const float *buffer = mpctx->audio_fft_buffer;
  int size = mpctx->audio_fft_buffer_size;
  int start = mpctx->audio_fft_buffer_pos - fft_size - (int)delay;
  if (start < 0)
    start += size;
  int first = size - start;
  if (first > fft_size)
    first = fft_size;
  memcpy(frame, buffer + start, first * sizeof(float));
  if (first < fft_size)
    memcpy(frame + first, buffer, (fft_size - first) * sizeof(float));

  audio_fft_analyze(mpctx);
  audio_fft_to_bytes(mpctx, bin_count);

  mpctx->audio_fft_output_valid = true;
  mpctx->audio_fft_captured = 0;
  if (mpctx->audio_fft_next_time == 0 ||
      now - mpctx->audio_fft_next_time > interval) {
    mpctx->audio_fft_next_time = now + interval;
  } else {
    mpctx->audio_fft_next_time += interval;
  }

  mp_notify_property(mpctx, "audio-fft");
}

void audio_fft_uninit(struct MPContext *mpctx) {
  audio_fft_free_tables(mpctx);
  audio_fft_free_capture(mpctx);
}
