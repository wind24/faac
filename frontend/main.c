/*
 * FAAC - Freeware Advanced Audio Coder
 * Copyright (C) 2001 Menno Bakker
 * Copyright (C) 2002-2004 Krzysztof Nikiel
 * Copyright (C) 2004 Dan Christiansen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id: main.c,v 1.57 2004/03/17 13:32:13 danchr Exp $
 */

#ifdef _MSC_VER
# define HAVE_LIBMP4V2	1
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_LIBMP4V2
# include <mp4.h>
#endif

#define DEFAULT_TNS     0

#ifdef WIN32
#include <windows.h>
#include <fcntl.h>
#else
#include <signal.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#ifdef HAVE_GETOPT_H
# include <getopt.h>
#else
# include "getopt.h"
# include "getopt.c"
#endif

#include "input.h"

#include <faac.h>

#ifndef min
#define min(a,b) ( (a) < (b) ? (a) : (b) )
#endif

/* globals */
char* progName;
#ifndef WIN32
volatile int running = 1;
#endif

enum stream_format {
  ADTS_STREAM,
  RAW_STREAM,
};

enum container_format {
  NO_CONTAINER,
#ifdef HAVE_LIBMP4V2
  MP4_CONTAINER,
#endif
};

#ifndef WIN32
void signal_handler(int signal) {
    running = 0;
}
#endif

static int *mkChanMap(int channels, int center, int lf)
{
    int *map;
    int inpos;
    int outpos;

    if (!center && !lf)
        return NULL;

    if (channels < 3)
        return NULL;

    if (lf > 0)
        lf--;
    else
        lf = channels - 1; // default AAC position

    if (center > 0)
        center--;
    else
        center = 0; // default AAC position

    map = malloc(channels * sizeof(map[0]));
    memset(map, 0, channels * sizeof(map[0]));

    outpos = 0;
    if ((center >= 0) && (center < channels))
        map[outpos++] = center;

    inpos = 0;
    for (; outpos < (channels - 1); inpos++)
    {
        if (inpos == center)
            continue;
        if (inpos == lf)
            continue;

        map[outpos++] = inpos;
    }
    if (outpos < channels)
    {
        if ((lf >= 0) && (lf < channels))
            map[outpos] = lf;
        else
            map[outpos] = inpos;
    }

    return map;
}

int main(int argc, char *argv[])
{
    int frames, currentFrame;
    faacEncHandle hEncoder;
    pcmfile_t *infile = NULL;

    unsigned long samplesInput, maxBytesOutput, totalBytesWritten=0;

    faacEncConfigurationPtr myFormat;
    unsigned int mpegVersion = MPEG2;
    unsigned int objectType = LOW;
    unsigned int useMidSide = 1;
    static unsigned int useTns = DEFAULT_TNS;
    enum container_format container = NO_CONTAINER;
    enum stream_format stream = ADTS_STREAM;
    int cutOff = -1;
    int bitRate = 0;
    unsigned long quantqual = 0;
    int chanC = 3;
    int chanLF = 4;

    char *audioFileName;
    char *aacFileName;
    char *aacFileExt;

    float *pcmbuf;
    int *chanmap = NULL;

    unsigned char *bitbuf;
    int samplesRead = 0;
    int dieUsage = 0;

    int rawChans = 0; // disabled by default
    int rawBits = 16;
    int rawRate = 44100;
    int rawEndian = 1;

    int shortctl = SHORTCTL_NORMAL;

    FILE *outfile;

#ifdef HAVE_LIBMP4V2
    MP4FileHandle MP4hFile = MP4_INVALID_FILE_HANDLE;
    MP4TrackId MP4track = 0;
    int ntracks = 0, trackno = 0;
    const char *artist = NULL, *title = NULL, *album = NULL, *date = NULL,
      *genre = NULL, *comment = NULL;
    u_int64_t total_samples = 0;
    u_int64_t encoded_samples = 0;
    unsigned int delay_samples;
    unsigned int frameSize;
#endif
    char *faac_id_string;
    char *faac_copyright_string;

#ifndef WIN32
    // install signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    // get faac version
    if (faacEncGetVersion(&faac_id_string, &faac_copyright_string) == FAAC_CFG_VERSION)
    {
        fprintf(stderr, "%s(see the faac.html file for more details)\n\n",
	      faac_copyright_string);
      fprintf(stderr, "libfaac version %s\n", faac_id_string);
    }
    else
    {
        fprintf(stderr, __FILE__ "(%d): wrong libfaac version\n", __LINE__);
        return 1;
    }

    /* begin process command line */
    progName = argv[0];
    while (1) {
        static struct option long_options[] = {
            { "mpeg", 0, 0, 'm' },
            { "objecttype", 0, 0, 'o' },
            { "raw", 0, 0, 'r' },
            { "nomidside", 0, 0, 'n' },
            { "cutoff", 1, 0, 'c' },
            { "quality", 1, 0, 'q' },
            { "pcmraw", 0, 0, 'P'},
            { "pcmsamplerate", 1, 0, 'R'},
            { "pcmsamplebits", 1, 0, 'B'},
            { "pcmchannels", 1, 0, 'C'},
            { "shortctl", 1, 0, 300},
            { "tns", 0, 0, 301},
            { "notns", 0, 0, 302},
#ifdef HAVE_LIBMP4V2
            { "createmp4", 0, 0, 'w'},
            { "artist", 1, 0, 'A'},
            { "title", 1, 0, 'T'},
            { "album", 1, 0, 'L'},
            { "track", 1, 0, 'N'},
            { "genre", 1, 0, 'G'},
            { "date", 1, 0, 'D'},
            { "comment", 1, 0, 'M'},
#endif
	    { "pcmswapbytes", 0, 0, 'X'},
            { 0, 0, 0, 0}
        };
        int c = -1;
        int option_index = 0;

        c = getopt_long(argc, argv, "a:m:o:rnc:q:PR:B:C:I:X"
#ifdef HAVE_LIBMP4V2
                        "wA:T:L:N:G:D:C:"
#endif
            ,long_options, &option_index);

        if (c == -1)
            break;

        if (!c)
        {
          dieUsage = 1;
          break;
        }

        switch (c) {
        case 'm':
            mpegVersion = atoi(optarg);
            switch(mpegVersion)
            {
            case 2:
                mpegVersion = MPEG2;
                break;
            case 4:
                mpegVersion = MPEG4;
                break;
            default:
                mpegVersion = MPEG4;
            }
            break;
        case 'o':
            objectType = atoi(optarg);
            switch (objectType)
            {
            case 1:
                objectType = MAIN;
                break;
            case 2:
                objectType = LTP;
                break;
            default:
                objectType = LOW;
                break;
            }
            break;
        case 'r': {
            stream = RAW_STREAM;
            break;
        }
        case 'n': {
            useMidSide = 0;
            break;
        }
        case 'c': {
            unsigned int i;
            if (sscanf(optarg, "%u", &i) > 0) {
                cutOff = i;
            }
            break;
        }
        case 'a': {
            unsigned int i;
            if (sscanf(optarg, "%u", &i) > 0)
            {
                bitRate = 1000 * i;
            }
            break;
        }
        case 'q':
        {
            unsigned int i;
            if (sscanf(optarg, "%u", &i) > 0)
            {
                if (i > 0 && i < 1000)
                    quantqual = i;
            }
            break;
        }
        case 'I':
            sscanf(optarg, "%d,%d", &chanC, &chanLF);
            break;
        case 'P':
            rawChans = 2; // enable raw input
            break;
        case 'R':
        {
            unsigned int i;
            if (sscanf(optarg, "%u", &i) > 0)
            {
                rawRate = i;
                rawChans = (rawChans > 0) ? rawChans : 2;
            }
            break;
        }
        case 'B':
        {
            unsigned int i;
            if (sscanf(optarg, "%u", &i) > 0)
            {
                if (i > 32)
                    i = 32;
                if (i < 8)
                    i = 8;
                rawBits = i;
                rawChans = (rawChans > 0) ? rawChans : 2;
            }
            break;
        }
        case 'C':
        {
            unsigned int i;
            if (sscanf(optarg, "%u", &i) > 0)
                rawChans = i;
            break;
        }
#ifdef HAVE_LIBMP4V2
        case 'w':
	    container = MP4_CONTAINER;
            break;
	case 'A':
	    artist = optarg;
	    break;
	case 'T':
	    title = optarg;
	    break;
	case 'L':
	    album = optarg;
	    break;
	case 'N':
	    sscanf(optarg, "%i/%i", &trackno, &ntracks);
	    break;
	case 'G':
	    genre = optarg;
	    break;
	case 'D':
	    date = optarg;
	    break;
	case 'M':
	    comment = optarg;
	    break;
#endif
        case 300:
            shortctl = atoi(optarg);
            break;
        case 301:
            useTns = 1;
            break;
        case 302:
            useTns = 0;
            break;
	case 'X':
	  rawEndian = 0;
	  break;
        case '?':
            break;
        default:
          dieUsage = 1;
          break;
        }
    }

#ifdef HAVE_LIBMP4V2
    if (container != MP4_CONTAINER && (ntracks || trackno || artist ||
				       title ||  album || date ||
				       genre || comment))
    {
      printf("\nERROR: Metadata requires MP4 output!");
	dieUsage = 1;
    }
#endif

    /* check that we have at least two non-option arguments */
    if ((argc - optind) < 2 || dieUsage == 1)
    {
        printf("\nUsage: %s -options infile outfile\n", progName);
        printf("Options:\n");
        printf("  -a <x> Set average bitrate to x kbps/channel. (lower quality mode)\n");
        printf("  -c <bandwidth>\tSet the bandwidth in Hz. (default=automatic)\n");
        printf("  -q <quality>\tSet quantizer quality.\n");
#if !DEFAULT_TNS
        printf("  --tns  \tEnable TNS coding.\n");
#else
        printf("  --notns\tDisable TNS coding.\n");
#endif
        printf("  -n     Don\'t use mid/side coding.\n");
        printf("  -m X   AAC MPEG version, X can be 2 or 4.\n");
        printf("  -o X   AAC object type. (0=Low Complexity (default), 1=Main, 2=LTP)\n");
        printf("  --shortctl <x>  Enforce block type (1 = no short; 2 = no long)\n");
        printf("  -P     Raw PCM input mode (default 44100Hz 16bit stereo).\n");
        printf("  -R     Raw PCM input rate.\n");
        printf("  -B     Raw PCM input sample size (8, 16 (default), 24 or 32bits).\n");
        printf("  -C     Raw PCM input channels.\n");
	printf("  -X     Raw PCM swap input bytes\n");
        printf("  -I <C,LF> Input channel config, default is 3,4 (Center third, LF fourth)\n");
        printf("  -r     Use RAW AAC output file.\n");
#ifdef HAVE_LIBMP4V2
        printf("\n");
        printf("MP4 specific options:\n");
	printf("  -w     Wrap AAC data in MP4 container. (default for *.mp4 and *.m4a)\n");
        printf("  -A X   Set artist to X\n");
        printf("  -T X   Set title to X\n");
        printf("  -G X   Set genre to X\n");
        printf("  -L X   Set album to X\n");
        printf("  -N X   Set track to X (number/total)\n");
        printf("  -D X   Set date to X\n");
        printf("  -M X   Set comment to X\n");
        printf("\n");
#else
        printf("\n");
        printf("MP4 support unavailable.\n");
#endif
        //printf("More details on FAAC usage can be found in the faac.html file.\n");
        printf("More tips on FAAC usage can be found in Knowledge base at www.audiocoding.com\n");

        return 1;
    }

    /* point to the specified file names */
    audioFileName = argv[optind++];
    aacFileName = argv[optind++];
    aacFileExt = strrchr(aacFileName, '.');

    /* open the audio input file */
    if (rawChans > 0) // use raw input
    {
        infile = wav_open_read(audioFileName, 1);
        if (infile)
        {
	  infile->bigendian = rawEndian;
            infile->channels = rawChans;
            infile->samplebytes = rawBits / 8;
            infile->samplerate = rawRate;
	    infile->samples /= (infile->channels * infile->samplebytes);
        }
    }
    else // header input
        infile = wav_open_read(audioFileName, 0);

    if (infile == NULL)
    {
        fprintf(stderr, "Couldn't open input file %s\n", audioFileName);
        return 1;
    }


    /* open the encoder library */
    hEncoder = faacEncOpen(infile->samplerate, infile->channels,
        &samplesInput, &maxBytesOutput);


    /* check file extension */
    if (aacFileExt && 
	(!strcmp(".m4a", aacFileExt) || !strcmp(".mp4", aacFileExt))) 
#ifndef HAVE_LIBMP4V2
        printf("WARNING: MP4 support unavailable!\n");
#else
    {
        container = MP4_CONTAINER;
    }

    if (container == MP4_CONTAINER)
    {
        mpegVersion = MPEG4;
	stream = RAW_STREAM;
    }

    frameSize = samplesInput/infile->channels;
    delay_samples = frameSize; // encoder delay 1024 samples
#endif
    pcmbuf = (float *)malloc(samplesInput*sizeof(float));
    bitbuf = (unsigned char*)malloc(maxBytesOutput*sizeof(unsigned char));
    chanmap = mkChanMap(infile->channels, chanC, chanLF);
    if (chanmap)
    {
        fprintf(stderr, "Remapping input channels: Center=%d, LFE=%d\n",
            chanC, chanLF);
    }

    if (cutOff <= 0)
    {
        if (cutOff < 0) // default
            cutOff = 0;
        else // disabled
            cutOff = infile->samplerate / 2;
    }
    if (cutOff > (infile->samplerate / 2))
        cutOff = infile->samplerate / 2;

    /* put the options in the configuration struct */
    myFormat = faacEncGetCurrentConfiguration(hEncoder);
    myFormat->aacObjectType = objectType;
    myFormat->mpegVersion = mpegVersion;
    myFormat->useTns = useTns;
    switch (shortctl)
    {
    case SHORTCTL_NOSHORT:
      fprintf(stderr, "disabling short blocks\n");
      myFormat->shortctl = shortctl;
      break;
    case SHORTCTL_NOLONG:
      fprintf(stderr, "disabling long blocks\n");
      myFormat->shortctl = shortctl;
      break;
    }
    if (infile->channels >= 6)
        myFormat->useLfe = 1;
    myFormat->allowMidside = useMidSide;
    if (bitRate)
        myFormat->bitRate = bitRate;
    myFormat->bandWidth = cutOff;
    if (quantqual > 0)
        myFormat->quantqual = quantqual;
    myFormat->outputFormat = stream == ADTS_STREAM;
    myFormat->inputFormat = FAAC_INPUT_FLOAT;
    if (!faacEncSetConfiguration(hEncoder, myFormat)) {
        fprintf(stderr, "Unsupported output format!\n");
#ifdef HAVE_LIBMP4V2
        if (container == MP4_CONTAINER) MP4Close(MP4hFile);
#endif
        return 1;
    }

#ifdef HAVE_LIBMP4V2
    /* initialize MP4 creation */
    if (container == MP4_CONTAINER) {
        u_int8_t *ASC = 0;
        u_int32_t ASCLength = 0;
	char *version_string;

        MP4hFile = MP4Create(aacFileName, 0, 0, 0);
        if (MP4hFile == MP4_INVALID_FILE_HANDLE) {
            fprintf(stderr, "Couldn't create output file %s\n", aacFileName);
            return 1;
        }

        MP4SetTimeScale(MP4hFile, 90000);
        MP4track = MP4AddAudioTrack(MP4hFile, infile->samplerate, MP4_INVALID_DURATION, MP4_MPEG4_AUDIO_TYPE);
        MP4SetAudioProfileLevel(MP4hFile, 0x0F);
        faacEncGetDecoderSpecificInfo(hEncoder, &ASC, &ASCLength);
        MP4SetTrackESConfiguration(MP4hFile, MP4track, ASC, ASCLength);

	/* set metadata */
	version_string = malloc(strlen(faac_id_string) + 6);
	strcpy(version_string, "FAAC ");
	strcpy(version_string + 5, faac_id_string);
	MP4SetMetadataTool(MP4hFile, version_string);
	free(version_string);

	if (artist) MP4SetMetadataArtist(MP4hFile, artist);
	if (title) MP4SetMetadataName(MP4hFile, title);
	if (album) MP4SetMetadataAlbum(MP4hFile, album);
	if (trackno > 0) MP4SetMetadataTrack(MP4hFile, trackno, ntracks);
	if (date > 0) MP4SetMetadataYear(MP4hFile, date);
	if (genre) MP4SetMetadataGenre(MP4hFile, genre);
	if (comment) MP4SetMetadataComment(MP4hFile, genre);
    }
    else
    {
#endif
        /* open the aac output file */
        outfile = fopen(aacFileName, "wb");
        if (!outfile)
        {
            fprintf(stderr, "Couldn't create output file %s\n", aacFileName);
            return 1;
        }
#ifdef HAVE_LIBMP4V2
    }
#endif

    cutOff = myFormat->bandWidth;
    quantqual = myFormat->quantqual;
    bitRate = myFormat->bitRate;
    if (bitRate)
      fprintf(stderr, "Average bitrate: %d kbps/channel\n",
	      (bitRate + 500)/1000);
    fprintf(stderr, "Quantization quality: %ld\n", quantqual);
    fprintf(stderr, "Bandwidth: %d Hz\n", cutOff);
    fprintf(stderr, "Object type: ");
    switch(objectType)
    {
    case LOW:
        fprintf(stderr, "Low Complexity");
        break;
    case MAIN:
        fprintf(stderr, "Main");
        break;
    case LTP:
        fprintf(stderr, "LTP");
        break;
    }
    fprintf(stderr, "(MPEG-%d)", (mpegVersion == MPEG4) ? 4 : 2);
    if (myFormat->useTns)
        fprintf(stderr, " + TNS");
    if (myFormat->allowMidside)
        fprintf(stderr, " + M/S");
    fprintf(stderr, "\n");

    fprintf(stderr, "File format: ");
    switch(container)
    {
    case NO_CONTAINER:
      switch(stream)
	{
	case RAW_STREAM:
	  fprintf(stderr, "Headerless AAC (RAW)\n");
	  break;
	case ADTS_STREAM:
	  fprintf(stderr, "MPEG-2 AAC (ADTS)\n");
	  break;
	}
        break;
#ifdef HAVE_LIBMP4V2
    case MP4_CONTAINER:
        fprintf(stderr, "MPEG-4 File Format (MP4)\n");
        break;
#endif
    }

    if (outfile
#ifdef HAVE_LIBMP4V2
        || MP4hFile != MP4_INVALID_FILE_HANDLE
#endif
       )
    {
        int showcnt = 0;
#ifdef _WIN32
        long begin = GetTickCount();
#endif
        if (infile->samples)
            frames = ((infile->samples + 1023) / 1024) + 1;
        else
            frames = 0;
        currentFrame = 0;

        fprintf(stderr, "Encoding %s to %s\n", audioFileName, aacFileName);
        if (frames != 0)
            fprintf(stderr, "   frame          | bitrate | elapsed/estim | "
		    "play/CPU | ETA\n");
        else
            fprintf(stderr, " frame | elapsed | play/CPU\n");

        /* encoding loop */
        while (running)
        {
            int bytesWritten;

            samplesRead = wav_read_float32(infile, pcmbuf, samplesInput, chanmap);

#ifdef HAVE_LIBMP4V2
            total_samples += samplesRead / infile->channels;
#endif

            /* call the actual encoding routine */
            bytesWritten = faacEncEncode(hEncoder,
                (int32_t *)pcmbuf,
                samplesRead,
                bitbuf,
                maxBytesOutput);

            if (bytesWritten)
            {
                currentFrame++;
                showcnt--;
		totalBytesWritten += bytesWritten;
            }

            if ((showcnt <= 0) || !bytesWritten)
            {
                double timeused;
#ifdef __unix__
                struct rusage usage;
#endif
#ifdef _WIN32
                char percent[MAX_PATH + 20];
                timeused = (GetTickCount() - begin) * 1e-3;
#else
#ifdef __unix__
                if (getrusage(RUSAGE_SELF, &usage) == 0) {
                    timeused = (double)usage.ru_utime.tv_sec +
                        (double)usage.ru_utime.tv_usec * 1e-6;
                }
                else
                    timeused = 0;
#else
                timeused = (double)clock() * (1.0 / CLOCKS_PER_SEC);
#endif
#endif
                if (currentFrame && (timeused > 0.1))
                {
                    showcnt += 50;

                    if (frames != 0)
                        fprintf(stderr,
                            "\r%5d/%-5d (%3d%%)|  %5.1f  | %6.1f/%-6.1f | %7.2fx | %.1f ",
                            currentFrame, frames, currentFrame*100/frames,
			    ((double)totalBytesWritten * 8.0 / 1000.0) /
			    ((double)infile->samples / infile->samplerate * currentFrame / frames),
                            timeused,
                            timeused * frames / currentFrame,
                            (1024.0 * currentFrame / infile->samplerate) / timeused,
                            timeused  * (frames - currentFrame) / currentFrame);
                    else
                        fprintf(stderr,
                            "\r %5d |  %6.1f | %7.2fx ",
                            currentFrame,
                            timeused,
                            (1024.0 * currentFrame / infile->samplerate) / timeused);

                    fflush(stderr);
#ifdef _WIN32
                    if (frames != 0)
                    {
                        sprintf(percent, "%.2f%% encoding %s",
                            100.0 * currentFrame / frames, audioFileName);
                        SetConsoleTitle(percent);
                    }
#endif
                }
            }

            /* all done, bail out */
            if (!samplesRead && !bytesWritten)
                break ;

            if (bytesWritten < 0)
            {
                fprintf(stderr, "faacEncEncode() failed\n");
                break ;
            }

            if (bytesWritten > 0)
            {
#ifdef HAVE_LIBMP4V2
                u_int64_t samples_left = total_samples - encoded_samples + delay_samples;
                MP4Duration dur = samples_left > frameSize ? frameSize : samples_left;
                MP4Duration ofs = encoded_samples > 0 ? 0 : delay_samples;

                if (container == MP4_CONTAINER)
                {
                    /* write bitstream to mp4 file */
                    MP4WriteSample(MP4hFile, MP4track, bitbuf, bytesWritten, dur, ofs, 1);
                }
                else
                {
#endif
                    /* write bitstream to aac file */
                    fwrite(bitbuf, 1, bytesWritten, outfile);
#ifdef HAVE_LIBMP4V2
                }

                encoded_samples += dur;
#endif
            }
        }
        fprintf(stderr, "\n\n");

#ifdef HAVE_LIBMP4V2
        /* clean up */
        if (container == MP4_CONTAINER)
            MP4Close(MP4hFile);
        else
#endif
            fclose(outfile);
    }

    faacEncClose(hEncoder);

    wav_close(infile);

    if (pcmbuf) free(pcmbuf);
    if (bitbuf) free(bitbuf);

    return 0;
}

/*
$Log: main.c,v $
Revision 1.57  2004/03/17 13:32:13  danchr
- New signal handler. Disabled on Windows, although it *should* work.
- Separated handling of stream format and output format.
- Bitrate + file format displayed on stderr.
- knik and myself added to the copyright header.

Revision 1.56  2004/03/15 20:15:48  knik
improved MP4 support by Dan Christiansen

Revision 1.55  2004/03/03 15:54:50  knik
libmp4v2 autoconf detection and mp4 metadata support by Dan Christiansen

Revision 1.54  2004/02/14 10:31:23  knik
Print help and exit when unknown option is specified.

Revision 1.53  2003/12/20 04:32:48  stux
i've added sms00's OSX patch to faac

Revision 1.52  2003/12/14 12:25:44  ca5e
Gapless MP4 handling method changed again...

Revision 1.51  2003/12/13 21:58:35  ca5e
Improved gapless encoding

Revision 1.50  2003/11/24 18:11:14  knik
using new version info interface

Revision 1.49  2003/11/13 18:30:19  knik
raw input bugfix

Revision 1.48  2003/10/17 17:11:18  knik
fixed raw input

Revision 1.47  2003/10/12 14:30:29  knik
more accurate average bitrate control

Revision 1.46  2003/09/24 16:30:34  knik
Added option to enforce block type.

Revision 1.45  2003/09/08 16:28:21  knik
conditional libmp4v2 compilation

Revision 1.44  2003/09/07 17:44:36  ca5e
length calculations/silence padding changed to match current libfaac behavior
changed tabs to spaces, fixes to indentation

Revision 1.43  2003/08/17 19:38:15  menno
fixes to MP4 files by Case

Revision 1.41  2003/08/15 11:43:14  knik
Option to add a number of silent frames at the end of output.
Small TNS option fix.

Revision 1.40  2003/08/07 11:28:06  knik
fixed win32 crash with long filenames

Revision 1.39  2003/07/21 16:33:49  knik
Fixed LFE channel mapping.

Revision 1.38  2003/07/13 08:34:43  knik
Fixed -o, -m and -I option.
Print object type setting.

Revision 1.37  2003/07/10 19:19:32  knik
Input channel remapping and 24-bit support.

Revision 1.36  2003/06/26 19:40:53  knik
TNS disabled by default.
Copyright info moved to library.
Print help to standard output.

Revision 1.35  2003/06/21 08:59:31  knik
raw input support moved to input.c

Revision 1.34  2003/05/10 09:40:35  knik
added approximate ABR option

Revision 1.33  2003/04/13 08:39:28  knik
removed psymodel setting

Revision 1.32  2003/03/27 17:11:06  knik
updated library interface
-b bitrate option replaced with -q quality option
TNS enabled by default

Revision 1.31  2002/12/23 19:02:43  knik
added some headers

Revision 1.30  2002/12/15 15:16:55  menno
Some portability changes

Revision 1.29  2002/11/23 17:34:59  knik
replaced libsndfile with input.c
improved bandwidth/bitrate calculation formula

Revision 1.28  2002/08/30 16:20:45  knik
misplaced #endif

Revision 1.27  2002/08/19 16:33:54  knik
automatic bitrate setting
more advanced status line

*/
