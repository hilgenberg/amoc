/* Jack plugin for moc by Alex Norman <alex@neisis.net> 2005
 * moc by Copyright (C) 2004 Damian Pietras <daper@daper.net>
 * use at your own risk
 */

#include <jack/jack.h>
#include <jack/types.h>
#include <jack/ringbuffer.h>
#include <math.h>

#include "../audio.h"

#define RINGBUF_SZ 32768

static int process_cb(jack_nframes_t nframes, void *driver);
static int update_sample_rate_cb(jack_nframes_t new_rate, void *driver);
static void error_cb (const char *msg) { error ("JACK: %s", msg); }
static void shutdown_cb (void *driver);

struct jack_driver : public AudioDriver
{
	jack_client_t *client;
	jack_port_t* output_port[2];
	jack_ringbuffer_t *ringbuffer[2]; /* the ring buffer, used to store the sound data before jack takes it */
	jack_default_audio_sample_t volume;
	int volume_integer; /* volume as an integer - needed to avoid cast errors on set/read */
	bool playing; /* indicates if we should be playing or not */
	int rate; /* current sample rate */
	volatile bool our_xrun; /* flag set if xrun occurred that was our fault (the ringbuffer doesn't contain enough data in the process callback) */
	volatile bool jack_shutdown; /* set to 1 if jack client thread exits */

	jack_driver(output_driver_caps &caps)
	: client(NULL)
	, volume(1.0)
	, volume_integer(100)
	, playing(false)
	, our_xrun(false)
	, jack_shutdown(false)
	{
		const char *client_name = options::JackClientName.c_str();
		jack_set_error_function (error_cb);

		/* open a client connection to the JACK server */
		jack_options_t options = JackNullOption;
		if (!options::JackStartServer)
			options = (jack_options_t)(options | JackNoStartServer);
		jack_status_t status;
		client = jack_client_open (client_name, options, &status, NULL);
		if (!client) {
			error ("jack_client_open() failed, status = 0x%2.0x", status);
			if (status & JackServerFailed)
				error ("Unable to connect to JACK server");
			throw std::runtime_error("Jack: init failed");
		}

		if (status & JackServerStarted)
			printf ("JACK server started\n");

		jack_on_shutdown (client, ::shutdown_cb, this);

		/* allocate memory for an array of 2 output ports */
		output_port[0] = jack_port_register (client, "output0", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		output_port[1] = jack_port_register (client, "output1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

		/* create the ring buffers */
		ringbuffer[0] = jack_ringbuffer_create(RINGBUF_SZ);
		ringbuffer[1] = jack_ringbuffer_create(RINGBUF_SZ);

		/* set the call back functions, activate the client */
		jack_set_process_callback (client, ::process_cb, this);
		jack_set_sample_rate_callback(client, ::update_sample_rate_cb, this);
		if (jack_activate (client)) {
			error ("cannot activate client");
			throw std::runtime_error("cannot activate client");
		}

		/* connect ports
		* a value of NULL in JackOut* gives no connection
		* */
		if (options::JackOutLeft != "NULL"){
			if(jack_connect(client,"moc:output0", options::JackOutLeft.c_str()))
				fprintf(stderr,"%s is not a valid Jack Client / Port", options::JackOutLeft.c_str());
		}
		if(options::JackOutRight != "NULL"){
			if(jack_connect(client,"moc:output1", options::JackOutRight.c_str()))
				fprintf(stderr,"%s is not a valid Jack Client / Port", options::JackOutRight.c_str());
		}

		caps.formats = SFMT_FLOAT;
		rate = jack_get_sample_rate (client);
		caps.max_channels = caps.min_channels = 2;
	}

	~jack_driver()
	{
		jack_port_unregister(client,output_port[0]);
		jack_port_unregister(client,output_port[1]);
		jack_client_close(client);
		jack_ringbuffer_free(ringbuffer[0]);
		jack_ringbuffer_free(ringbuffer[1]);
	}

	bool open (const sound_params &sound_params) override
	{
		if (sound_params.fmt != SFMT_FLOAT) {
			char fmt_name[SFMT_STR_MAX];

			error ("Unsupported sound format: %s.",
					sfmt_str(sound_params.fmt, fmt_name, sizeof(fmt_name)));
			return false;
		}
		if (sound_params.channels != 2) {
			error ("Unsupported number of channels");
			return false;
		}

		logit ("jack open");
		playing = true;

		return 1;
	}

	void close () override
	{
		logit ("jack close");
		playing = false;
	}

	int play (const char *buff, size_t size) override
	{
		size_t remain = size;
		size_t pos = 0;

		if (jack_shutdown) {
			logit ("Refusing to play, because there is no client thread.");
			return -1;
		}

		debug ("Playing %zu bytes", size);

		if (our_xrun) {
			logit ("xrun");
			our_xrun = false;
		}

		while (remain && !jack_shutdown) {
			size_t space;

			/* check if some space is available only in the second
			* ringbuffer, because it is read later than the first. */
			if ((space = jack_ringbuffer_write_space(ringbuffer[1]))
					> sizeof(jack_default_audio_sample_t)) {
				size_t to_write;

				space *= 2; /* we have 2 channels */
				debug ("Space in the ringbuffer: %zu bytes", space);

				to_write = MIN (space, remain);

				to_write /= sizeof(jack_default_audio_sample_t) * 2;
				remain -= to_write * sizeof(float) * 2;
				while (to_write--) {
					jack_default_audio_sample_t sample;

					sample = *(jack_default_audio_sample_t *)
						(buff + pos) * volume;
					pos += sizeof (jack_default_audio_sample_t);
					jack_ringbuffer_write (ringbuffer[0],
							(char *)&sample,
							sizeof(sample));

					sample = *(jack_default_audio_sample_t *)
						(buff + pos) * volume;
					pos += sizeof (jack_default_audio_sample_t);
					jack_ringbuffer_write (ringbuffer[1],
							(char *)&sample,
							sizeof(sample));
				}
			}
			else {
				debug ("Sleeping for %uus", (unsigned int)(RINGBUF_SZ
						/ (float)(audio_get_bps()) * 100000.0));
				xsleep (RINGBUF_SZ, audio_get_bps ());
			}
		}

		if (jack_shutdown) return -1;

		return size;
	}

	int read_mixer () const override
	{
		return volume_integer;
	}

	void set_mixer (int vol) override
	{
		volume_integer = vol;
		volume = (jack_default_audio_sample_t)((exp((double)vol / 100.0) - 1)
				/ (M_E - 1));
	}

	int get_buff_fill () const override
	{
		/* FIXME: should we also use jack_port_get_latency() here? */
		return sizeof(float) * (jack_ringbuffer_read_space(ringbuffer[0])
				+ jack_ringbuffer_read_space(ringbuffer[1]))
			/ sizeof(jack_default_audio_sample_t);
	}

	bool reset () override
	{
		//jack_ringbuffer_reset(ringbuffer); /*this is not threadsafe!*/
		return true;
	}

	int get_rate () const override
	{
		return rate;
	}

	str get_mixer_channel_name () const override
	{
		return "soft mixer";
	}

	void toggle_mixer_channel () override
	{
	}

	int process_cb(jack_nframes_t nframes)
	{
		jack_default_audio_sample_t *out[2];

		if (nframes <= 0) return 0;

		/* get the jack output ports */
		out[0] = (jack_default_audio_sample_t *) jack_port_get_buffer (output_port[0], nframes);
		out[1] = (jack_default_audio_sample_t *) jack_port_get_buffer (output_port[1], nframes);

		if (playing) {

			/* ringbuffer[1] is filled later, so we only need to check
			* it's space. */
			size_t avail_data = jack_ringbuffer_read_space(ringbuffer[1]);
			size_t avail_frames = avail_data
				/ sizeof(jack_default_audio_sample_t);

			if (avail_frames > nframes) {
				avail_frames = nframes;
				avail_data = nframes
					* sizeof(jack_default_audio_sample_t);
			}

			jack_ringbuffer_read (ringbuffer[0], (char *)out[0],
					avail_data);
			jack_ringbuffer_read (ringbuffer[1], (char *)out[1],
					avail_data);


			/* we must provide nframes data, so fill with silence
			* the remaining space. */
			if (avail_frames < nframes) {
				our_xrun = 1;

				for (size_t i = avail_frames; i < nframes; i++)
					out[0][i] = out[1][i] = 0.0;
			}
		}
		else {
			/* consume the input */
			size_t size = jack_ringbuffer_read_space(ringbuffer[1]);
			jack_ringbuffer_read_advance (ringbuffer[0], size);
			jack_ringbuffer_read_advance (ringbuffer[1], size);

			for (size_t i = 0; i < nframes; i++) {
				out[0][i] = 0.0;
				out[1][i] = 0.0;
			}
		}

		return 0;
	}
	void shutdown_cb ()
	{
		jack_shutdown = true;
	}
	int update_sample_rate_cb(jack_nframes_t new_rate)
	{
		rate = new_rate;
		return 0;
	}
};

/* this is the function that jack calls to get audio samples from us */
static int process_cb(jack_nframes_t nframes, void *data)
{
	return ((jack_driver*)data)->process_cb(nframes);
}
static void shutdown_cb (void *data)
{
	((jack_driver*)data)->shutdown_cb();
}
/* this is called if jack changes its sample rate */
static int update_sample_rate_cb(jack_nframes_t new_rate, void *data)
{
	return ((jack_driver*)data)->update_sample_rate_cb(new_rate);
}

AudioDriver *JACK_init(output_driver_caps &caps)
{
	try
	{
		return new jack_driver(caps);
	}
	catch(...) {}
	
	return NULL;
}
