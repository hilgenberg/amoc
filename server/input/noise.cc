#include "decoder.h"
#include <random>
static std::mt19937 seed_rng;

struct noise_data : public Codec
{
	std::mt19937 rng;
	std::uniform_real_distribution<float> udist;
	float x[2], T[2];
	enum Type { White, Brown, Rain, Test };
	Type type;
	
	noise_data(const str &file)
	: rng(seed_rng())
	, udist(-1.0f, 1.0f)
	, x{0.0f}, T{0.0f}
	{
		str t = file_name(file);
		if      (t == "white.noise") type = White;
		else if (t == "brown.noise") type = Brown;
		else if (t == "rain.noise")  type = Rain;
		else if (t == "test.noise")  type = Test;
		else error.fatal("Unknown noise type %s", t.c_str());
	}

	int decode (char *buf_, int N_, sound_params &sp)
	{
		sp.channels = 2;
		sp.rate = 44100;
		sp.fmt = SFMT_FLOAT;

		int N = N_ / sizeof(float);// * sp.channels
		if (N > 8*1024) N = 8*1024;
		float *buf = (float*)buf_;
		switch (type)
		{
			case White:
				for (int i = 0; i < N; ++i) *buf++ = 0.9f*udist(rng);
				break;
			case Brown:
				for (int i = 0; i < N/2; ++i)
				for (int c = 0; c < 2; ++c)
				{
					float dx = udist(rng);
					// next line adds a bias to return to 0 and prevents
					// it from going outside of [-1,1]
					dx *= 1.0f + (dx < 0.0f ? x[c] : -x[c]);
					x[c] += dx*0.15f;
					// alternative 1 (leads to pauses):
					//x[c] = CLAMP(-1.0f, x[c], 1.0f);
					// alternative 2 (leads to pops, but left in for paranoia):
					if (x[c] < -1.0f || x[c] > 1.0f) x[c] = 0.0f;

					*buf++ = x[c];
				}
				break;
			case Rain:
				for (int i = 0; i < N/2; ++i)
				{
					float x0[2] = {x[0], x[1]};
					if (fabsf(x0[0]) < 0.1) x0[0] = 0.1;
					if (fabsf(x0[1]) < 0.1) x0[1] = 0.1;
					for (int c = 0; c < 2; ++c)
					{
						float dx = udist(rng)*x0[1-c];
						dx *= 1.0f + (dx < 0.0f ? x[c] : -x[c]);
						x[c] += dx*0.4f;
						if (x[c] < -1.0f || x[c] > 1.0f) x[c] = 0.0f;

						*buf++ = x[c];
					}
				}
				break;
			case Test:
				for (int i = 0; i < N/2; ++i)
				{
					float x0[2] = {x[0], x[1]};
					if (fabsf(x0[0]) < 0.1) x0[0] = 0.1;
					if (fabsf(x0[1]) < 0.1) x0[1] = 0.1;
					for (int c = 0; c < 2; ++c)
					{
						float dx = udist(rng)*x0[1-c];
						dx *= 1.0f + (dx < 0.0f ? x[c] : -x[c]);
						x[c] += dx*0.2f;
						if (x[c] < -1.0f || x[c] > 1.0f) x[c] = 0.0f;

						T[c] += (x[c]+1.0f)*0.1f;
						*buf++ = sinf(T[c]);
					}
				}
				break;
		}
		return N*sizeof(float);
	}
	int get_duration () const override { return -1; }
	int get_avg_bitrate() const override { return 0; }
	int get_bitrate() const override { return 0; }
};

struct noise_decoder : public Decoder
{
	Codec* open(const str &f) { return new noise_data(f); }

	bool matches_ext(const char *ext) const override
	{
		return !strcmp(ext, "noise");
	}

	int get_duration(const str &file_name) override
	{
		return -1;
	}

	void read_tags(const str &file, file_tags &tags) override
	{
	}

	bool write_tags(const str &file, const tag_changes &tags) override
	{
		return false;
	}

};

Decoder *noise_plugin ()
{
	return new noise_decoder;
}
