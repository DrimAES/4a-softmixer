/*
 * Copyright(C) 2018 "IoT.bzh"
 * Author Fulup Ar Foll <fulup@iot.bzh>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http : //www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License

for the specific language governing permissions and
 * limitations under the License.
 *
 * reference :
 * https://github.com/zonque/simple-alsa-loop/blob/master/loop.c
 * https://www.alsa-project.org/alsa-doc/alsa-lib/_2test_2pcm_8c-example.html#a31
 *
 */

#define _GNU_SOURCE  // needed for vasprintf

#include "alsa-softmixer.h"
#include <pthread.h>
#include <sys/syscall.h>
#include <sched.h>

static int xrun(snd_pcm_t * pcm);
static int suspend(snd_pcm_t * pcm);

static inline snd_pcm_uframes_t buf_avail(AlsaPcmCopyHandleT * chandle)
{
	return chandle->buf_size - chandle->buf_count;
}


STATIC int AlsaPeriodSize(snd_pcm_format_t pcmFormat) {
    int pcmSampleSize;

    switch (pcmFormat) {

        case SND_PCM_FORMAT_S8:
        case SND_PCM_FORMAT_U8:
            pcmSampleSize = 1;
            break;

        case SND_PCM_FORMAT_U16_LE:
        case SND_PCM_FORMAT_U16_BE:
        case SND_PCM_FORMAT_S16_LE:
        case SND_PCM_FORMAT_S16_BE:
            pcmSampleSize = 2;
            break;

        case SND_PCM_FORMAT_U32_LE:
        case SND_PCM_FORMAT_U32_BE:
        case SND_PCM_FORMAT_S32_LE:
        case SND_PCM_FORMAT_S32_BE:
            pcmSampleSize = 4;
            break;

        default:
            pcmSampleSize = 0;
    }

    return pcmSampleSize;
}

PUBLIC int AlsaPcmConf(SoftMixerT *mixer, AlsaPcmCtlT *pcm, AlsaPcmHwInfoT *opts, int mode) {
    int error;
    snd_pcm_hw_params_t *pxmHwParams;
    snd_pcm_sw_params_t *pxmSwParams;
    snd_pcm_format_t format;
    snd_pcm_access_t access;

    AFB_ApiInfo(mixer->api,
                "%s: mixer info %s uid %s , pcm %s, mode %d",
                __func__, mixer->info, mixer->uid, pcm->cid.cardid, mode);

    // retrieve hardware config from PCM
    snd_pcm_hw_params_alloca(&pxmHwParams);
    error = snd_pcm_hw_params_any(pcm->handle, pxmHwParams);
    if (error < 0) {
        AFB_ApiError(mixer->api, "%s: Failed to get parameters: %s", __func__, snd_strerror(error));
        goto OnErrorExit;
    }

    AFB_ApiNotice(mixer->api, "PARAMS before:\n");
    AlsaDumpPcmParams(mixer, pxmHwParams);

    if (!opts->access)
         opts->access = SND_PCM_ACCESS_RW_INTERLEAVED;

    snd_pcm_hw_params_get_access(pxmHwParams, &access);
    error = snd_pcm_hw_params_set_access(pcm->handle, pxmHwParams, opts->access);
    if (error) {
        AFB_ApiError(mixer->api,
                     "%s set_access failed (ignore this error): mixer=%s cardid=%s access=%d Fail current=%d mode error=%s",
                     __func__, mixer->uid, pcm->cid.cardid, opts->access, access, snd_strerror(error));
//Fulup        goto OnErrorExit;
    };

    if (opts->format != SND_PCM_FORMAT_UNKNOWN) {
        snd_pcm_hw_params_get_format(pxmHwParams, &format);
        if ((error = snd_pcm_hw_params_set_format(pcm->handle, pxmHwParams, opts->format)) < 0) {
            AFB_ApiError(mixer->api,
                         "%s: mixer=%s cardid=%s Set_Format=%s (%d) FAILED current=%d error=%s",
                         __func__, mixer->uid, pcm->cid.cardid, opts->formatS, opts->format, format, snd_strerror(error));
            AlsaDumpFormats(mixer, pcm->handle);
            goto OnErrorExit;
        }
    }

    if (opts->rate > 0) {
        unsigned int pcmRate = opts->rate;
        if ((error = snd_pcm_hw_params_set_rate_near(pcm->handle, pxmHwParams, &opts->rate, 0)) < 0) {
            AFB_ApiError(mixer->api, "AlsaPcmConf: mixer=%s cardid=%s FailSet_Rate=%d error=%s", mixer->uid, pcm->cid.cardid, opts->rate, snd_strerror(error));
            goto OnErrorExit;
        }

        // check we got requested rate
        if (opts->rate != pcmRate) {
            AFB_ApiError(mixer->api, "AlsaPcmConf: mixer=%s cardid=%s Set_Rate Fail ask=%dHz get=%dHz", mixer->uid, pcm->cid.cardid,pcmRate, opts->rate);
            goto OnErrorExit;
        }
    }

    if (opts->channels) {
        if ((error = snd_pcm_hw_params_set_channels(pcm->handle, pxmHwParams, opts->channels)) < 0) {
            AFB_ApiError(mixer->api, "AlsaPcmConf: mixer=%s cardid=%s Set_Channels=%d Fail error=%s",mixer->uid, pcm->cid.cardid, opts->channels, snd_strerror(error));
            goto OnErrorExit;
        };
    }

    int err;
    unsigned buffer_time = 0;
    unsigned period_time = 0;
    snd_pcm_uframes_t buffer_frames = 0;
    snd_pcm_uframes_t period_frames = 0;

	err = snd_pcm_hw_params_get_buffer_time_max(pxmHwParams, &buffer_time, 0);
	if (buffer_time > 500000)
		buffer_time = 500000;

	if (period_time == 0 && period_frames == 0) {
		if (buffer_time > 0)
			period_time = buffer_time / 4;
		else
			period_frames = buffer_frames / 4;
	}
	if (period_time > 0)
		err = snd_pcm_hw_params_set_period_time_near(pcm->handle, pxmHwParams, &period_time, 0);
	else
		err = snd_pcm_hw_params_set_period_size_near(pcm->handle, pxmHwParams, &period_frames, 0);

	assert(err >= 0);

	if (buffer_time > 0) {
		err = snd_pcm_hw_params_set_buffer_time_near(pcm->handle, pxmHwParams, &buffer_time, 0);
	} else {
		err = snd_pcm_hw_params_set_buffer_size_near(pcm->handle, pxmHwParams, &buffer_frames);
	}

	assert(err >= 0);

    // store selected values
    if ((error = snd_pcm_hw_params(pcm->handle, pxmHwParams)) < 0) {
        AFB_ApiError(mixer->api,
        		     "%s: mixer=%s cardid=%s Fail apply hwparams error=%s",
        		     __func__, mixer->uid, pcm->cid.cardid, snd_strerror(error));
        goto OnErrorExit;
    }

    AFB_ApiNotice(mixer->api, "PARAMS after:\n");
    AlsaDumpPcmParams(mixer, pxmHwParams);

    // check we effective hw params after optional format change
    snd_pcm_hw_params_get_channels(pxmHwParams, &opts->channels);
    snd_pcm_hw_params_get_format(pxmHwParams, &opts->format);
    snd_pcm_hw_params_get_rate(pxmHwParams, &opts->rate, 0);
    opts->sampleSize = AlsaPeriodSize(opts->format);
    if (opts->sampleSize == 0) {
        AFB_ApiError(mixer->api,
                     "%s: mixer=%s cardid=%s Fail unsupported format format=%d",
                     __func__, mixer->uid, pcm->cid.cardid, opts->format);
        goto OnErrorExit;
    }

    // retrieve software config from PCM
    snd_pcm_sw_params_alloca(&pxmSwParams);
    snd_pcm_sw_params_current(pcm->handle, pxmSwParams);

    if ((error = snd_pcm_sw_params_set_avail_min(pcm->handle, pxmSwParams, 6000)) < 0) {
        AFB_ApiError(mixer->api,
                     "%s: mixer=%s cardid=%s Fail set_buffersize error=%s",
					 __func__, mixer->uid, pcm->cid.cardid, snd_strerror(error));
        goto OnErrorExit;
    };

    // push software params into PCM
    if ((error = snd_pcm_sw_params(pcm->handle, pxmSwParams)) < 0) {
        AFB_ApiError(mixer->api,
        		     "%s: mixer=%s cardid=%s Fail to push SW params error=%s",
					 __func__, mixer->uid, pcm->cid.cardid, snd_strerror(error));
        goto OnErrorExit;
    };

    AFB_ApiNotice(mixer->api,
    		      "%s: mixer=%s cardid=%s Done channels=%d rate=%d format=%d access=%d done",
				  __func__, mixer->uid, pcm->cid.cardid, opts->channels, opts->rate, opts->format, opts->access);
    return 0;

OnErrorExit:
    return -1;
}

STATIC int AlsaPcmReadCB( struct pollfd * pfd, AlsaPcmCopyHandleT * pcmCopyHandle) {
	char string[32];

	snd_pcm_sframes_t availIn, availOut, availInBuf;
	int err;

	// PCM has was closed
	if ((pfd->revents & POLLHUP) != 0) {
		AFB_ApiNotice(pcmCopyHandle->api,
				"%s PCM=%s hanghup/disconnected",
				__func__, ALSA_PCM_UID(pcmCopyHandle->pcmIn, string));
		goto ExitOnSuccess;
	}

	// ignore any non input events
	if ((pfd->revents & EPOLLIN) == 0) {
		goto ExitOnSuccess;
	}

	// do we have waiting frame
	availIn = snd_pcm_avail_update(pcmCopyHandle->pcmIn);
	if (availIn <= 0) {
		if (availIn == -EPIPE) {
			printf("XXX read EPIPE\n");
			xrun(pcmCopyHandle->pcmIn);
		}
		goto ExitOnSuccess;
	}

	availInBuf = buf_avail(pcmCopyHandle);

	/* we get too much data, take what we can now,
	 * hopefully we will have more luck next time */

	if (availIn > availInBuf)
		availIn = availInBuf;

	snd_pcm_sframes_t totalRead = 0;
	snd_pcm_sframes_t totalWrite = 0;

	while (availIn > 0) {
		snd_pcm_sframes_t r = buf_avail(pcmCopyHandle);
		if (r + pcmCopyHandle->buf_pos > pcmCopyHandle->buf_size)
			r = pcmCopyHandle->buf_size - pcmCopyHandle->buf_pos;
		if (r > availIn)
			r = availIn;
		r = snd_pcm_readi(pcmCopyHandle->pcmIn,
				pcmCopyHandle->buf +
				pcmCopyHandle->buf_pos *
				pcmCopyHandle->frame_size, r);
		if (r == 0)
			goto ExitOnSuccess;
		if (r < 0) {
			if (r == -EPIPE) {
				printf("read EPIPE (%d)\n", ++pcmCopyHandle->read_err_count);
				err = xrun(pcmCopyHandle->pcmIn);
				goto ExitOnSuccess;
			} else if (r == -ESTRPIPE) {
				printf("read ESTRPIPE\n");
				if ((err = suspend(pcmCopyHandle->pcmIn)) < 0)
					goto ExitOnSuccess;
				r = 0;
			} else {
				goto ExitOnSuccess;;
			}
		}

		totalRead += r;

		pcmCopyHandle->buf_count += r;
		pcmCopyHandle->buf_pos += r;
		pcmCopyHandle->buf_pos %= pcmCopyHandle->buf_size;
		availIn -= r;
	}

	// do we have space to push frame
	availOut = snd_pcm_avail_update(pcmCopyHandle->pcmOut);
	if (availOut < 0) {
		if (availOut == -EPIPE) {
			printf("write update EPIPE\n");
			xrun(pcmCopyHandle->pcmOut);
			goto ExitOnSuccess;
		}
		if (availOut == -ESTRPIPE) {
			printf("write update ESTRPIPE\n");
			suspend(pcmCopyHandle->pcmOut);
			goto ExitOnSuccess;
		}
	}

	if (availOut == 0) {
		printf("No space on playback\n");
		goto ExitOnSuccess;
	}

	while (pcmCopyHandle->buf_count > 0) {
		// In/Out frames transfer through buffer copy
		snd_pcm_sframes_t r = pcmCopyHandle->buf_count;

		if (r + pcmCopyHandle->buf_pos > pcmCopyHandle->buf_size)
			r = pcmCopyHandle->buf_size - pcmCopyHandle->buf_pos;
		if (r > availOut)
			r = availOut;

		r = snd_pcm_writei(
				pcmCopyHandle->pcmOut,
				pcmCopyHandle->buf +
				pcmCopyHandle->buf_pos *
				pcmCopyHandle->frame_size, r);
		if (r <= 0) {
			if (r == -EPIPE) {
				printf("XXX write EPIPE (%d)\n", ++pcmCopyHandle->write_err_count );
				err = xrun(pcmCopyHandle->pcmOut);
				continue;
			} else if (r == -ESTRPIPE) {
				printf("XXX write ESTRPIPE\n");
				goto ExitOnSuccess;
			}
			printf("Unhandled error %s\n", strerror(errno));
			goto ExitOnSuccess;
		}

		totalWrite += r;

		pcmCopyHandle->buf_count -= r;
		pcmCopyHandle->buf_pos += r;
		pcmCopyHandle->buf_pos %= pcmCopyHandle->buf_size;

		/* We evaluate the available space on device again,
		 * because it may have grown since the measured the value
		 * before the loop. If we ignore a new bigger value, we will keep
		 * writing small chunks, keep in tight loops and stave the CPU */

		snd_pcm_sframes_t newAvailOut = snd_pcm_avail(pcmCopyHandle->pcmOut);

		if (newAvailOut == 0) {
			snd_pcm_wait(pcmCopyHandle->pcmOut, 5);
		} else if (newAvailOut > 0) {
			availOut = newAvailOut;
		}
	}

	return 0;

	// Cannot handle error in callback
ExitOnSuccess:
	return 0;
}


static int xrun( snd_pcm_t * pcm)
{
	int err;

	if ((err = snd_pcm_prepare(pcm)) < 0) {
		return err;
	}

	return 0;
}

static int suspend( snd_pcm_t * pcm)
{
	int err;

	while ((err = snd_pcm_resume(pcm)) == -EAGAIN) {
		usleep(1);
	}
	if (err < 0)
		return xrun(pcm);
	return 0;
}


static void *LoopInThread(void *handle) {
    AlsaPcmCopyHandleT *pcmCopyHandle = (AlsaPcmCopyHandleT*) handle;
    pcmCopyHandle->tid = (int) syscall(SYS_gettid);

    AFB_ApiNotice(pcmCopyHandle->api,
                  "%s :%s/%d Started",
                  __func__, pcmCopyHandle->info, pcmCopyHandle->tid);

    for (int ix=0; ix<pcmCopyHandle->pcmInCount; ix++) {
    	struct pollfd * pfd = &pcmCopyHandle->pollFds[ix];
    	pfd->events = POLLIN | POLLHUP;
    }

    /* loop until end */
    for (;;) {

    	int err = poll(pcmCopyHandle->pollFds, pcmCopyHandle->pcmInCount, 5000); // TODO

    	if (err < 0)
    		continue;

    	if (err == 0) {
    		/* timeout */
    		continue;
    	}

    	for (int ix=0;ix<pcmCopyHandle->pcmInCount; ix++)
    		AlsaPcmReadCB(&pcmCopyHandle->pollFds[ix], pcmCopyHandle);
    	}

      pthread_exit(0);
}

static inline snd_pcm_uframes_t time_to_frames(unsigned int rate,
					       unsigned long long time)
{
	return (time * rate) / 1000000ULL;
}

PUBLIC int AlsaPcmCopy(SoftMixerT *mixer, AlsaStreamAudioT *stream, AlsaPcmCtlT *pcmIn, AlsaPcmCtlT *pcmOut, AlsaPcmHwInfoT * opts) {
    char string[32];
    struct pollfd *pcmInFds; 
    int error;
    
    // Fulup need to check https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m___direct.html
    
    AlsaDumpPcmInfo(mixer,"PcmIn",pcmIn->handle);
    AlsaDumpPcmInfo(mixer,"PcmOut",pcmOut->handle);

    AFB_ApiInfo(mixer->api, "%s: Configure CAPTURE PCM", __func__);

    // prepare PCM for capture and replay
    error = AlsaPcmConf(mixer, pcmIn, opts, SND_PCM_STREAM_CAPTURE);
    if (error) {
    	AFB_ApiError(mixer->api, "%s: PCM configuration for capture failed", __func__);
    	goto OnErrorExit;
    }
    
    AFB_ApiInfo(mixer->api, "%s: Configure PLAYBACK PCM", __func__);

    // input and output should match
    error = AlsaPcmConf(mixer, pcmOut, opts, SND_PCM_STREAM_PLAYBACK);
    if (error) {
    	AFB_ApiError(mixer->api, "%s: PCM configuration for playback failed", __func__);
    	goto OnErrorExit;
    }

    // Prepare PCM for usage
    if ((error = snd_pcm_prepare(pcmOut->handle)) < 0) {
        AFB_ApiError(mixer->api, "%s: Fail to prepare PLAYBACK PCM=%s error=%s", __func__, ALSA_PCM_UID(pcmOut->handle, string), snd_strerror(error));
        goto OnErrorExit;
    };

    // Prepare PCM for usage
    if ((error = snd_pcm_prepare(pcmIn->handle)) < 0) {
        AFB_ApiError(mixer->api, "%s: Fail to prepare CAPTURE PCM=%s error=%s", __func__, ALSA_PCM_UID(pcmOut->handle, string), snd_strerror(error));
        goto OnErrorExit;
    };

    // Start PCM
    if ((error = snd_pcm_start(pcmOut->handle)) < 0) {
        AFB_ApiError(mixer->api, "%s: Fail to start PLAYBACK PCM=%s error=%s", __func__, ALSA_PCM_UID(pcmIn->handle, string), snd_strerror(error));
        goto OnErrorExit;
    };

    // Start PCM
    if ((error = snd_pcm_start(pcmIn->handle)) < 0) {
        AFB_ApiError(mixer->api, "%s: Fail to start CAPTURE PCM=%s error=%s", __func__, ALSA_PCM_UID(pcmIn->handle, string), snd_strerror(error));
        goto OnErrorExit;
    };
    
    AlsaPcmCopyHandleT *cHandle= calloc(1, sizeof(AlsaPcmCopyHandleT));

    cHandle->info = "pcmCpy";
    cHandle->pcmIn = pcmIn->handle;
    cHandle->pcmOut = pcmOut->handle;
    cHandle->api = mixer->api;
    cHandle->channels = opts->channels;

    cHandle->latency_reqtime = 10000;
    cHandle->latency = time_to_frames(opts->rate, cHandle->latency_reqtime);
    AFB_ApiInfo(mixer->api, "%s: Latency = %ld", __func__, cHandle->latency);

	cHandle->frame_size = (snd_pcm_format_physical_width(opts->format) / 8) * opts->channels;

	AFB_ApiInfo(mixer->api, "%s: Frame size is %zu", __func__, cHandle->frame_size);

    size_t size = cHandle->latency * 2;
    cHandle->buf = calloc(1, size*cHandle->frame_size);
    cHandle->buf_count = 0;
    cHandle->buf_pos   = 0;
    cHandle->buf_size  = size;
    cHandle->read_err_count  = 0;
    cHandle->write_err_count = 0;
    
    AFB_ApiInfo(mixer->api, "%s Copy buf size is %zu", __func__, size);

    // get FD poll descriptor for capture PCM
    int pcmInCount = snd_pcm_poll_descriptors_count(cHandle->pcmIn);
    if (pcmInCount <= 0) {
        AFB_ApiError(mixer->api,
                     "%s: Fail pcmIn=%s get fds count error=%s",
                     __func__, ALSA_PCM_UID(pcmIn->handle, string), snd_strerror(error));
        goto OnErrorExit;
    };

    pcmInFds = malloc(sizeof (*pcmInFds) * pcmInCount);
    if ((error = snd_pcm_poll_descriptors(pcmIn->handle, pcmInFds, pcmInCount)) < 0) {
        AFB_ApiError(mixer->api,
                     "%s: Fail pcmIn=%s get pollfds error=%s",
                     __func__, ALSA_PCM_UID(pcmOut->handle, string), snd_strerror(error));
        goto OnErrorExit;
    };

    cHandle->pollFds = pcmInFds;
    cHandle->pcmInCount = pcmInCount;

    // start a thread with a mainloop to monitor Audio-Agent
    if ((error = pthread_create(&cHandle->thread, NULL, &LoopInThread, cHandle)) < 0) {
        AFB_ApiError(mixer->api,
                     "%s Fail create waiting thread pcmIn=%s err=%d",
                     __func__, ALSA_PCM_UID(pcmIn->handle, string), error);
        goto OnErrorExit;
    }
    
    // request a higher priority for each audio stream thread
    struct sched_param params;
    params.sched_priority = sched_get_priority_max(SCHED_FIFO);
    error= pthread_setschedparam(cHandle->thread, SCHED_FIFO, &params);
    if (error) {
        AFB_ApiWarning(mixer->api,
                       "%s: Failed to increase stream thread priority pcmIn=%s err=%s",
                       __func__, ALSA_PCM_UID(pcmIn->handle, string), strerror(error));
    }
    return 0;

OnErrorExit:
    AFB_ApiError(mixer->api, "%s: - pcmIn=%s" , __func__, ALSA_PCM_UID(pcmIn->handle, string));
    AFB_ApiError(mixer->api, "%s: - pcmOut=%s", __func__, ALSA_PCM_UID(pcmOut->handle, string));
    return -1;
}

