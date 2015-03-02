/*
** Copyright (C) 2014 Alexander Regueiro <alex@noldorin.com>
** Copyright (C) 2013 elboulangero <elboulangero@gmail.com>
** Copyright (C) 2007-2012 Erik de Castro Lopo <erikd@mega-nerd.com>
** Copyright (C) 2007 Jonatan Liljedahl <lijon@kymatica.com>
**
** This program is free software ; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation ; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY ; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program ; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/


#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <pthread.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

#include <sndfile.h>

#define RB_SIZE		(1 << 16)
#define SAMPLE_SIZE (sizeof (jack_default_audio_sample_t))

#define	NOT(x)	(! (x))

typedef struct
{	jack_client_t *client ;
	jack_ringbuffer_t *ringbuf ;
	jack_nframes_t pos ;
	jack_default_audio_sample_t ** outs ;
	jack_port_t ** output_port ;

	SNDFILE *sndfile ;

	unsigned int channels ;
	unsigned int samplerate ;

	volatile int can_process ;
	volatile int read_done ;
	volatile int play_done ;

	volatile unsigned int loop_count ;
	volatile unsigned int current_loop ;
} thread_info_t ;

static pthread_mutex_t disk_thread_lock = PTHREAD_MUTEX_INITIALIZER ;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER ;

static int
process_callback (jack_nframes_t nframes, void * arg)
{
	thread_info_t *info = (thread_info_t *) arg ;
	jack_default_audio_sample_t buf [info->channels] ;
	unsigned i, n ;

	if (NOT (info->can_process))
		return 0 ;

	for (n = 0 ; n < info->channels ; n++)
		info->outs [n] = jack_port_get_buffer (info->output_port [n], nframes) ;

	for (i = 0 ; i < nframes ; i++)
	{	size_t read_count ;

		/* Read one frame of audio. */
		read_count = jack_ringbuffer_read (info->ringbuf, (void *) buf, SAMPLE_SIZE * info->channels) ;
		if (read_count == 0 && info->read_done)
		{	/* File is done, so stop the main loop. */
			info->play_done = 1 ;
			return 0 ;
			} ;

		/* Update play-position counter. */
		info->pos += read_count / (SAMPLE_SIZE * info->channels) ;

		/* Output each channel of the frame. */
		for (n = 0 ; n < info->channels ; n++)
			info->outs [n][i] = buf [n] ;
		} ;

	/* Wake up the disk thread to read more data. */
	if (pthread_mutex_trylock (&disk_thread_lock) == 0)
	{	pthread_cond_signal (&data_ready) ;
		pthread_mutex_unlock (&disk_thread_lock) ;
		} ;

	return 0 ;
} /* process_callback */

static void *
disk_thread (void *arg)
{	thread_info_t *info = (thread_info_t *) arg ;
	sf_count_t buf_avail, read_frames ;
	jack_ringbuffer_data_t vec [2] ;
	size_t bytes_per_frame = SAMPLE_SIZE * info->channels ;

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL) ;
	pthread_mutex_lock (&disk_thread_lock) ;

	while (1)
	{	jack_ringbuffer_get_write_vector (info->ringbuf, vec) ;

		read_frames = 0 ;

		if (vec [0].len)
		{	/* Fill the first part of the ringbuffer. */
			buf_avail = vec [0].len / bytes_per_frame ;
			read_frames = sf_readf_float (info->sndfile, (float *) vec [0].buf, buf_avail) ;
			if (vec [1].len)
			{	/* Fill the second part of the ringbuffer? */
				buf_avail = vec [1].len / bytes_per_frame ;
				read_frames += sf_readf_float (info->sndfile, (float *) vec [1].buf, buf_avail) ;
				} ;
			} ;

		if (read_frames == 0)
		{	info->current_loop ++ ;

			if (info->loop_count >= 1 && info->current_loop >= info->loop_count)
				break ; /* end of file? */

			sf_seek (info->sndfile, 0, SEEK_SET) ;
			}

		jack_ringbuffer_write_advance (info->ringbuf, read_frames * bytes_per_frame) ;

		/* Tell process_callback that we've filled the ringbuffer. */
		info->can_process = 1 ;

		/* Wait for the process_callback thread to wake us up. */
		pthread_cond_wait (&data_ready, &disk_thread_lock) ;
		} ;

	/* Tell that we're done reading the file. */
	info->read_done = 1 ;
	pthread_mutex_unlock (&disk_thread_lock) ;

	return NULL ;
} /* disk_thread */

static void
jack_shutdown (void *arg)
{	(void) arg ;
} /* jack_shutdown */

int
jackplay (const char *filename)
{	pthread_t thread_id ;
	SNDFILE *sndfile ;
	SF_INFO sfinfo ;
	jack_client_t *client ;
	jack_status_t status = 0 ;
	thread_info_t info ;
	char * auto_connect_str = "system:playback_%d" ;
	bool wait_before_play = false ;
	int i, jack_sr, loop_count = 1 ;

	/* Create jack client */
	if ((client = jack_client_open ("jackplay", JackNullOption | JackNoStartServer, &status)) == 0)
	{	if (status & JackServerFailed)
			fprintf (stderr, "Unable to connect to JACK server\n") ;
		else
			fprintf (stderr, "jack_client_open () failed, status = 0x%2.0x\n", status) ;

		exit (1) ;
		} ;

	if (status & JackServerStarted)
		fprintf (stderr, "JACK server started\n") ;

	if (status & JackNameNotUnique)
	{	const char * client_name = jack_get_client_name (client) ;
		fprintf (stderr, "Unique name `%s' assigned\n", client_name) ;
		} ;

	/* Open the soundfile. */
	memset (&sfinfo, 0, sizeof (sfinfo)) ;
	sndfile = sf_open (filename, SFM_READ, &sfinfo) ;
	if (sndfile == NULL)
	{	fprintf (stderr, "Could not open soundfile '%s'\n", filename) ;
		return 1 ;
		} ;

	//fprintf (stderr, "Channels    : %d\nSample rate : %d Hz\nDuration    : ", sfinfo.channels, sfinfo.samplerate) ;
	//print_time (loop_count * sfinfo.frames, sfinfo.samplerate) ;
	//fprintf (stderr, "\n") ;

	if (loop_count < 1)
		fprintf (stderr, "Loop count  : infinite\n") ;
	else if (loop_count > 1)
		fprintf (stderr, "Loop count  : %d\n", loop_count) ;

	jack_sr = jack_get_sample_rate (client) ;

	if (sfinfo.samplerate != jack_sr)
		fprintf (stderr, "Warning: samplerate of soundfile (%d Hz) does not match jack server (%d Hz).\n", sfinfo.samplerate, jack_sr) ;

	/* Init the thread info struct. */
	memset (&info, 0, sizeof (info)) ;
	info.can_process = 0 ;
	info.read_done = 0 ;
	info.play_done = 0 ;
	info.sndfile = sndfile ;
	info.channels = sfinfo.channels ;
	info.samplerate = jack_sr ;
	info.client = client ;
	info.pos = 0 ;

	info.current_loop = 0 ;
	info.loop_count = loop_count ;

	/* Allocate output ports. */
	info.output_port = calloc (sfinfo.channels, sizeof (jack_port_t *)) ;
	info.outs = calloc (sfinfo.channels, sizeof (jack_default_audio_sample_t *)) ;
	for (i = 0 ; i < sfinfo.channels ; i++)
	{	char name [16] ;

		snprintf (name, sizeof (name), "out_%d", i + 1) ;
		info.output_port [i] = jack_port_register (client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0) ;
		} ;

	/* Allocate and clear ringbuffer. */
	info.ringbuf = jack_ringbuffer_create (sizeof (jack_default_audio_sample_t) * RB_SIZE) ;
	memset (info.ringbuf->buf, 0, info.ringbuf->size) ;

	/* Set up callbacks. */
	jack_set_process_callback (client, process_callback, &info) ;
	jack_on_shutdown (client, jack_shutdown, 0) ;

	/* Activate client. */
	if (jack_activate (client))
	{	fprintf (stderr, "Cannot activate client.\n") ;
		return 1 ;
		} ;

	if (auto_connect_str != NULL)
	{	/* Auto-connect all channels. */
		for (i = 0 ; i < sfinfo.channels ; i++)
		{	char name [64] ;

			snprintf (name, sizeof (name), auto_connect_str, i + 1) ;

			if (jack_connect (client, jack_port_name (info.output_port [i]), name))
				fprintf (stderr, "Cannot connect output port %d (%s).\n", i, name) ;
			} ;
		}

	if (wait_before_play)
	{	/* Wait for key press before playing. */
		printf ("Press <ENTER> key to start playing...") ;
		getchar () ;
		}

	/* Start the disk thread. */
	pthread_create (&thread_id, NULL, disk_thread, &info) ;

	/* Sit in a loop, displaying the current play position. */
	while (NOT (info.play_done))
	{	//print_status (&info) ;
		usleep (10000) ;
		} ;

	pthread_join (thread_id, NULL) ;

	//print_status (&info) ;

	/* Clean up. */
	for (i = 0 ; i < sfinfo.channels ; i++)
		jack_port_unregister (client, info.output_port [i]) ;

	jack_ringbuffer_free (info.ringbuf) ;
	jack_client_close (client) ;

	free (info.output_port) ;
	free (info.outs) ;

	sf_close (sndfile) ;


	return 0 ;
} /* main */

