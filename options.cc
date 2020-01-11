#include "options.h"
#include <pwd.h>
#include <regex.h>

static void parse(bool &o, const char *v)
{
	if      (!strcasecmp (v, "yes")) o = true;
	else if (!strcasecmp (v, "no")) o = false;
	else throw std::runtime_error(format("Invalid boolean: '%s'", v));
}
static void parse(int &o, const char *v)
{
	if (!is_int(v, o)) throw std::runtime_error(format("Invalid integer: '%s'", v));
}
static void parse(std::string &o, const char *v)
{
	o = v;
}
static void eparse(int &o, const char *v, ...)
{
	va_list ap; va_start(ap, v);
	for (int i = 0; true; ++i)
	{
		const char *w = va_arg(ap, const char *);
		if (!w) break;
		if (!strcasecmp (v, w))
		{
			va_end(ap);
			o = i;
			return;
		}
	}
	va_end(ap);
	throw std::runtime_error(format("Invalid value: '%s'", v));
}

static void build_rating_strings(const char *s0 = " ", const char *s1 = "*")
{
	const char** &S = options::rating_strings;
	const size_t l0 = strlen (s0), l1 = strlen (s1);
	free(S); S = (const char**)malloc(6 * sizeof(char*) + (l0+l1)*15 + 6);
	char *s = (char*)(S+6);
	for (int i = 0; i <= 5; ++i)
	{
		int j = 0; S[i] = s;
		for (; j < i; ++j) { memcpy (s, s1, l1); s += l1; }
		for (; j < 5; ++j) { memcpy (s, s0, l0); s += l0; }
		*s++ = 0;
	}
}

namespace options {

str config_file_path(const char *file) { return add_path(ConfigDir, file); }
str run_file_path(const char *file) { return add_path(RunDir, file); }

void init()
{
	const char *home = getenv ("HOME");
	if (!home) {
		errno = 0;
		struct passwd *passwd = getpwuid (geteuid ());
		if (passwd)
			home = passwd->pw_dir;
		else if (errno != 0) {
			char *err = xstrerror (errno);
			logit ("getpwuid(%d): %s", geteuid (), err);
			free (err);
		}
	}
	if (!home) fatal("Can't get home directory!");
	Home = home; normalize_path(Home);

	str cfg1 = add_path(Home, ".config/amoc"), cfg2 = add_path(Home, ".amoc");
	ConfigDir = is_dir(cfg1) ? cfg1 : is_dir(cfg2) ? cfg2 : 
		is_dir(add_path(Home, ".config")) ? cfg1 : cfg2;

	str config_file = add_path(ConfigDir, "config");
	FILE *file = fopen(config_file.c_str(), "r");
	if (!file)
	{
		char *err = STRERROR_FN (errno);
		logit ("Can't open config file \"%s\": %s", config_file, err);
		free (err);
	}

	std::string RatingSpace(" "), RatingStar("*");
	RunDir = ConfigDir;

	if (file)
	{
		std::map<std::string, std::string> items;

		int line = 0;
		while (true)
		{
			int c = fgetc(file); if (c == EOF) break;
			++line;
			
			// skip initial whitespace and comment lines
			while (isspace(c)) c = fgetc(file); if (c == EOF) break;
			if (c == '#')
			{
				do c = fgetc(file); while (c != EOF && c != '\n');
				continue;
			}

			// read this option's key
			std::string key, value;
			while (!isspace(c) && c != EOF && c != '=') { key += c; c = fgetc(file); }

			// skip space, equal sign, space
			while (isspace(c)) c = fgetc(file); if (c == EOF) break;
			if (c != '=')
			{
				fclose(file);
				fatal("Syntax error on line %d", line);
				return;
			}
			c = fgetc(file);
			while (isspace(c) && c != EOF) c = fgetc(file);

			// read this option's value
			if (c == '\'' || c == '"')
			{
				int q = c; c = fgetc(file);
				while (c != EOF && c != '\n' && c != q)
				{
					value += c;
					c = fgetc(file);
				}
				if (c != q)
				{
					value.insert(0, 1, (char)q);
				}
				else
				{
					while (c != '\n' && c != EOF) c = fgetc(file);
				}
			}
			else
			{
				while (c != '#' && c != '\n' && c != EOF) { value += c; c = fgetc(file); }
				while (!value.empty() && isspace(value.back())) value.pop_back();
				if (c == '#') while (c != '\n' && c != EOF) c = fgetc(file);
			}

			// finally add it to the map, replacing any prior value
			items[key] = value;
		}
		fclose (file);

		#define OPT(x) if (items.count(#x)) ::parse(x, items[#x].c_str())
		#define EOPT(x,...) if (items.count(#x)) ::eparse((int&)x, items[#x].c_str(), __VA_ARGS__, NULL)

		OPT(RatingSpace);
		OPT(RatingStar);

		OPT(ReadTags);
		OPT(MusicDir);
		OPT(StartInMusicDir);

		OPT(Repeat);
		OPT(Shuffle);
		OPT(AutoNext);
		OPT(ASCIILines);
		//str  ("FormatString", "%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", CHECK_NONE);
		OPT(InputBuffer);
		OPT(OutputBuffer);
		OPT(Prebuffering);
		OPT(HTTPProxy);

		EOPT(SoundDriver, "SNDIO", "JACK", "ALSA", "OSS", "NULL");
		OPT(JackClientName);
		OPT(JackStartServer);
		OPT(JackOutLeft);
		OPT(JackOutRight);
		OPT(OSSDevice);
		OPT(OSSMixerDevice);
		OPT(OSSMixerChannel1);
		OPT(OSSMixerChannel2);
		OPT(ALSADevice);
		OPT(ALSAMixer1);
		OPT(ALSAMixer2);
		OPT(ALSAStutterDefeat);

		OPT(Softmixer_SaveState);
		OPT(Equalizer_SaveState);
		OPT(ShowMixer);

		OPT(ShowHiddenFiles);
		OPT(HideFileExtension);

		OPT(RunDir);
		OPT(UseMMap);
		OPT(UseMimeMagic);
		OPT(ID3v1TagsEncoding);
		OPT(UseRCC);
		OPT(UseRCCForFilesystem);
		OPT(EnforceTagsEncoding);
		OPT(FileNamesIconv);
		OPT(TiMidity_Config);
		OPT(TERM);

		OPT(RatingFile);

		OPT(TimeBarLine);
		OPT(TimeBarSpace);

		OPT(SeekTime);
		OPT(SilentSeekTime);

		EOPT(ResampleMethod, "SincBestQuality", "SincMediumQuality", "SincFastest", "ZeroOrderHold", "Linear");

		OPT(ForceSampleRate);
		OPT(Allow24bitOutput);
		OPT(UseRealtimePriority);

		OPT(PlaylistFullPaths);

		OPT(MessageLingerTime);
	}

	build_rating_strings(RatingSpace.c_str(), RatingStar.c_str());
	if (Prebuffering > InputBuffer) InputBuffer = Prebuffering;

	if (RunDir.empty()) RunDir = ConfigDir;
	normalize_path(RunDir);
	if (!MusicDir.empty()) normalize_path(MusicDir);
	SocketPath = add_path(RunDir, "socket");

	#define UNIX_PATH_MAX	108
	if (SocketPath.length() > UNIX_PATH_MAX) fatal ("Can't create socket name!");
}

str Home, ConfigDir, MusicDir, RunDir, SocketPath;

const char **rating_strings = NULL;

bool ReadTags = true;
bool StartInMusicDir = false;
bool Repeat = false;
bool Shuffle = false;
bool AutoNext = true;
bool ASCIILines = false;
//str  ("FormatString", "%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", CHECK_NONE);
int InputBuffer = 512;
int OutputBuffer = 512;
int Prebuffering = 64;
str HTTPProxy = "";

SoundDriver_t SoundDriver = SoundDriver_t::AUTO;

str  JackClientName = "moc";
bool JackStartServer = false;
str  JackOutLeft  = "system:playback_1";
str  JackOutRight = "system:playback_2";

str OSSDevice = "/dev/dsp";
str OSSMixerDevice ="/dev/mixer";
str OSSMixerChannel1 = "pcm";
str OSSMixerChannel2 = "master";

str  ALSADevice = "default";
str  ALSAMixer1 = "PCM";
str  ALSAMixer2 = "Master";
bool ALSAStutterDefeat = false;

bool Softmixer_SaveState = true;
bool Equalizer_SaveState = true;
bool ShowMixer = true;

bool ShowHiddenFiles = false;
bool HideFileExtension = false;

bool UseMMap = false;
bool UseMimeMagic = false;
str  ID3v1TagsEncoding = "WINDOWS-1250";
bool UseRCC = true;
bool UseRCCForFilesystem = false;
bool EnforceTagsEncoding = false;
bool FileNamesIconv = false;
str  TiMidity_Config = "no";
str  TERM = "";

str  RatingFile = "ratings";

str  TimeBarLine =  "";
str  TimeBarSpace = "";

int  SeekTime = 1;
int  SilentSeekTime = 5;

ResampleMethod_t ResampleMethod = ResampleMethod_t::Linear;

int  ForceSampleRate = 0;
bool Allow24bitOutput = false;
bool UseRealtimePriority = false;

bool PlaylistFullPaths = true;

int  MessageLingerTime = 2;

} // end of namespace
