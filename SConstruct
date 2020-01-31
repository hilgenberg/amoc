#!python
import os
import fnmatch
import multiprocessing
env = Environment(tools=['default','gch'], toolpath='.')
Decider('MD5-timestamp')
Help("""
'scons' builds the debug version
'scons --release' the release version
""")

# use ncpu jobs
SetOption('num_jobs', multiprocessing.cpu_count())
#print("Using %d parallel jobs" % GetOption('num_jobs'))

# compile all .c and .cc files
src = []
for R,D,F in os.walk('.'):
	for f in fnmatch.filter(F, '*.c'): src.append(os.path.join(R, f))
	for f in fnmatch.filter(F, '*.cc'): src.append(os.path.join(R, f))

# less verbose output
env['GCHCOMSTR']  = "HH $SOURCE"
env['CXXCOMSTR']  = "CC $SOURCE"
env['CCCOMSTR']   = "C  $SOURCE"
env['LINKCOMSTR'] = "LL $TARGET"

# g++ flags
env.Append(CXXFLAGS='-std=c++17 -DUSE_PTHREADS'.split())
env.Append(CXXFLAGS=('-Wall -Wextra -Wno-sign-compare -Wno-deprecated-declarations ' +
	'-Wno-parentheses -Wno-misleading-indentation -Wno-reorder -fstrict-enums ' +
	'-Wno-variadic-macros -Wno-unused-parameter -Wno-unknown-pragmas ' +
	'-Wno-implicit-fallthrough -Wno-missing-field-initializers').split())

# precompiled header
env.Append(CXXFLAGS='-Winvalid-pch -include config.h'.split())
env.Append(CCFLAGS='-Winvalid-pch -include config.h'.split())
env['precompiled_header'] = File('config.h')
env['Gch'] = env.Gch(target='config.h.gch', source=env['precompiled_header'])
pch = env.Alias('pch', 'config.h.gch')
for s in src: env.Depends(s, pch)

# release/debug build
AddOption('--release', dest='release', action='store_true', default=False)
release = GetOption('release')
print(("Release" if release else "Debug")+" Build");
frel = '-O3 -s -DNDEBUG'
fdbg = '-Og -DDEBUG -D_DEBUG -g'
env.Append(CCFLAGS=Split(frel if release else fdbg))

# libs
libs = 'z m id3tag pthread faad avcodec avutil avformat speex modplug ogg vorbis vorbisfile popt ltdl sidplay2 sidutils resid-builder sndfile FLAC sidplay wavpack mad ncurses curl asound jack db rcc tag tag_c sndio magic mpcdec resample samplerate timidity'
env.Append(LIBS=libs.split());
env.ParseConfig('pkg-config --cflags --libs pangocairo')

# target
amoc = env.Program(target='amoc', source=src)
Default(amoc)

