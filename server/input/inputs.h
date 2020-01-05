#include "aac.h"
#include "ffmpeg.h"
#include "flac.h"
#include "modplug.h"
#include "mp3.h"
#include "musepack.h"
#include "sidplay2.h"
#include "sndfile.h"
#include "speex.h"
#include "vorbis.h"
#include "wavpack.h"

#define ALL_INPUTS \
	H(aac);\
	H(ffmpeg);\
	H(flac);\
	H(mod);\
	H(mp3);\
	H(muse);\
	H(sid);\
	H(sndfile);\
	H(speex);\
	H(vorbis);\
	H(wav);

