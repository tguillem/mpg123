/*
	out123: stream data from libmpg123 or libsyn123 to an audio output device

	copyright 1995-2019 by the mpg123 project,
	free software under the terms of the LGPL 2.1
	see COPYING and AUTHORS files in distribution or http://mpg123.org

	initially written by Thomas Orgis (extracted from mpg123.c)

	This is a stripped down mpg123 that only uses libout123 to write standard
	input to an audio device. Of course, it got some enhancements with the
	advent of libsyn123.

	Please bear in mind that the code started out as a nasty hack on a very
	old piece made out of nasty hacks and plain ugly code. Some nastiness
	(like lax parameter checking) even serves a purpose: Test the robustness
	of our libraries in catching bad caller behaviour.

	TODO: Add basic parsing of WAV headers to be able to pipe in WAV files, especially
	from something like mpg123 -w -.

	TODO: Add option for phase shift between channels (delaying the second one).
	This might be useful with generated signals, to locate left/right speakers
	or just generally enhance the experience ... compensating for speaker locations.
	This also means the option of mixing, channel attentuation. This is not
	too hard to implement and might be useful for debugging outputs.
*/

#define ME "out123"
#include "config.h"
#include "compat.h"
#include <ctype.h>
#if WIN32
#include "win32_support.h"
#endif
#include "out123.h"

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <errno.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#include "sysutil.h"
#include "getlopt.h"

#include "syn123.h"

#include "debug.h"

/* be paranoid about setpriority support */
#ifndef PRIO_PROCESS
#undef HAVE_SETPRIORITY
#endif

static int intflag = FALSE;

static void usage(int err);
static void want_usage(char* arg);
static void long_usage(int err);
static void want_long_usage(char* arg);
static void print_title(FILE* o);
static void give_version(char* arg);

static int verbose = 0;
static int quiet = FALSE;

static FILE* input = NULL;
static char *encoding_name = NULL;
static int  encoding = MPG123_ENC_SIGNED_16;
static char *inputenc_name = NULL;
static int  inputenc = 0;
static int  mixenc = -1;
static int  channels = 2;
static int  inputch  = 0;
static long rate     = 44100;
static long inputrate = 0;
static char *driver = NULL;
static char *device = NULL;
int also_stdout = FALSE;
size_t buffer_kb = 0;
static int realtime = FALSE;
#ifdef HAVE_WINDOWS_H
static int w32_priority = 0;
#endif
static int aggressive = FALSE;
static double preload = 0.2;
static long outflags = 0;
double preamp = 0.;
double preamp_factor = 1.;
double preamp_offset = 0.;
static const char *name = NULL; /* Let the out123 library choose "out123". */
static double device_buffer; /* output device buffer */
long timelimit_samples = -1;
double timelimit_seconds = -1.;
off_t offset = 0;
off_t timelimit = -1;
const char *clip_mode = "implicit";
int soft_clip = FALSE;
int do_clip = FALSE;
int do_preamp = FALSE;
int do_resample = FALSE;
int dither = 0;
double clip_limit = 1.;
double clip_width = 0.0234;

char *wave_patterns = NULL;
char *wave_freqs    = NULL;
char *wave_phases   = NULL;
char *wave_direction = NULL;
double sweep_freq = 0.;
double sweep_time = 0.;
int sweep_hard = 0;
long sweep_count = -1;
const char *sweep_type = "quad";
const char *signal_source = "file";
/* Default to around 2 MiB memory for the table. */
long wave_limit     = 300000;
int pink_rows = 0;
double geiger_activity = 17;
char *resampler = "fine";

size_t pcmblock = 1152; /* samples (pcm frames) we treat en bloc */
size_t resample_block = 0; /* resample that many samples in one go */
/* To be set after settling format. */
size_t pcmframe = 0; // Bytes for one PCM frame of output format.
size_t pcminframe = 0; // ... of input format
size_t mixframe = 0;   // ... of mixing format
unsigned char *audio = NULL; // in/output buffer
unsigned char *inaudio = NULL; // separate input buffer
unsigned char *mixaudio = NULL; // separate mixing buffer
float *resaudio = NULL; // separate resampling buffer, always 32 bit float
char *mixmat_string = NULL;
double *mixmat = NULL;

// Option to play some oscillatory test signals.
// Also used for conversions.
syn123_handle *waver = NULL;
int generate = FALSE; // Wheter to use the syn123 generator.

out123_handle *ao = NULL;
char *cmd_name = NULL;
/* ThOr: pointers are not TRUE or FALSE */
char *equalfile = NULL;
int fresh = TRUE;

int OutputDescriptor;

char *fullprogname = NULL; /* Copy of argv[0]. */
char *binpath; /* Path to myself. */

/* File-global storage of command line arguments.
   They may be needed for cleanup after charset conversion. */
static char **argv = NULL;
static int    argc = 0;

/* Drain output device/buffer, but still give the option to interrupt things. */
static void controlled_drain(void)
{
	int framesize;
	long rate;
	size_t drain_block;

	if(intflag || !out123_buffered(ao))
		return;
	if(out123_getformat(ao, &rate, NULL, NULL, &framesize))
		return;
	drain_block = 1024*framesize;
	if(!quiet)
		fprintf( stderr
		,	"\n"ME": draining buffer of %.1f s (you may interrupt)\n"
		,	(double)out123_buffered(ao)/framesize/rate );
	do {
		out123_ndrain(ao, drain_block);
	} while(!intflag && out123_buffered(ao));
}

static void safe_exit(int code)
{
	char *dummy, *dammy;

	if(!code)
		controlled_drain();
	if(intflag || code)
		out123_drop(ao);
	out123_del(ao);
#ifdef WANT_WIN32_UNICODE
	win32_cmdline_free(argc, argv); /* This handles the premature argv == NULL, too. */
#endif
	/* It's ugly... but let's just fix this still-reachable memory chunk of static char*. */
	split_dir_file("", &dummy, &dammy);
	if(fullprogname) free(fullprogname);
	if(mixmat) free(mixmat);
	if(inaudio && inaudio != audio) free(inaudio);
	if(audio) free(audio);
	if(mixaudio) free(mixaudio);
	if(resaudio) free(resaudio);
	if(waver) syn123_del(waver);
	exit(code);
}

static void check_fatal_output(int code)
{
	if(code)
	{
		if(!quiet)
			error2( "out123 error %i: %s"
			,	out123_errcode(ao), out123_strerror(ao) );
		safe_exit(133);
	}
}

static void check_fatal_syn(int code)
{
	if(code)
	{
		if(!quiet)
			merror("syn123 error %i: %s", code, syn123_strerror(code));
		safe_exit(132);
	}
}

static void set_output_module( char *arg )
{
	unsigned int i;
		
	/* Search for a colon and set the device if found */
	for(i=0; i< strlen( arg ); i++) {
		if (arg[i] == ':') {
			arg[i] = 0;
			device = &arg[i+1];
			debug1("Setting output device: %s", device);
			break;
		}	
	}
	/* Set the output module */
	driver = arg;
	debug1("Setting output module: %s", driver );
}

static void set_output_flag(int flag)
{
  if(outflags <= 0) outflags = flag;
  else outflags |= flag;
}

static void set_output_h(char *a)
{
	set_output_flag(OUT123_HEADPHONES);
}

static void set_output_s(char *a)
{
	set_output_flag(OUT123_INTERNAL_SPEAKER);
}

static void set_output_l(char *a)
{
	set_output_flag(OUT123_LINE_OUT);
}

static void set_output(char *arg)
{
	/* If single letter, it's the legacy output switch for AIX/HP/Sun.
	   If longer, it's module[:device] . If zero length, it's rubbish. */
	if(strlen(arg) <= 1) switch(arg[0])
	{
		case 'h': set_output_h(arg); break;
		case 's': set_output_s(arg); break;
		case 'l': set_output_l(arg); break;
		default:
			error1("\"%s\" is no valid output", arg);
			safe_exit(1);
	}
	else set_output_module(arg);
}

static void set_verbose (char *arg)
{
    verbose++;
}

static void set_quiet (char *arg)
{
	verbose=0;
	quiet=TRUE;
}

static void set_out_wav(char *arg)
{
	driver = "wav";
	device = arg;
}

void set_out_cdr(char *arg)
{
	driver = "cdr";
	device = arg;
}

void set_out_au(char *arg)
{
	driver = "au";
	device = arg;
}

void set_out_test(char *arg)
{
	driver = "test";
	device = NULL;
}

static void set_out_file(char *arg)
{
	driver = "raw";
	device = arg;
}

static void set_out_stdout(char *arg)
{
	driver = "raw";
	device = NULL;
}

static void set_out_stdout1(char *arg)
{
	also_stdout = TRUE;
}

#if !defined (HAVE_SCHED_SETSCHEDULER) && !defined (HAVE_WINDOWS_H)
static void realtime_not_compiled(char *arg)
{
	fprintf(stderr, ME": Option '-T / --realtime' not compiled into this binary.\n");
}
#endif

static void list_output_modules(char *arg)
{
	char **names = NULL;
	char **descr = NULL;
	int count = -1;
	out123_handle *lao;

	if((lao=out123_new()))
	{
		out123_param_string(lao, OUT123_BINDIR, binpath);
		out123_param_int(lao, OUT123_VERBOSE, verbose);
		if(quiet)
			out123_param_int(lao, OUT123_FLAGS, OUT123_QUIET);
		if((count=out123_drivers(lao, &names, &descr)) >= 0)
		{
			int i;
			for(i=0; i<count; ++i)
			{
				printf( "%-15s\t%s\n", names[i], descr[i] );
				free(names[i]);
				free(descr[i]);
			}
			free(names);
			free(descr);
		}
		out123_del(lao);
	}
	else if(!quiet)
		error("Failed to create an out123 handle.");
	exit(count >= 0 ? 0 : 1);
}

static void list_encodings(char *arg)
{
	int i;
	int enc_count = 0;
	int *enc_codes = NULL;

	enc_count = out123_enc_list(&enc_codes);
	/* All of the returned encodings have to have proper names!
	   It is a libout123 bug if not, and it should be quickly caught. */
	for(i=0;i<enc_count;++i)
		printf( "%s:\t%s\n"
		,	out123_enc_name(enc_codes[i]), out123_enc_longname(enc_codes[i]) );
	free(enc_codes);
	exit(0);
}

static int getencs(void)
{
	int encs = 0;
	out123_handle *lao;
	if(verbose)
		fprintf( stderr
		,	ME": getting supported encodings for %li Hz, %i channels\n"
		,	rate, channels );
	if((lao=out123_new()))
	{
		out123_param_int(lao, OUT123_VERBOSE, verbose);
		if(quiet)
			out123_param_int(lao, OUT123_FLAGS, OUT123_QUIET);
		if(!out123_open(lao, driver, device))
			encs = out123_encodings(lao, rate, channels);
		else if(!quiet)
			error1("cannot open driver: %s", out123_strerror(lao));
		out123_del(lao);
	}
	else if(!quiet)
		error("Failed to create an out123 handle.");
	return encs;
}

static void test_format(char *arg)
{
	int encs;
	encs = getencs();
	exit((encs & encoding) ? 0 : -1);
}

static void test_encodings(char *arg)
{
	int encs, enc_count, *enc_codes, i;

	encs = getencs();
	enc_count = out123_enc_list(&enc_codes);
	for(i=0;i<enc_count;++i)
	{
		if((encs & enc_codes[i]) == enc_codes[i])
			printf("%s\n", out123_enc_name(enc_codes[i]));
	}
	free(enc_codes);
	exit(!encs);
}

static void query_format(char *arg)
{
	out123_handle *lao;

	if(verbose)
		fprintf(stderr, ME": querying default format\n");
	if((lao=out123_new()))
	{
		out123_param_int(lao, OUT123_VERBOSE, verbose);
		if(quiet)
			out123_param_int(lao, OUT123_FLAGS, OUT123_QUIET);
		if(!out123_open(lao, driver, device))
		{
			struct mpg123_fmt *fmts = NULL;
			int count;
			count = out123_formats(lao, NULL, 0, 0, 0, &fmts);
			if(count > 0 && fmts[0].encoding > 0)
			{
				const char *encname = out123_enc_name(fmts[0].encoding);
				printf( "--rate %li --channels %i --encoding %s\n"
				,	fmts[0].rate, fmts[0].channels
				,	encname ? encname : "???" );
			}
			else
			{
				if(verbose)
					fprintf(stderr, ME": no default format found\n");
			}
			free(fmts);
		}
		else if(!quiet)
			error1("cannot open driver: %s", out123_strerror(lao));
		out123_del(lao);
	}
	else if(!quiet)
		error("Failed to create an out123 handle.");
	exit(0);
}

void set_wave_freqs(char *arg)
{
	signal_source = "wave";
	wave_freqs = arg;
}

void set_pink_rows(char *arg)
{
	signal_source = "pink";
	pink_rows = atoi(arg);
}

void set_geiger_act(char *arg)
{
	signal_source = "geiger";
	geiger_activity = atof(arg);
}

void set_sweep_freq(char *arg)
{
	signal_source = "sweep";
	sweep_freq = atof(arg);
}

/* Please note: GLO_NUM expects point to LONG! */
/* ThOr:
 *  Yeah, and despite that numerous addresses to int variables were 
passed.
 *  That's not good on my Alpha machine with int=32bit and long=64bit!
 *  Introduced GLO_INT and GLO_LONG as different bits to make that clear.
 *  GLO_NUM no longer exists.
 */
topt opts[] = {
	{'t', "test",        GLO_INT,  set_out_test, NULL, 0},
	{'s', "stdout",      GLO_INT,  set_out_stdout,  NULL, 0},
	{'S', "STDOUT",      GLO_INT,  set_out_stdout1, NULL, 0},
	{'O', "outfile",     GLO_ARG | GLO_CHAR, set_out_file, NULL, 0},
	{'v', "verbose",     0,        set_verbose, 0,           0},
	{'q', "quiet",       0,        set_quiet,   0,           0},
	{'m',  "mono",       GLO_INT,  0, &channels, 1},
	{0,   "stereo",      GLO_INT,  0, &channels, 2},
	{'c', "channels",    GLO_ARG | GLO_INT,  0, &channels, 0},
	{'C', "inputch",     GLO_ARG | GLO_INT,  0, &inputch, 0},
	{'M', "mix",         GLO_ARG | GLO_CHAR, 0, &mixmat_string, 0},
	{'P', "preamp",      GLO_ARG | GLO_DOUBLE, 0, &preamp, 0},
	{0,   "offset",      GLO_ARG | GLO_DOUBLE, 0, &preamp_offset, 0},
	{'r', "rate",        GLO_ARG | GLO_LONG, 0, &rate,  0},
	{'R', "inputrate",   GLO_ARG | GLO_LONG, 0, &inputrate,  0},
	{0,   "clip",        GLO_ARG | GLO_CHAR, 0, &clip_mode, 0},
	{0,   "dither",      GLO_INT,            0, &dither,    1},
	{0,   "headphones",  0,                  set_output_h, 0,0},
	{0,   "speaker",     0,                  set_output_s, 0,0},
	{0,   "lineout",     0,                  set_output_l, 0,0},
	{'o', "output",      GLO_ARG | GLO_CHAR, set_output, 0,  0},
	{0,   "list-modules",0,       list_output_modules, NULL,  0},
	{'a', "audiodevice", GLO_ARG | GLO_CHAR, 0, &device,  0},
#ifndef NOXFERMEM
	{'b', "buffer",      GLO_ARG | GLO_LONG, 0, &buffer_kb,  0},
	{0, "preload", GLO_ARG|GLO_DOUBLE, 0, &preload, 0},
#endif
#ifdef HAVE_SETPRIORITY
	{0,   "aggressive",	 GLO_INT,  0, &aggressive, 2},
#endif
#if defined (HAVE_SCHED_SETSCHEDULER) || defined (HAVE_WINDOWS_H)
	/* check why this should be a long variable instead of int! */
	{'T', "realtime",    GLO_INT,  0, &realtime, TRUE },
#else
	{'T', "realtime",    0,  realtime_not_compiled, 0,           0 },    
#endif
#ifdef HAVE_WINDOWS_H
	{0, "priority", GLO_ARG | GLO_INT, 0, &w32_priority, 0},
#endif
	{'w', "wav",         GLO_ARG | GLO_CHAR, set_out_wav, 0, 0 },
	{0, "cdr",           GLO_ARG | GLO_CHAR, set_out_cdr, 0, 0 },
	{0, "au",            GLO_ARG | GLO_CHAR, set_out_au, 0, 0 },
	{'?', "help",            0,  want_usage, 0,           0 },
	{0 , "longhelp" ,        0,  want_long_usage, 0,      0 },
	{0 , "version" ,         0,  give_version, 0,         0 },
	{'e', "encoding", GLO_ARG|GLO_CHAR, 0, &encoding_name, 0},
	{'E', "inputenc", GLO_ARG|GLO_CHAR, 0, &inputenc_name, 0}, 
	{0, "list-encodings", 0, list_encodings, 0, 0 },
	{0, "test-format", 0, test_format, 0, 0 },
	{0, "test-encodings", 0, test_encodings, 0, 0},
	{0, "query-format", 0, query_format, 0, 0},
	{0, "name", GLO_ARG|GLO_CHAR, 0, &name, 0},
	{0, "devbuffer", GLO_ARG|GLO_DOUBLE, 0, &device_buffer, 0},
	{0, "timelimit", GLO_ARG|GLO_LONG, 0, &timelimit_samples, 0},
	{0, "seconds", GLO_ARG|GLO_DOUBLE, 0, &timelimit_seconds, 0},
	{0, "source", GLO_ARG|GLO_CHAR, 0, &signal_source, 0},
	{0, "wave-pat", GLO_ARG|GLO_CHAR, 0, &wave_patterns, 0},
	{0, "wave-freq", GLO_ARG|GLO_CHAR, set_wave_freqs, 0, 0},
	{0, "wave-phase", GLO_ARG|GLO_CHAR, 0, &wave_phases, 0},
	{0, "wave-direction", GLO_ARG|GLO_CHAR, 0, &wave_direction, 0},
	{0, "wave-limit", GLO_ARG|GLO_LONG, 0, &wave_limit, 0},
	{0, "genbuffer", GLO_ARG|GLO_LONG, 0, &wave_limit, 0},
	{0, "pink-rows", GLO_ARG|GLO_INT, set_pink_rows, 0, 0},
	{0, "geiger-activity", GLO_ARG|GLO_DOUBLE, set_geiger_act, 0, 0},
	{0, "wave-sweep", GLO_ARG|GLO_DOUBLE, set_sweep_freq, 0, 0},
	{0, "sweep-type", GLO_ARG|GLO_CHAR, 0, &sweep_type, 0 },
	{0, "sweep-time", GLO_ARG|GLO_DOUBLE, 0, &sweep_time, 0},
	{0, "sweep-hard", GLO_INT, 0, &sweep_hard, TRUE},
	{0, "sweep-count", GLO_ARG|GLO_LONG, 0, &sweep_count, 0},
	{0, "resampler", GLO_ARG|GLO_CHAR, 0, &resampler, 0},
	{0, 0, 0, 0, 0, 0}
};

/* An strtok() that also returns empty tokens on multiple separators. */

static size_t mytok_count(const char *choppy)
{
	size_t count = 0;
	if(choppy)
	{
		count = 1;
		do {
			if(*choppy == ',')
				++count;
		} while(*(++choppy));
	}
	return count;
}

static char *mytok(char **choppy)
{
	char *tok;
	if(!*choppy)
		return NULL;
	tok  = *choppy;
	while(**choppy && **choppy != ',')
		++(*choppy);
	/* Another token follows if we found a separator. */
	if(**choppy == ',')
	{
		*(*choppy)++ = 0;
		while(isspace(**choppy))
			*(*choppy)++ = 0;
	}
	else
		*choppy = NULL; /* Nothing left. */
	return tok;
}

static void setup_wavegen(void)
{
	size_t count = 0;
	size_t i;
	double *freq = NULL;
	double *freq_real = NULL;
	double *phase = NULL;
	int *backwards = NULL;
	int *id = NULL;
	int synerr = 0;
	size_t common = 0;

	if(!generate)
		wave_limit = 0;
	waver = syn123_new(inputrate, inputch, inputenc, wave_limit, &synerr);
	check_fatal_syn(synerr);
	check_fatal_syn(syn123_dither(waver, dither, NULL));
	if(!waver)
		safe_exit(132);
	if(do_resample)
	{
		int dirty = -1;
		if(!strcasecmp(resampler, "fine"))
			dirty = 0;
		else if(!strcasecmp(resampler, "dirty"))
			dirty = 1;
		else
		{
			if(!quiet)
				error1("Bad value for resampler type given: %s\n", resampler);
			safe_exit(132);
		}
		check_fatal_syn( syn123_setup_resample(waver
		,	inputrate, rate, channels, dirty ) );
		
	}
	// At least have waver handy for conversions.
	if(!generate)
		return;

	if(!strcmp(signal_source, "pink"))
	{
		synerr = syn123_setup_pink(waver, pink_rows, 123456, &common);
		if(synerr)
		{
			if(!quiet)
				merror("setting up pink noise generator: %s\n", syn123_strerror(synerr));
			safe_exit(132);
		}
		if(verbose)
		{
			fprintf( stderr
			,	ME ": pink noise with %i generator rows (0=internal default)\n"
			,	pink_rows );
		}
		goto setup_waver_end;
	}
	if(!strcmp(signal_source, "white"))
	{
		synerr = syn123_setup_white(waver, 123456, &common);
		if(synerr)
		{
			if(!quiet)
				merror("setting up white noise generator: %s\n", syn123_strerror(synerr));
			safe_exit(132);
		}
		if(verbose)
		{
			fprintf( stderr, ME ": white noise\n" );
		}
		goto setup_waver_end;
	}
	else if(!strcmp(signal_source, "geiger"))
	{
		synerr = syn123_setup_geiger(waver, geiger_activity, 123456, &common);
		if(synerr)
		{
			if(!quiet)
				merror("setting up geiger generator: %s\n", syn123_strerror(synerr));
			safe_exit(132);
		}
		if(verbose)
		{
			fprintf(stderr, ME ": geiger with actvity %g\n", geiger_activity);
		}
		goto setup_waver_end;
	}
	else if(!strcmp(signal_source, "sweep"))
	{
		double f1 = 0.;
		double f2 = sweep_freq;
		int wid = SYN123_WAVE_SINE;
		int backwards = FALSE;
		int sid = SYN123_SWEEP_QUAD;
		// Yes, could overflow. You get a short time, then.
		size_t duration = timelimit > -1 ? (size_t)timelimit : inputrate;
		size_t period = 0;
		double sweep_phase = 0.;
		double endphase = -1;
		if(sweep_type)
		{
			if(!strncmp("lin", sweep_type, 3))
				sid = SYN123_SWEEP_LIN;
			else if(!strncmp("qua", sweep_type, 3))
				sid = SYN123_SWEEP_QUAD;
			else if(!strncmp("exp", sweep_type, 3))
				sid = SYN123_SWEEP_EXP;
			else
			{
				if(!quiet)
					merror("bad sweep choice: %s", sweep_type);
				safe_exit(132);
			}
		}
		if(wave_freqs)
		{
			char *next = wave_freqs;
			f1 = atof(mytok(&next));
		}
		if(wave_patterns)
		{
			char *next = wave_patterns;
			wid = syn123_wave_id(mytok(&next));
			if(wid < 0 && !quiet)
				fprintf(stderr, "Warning: bad wave pattern: %s\n", wave_patterns);
		}
		if(wave_phases)
		{
			char *next = wave_phases;
			sweep_phase = atof(mytok(&next));
			if(sweep_phase < 0)
			{
				backwards = TRUE;
				sweep_phase = -sweep_phase;
			}
		}
		if(wave_direction)
		{
			char *next = wave_phases;
			backwards = atof(mytok(&next)) < 0 ? TRUE : FALSE;
		}
		if(sweep_time > 0)
			duration = (size_t)(sweep_time*inputrate);
		synerr = syn123_setup_sweep( waver, wid, sweep_phase, backwards
		, sid, &f1, &f2, !sweep_hard, duration, &endphase, &period, &common);
		if(synerr)
		{
			if(!quiet)
				merror("setting up sweep generator: %s\n", syn123_strerror(synerr));
			safe_exit(132);
		}
		if(sweep_count > -1)
			timelimit = sweep_count * period;
		if(verbose)
		{
			fprintf( stderr, ME ": %s sweep of %"SIZE_P" samples (%s)\n"
			,	sweep_type
			,	(timelimit > -1 && timelimit < period) ? (size_t)timelimit : period
			,	(timelimit > -1 && timelimit < period)
			?	((timelimit == duration) ? "exactly" : "cut off" )
			:	(sweep_hard ? "periodic with phase jumps" : "smoothly periodic") );
			fprintf( stderr, ME ": from: %s @ %g Hz p %g\n"
				,	syn123_wave_name(wid), f1, sweep_phase );
			fprintf( stderr, ME ": to:   %s @ %g Hz p %g\n"
			,	syn123_wave_name(wid), f2
			,	(timelimit < 0 || timelimit >= period) ? sweep_phase : endphase );
		}
		goto setup_waver_end;
	}
	else if(strcmp(signal_source, "wave"))
	{
		if(!quiet)
			merror("unknown signal source: %s", signal_source);
		safe_exit(132);
	}

	// The big default code block is for wave setup.
	// Exceptions jump over it.
	if(wave_freqs)
	{
		char *tok;
		char *next;
		count = mytok_count(wave_freqs);
		freq = malloc(sizeof(double)*count);
		freq_real = malloc(sizeof(double)*count);
		if(!freq || !freq_real){ error("OOM!"); safe_exit(1); }
		next = wave_freqs;
		for(i=0; i<count; ++i)
		{
			tok = mytok(&next);
			if(tok && *tok)
				freq[i] = atof(tok);
			else if(i)
				freq[i] = freq[i-1];
			else
				freq[i] = 0;
		}
		memcpy(freq_real, freq, sizeof(double)*count);
	}

	if(count && wave_patterns)
	{
		char *tok;
		char *next = wave_patterns;
		id = malloc(sizeof(int)*count);
		if(!id){ error("OOM!"); safe_exit(1); }
		for(i=0; i<count; ++i)
		{
			tok = mytok(&next);
			if((tok && *tok) || i==0)
			{
				id[i] = syn123_wave_id(tok);
				if(id[i] < 0 && !quiet)
					fprintf(stderr, "Warning: bad wave pattern: %s\n", tok);
			}
			else
				id[i] = id[i-1];
		}
	}

	if(count && wave_phases)
	{
		char *tok;
		char *next = wave_phases;
		phase = malloc(sizeof(double)*count);
		backwards = malloc(sizeof(int)*count);
		if(!phase || !backwards){ error("OOM!"); safe_exit(1); }
		for(i=0; i<count; ++i)
		{
			tok = mytok(&next);
			if(tok && *tok)
				phase[i] = atof(tok);
			else if(i)
				phase[i] = phase[i-1];
			else
				phase[i] = 0;
			if(phase[i] < 0)
			{
				phase[i] *= -1;
				backwards[i] = TRUE;
			}
			else
				backwards[i] = FALSE;
		}
	}

	if(count && wave_direction)
	{
		char *tok;
		char *next = wave_direction;
		if(!backwards)
			backwards = malloc(sizeof(int)*count);
		if(!backwards){ error("OOM!"); safe_exit(1); }
		for(i=0; i<count; ++i)
		{
			tok = mytok(&next);
			if(tok && *tok)
				backwards[i] = atof(tok) < 0 ? TRUE : FALSE;
			else if(i)
				backwards[i] = backwards[i-1];
			else
				backwards[i] = FALSE;
		}
	}

	synerr = syn123_setup_waves( waver, count
	,	id, freq_real, phase, backwards, &common );
	if(synerr)
	{
		if(!quiet)
			merror("setting up wave generator: %s\n", syn123_strerror(synerr));
		safe_exit(132);
	}
	if(verbose)
	{
		if(count) for(i=0; i<count; ++i)
			fprintf( stderr, ME ": wave %" SIZE_P ": %s @ %g Hz (%g Hz) p %g\n"
			,	i
			,	syn123_wave_name(id ? id[i] : SYN123_WAVE_SINE)
			,	freq[i], freq_real[i]
			,	phase ? phase[i] : 0 );
		else
			fprintf(stderr, ME ": default sine wave\n");
	}

setup_waver_end:
	if(verbose)
	{
		if(common)
			fprintf(stderr, ME ": periodic signal table of %" SIZE_P " samples\n", common);
		else
			fprintf(stderr, ME ": live signal generation\n");
	}
	if(phase)
		free(phase);
	if(id)
		free(id);
	if(freq)
		free(freq);
}


void push_output(unsigned char *buf, size_t samples)
{
	errno = 0;
	size_t bytes = samples*pcmframe;
	mdebug("playing %zu bytes", bytes);
	check_fatal_output(out123_play(ao, buf, bytes) < (int)bytes);
	if(also_stdout && fwrite(buf, pcmframe, samples, stdout) < samples)
	{
		// TODO: I guess some signal handling should be considered here.
		if(!quiet && errno != EINTR)
			error1( "failed to copy stream to stdout: %s", strerror(errno));
		safe_exit(133);
	}
}

// Clipping helper, called with mixbuf, resampler buffer, playback buffer ...
// This implies the output channel count.
void clip(void *buf, int enc, size_t samples)
{
	size_t clipped = soft_clip
	?	syn123_soft_clip( buf, enc, samples*channels
		,	clip_limit, clip_width, waver )
	:	syn123_clip(buf, enc, samples*channels);
	if(verbose > 1 && clipped)
		fprintf(stderr, ME ": explicitly clipped %"SIZE_P" samples\n", clipped);
}

/* return 1 on success, 0 on failure */
int play_frame(void)
{
	size_t got_samples;
	size_t get_samples = pcmblock;
	debug("play_frame");
	if(timelimit >= 0)
	{
		if(offset >= timelimit)
			return 0;
		else if(timelimit < offset+get_samples)
			get_samples = (off_t)timelimit-offset;
	}
	if(generate)
		got_samples = syn123_read(waver, inaudio, get_samples*pcminframe)
		/	pcminframe;
	else
		got_samples = fread(inaudio, pcminframe, get_samples, input);
	/* Play what is there to play (starting with second decode_frame call!) */
	if(!got_samples)
		return 0;

	if(mixaudio)
	{
		// All complicated cases trigger mixing/conversion into intermediate
		// buffer.
		// First either mix or convert into the mixing buffer.
		if(mixmat)
			check_fatal_syn(syn123_mix( mixaudio, mixenc, channels
			,	inaudio, inputenc, inputch, mixmat, got_samples, TRUE, NULL, waver ));
		else
			check_fatal_syn(syn123_conv( mixaudio, mixenc, got_samples*mixframe
			,	inaudio, inputenc, got_samples*pcminframe, NULL, NULL, waver ));
		// Do pre-amplification in-place.
		if(do_preamp)
			check_fatal_syn(syn123_amp( mixaudio, mixenc, got_samples*channels
			,	preamp_factor, preamp_offset, NULL, NULL ));
		// Resampling needs another buffer.
		if(do_resample)
		{
			// Resampling, clipping, and conversion to end buffer in a loop
			// of small blocks since resampling can mean a lot of output from tiny
			// input.
			// Mixaudio has to carry 32 bit float!
			float * fmixaudio = (float*)mixaudio;
			size_t insamples = got_samples;
			size_t inoff = 0;
			while(insamples)
			{
				size_t inblock = insamples < resample_block
				?	insamples
				:	resample_block;
				size_t outsamples = syn123_resample( waver
				,	resaudio, fmixaudio+inoff*channels, inblock );
				if(do_clip)
					clip(resaudio, MPG123_ENC_FLOAT_32, outsamples);
				// Damn, another loop inside the loop for a smaller output buffer?!
				// No. I will ensure pcmblock being as large as the largest to be
				// expected resampling block output!
				size_t clipped = 0;
				check_fatal_syn(syn123_conv( audio, encoding, outsamples*pcmframe
				,	resaudio, MPG123_ENC_FLOAT_32, outsamples*sizeof(float)*channels
				,	NULL, &clipped, NULL ));
				if(verbose > 1 && clipped)
					fprintf(stderr, ME ": clipped %"SIZE_P" samples\n", clipped);
				// Finally, some output!
				push_output(audio, outsamples);
				// Advance.
				insamples -= inblock;
				inoff     += inblock;
				if(intflag)
					return 1; // 1 or 0? Does it matter?
			}
		} else
		{
			// Just handle explicit clipping and do final conversion.
			if(do_clip)
				clip(mixaudio, mixenc, got_samples);
			// Finally, convert to output.
			size_t clipped = 0;
			check_fatal_syn(syn123_conv( audio, encoding, got_samples*pcmframe
			,	mixaudio, mixenc, got_samples*mixframe, NULL, &clipped, NULL ));
			if(verbose > 1 && clipped)
				fprintf(stderr, ME ": clipped %"SIZE_P" samples\n", clipped);
		}
	} else
	{
		size_t clipped = 0;
		// No separate mixing buffer, but possibly some mixing or conversion
		// directly into the output buffer. This path should only be taken if
		// there is either only one operation happening, or if the output
		// is in floating point encoding which makes intermediate conversions
		// unnecessary. The path should still be correct for other cases, just
		// slow and with suboptimal quality.
		if(inaudio != audio)
		{
			if(mixmat)
			{
				check_fatal_syn(syn123_mix( audio, encoding, channels
				,	inaudio, inputenc, inputch, mixmat, got_samples, TRUE, &clipped
				,	waver ));
			} else
			{
				check_fatal_syn(syn123_conv( audio, encoding, got_samples*pcmframe
				,	inaudio, inputenc, got_samples*pcminframe, NULL, &clipped
				,	waver ));
			}
		}
		if(do_preamp)
		{
			check_fatal_syn(syn123_amp (audio, encoding, got_samples*channels
			,	preamp_factor, preamp_offset, &clipped, waver ));
			if(verbose > 1 && clipped)
				fprintf(stderr, ME ": clipped %"SIZE_P" samples\n", clipped);
		}
		if(do_clip)
			clip(audio, encoding, got_samples);
	}

	// To recap: Without any conversion, the simplest code path has
	// optional preamp and possibly clipping, but only one of those.
	// The next stage is a combination, mixing or conversion before
	// those. If resampling is desired, the complex
	// code path with mixing buffer and resampling buffer is chosen, with
	// all the other bits. Soft clipping is probably a default with resampling,
	// for integer output. Also, if soft clip and preamp is desired for
	// integer output, this also triggers the complex code path. I rather
	// create separate copies than to convert from/to integers multiple
	// times.

	// The resampling loop triggers its own output. All other
	// paths want output of the prepared data here.
	if(!do_resample)
		push_output(audio, got_samples);
	offset += got_samples;
	return 1;
}

#if !defined(WIN32) && !defined(GENERIC)
static void catch_interrupt(void)
{
        intflag = TRUE;
}
#endif

static void *fatal_malloc(size_t bytes)
{
	void *buf;
	if(!(buf = malloc(bytes)))
	{
		if(!quiet)
			error("OOM");
		safe_exit(1);
	}
	return buf;
}

// Prepare all the buffers and some of the flags for the processing chain.
// The goal is to avoid unnecessary copies and format conversions.
// If we do multiple operations on the intermediate data, it gets a separate
// buffer in floating point encoding. If there is not much to do, this
// intermediate copy is skipped.
static void setup_processing(void)
{
	pcminframe = out123_encsize(inputenc)*inputch;
	pcmframe = out123_encsize(encoding)*channels;
	audio = fatal_malloc(pcmblock*pcmframe);

	unsigned int op_count = 0;

	// Full mixing is initiated if channel counts differ or a non-empty
	// mixing matrix has been specified.
	if(inputch != channels || (mixmat_string && mixmat_string[0]))
	{
		mixmat = fatal_malloc(sizeof(double)*inputch*channels);
		size_t mmcount = (mixmat_string && mixmat_string[0])
		?	mytok_count(mixmat_string)
		:	0;
		// Special cases of trivial down/upmixing need no user input.
		if(mmcount == 0 && inputch == 1)
		{
			for(int oc=0; oc<channels; ++oc)
				mixmat[oc] = 1.;
		}
		else if(mmcount == 0 && channels == 1)
		{
			for(int ic=0; ic<inputch; ++ic)
				mixmat[ic] = 1./inputch;
		}
		else if(mmcount != inputch*channels)
		{
			merror( "Need %i mixing matrix entries, got %zu."
			,	inputch*channels, mmcount );
			safe_exit(1);
		} else
		{
			char *next = mixmat_string;
			for(int i=0; i<inputch*channels; ++i)
			{
				char *tok = mytok(&next);
				mixmat[i] = tok ? atof(tok) : 0.;
			}
		}
	}

	if(inputrate != rate)
	{
		do_resample = TRUE;
		++op_count;
		// Buffer computations only make sense if resampling possibly can
		// be set up at all. 
		if( inputrate > syn123_resample_maxrate()
		||       rate > syn123_resample_maxrate() )
		{
			error("Sampling rates out of range for the resampler.");
			safe_exit(134);
		}
		// Settle resampling block size, which determines the resampling
		// buffer size.
		// First attempt: Try if pcmblock input yields a reasonable
		// number of samples/frames for the resampling output.
		// if it is too large, reduce until we are below a threshold.
		// How big should the threshold be? When I say 10 times pcmblock ...
		// I still to the channel count as variable ... but let's say that.
		// Then the baddest resampling ratio that could barely work is about
		// 1:10000. That's quite generous. Even fixing things at pcmblock
		// max in the output would still be ok for factors close to 1000.
		// Well, let's try this to stay reasonable: limit to 10*pcmblock
		// at first. If that is not enough, reduce resample_block down to
		// 100 samples or so, to avoid too degenerate situations. If
		// that lower limit of resample_block is reached and still produces
		// more than 10*pcmblock output, it is attempted to just allocate
		// that bigger buffer. If you have the memory, you can choose the
		// rates. Reasonable resampling tasks should be well within the
		// factor 10 allowed for by default.
		// Another question: Should I try to increase pcmblock for the case of
		// extreme downsampling? This is just a matter of efficiency, having
		// lots of code only producing a single sample now and then, the
		// block treatment in libsyn123 not getting active.
		resample_block = pcmblock;
		size_t resample_out;
		while( (resample_out = syn123_resample_count( inputrate, rate
		,	resample_block )) > 10*pcmblock)
			resample_block /= 2;
		if(resample_block < 128)
		{
			resample_block = 128;
			resample_out = syn123_resample_count(inputrate, rate, resample_block);
		}
		if(verbose)
			fprintf( stderr, ME": resampling %zu samples @ %ld Hz"
				" to up to %zu samples @ %ld Hz\n", resample_block, inputrate
			,	resample_out, rate );
		if(!resample_out)
		{
			error("Cannot compute resampler output count.");
			safe_exit(134);
		}
		// Now we got some number of resampler output samples.
		// That buffer will always be big enough when handing in resample_block.
		resaudio = fatal_malloc(resample_out*sizeof(float)*channels);
	}

	// If converting or mixing, use separate input buffer.
	if(inputenc != encoding || mixmat)
	{
		inaudio = fatal_malloc(pcmblock*pcminframe);
		++op_count; // conversion or mixing
	}
	else
		inaudio = audio;

	if(preamp != 0. || preamp_offset != 0.)
	{
		preamp_factor = syn123_db2lin(preamp);
		// Store limited value for proper reporting.
		preamp = syn123_lin2db(preamp_factor);
		if(preamp_offset == 0. && mixmat)
		{
			// If we are mixing already, just include preamp in this.
			for(int i=0; i<inputch*channels; ++i)
				mixmat[i] *= preamp_factor;
			preamp_factor = 1.;
		}
		do_preamp = TRUE;
		++op_count;
	}

	do_clip = FALSE;
	if(!strcasecmp(clip_mode, "soft"))
	{
		do_clip = TRUE;
		soft_clip = TRUE;
	}
	else if(!strcasecmp(clip_mode, "hard"))
	{
		if(encoding & MPG123_ENC_FLOAT)
			do_clip = TRUE;
		soft_clip = FALSE;
	}
	else if(strcasecmp(clip_mode, "implicit"))
	{
		if(!quiet)
			error1("Bad value for clipping mode given: %s\n", clip_mode);
		safe_exit(135);
	}
	if(do_clip)
		++op_count;

	if(do_resample || op_count > 1)
	{
		// Create a separate mixing buffer for the complicated cases.
		// Resampling limits quality to 32 bits float anyway, so avoid
		// trying to mix in double.
		mixenc = do_resample
		?	MPG123_ENC_FLOAT_32
		:	syn123_mixenc(inputenc, encoding);
		mixframe = out123_encsize(mixenc)*channels;
		mixaudio = fatal_malloc(mixframe*pcmblock);
	}
}

int main(int sys_argc, char ** sys_argv)
{
	int result;
#if defined(WIN32)
	_setmode(STDIN_FILENO,  _O_BINARY);
#endif

#if defined (WANT_WIN32_UNICODE)
	if(win32_cmdline_utf8(&argc, &argv) != 0)
	{
		error("Cannot convert command line to UTF8!");
		safe_exit(76);
	}
#else
	argv = sys_argv;
	argc = sys_argc;
#endif

	if(!(fullprogname = compat_strdup(argv[0])))
	{
		error("OOM"); /* Out Of Memory. Don't waste bytes on that error. */
		safe_exit(1);
	}
	/* Extract binary and path, take stuff before/after last / or \ . */
	if(  (cmd_name = strrchr(fullprogname, '/')) 
	  || (cmd_name = strrchr(fullprogname, '\\')))
	{
		/* There is some explicit path. */
		cmd_name[0] = 0; /* End byte for path. */
		cmd_name++;
		binpath = fullprogname;
	}
	else
	{
		cmd_name = fullprogname; /* No path separators there. */
		binpath = NULL; /* No path at all. */
	}

	/* Get default flags. */
	{
		out123_handle *paro = out123_new();
		out123_getparam_int(paro, OUT123_FLAGS, &outflags);
		out123_del(paro);
	}

#ifdef OS2
        _wildcard(&argc,&argv);
#endif

	while ((result = getlopt(argc, argv, opts)))
	switch (result) {
		case GLO_UNKNOWN:
			fprintf (stderr, ME": invalid argument: %s\n", loptarg);
			usage(1);
		case GLO_NOARG:
			fprintf (stderr, ME": missing argument for parameter: %s\n", loptarg);
			usage(1);
	}

	if(inputrate < 1)
		inputrate = rate;

	if(quiet)
		verbose = 0;

	/* Ensure cleanup before we cause too much mess. */
#if !defined(WIN32) && !defined(GENERIC)
	catchsignal(SIGINT, catch_interrupt);
	catchsignal(SIGTERM, catch_interrupt);
#endif
	ao = out123_new();
	if(!ao){ error("Failed to allocate output."); exit(1); }

	if
	( 0
	||	out123_param_int(ao, OUT123_FLAGS, outflags)
	|| out123_param_float(ao, OUT123_PRELOAD, preload)
	|| out123_param_int(ao, OUT123_VERBOSE, verbose)
	|| out123_param_string(ao, OUT123_NAME, name)
	|| out123_param_string(ao, OUT123_BINDIR, binpath)
	|| out123_param_float(ao, OUT123_DEVICEBUFFER, device_buffer)
	)
	{
		error("Error setting output parameters. Do you need a usage reminder?");
		usage(1);
	}
	
#ifdef HAVE_SETPRIORITY
	if(aggressive) { /* tst */
		int mypid = getpid();
		if(!quiet) fprintf(stderr, ME": Aggressively trying to increase priority.\n");
		if(setpriority(PRIO_PROCESS,mypid,-20))
			error("Failed to aggressively increase priority.\n");
	}
#endif

#if defined (HAVE_SCHED_SETSCHEDULER) && !defined (__CYGWIN__) && !defined (HAVE_WINDOWS_H)
/* Cygwin --realtime seems to fail when accessing network, using win32 set priority instead */
/* MinGW may have pthread installed, we prefer win32API */
	if(realtime)
	{  /* Get real-time priority */
		struct sched_param sp;
		if(!quiet) fprintf(stderr, ME": Getting real-time priority\n");
		memset(&sp, 0, sizeof(struct sched_param));
		sp.sched_priority = sched_get_priority_min(SCHED_FIFO);
		if (sched_setscheduler(0, SCHED_RR, &sp) == -1)
			error("Can't get realtime priority\n");
	}
#endif

/* make sure not Cygwin, it doesn't need it */
#if defined(WIN32) && defined(HAVE_WINDOWS_H)
	/* argument "3" is equivalent to realtime priority class */
	win32_set_priority(realtime ? 3 : w32_priority);
#endif

	if(encoding_name)
	{
		encoding = out123_enc_byname(encoding_name);
		if(encoding < 0)
		{
			error1("Unknown encoding '%s' given!\n", encoding_name);
			safe_exit(1);
		}
	}

	input = stdin;
	if(strcmp(signal_source, "file"))
		generate = TRUE;

	// Genererally generate signal in floating point for later conversion
	// after possible additional processing.
	inputenc = generate && encoding != MPG123_ENC_FLOAT_64
	?	MPG123_ENC_FLOAT_32
	:	encoding;
	// User can override.
	if(inputenc_name)
	{
		inputenc = out123_enc_byname(inputenc_name);
		if(inputenc < 0)
		{
			error1("Unknown input encoding '%s' given!\n", inputenc_name);
			safe_exit(1);
		}
	}
	if(!inputch)
		inputch = channels;

	// Settle buffers and the mixing/conversion code path.
	setup_processing();
	check_fatal_output(out123_set_buffer(ao, buffer_kb*1024));
	check_fatal_output(out123_open(ao, driver, device));

	if(timelimit_seconds >= 0.)
		timelimit = timelimit_seconds*inputrate;
	if(timelimit_samples >= 0)
		timelimit = timelimit_samples;

	if(verbose)
	{
		long props = 0;
		const char *encname;
		char *realname = NULL;
		if(inaudio != audio)
		{
			encname = out123_enc_name(inputenc);
			fprintf( stderr, ME": input format: %li Hz, %i channels, %s\n"
			,	inputrate, inputch, encname ? encname : "???" );
			encname = out123_enc_name(syn123_mixenc(inputenc, encoding));
			if(mixmat)
			{
				fprintf( stderr, ME": mixing in %s\n", encname ? encname : "???" );
				for(int oc=0; oc<channels; ++oc)
				{
					fprintf(stderr, ME": out ch %i mix:", oc);
					for(int ic=0; ic<inputch; ++ic)
						fprintf(stderr, " %6.2f", mixmat[SYN123_IOFF(oc,ic,inputch)]);
					fprintf(stderr, "\n");
				}
			}
			else
				fprintf( stderr, ME": converting via %s\n", encname ? encname : "???" );
		}
		encname = out123_enc_name(encoding);
		fprintf(stderr, ME": format: %li Hz, %i channels, %s\n"
		,	rate, channels, encname ? encname : "???" );
		if(preamp != 0.)
			fprintf( stderr, ME": preamp: %.1f dB%s\n", preamp
			,	preamp_factor != 1. ? "" : " (during mixing)" );
		if(preamp_offset != 0.)
			fprintf(stderr, ME": applying scaled offset: %f\n", preamp_offset);
		out123_getparam_string(ao, OUT123_NAME, &realname);
		if(realname)
			fprintf(stderr, ME": output real name: %s\n", realname);
		out123_getparam_int(ao, OUT123_PROPFLAGS, &props);
		if(props & OUT123_PROP_LIVE)
			fprintf(stderr, ME": This is a live sink.\n");
	}
	check_fatal_output(out123_start(ao, rate, channels, encoding));

	setup_wavegen(); // Used also for conversion/mixing.

	while(play_frame() && !intflag)
	{
		/* be happy */
	}
	if(intflag) /* Make it quick! */
	{
		if(!quiet)
			fprintf(stderr, ME": Interrupted. Dropping the ball.\n");
		out123_drop(ao);
	}

	safe_exit(0); /* That closes output and restores terminal, too. */
	return 0;
}

static char* output_enclist(void)
{
	int i;
	char *list = NULL;
	char *pos;
	size_t len = 0;
	int enc_count = 0;
	int *enc_codes = NULL;

	enc_count = out123_enc_list(&enc_codes);
	if(enc_count < 0 || !enc_codes)
		return NULL;
	for(i=0;i<enc_count;++i)
		len += strlen(out123_enc_name(enc_codes[i]));
	len += enc_count;

	if((pos = list = malloc(len))) for(i=0;i<enc_count;++i)
	{
		const char *name = out123_enc_name(enc_codes[i]);
		if(i>0)
			*(pos++) = ' ';
		strcpy(pos, name);
		pos+=strlen(name);
	}
	free(enc_codes);
	return list;
}

static void print_title(FILE *o)
{
	fprintf(o, "Simple audio output with raw PCM input\n");
	fprintf(o, "\tversion %s; derived from mpg123 by Michael Hipp and others\n", PACKAGE_VERSION);
	fprintf(o, "\tfree software (LGPL) without any warranty but with best wishes\n");
}

static void usage(int err)  /* print syntax & exit */
{
	FILE* o = stdout;
	if(err)
	{
		o = stderr; 
		fprintf(o, ME": You made some mistake in program usage... let me briefly remind you:\n\n");
	}
	print_title(o);
	fprintf(o,"\nusage: %s [option(s)] [file(s) | URL(s) | -]\n", cmd_name);
	fprintf(o,"supported options [defaults in brackets]:\n");
	fprintf(o,"   -v    increase verbosity level       -q    quiet (only print errors)\n");
	fprintf(o,"   -t    testmode (no output)           -s    write to stdout\n");
	fprintf(o,"   -w f  write output as WAV file\n");
	fprintf(o,"   -b n  output buffer: n Kbytes [0]                                  \n");
	fprintf(o,"   -r n  set samplerate [44100]\n");
	fprintf(o,"   -o m  select output module           -a d  set audio device\n");
	fprintf(o,"   -m    single-channel (mono) instead of stereo\n");
	#ifdef HAVE_SCHED_SETSCHEDULER
	fprintf(o,"   -T get realtime priority\n");
	#endif
	fprintf(o,"   -?    this help                      --version  print name + version\n");
	fprintf(o,"See the manpage out123(1) or call %s with --longhelp for more parameters and information.\n", cmd_name);
	safe_exit(err);
}

static void want_usage(char* arg)
{
	usage(0);
}

static void long_usage(int err)
{
	char *enclist;
	FILE* o = stdout;
	if(err)
	{
  	o = stderr; 
  	fprintf(o, "You made some mistake in program usage... let me remind you:\n\n");
	}
	enclist = output_enclist();
	print_title(o);
	fprintf(o,"\nusage: %s [option(s)] [file(s) | URL(s) | -]\n", cmd_name);

	fprintf(o,"        --name <n>         set instance name (p.ex. JACK client)\n");
	fprintf(o," -o <o> --output <o>       select audio output module\n");
	fprintf(o,"        --list-modules     list the available modules\n");
	fprintf(o," -a <d> --audiodevice <d>  select audio device (for files, empty or - is stdout)\n");
	fprintf(o," -s     --stdout           write raw audio to stdout (-o raw -a -)\n");
	fprintf(o," -S     --STDOUT           play AND output stream to stdout\n");
	fprintf(o," -O <f> --output <f>       raw output to given file (-o raw -a <f>)\n");
	fprintf(o," -w <f> --wav <f>          write samples as WAV file in <f> (-o wav -a <f>)\n");
	fprintf(o,"        --au <f>           write samples as Sun AU file in <f> (-o au -a <f>)\n");
	fprintf(o,"        --cdr <f>          write samples as raw CD audio file in <f> (-o cdr -a <f>)\n");
	fprintf(o," -r <r> --rate <r>         set the audio output rate in Hz (default 44100)\n");
	fprintf(o," -R <r> --inputrate <r>    set intput rate in Hz for conversion (if > 0)\n"
	          "                           (always last operation before output)\n");
	fprintf(o,"        --resampler <s>    set resampler method (fine (default) or dirty)\n");
	fprintf(o," -c <n> --channels <n>     set channel count to <n>\n");
	fprintf(o," -m     --mono             set output channel count to 1\n");
	fprintf(o,"        --stereo           set output channel count to 2 (default)\n");
	fprintf(o," -C <n  --inputch <n>      set input channel count for conversion\n");
	fprintf(o," -e <c> --encoding <c>     set output encoding (%s)\n"
	,	enclist != NULL ? enclist : "OOM!");
	fprintf(o," -E <c> --inputenc <c>     set input encoding for conversion\n");
	fprintf(o,"        --list-encodings   list of encoding short and long names\n");
	fprintf(o,"        --mix <m>          mixing matrix <m> between input and output channels\n");
	fprintf(o,"                           as linear factors, comma separated list for output\n");
	fprintf(o,"                           channel 1, then 2, ... default unity if channel counts\n");
	fprintf(o,"                           match, 0.5,0.5 for stereo to mono, 1,1 for the other way\n");
	fprintf(o," -P <p> --preamp <p>       amplify signal with <p> dB before output\n");
	fprintf(o,"        --offset <o>       apply PCM offset (floating point scaled in [-1:1]");
	fprintf(o,"        --clip <s>         select clipping mode: soft or hard for forced\n"
	          "                           clipping also for floating point output, implicit\n"
	          "                           (default) for implied clipping during conversion\n" );
	fprintf(o,"        --dither           enable dithering for conversions to integer\n");
	fprintf(o,"        --test-format      return 0 if audio format set by preceeding options is supported\n");
	fprintf(o,"        --test-encodings   print out possible encodings with given channels/rate\n");
	fprintf(o,"        --query-format     print out default format for given device, if any\n");
	fprintf(o," -o h   --headphones       (aix/hp/sun) output on headphones\n");
	fprintf(o," -o s   --speaker          (aix/hp/sun) output on speaker\n");
	fprintf(o," -o l   --lineout          (aix/hp/sun) output to lineout\n");
#ifndef NOXFERMEM
	fprintf(o," -b <n> --buffer <n>       set play buffer (\"output cache\")\n");
	fprintf(o,"        --preload <value>  fraction of buffer to fill before playback\n");
#endif
	fprintf(o,"        --devbuffer <s>    set device buffer in seconds; <= 0 means default\n");
	fprintf(o,"        --timelimit <s>    set time limit in PCM samples if >= 0\n");
	fprintf(o,"        --seconds <s>      set time limit in seconds if >= 0\n");
	fprintf(o,"        --source <s>       choose signal source: file (default),\n");
	fprintf(o,"                           wave, sweep, pink, geiger; implied by\n");
	fprintf(o,"                           --wave-freq, --wave-sweep,\n");
	fprintf(o,"                           --pink-rows, --geiger-activity\n");
	fprintf(o,"        --wave-freq <f>    set wave generator frequency or list of those\n");
	fprintf(o,"                           with comma separation for enabling a generated\n");
	fprintf(o,"                           test signal instead of standard input,\n");
	fprintf(o,"                           empty value repeating the previous\n");
	fprintf(o,"        --wave-pat <p>     set wave pattern(s) (out of those:\n");
	{
		int i=0;
		const char* wn;
		while((wn=syn123_wave_name(i++)) && wn[0] != '?')
			fprintf(o, "                           %s\n", wn);
	}
	fprintf(o,"                           ),\n");
	fprintf(o,"                           empty value repeating the previous\n");
	fprintf(o,"        --wave-phase <p>   set wave phase shift(s), negative values\n");
	fprintf(o,"                           inverting the pattern in time and\n");
	fprintf(o,"                           empty value repeating the previous,\n");
	fprintf(o,"                           --wave-direction overriding the negative bit\n");
	fprintf(o,"        --wave-direction <d> set direction explicitly (the sign counts)\n");
	fprintf(o,"        --wave-sweep <f>   sweep a generated wave to frequency f, from\n");
	fprintf(o,"                           first one specified for --wave-freq, using the\n");
	fprintf(o,"                           first wave pattern and direction, too\n");
	fprintf(o,"        --sweep-time <s>   set frequency sweep duration to s seconds if > 0\n");
	fprintf(o,"                           (defaulting to timelimit if set, otherwise one second)\n");
	fprintf(o,"        --sweep-count <c>  set timelimit to exactly produce that many (smooth) sweeps");
	fprintf(o,"        --sweep-type <t>   set sweep type: lin(ear), qua(d) (default),\n");
	fprintf(o,"                           exp(onential)\n");
	fprintf(o,"        --sweep-hard       disable post-sweep smoothing for periodicity\n");
	fprintf(o,"        --genbuffer <b>    buffer size (limit) for signal generators,\n");
	fprintf(o,"                           if > 0 (default), this enforces a periodic\n");
	fprintf(o,"                           buffer also for non-periodic signals, benefit:\n");
	fprintf(o,"                           less runtime CPU overhead\n");
	fprintf(o,"        --wave-limit <l>   alias for --genbuffer\n");
	fprintf(o,"        --pink-rows <r>    activate pink noise source and choose rows for\n");
	fprintf(o,"                   `       the algorithm (<1 chooses default)\n");
	fprintf(o,"        --geiger-activity <a> a Geiger-Mueller counter as source, with\n");
	fprintf(o,"                           <a> average events per second\n");
	fprintf(o," -t     --test             no output, just read and discard data (-o test)\n");
	fprintf(o," -v[*]  --verbose          increase verboselevel\n");
	#ifdef HAVE_SETPRIORITY
	fprintf(o,"        --aggressive       tries to get higher priority (nice)\n");
	#endif
	#if defined (HAVE_SCHED_SETSCHEDULER) || defined (HAVE_WINDOWS_H)
	fprintf(o," -T     --realtime         tries to get realtime priority\n");
	#endif
	#ifdef HAVE_WINDOWS_H
	fprintf(o,"        --priority <n>     use specified process priority\n");
	fprintf(o,"                           accepts -2 to 3 as integer arguments\n");
	fprintf(o,"                           -2 as idle, 0 as normal and 3 as realtime.\n");
	#endif
	fprintf(o," -?     --help             give compact help\n");
	fprintf(o,"        --longhelp         give this long help listing\n");
	fprintf(o,"        --version          give name / version string\n");

	fprintf(o,"\nSee the manpage out123(1) for more information. Also, note that\n");
	fprintf(o,"any numeric arguments are parsed in C locale (pi is 3.14, not 3,14).\n");
	free(enclist);
	safe_exit(err);
}

static void want_long_usage(char* arg)
{
	long_usage(0);
}

static void give_version(char* arg)
{
	fprintf(stdout, "out123 "PACKAGE_VERSION"\n");
	safe_exit(0);
}
