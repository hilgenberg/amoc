#pragma once

enum Component { CLI, GUI, SERVER };

enum Layout { HSPLIT=0, VSPLIT=1, SINGLE=2 };
enum RepeatType { REPEAT_OFF=0, REPEAT_ALL=1, REPEAT_ONE=2 };

namespace options
{
	void load(Component who);
	void save(Component who);

	extern str Home, ConfigDir, MusicDir, RunDir, SocketPath;
	str config_file_path(const char *file); // return ConfigDir/file
	str run_file_path(const char *file); // return RunDir/file
	extern const char **rating_strings; // "     ", "*    ", ... "*****"

	extern bool StartInMusicDir;
	extern str  LastDir; // client's cwd when it was last shut down
	extern int  SeekTime;
	extern int  SilentSeekTime;
	extern int  MessageLingerTime;
	extern bool Softmixer_SaveState;
	extern bool SoftmixerActive, SoftmixerMono;
	extern int  SoftmixerValue;
	extern bool Equalizer_SaveState;
	extern bool EqualizerActive;
	extern str  EqualizerPreset;

	extern Ratio  hsplit, vsplit;
	extern Layout layout;
	extern str    TERM;

	extern bool ReadTags;
	extern bool PlaylistFullPaths;
	extern bool ShowHiddenFiles;
	extern bool HideFileExtension;
	extern bool ASCIILines, HideBorder;
	extern bool ShowMixer;
	extern str  RatingStar;
	extern str  RatingSpace;
	extern str  RatingFile;
	extern str  TimeBarLine;
	extern str  TimeBarSpace;

	extern int  InputBuffer, OutputBuffer;
	extern bool UseRealtimePriority;
	extern RepeatType Repeat;
	extern bool Shuffle;
	extern str  HTTPProxy;
	extern str  TiMidity_Config;
	extern bool UseMimeMagic;
	extern bool FileNamesIconv;
	enum class ResampleMethod_t : int { SincBestQuality, SincMediumQuality, SincFastest, ZeroOrderHold, Linear };
	extern ResampleMethod_t ResampleMethod;
	extern int  ForceSampleRate;
	extern bool Allow24bitOutput;
	
	enum class SoundDriver_t : int { AUTO = -1, SNDIO, JACK, ALSA, OSS, NOSOUND };
	extern SoundDriver_t SoundDriver;
	extern str  JackClientName;
	extern bool JackStartServer;
	extern str  JackOutLeft;
	extern str  JackOutRight;
	extern str  ALSADevice;
	extern str  ALSAMixer1;
	extern str  ALSAMixer2;
	extern bool ALSAStutterDefeat;
	extern str  OSSDevice;
	extern str  OSSMixerDevice;
	extern str  OSSMixerChannel1;
	extern str  OSSMixerChannel2;
}

