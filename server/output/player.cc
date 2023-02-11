/*
 * MOC - music on console
 * Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <pthread.h>
#include <deque>

#include "../input/decoder.h"
#include "../audio.h"
#include "../server.h"
#include "player.h"

#define PCM_BUF_SIZE		(36 * 1024)

enum Request
{
	REQ_NOTHING,
	REQ_SEEK,
	REQ_STOP
};
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t request_cond_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile Request request = REQ_NOTHING;
static volatile int req_seek = 0;

//-----------------------------------------------------------------------------
// BitrateList
//-----------------------------------------------------------------------------
// List of points where bitrate has changed. We use it to show bitrate at the
// right time when playing, because the output buffer may be big and decoding
// may be many seconds ahead of what the user can hear.
//-----------------------------------------------------------------------------

struct BitrateList
{
	struct Entry { int time, bitrate; };
	std::deque<Entry> entries;
	pthread_mutex_t mtx;

	BitrateList() { pthread_mutex_init (&mtx, NULL); }
	~BitrateList()
	{
		int rc = pthread_mutex_destroy (&mtx);
		if (rc != 0) log_errno ("Can't destroy bitrate list mutex", rc);
	}
	void swap(BitrateList &other)
	{
		LockGuard G1(mtx), G2(other.mtx);
		entries.swap(other.entries);
	}
	void clear() { LockGuard G(mtx); entries.clear(); }

	void add(int time, int bitrate)
	{
		LockGuard G(mtx);
		assert(entries.empty() || time >= entries.back().time);
		if (!entries.empty() && time == entries.back().time)
			entries.back().bitrate = bitrate;
		else
			entries.push_back({time, bitrate});
	}

	int get (int t)
	{
		LockGuard G(mtx);
		if (entries.empty() || entries.front().time > t) return -1;
		while (entries.size() > 1 && entries[1].time <= t) entries.pop_front();
		return entries.front().bitrate;
	}
};
static BitrateList bitrate_list;

//-----------------------------------------------------------------------------
// DecoderState
//-----------------------------------------------------------------------------

struct DecoderState
{
	DecoderState(const str &path)
	: buf(PCM_BUF_SIZE), buf_fill(0), path(path), time(0.0)
	, sp{ -1, -1, -1 }, sound_params_changed(false), tags_changed(false)
	, stream(NULL), codec(NULL), done(true)
	{
		if (is_url(path))
		{
			try {
				stream = new io_stream(path.c_str());
			}
			catch (std::exception &e)
			{
				error ("Could not open URL: %s", e.what());
				audio_fail_file (path);
				return;
			}
		}
		Decoder *f = stream ? get_decoder_by_content(*stream) : get_decoder(path);;
		if (!f)
		{
			delete stream; stream = NULL;
			error ("No decoder for %s", path.c_str());
			audio_fail_file (path);
			return;
		}
		codec = stream ? f->open(*stream) : f->open(path);
		if (!codec || codec->error.type == ERROR_FATAL)
		{
			delete stream; stream = NULL;
			if (codec)
				error ("Codec error for %s: %s", path.c_str(), codec->error.desc.c_str());
			else
				error ("No codec for %s", path.c_str());
			delete codec;
			audio_fail_file (path);
			return;
		}
		done = false;
	}
	~DecoderState()
	{
		delete codec;
		delete stream;
	}

	bool decode() // returns false if buffer was already full
	{
		const size_t N = buf.size();
		if (done || buf_fill >= N) return false;
		
		// decode next chunk
		sound_params sp0 = sp;
		int n = codec->decode(buf.data() + buf_fill, N - buf_fill, sp);
		buf_fill += n; assert(buf_fill <= N);
		
		// update bitrate and such
		if (sp != sp0) sound_params_changed = true;
		bitrate.add(time, codec->get_bitrate());
		time += n / (double)(sfmt_Bps(sp.fmt) * sp.rate * sp.channels);
		
		// if it fails without producing any sound, mark the file as broken
		if (!n && sp0.channels == -1) audio_fail_file(path);

		// check if we're done
		if (!n || codec->error.type == ERROR_FATAL) done = true;
		//if (!done && codec->current_tags(tags)) if (sp0.channels != -1) tags_changed = true;

		return true;
	}
	
	void flush()
	{
		audio_send_buf(buf.data(), buf_fill);
		buf_fill = 0;
	}


	Codec *codec;
	io_stream *stream;
	str path; // what is this decoding?

	std::vector<char> buf;
	size_t buf_fill;
	
	double time; // at the end of buf (used to update bitrate)
	sound_params sp; // from last decode call
	BitrateList bitrate;
	file_tags tags;
	bool tags_changed;
	bool sound_params_changed;

	bool done;
};

//-----------------------------------------------------------------------------
// Precache
//-----------------------------------------------------------------------------

static void *precache_thread (void *data); // below

struct Precache
{
	Precache() : decoder(NULL), running(false), tid(0) {}
	~Precache() { assert(!running); delete decoder; }

	DecoderState *decoder;
	bool running; /* if the precache thread is running */
	pthread_t tid; /* tid of the precache thread */
	str path;

	bool start(const str &p)
	{
		if (running) finish();
		path = p;
		logit ("Precaching %s", path.c_str());
		int rc = pthread_create (&tid, NULL, precache_thread, NULL);
		if (rc != 0)
			log_errno ("Could not run precache thread", rc);
		else
			running = true;
		return running;
	}
	void finish()
	{
		if (!running) return;
		debug ("Waiting for precache thread...");
		int rc = pthread_join(tid, NULL);
		if (rc != 0) fatal("pthread_join() for precache thread failed: %s", xstrerror(rc));
		running = false;
		debug ("done");
	}
	void drop()
	{
		finish();
		delete decoder;
		decoder = NULL;
	}
};
static Precache precache;

static void *precache_thread (void *)
{
	delete precache.decoder;
	precache.decoder = new DecoderState(precache.path);
	if (precache.decoder->done)
	{
		delete precache.decoder;
		precache.decoder = NULL;
		return NULL;
	}
	auto &decoder = *precache.decoder;
	assert(decoder.sp.channels == -1);

	while (!decoder.done)
	{
		bool first = (decoder.sp.channels == -1);
		bool ok = decoder.decode();
		if (!ok) break;

		// if sound_params change, we have no place to put that
		// information, so cancel (this thing can't be precached)
		if (decoder.sound_params_changed || decoder.tags_changed)
		{
			if (first)
			{
				decoder.sound_params_changed = false;
				decoder.tags_changed = false;
			}
			else
			{
				logit("Cannot precache");
				// we can pre-open the thing again though...
				delete precache.decoder;
				precache.decoder = new DecoderState(precache.path);
				break;
			}
		}
	}

	logit ("Precached %d bytes from %s", decoder.buf_fill, precache.path.c_str());
	return NULL;
}

//-----------------------------------------------------------------------------
// Player
//-----------------------------------------------------------------------------

static DecoderState *decoder = NULL;
static pthread_mutex_t decoder_stream_mtx = PTHREAD_MUTEX_INITIALIZER;

void player_init ()
{
}

/* Called when some free space in the output buffer appears. */
static void buf_free_cb ()
{
	LOCK (request_cond_mtx);
	pthread_cond_broadcast (&request_cond);
	UNLOCK (request_cond_mtx);

	static int last_time = 0;
	int ctime = audio_get_time ();
	if (ctime >= 0 && ctime != last_time) {
		last_time = ctime;
		ctime_change ();
		set_info_bitrate(decoder ? decoder->bitrate.get(ctime) : -1);
	}
}

/* Open a file, decode it and put output into the buffer. At the end, start
 * precaching next_file. */
void player (const char *file, const char *next_file, struct out_buf *out_buf)
{
	out_buf_reset (out_buf);

	DecoderState *d = NULL;
	precache.finish();
	if (precache.decoder)
	{
		if (precache.decoder->path == file)
		{
			logit ("Using precached file");
			d = precache.decoder;
			precache.decoder = NULL;

			auto &sp = d->sp;
			set_info_channels (sp.channels);
			set_info_rate (sp.rate / 1000);
			if (!audio_open(&sp))
			{
				delete d;
				return;
			}

			d->flush();
			set_info_avg_bitrate (d->codec ? d->codec->get_avg_bitrate() : -1);
		}
		else
		{
			logit ("The precached file is not the file we want.");
			precache.drop();
		}
	}
	
	if (!d) d = new DecoderState(file);
	if (d->done && !d->buf_fill) return;

	delete decoder; decoder = d;
	if (is_url(file)) next_file = NULL;
	
	audio_state_started_playing ();
	assert(decoder); if (!decoder) return;

	bool stopped = false;

	out_buf_set_free_callback (out_buf, buf_free_cb);

	while (true)
	{
		if (!decoder->done)
		{
			decoder->decode();

			decoder_error &err = decoder->codec->error;
			if (err) error ("%s", err.desc.c_str());
		}

		/* Wait, if there is no space in the buffer to put the decoded
		 * data or EOF occurred and there is something in the buffer. */
		if (decoder->buf_fill > out_buf_get_free(out_buf)
		|| (decoder->done && out_buf_get_fill(out_buf)))
		{
			if (next_file && !precache.running && !precache.decoder && 
			plist_item::ftype(next_file) == F_SOUND)
				precache.start(next_file);
			
			LOCK (request_cond_mtx);
			pthread_cond_wait (&request_cond, &request_cond_mtx);
			UNLOCK (request_cond_mtx);
		}

		if (request != REQ_NOTHING)
		{
			LOCK (request_cond_mtx);
			auto rq = request; request = REQ_NOTHING;
			int  sq = std::max(0, (int)req_seek);
			UNLOCK (request_cond_mtx);

			switch (rq)
			{
				case REQ_STOP:
					logit ("stop");
					stopped = true;
					out_buf_stop (out_buf);
					break;

				case REQ_SEEK:
				{
					logit ("seeking");
					int pos = decoder->codec->seek(sq);
					if (pos == -1)
					{
						logit ("error when seeking - checking for end of song");
						int m = decoder->codec->get_duration();
						if (m > 0 && m <= sq)
						{
							logit ("seeking to EOF");
							decoder->done = true;
							stopped = true;
							pos = m;
						}
						else logit ("true error when seeking");
					}

					if (pos != -1) {
						out_buf_stop (out_buf);
						out_buf_reset (out_buf);
						out_buf_time_set (out_buf, pos);
						decoder->bitrate.clear();
						decoder->time = pos;
						decoder->buf_fill = 0;
					}
					break;
				}

				default: break;
			}
		}
		if (stopped) break;

		if (decoder->buf_fill <= out_buf_get_free(out_buf) && !decoder->sound_params_changed)
		{
			decoder->flush();
		}
		
		if (decoder->sound_params_changed && out_buf_get_fill(out_buf) == 0)
		{
			decoder->sound_params_changed = false;
			auto &sp = decoder->sp;
			logit ("Sound parameters have changed.");
			set_info_channels (sp.channels);
			set_info_rate (sp.rate / 1000);
			out_buf_wait (out_buf);
			if (!audio_open(&sp)) break;
		}
		
		if (decoder->done && out_buf_get_fill(out_buf) == 0)
		{
			logit ("played everything");
			break;
		}
	}

	LOCK (decoder_stream_mtx);
	delete decoder; decoder = NULL;
	UNLOCK (decoder_stream_mtx);

	out_buf_wait (out_buf);

	logit ("exiting");
}

void player_cleanup ()
{
	int rc;

	rc = pthread_mutex_destroy (&request_cond_mtx);
	if (rc != 0) log_errno ("Can't destroy request mutex", rc);
	rc = pthread_mutex_destroy (&decoder_stream_mtx);
	if (rc != 0) log_errno ("Can't destroy decoder_stream mutex", rc);
	rc = pthread_cond_destroy (&request_cond);
	if (rc != 0) log_errno ("Can't destroy request condition", rc);

	precache.drop();
	delete decoder; decoder = NULL;
}

void player_reset ()
{
	request = REQ_NOTHING;
}

void player_stop ()
{
	logit ("requesting stop");
	LOCK (request_cond_mtx);
	request = REQ_STOP;
	pthread_cond_signal (&request_cond);
	UNLOCK (request_cond_mtx);
}

void player_seek (const int sec)
{
	int time = audio_get_time ();
	if (time < 0) return;
	
	LockGuard g(request_cond_mtx);
	request = REQ_SEEK;
	req_seek = sec + time;
	pthread_cond_signal (&request_cond);
}

void player_jump_to (const int sec)
{
	LockGuard g(request_cond_mtx);
	request = REQ_SEEK;
	req_seek = sec;
	pthread_cond_signal (&request_cond);
}
