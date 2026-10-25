/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"
#include "apu_output.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"

/* Output device requests from the UI, applied on the audio thread */
static QemuMutex device_lock;
static char device_name[256];
static int device_gen;
static int ui_gain_milli = 1000; /* qatomic needs an integer */

void mcpx_apu_monitor_set_device(const char *name)
{
    qemu_mutex_lock(&device_lock);
    g_strlcpy(device_name, name ? name : "", sizeof(device_name));
    qatomic_inc(&device_gen);
    qemu_mutex_unlock(&device_lock);
}

void mcpx_apu_monitor_set_ui_gain(float gain)
{
    qatomic_set(&ui_gain_milli, (int)(fmaxf(0.0f, fminf(gain, 1.0f)) * 1000 + 0.5f));
}

/* The playback device with this name, or the default */
static SDL_AudioDeviceID find_device(const char *name)
{
    SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!name || !name[0]) {
        return found;
    }
    int count = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; ids && i < count; i++) {
        const char *n = SDL_GetAudioDeviceName(ids[i]);
        if (n && !strcmp(n, name)) {
            found = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return found;
}

static void open_stream(MCPXAPUState *d, const char *name, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    d->monitor.stream = SDL_OpenAudioDeviceStream(find_device(name), &spec,
                                                  NULL, NULL);
    if (d->monitor.stream == NULL && name && name[0]) {
        /* Unplugged or renamed: fall back to the default */
        d->monitor.stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    }
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_OpenAudioDeviceStream failed: %s",
                   SDL_GetError());
        return;
    }

    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(d->monitor.stream);

    SDL_AudioSpec dev_spec;
    int dev_buf_frames = 0;
    int dev_drain_bytes = 0;
    if (SDL_GetAudioDeviceFormat(dev, &dev_spec, &dev_buf_frames)) {
        dev_drain_bytes = dev_buf_frames * spec.channels *
                          SDL_AUDIO_BYTESIZE(spec.format) *
                          spec.freq / dev_spec.freq;
    }
    int frame_bytes = sizeof(d->monitor.frame_buf);
    int drain = MAX(dev_drain_bytes, frame_bytes);
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    SDL_ResumeAudioDevice(dev);
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    d->monitor.stream = NULL;
    qemu_mutex_init(&device_lock);
    g_strlcpy(device_name, g_config.audio.output_device ?
              g_config.audio.output_device : "", sizeof(device_name));
    d->monitor.device_gen = qatomic_read(&device_gen);

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    open_stream(d, device_name, errp);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    int gen = qatomic_read(&device_gen);
    if (gen != d->monitor.device_gen) {
        /* Another output device was chosen: reopen on this thread, the
         * only one that uses the stream */
        char name[sizeof(device_name)];
        qemu_mutex_lock(&device_lock);
        g_strlcpy(name, device_name, sizeof(name));
        qemu_mutex_unlock(&device_lock);
        d->monitor.device_gen = gen;
        if (d->monitor.stream) {
            SDL_DestroyAudioStream(d->monitor.stream);
            d->monitor.stream = NULL;
        }
        Error *err = NULL;
        open_stream(d, name, &err);
        if (err) {
            warn_report_err(err);
        }
    }

    if (d->monitor.stream) {
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E) *
                   qatomic_read(&ui_gain_milli) / 1000.0;
        SDL_SetAudioStreamGain(d->monitor.stream, vu);
        SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                            sizeof(d->monitor.frame_buf));
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
