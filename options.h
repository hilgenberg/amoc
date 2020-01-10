#pragma once

namespace options
{
	void init(const char *config_file = NULL);


	extern bool shuffle;
	extern int PreBuffering, InputBuffer;

	extern const char **rating_strings; // "     ", "*    ", ... "*****"

	extern bool ReadTags;
	extern str  MusicDir;
	extern bool StartInMusicDir;
	extern bool Repeat;
	extern bool Shuffle;
	extern bool AutoNext;
	extern bool ASCIILines;
	//str  ("FormatString", "%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", CHECK_NONE);
	extern int InputBuffer;
	extern int OutputBuffer;
	extern int Prebuffering;
	extern str HTTPProxy;

	enum class SoundDriver_t : int { AUTO = -1, SNDIO, JACK, ALSA, OSS, NOSOUND };
	extern SoundDriver_t SoundDriver;

	extern str  JackClientName;
	extern bool JackStartServer;
	extern str  JackOutLeft ;
	extern str  JackOutRight;

	extern str OSSDevice;
	extern str OSSMixerDevice;
	extern str OSSMixerChannel1;
	extern str OSSMixerChannel2;

	extern str  ALSADevice;
	extern str  ALSAMixer1;
	extern str  ALSAMixer2;
	extern bool ALSAStutterDefeat;

	extern str  TiMidity_Config;

	extern bool Softmixer_SaveState;
	extern bool Equalizer_SaveState;
	extern bool ShowMixer;

	extern bool ShowHiddenFiles;
	extern bool HideFileExtension;

	extern str  MOCDir;
	extern bool UseMMap;
	extern bool UseMimeMagic;
	extern str  ID3v1TagsEncoding;
	extern bool UseRCC;
	extern bool UseRCCForFilesystem;
	extern bool EnforceTagsEncoding;
	extern bool FileNamesIconv;
	extern str  TERM;

	extern str  RatingStar;
	extern str  RatingSpace;
	extern str  RatingFile;

	extern str  TimeBarLine;
	extern str  TimeBarSpace;

	extern int  SeekTime;
	extern int  SilentSeekTime;

	enum class ResampleMethod_t : int { SincBestQuality, SincMediumQuality, SincFastest, ZeroOrderHold, Linear };
	extern ResampleMethod_t ResampleMethod;
	
	extern int  ForceSampleRate;
	extern bool Allow24bitOutput;
	extern bool UseRealtimePriority;

	extern bool PlaylistFullPaths;

	extern int  MessageLingerTime;
}

