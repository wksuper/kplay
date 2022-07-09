/* kplay - A WAV File Player with Real-Time Sound Tuning
 *
 * This is the main function for the kplay command line program.
 *
 * Copyright (C) 2022  Kui Wang
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <lark/lark.h>
#include <cstdio>
#include <unistd.h>
#include <termios.h>
#include <fstream>
#include <mutex>
#include <cstring>

#define CONSOLE_PRINT(...) kloga( \
    KLOGGING_TO_STDERR | KLOGGING_NO_TIMESTAMP | KLOGGING_NO_LOGTYPE | KLOGGING_NO_SOURCEFILE | KLOGGING_FLUSH_IMMEDIATELY, \
    KLOGGING_TO_STDOUT, NULL, __VA_ARGS__)

#define STATUS_PRINT(...) kloga( \
    KLOGGING_TO_STDERR | KLOGGING_NO_TIMESTAMP | KLOGGING_NO_LOGTYPE | KLOGGING_NO_SOURCEFILE | KLOGGING_FLUSH_IMMEDIATELY, \
    KLOGGING_TO_STDOUT, "", "\r" __VA_ARGS__)

#if defined(__APPLE__)
#define SUFFIX ".dylib"
#elif defined(_WIN32)
#define SUFFIX ".dll"
#else
#define SUFFIX ".so"
#endif

#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164
#define FORMAT_PCM 1

#define BLKSOUNDTOUCH_PARAMID_PITCH  (1)
#define BLKSOUNDTOUCH_PARAMID_TEMPO  (2)
#define BLKGAIN_PARAMID_GAIN         (1)

struct wav_header {
	uint32_t riff_id;
	uint32_t riff_sz;
	uint32_t riff_fmt;
	uint32_t fmt_id;
	uint32_t fmt_sz;
	uint16_t audio_format;
	uint16_t num_channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	uint32_t data_id;
	uint32_t data_sz;
};

enum Output { PORTAUDIO, ALSA, TINYALSA, STDOUT, NULLDEV };

class WavFile : public lark::DataProducer {
public:
    WavFile(const char *wavFileName);
    void SeekToBegin();

    operator bool() const
    {
        return (!!m_fin) && m_sampleSize;
    }

    const struct wav_header &Header() const
    {
        return m_header;
    }

private:
    virtual int Produce(void *data, lark::samples_t samples, bool blocking, int64_t *timestamp) override;

    std::ifstream m_fin;
    struct wav_header m_header;
    size_t m_sampleSize;
    std::mutex m_mutex;
};

WavFile::WavFile(const char *wavFileName)
    : m_fin(wavFileName, std::ifstream::binary), m_sampleSize(0)
{
	if (!m_fin) {
		CONSOLE_PRINT("Unable to open %s", wavFileName);
		return;
	}

	if (!m_fin.read((char *)&m_header, sizeof(m_header))) {
		CONSOLE_PRINT("Unable to read riff/wave header");
        return;
	}

	if ((m_header.riff_id != ID_RIFF) ||
	    (m_header.riff_fmt != ID_WAVE)) {
		CONSOLE_PRINT("Not a riff/wave header");
        return;
	}

    if (m_header.audio_format != FORMAT_PCM) {
		CONSOLE_PRINT("Not PCM format");
        return;
    }

    if (memcmp(&m_header.data_id, "data", 4) != 0) {
		CONSOLE_PRINT("No data chunk\n");
        return;
    }

    if (m_header.num_channels > 2) {
		CONSOLE_PRINT("Can't support %u channels\n", m_header.num_channels);
        CONSOLE_PRINT("Mono and stereo are supported");
        return;
    }

    m_sampleSize = m_header.bits_per_sample / 8 * m_header.num_channels;

	m_fin.seekg(sizeof(struct wav_header), std::ios::beg);
}

int WavFile::Produce(void *data, lark::samples_t samples, bool blocking, int64_t *timestamp)
{
    if (timestamp)
        *timestamp = -1;

    std::lock_guard<std::mutex> _l(m_mutex);
    if (m_fin.read((char *)data, m_sampleSize * samples))
        return samples;
    else
        return lark::E_EOF;
}

void WavFile::SeekToBegin()
{
    std::lock_guard<std::mutex> _l(m_mutex);
    m_fin.clear();
    m_fin.seekg(sizeof(struct wav_header), std::ios::beg);
}

class Player : public lark::Route::Callbacks {
public:
    int Go(int argc, char *argv[]);

private:
    virtual void OnStarted(lark::Route *route) override;
    virtual void OnStopped(lark::Route *route) override;

    inline void RefreshDisplay() const
    {
        if (m_chNum == 2) {
            STATUS_PRINT("L-CH VOLUME: %-8g R-CH VOLUME: %-8g %s       PITCH: %-8g  TEMPO: %-8g    %-7s ",
                m_volL * m_volMaster, m_volR * m_volMaster, m_mute ? "MUTED" : "     ", m_pitch, m_tempo, m_routeStatus);
        } else {
            STATUS_PRINT("MONO-CH VOLUME: %-8g                    %s       PITCH: %-8g  TEMPO: %-8g    %-7s ",
                m_volMaster, m_mute ? "MUTED" : "     ", m_pitch, m_tempo, m_routeStatus);
        }
    }

    double m_pitch = 1.0;
    const double PITCH_MIN = 0.1;
    const double PITCH_MAX = 100.0;

    double m_tempo = 1.0;
    const double TEMPO_MIN = 0.1;
    const double TEMPO_MAX = 30.0;

    double m_volL = 1.0;
    double m_volR = 1.0;
    double m_volMaster = 1.0;

    bool m_mute = false;

    unsigned int m_chNum = 0;
    const char *m_routeStatus = nullptr;
};

int Player::Go(int argc, char *argv[])
{
	if (argc < 2) {
		CONSOLE_PRINT(
            "Usage: kplay [-o OUTPUT] [-s SAVINGFILE] [-v VOLUME] [-p PITCH] [-t TEMPO] WAVFILE\n"
            "\n"
            "Mandatory argument\n"
            "WAVFILE                    The wav file to play\n"
            "\n"
            "Optional arguments\n"
            "-o OUTPUT                  One of portaudio|alsa|tinyalsa|stdout|null\n"
            "                           that audio will output to (default portaudio)\n"
            "-s SAVINGFILE              The file that audio will be saved to while playback\n"
            "-v VOLUME                  The initial volume (default 1.0)\n"
            "-p PITCH                   The initial pitch (default 1.0)\n"
            "-t TEMPO                   The initial tempo (default 1.0)");
		return 0;
	}

    int ch;
    std::string savingFile;
    Output output = PORTAUDIO;
    while ((ch = getopt(argc, argv, "o:s:v:p:t:")) != -1) {
        switch (ch) {
        case 'o':
            if (strcmp(optarg, "stdout") == 0) {
                output = STDOUT;
            } else if (strcmp(optarg, "portaudio") == 0) {
                output = PORTAUDIO;
            } else if (strcmp(optarg, "alsa") == 0) {
                output = ALSA;
            } else if (strcmp(optarg, "tinyalsa") == 0) {
                output = TINYALSA;
            } else if (strcmp(optarg, "null") == 0) {
                output = NULLDEV;
            } else {
                CONSOLE_PRINT("Invalid -o argument: %s", optarg);
                CONSOLE_PRINT("Supported -o arguments: portaudio, alsa, tinyalsa, stdout");
                return -1;
            }
            break;
        case 's':
            savingFile = optarg;
            break;
        case 'v':
            m_volMaster = atof(optarg);
            if (m_volMaster < 0.0) {
                CONSOLE_PRINT("Invalid -v argument: %s", optarg);
                return -1;
            }
            if (m_volMaster == 0.0) {
                m_mute = true;
            } else if (m_volMaster > 1.0) {
                CONSOLE_PRINT("'-v %g' is too high, defaulting to 1.0", m_volMaster);
                m_volMaster = 1.0;
            }
            break;
        case 'p':
            m_pitch = atof(optarg);
            if (m_pitch == 0.0) {
                CONSOLE_PRINT("Invalid -p argument: %s", optarg);
                return -1;
            }
            if (m_pitch > PITCH_MAX) {
                CONSOLE_PRINT("'-p %g' is too high, defaulting to %g", m_pitch, PITCH_MAX);
                m_pitch = PITCH_MAX;
            } else if (m_pitch < PITCH_MIN) {
                CONSOLE_PRINT("'-p %g' is too low, defaulting to %g", m_pitch, PITCH_MIN);
                m_pitch = PITCH_MIN;
            }
            break;
        case 't':
            m_tempo = atof(optarg);
            if (m_tempo == 0.0) {
                CONSOLE_PRINT("Invalid -t argument: %s", optarg);
                return -1;
            }
            if (m_tempo > TEMPO_MAX) {
                CONSOLE_PRINT("'-t %g' is too fast, defaulting to %g", m_tempo, TEMPO_MAX);
                m_tempo = TEMPO_MAX;
            } else if (m_tempo < TEMPO_MIN) {
                CONSOLE_PRINT("'-t %g' is too slow, defaulting to %g", m_tempo, TEMPO_MIN);
                m_tempo = TEMPO_MIN;
            }
            break;
        default:
            break;
        }
    }

    if (!argv[optind]) {
        CONSOLE_PRINT("Missing WAVFILE");
        return -1;
    }

    WavFile wav(argv[optind]);
	if (!wav)
		return -1;

    const struct wav_header &header = wav.Header();
    lark::SampleFormat format = lark::SampleFormat::BYTE;
	switch (header.bits_per_sample) {
	case 32:
        format = lark::SampleFormat_S32;
		break;
	case 24:
		format = lark::SampleFormat_S24_3;
		break;
	case 16:
		format = lark::SampleFormat_S16;
		break;
	default:
		CONSOLE_PRINT("%u bits is not supported", header.bits_per_sample);
		return -1;
	}
    m_chNum = header.num_channels;
	const unsigned int rate = header.sample_rate;
    const lark::samples_t frameSizeInSamples = 20/*ms*/ * rate / 1000;

    // Disable lark logging to either stdout or stderr
    lark::Lark::Instance();
    KLOG_DISABLE_OPTIONS(KLOGGING_TO_STDOUT | KLOGGING_TO_STDERR);

    // Create the playback route named RouteA
    lark::Route *route = lark::Lark::Instance().NewRoute("RouteA", this);
    if (!route) {
        CONSOLE_PRINT("Failed to create route");
        return -1;
    }

    // Create RouteA's blocks
    const char *soFileName = "libblkstreamin" SUFFIX;
    lark::Parameters args;
    lark::DataProducer *producer = &wav;
    producer->SetBlocking(true);
    args.clear();
    args.push_back(std::to_string((unsigned long)producer));
    lark::Block *blkStreamIn = route->NewBlock(soFileName, true, false, args);
    if (!blkStreamIn) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        return -1;
    }

    soFileName = "libblkgain" SUFFIX;
    lark::Block *blkGain = route->NewBlock(soFileName, false, false);
    if (!blkGain) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        return -1;
    }
    args.clear();
    args.push_back("0");
    args.push_back(std::to_string(m_volL * m_volMaster * (m_mute ? 0.0 : 1.0)));
    if (m_chNum == 2) {
        args.push_back("1");
        args.push_back(std::to_string(m_volR * m_volMaster * (m_mute ? 0.0 : 1.0)));
    }
    route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);

    lark::Block *blkDeinterleave = nullptr;
    lark::Block *blkInterleave = nullptr;
    if (m_chNum == 2) {
        soFileName = "libblkdeinterleave" SUFFIX;
        blkDeinterleave = route->NewBlock(soFileName, false, false);
        if (!blkDeinterleave) {
            CONSOLE_PRINT("Failed to new a block from %s", soFileName);
            return -1;
        }

        soFileName = "libblkinterleave" SUFFIX;
        blkInterleave = route->NewBlock(soFileName, false, false);
        if (!blkInterleave) {
            CONSOLE_PRINT("Failed to new a block from %s", soFileName);
            return -1;
        }
    }

    soFileName = "libblkformatadapter" SUFFIX;
    lark::Block *blkFormatAdapter = route->NewBlock(soFileName, false, false);
    if (!blkFormatAdapter) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        return -1;
    }

    soFileName = "libblksoundtouch" SUFFIX;
    lark::Block *blkSoundTouch = route->NewBlock(soFileName, false, false);
    if (!blkSoundTouch) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        return -1;
    }
    args.clear();
    args.push_back(std::to_string(m_pitch));
    route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_PITCH, args);
    args.clear();
    args.push_back(std::to_string(m_tempo));
    route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_TEMPO, args);

    soFileName = "libblkformatadapter" SUFFIX;
    lark::Block *blkFormatAdapter1 = route->NewBlock(soFileName, false, false);
    if (!blkFormatAdapter1) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        return -1;
    }

    lark::Block *blkOutput = nullptr;
    switch (output) {
    case PORTAUDIO:
        soFileName = "libblkpaplayback" SUFFIX;
        blkOutput = route->NewBlock(soFileName, false, true);
        break;
    case ALSA:
        soFileName = "libblkalsaplayback" SUFFIX;
        blkOutput = route->NewBlock(soFileName, false, true);
        break;
    case TINYALSA:
        soFileName = "libblktinyalsaplayback" SUFFIX;
        blkOutput = route->NewBlock(soFileName, false, true);
        break;
    case STDOUT:
        soFileName = "libblkfilewriter" SUFFIX;
        args.clear();
        args.push_back("--"); // stdout
        blkOutput = route->NewBlock(soFileName, false, true, args);
        break;
    case NULLDEV:
        soFileName = "libblkfilewriter" SUFFIX;
        args.clear();
        args.push_back("/dev/null");
        blkOutput = route->NewBlock(soFileName, false, true, args);
        break;
    default:
        return -1;
    }
    if (!blkOutput) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        return -1;
    }

    // Create RouteA's links
    if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkStreamIn, 0, blkFormatAdapter, 0)) {
        CONSOLE_PRINT("Failed to new a link");
        return -1;
    }
    if (m_chNum == 2) { // stereo
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkFormatAdapter, 0, blkDeinterleave, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkDeinterleave, 0, blkGain, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkDeinterleave, 1, blkGain, 1)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkGain, 0, blkInterleave, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkGain, 1, blkInterleave, 1)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkInterleave, 0, blkSoundTouch, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
    } else { // mono
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkFormatAdapter, 0, blkGain, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkGain, 0, blkSoundTouch, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
    }
    if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkSoundTouch, 0, blkFormatAdapter1, 0)) {
        CONSOLE_PRINT("Failed to new a link");
        return -1;
    }

    if (savingFile != "") {
        soFileName = "libblkfilewriter" SUFFIX;
        args.clear();
        args.push_back(savingFile);
        lark::Block *blkFileWriter = route->NewBlock(soFileName, false, true, args);
        if (!blkFileWriter) {
            CONSOLE_PRINT("Failed to new a block from %s", soFileName);
            return -1;
        }

        soFileName = "libblkduplicator" SUFFIX;
        lark::Block *blkDuplicator = route->NewBlock(soFileName, false, false);
        if (!blkDuplicator) {
            CONSOLE_PRINT("Failed to new a block from %s", soFileName);
            return -1;
        }

        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkFormatAdapter1, 0, blkDuplicator, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkDuplicator, 0, blkOutput, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkDuplicator, 1, blkFileWriter, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
    } else {
        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkFormatAdapter1, 0, blkOutput, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            return -1;
        }
    }

    if (m_chNum == 2) {
        CONSOLE_PRINT(
            "*****************************************************************************************************\n"
            "* [q] Balance Left  [w] Balance Mid  [e] Balance Right  [r] Pitch High   [t] Tempo Fast   |  KPLAY  *\n"
            "* [a] Volume Down   [s] Volume Up    [d] Mute/Unmute    [f] Pitch Low    [g] Tempo Slow   | POWERED *\n"
            "* [z] Re-start      [x] Stop         [c] Exit           [v] Pitch Reset  [b] Tempo Reset  | BY LARK *\n"
            "*****************************************************************************************************");
    } else {
        CONSOLE_PRINT(
            "*****************************************************************************************************\n"
            "*                                                       [r] Pitch High   [t] Tempo Fast   |  KPLAY  *\n"
            "* [a] Volume Down   [s] Volume Up    [d] Mute/Unmute    [f] Pitch Low    [g] Tempo Slow   | POWERED *\n"
            "* [z] Re-start      [x] Stop         [c] Exit           [v] Pitch Reset  [b] Tempo Reset  | BY LARK *\n"
            "*****************************************************************************************************");
    }

    struct termios attr;
    tcgetattr(0, &attr);
    attr.c_lflag &= ~(ICANON | ECHO);
    attr.c_cc[VTIME] = 0;
    attr.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &attr);

    // Start
    if (route->Start() < 0) {
        CONSOLE_PRINT("Failed to start route");
        return -1;
    }

    while (1) {
        this->RefreshDisplay();
        int c = getchar();
        if (c == 'c')  // Exit
            break;
        switch (c) {
        case 'x':  // Stop
            route->Stop();
            break;
        case 'z':  // Re-start
            wav.SeekToBegin();
            route->Start();
            break;
        case 'r':  // Pitch High
            if (m_pitch >= PITCH_MAX)
                break;
            m_pitch = std::min(m_pitch * 1.01, PITCH_MAX);
            args.clear();
            args.push_back(std::to_string(m_pitch));
            route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_PITCH, args);
            break;
        case 'f':  // Pitch Low
            if (m_pitch <= PITCH_MIN)
                break;
            m_pitch = std::max(m_pitch * 0.99, PITCH_MIN);
            args.clear();
            args.push_back(std::to_string(m_pitch));
            route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_PITCH, args);
            break;
        case 'v':  // Pitch Reset
            m_pitch = 1.0;
            args.clear();
            args.push_back(std::to_string(m_pitch));
            route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_PITCH, args);
            break;
        case 't':  // Tempo Fast
            if (m_tempo >= TEMPO_MAX)
                break;
            m_tempo = std::min(m_tempo * 1.01, TEMPO_MAX);
            args.clear();
            args.push_back(std::to_string(m_tempo));
            route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_TEMPO, args);
            break;
        case 'g':  // Tempo Slow
            if (m_tempo <= TEMPO_MIN)
                break;
            m_tempo = std::max(m_tempo * 0.99, TEMPO_MIN);
            args.clear();
            args.push_back(std::to_string(m_tempo));
            route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_TEMPO, args);
            break;
        case 'b':  // Tempo Reset
            m_tempo = 1.0;
            args.clear();
            args.push_back(std::to_string(m_tempo));
            route->SetParameter(blkSoundTouch, BLKSOUNDTOUCH_PARAMID_TEMPO, args);
            break;
        case 'e':  // Balance Right
            if (m_chNum == 1)
                break;
            if (m_volR < 1.0) {
                m_volR = std::min(m_volR + 0.01, 1.0);
                args.clear();
                args.push_back("1");
                args.push_back(std::to_string(m_volR * m_volMaster * (m_mute ? 0.0 : 1.0)));
                route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            } else { // m_volR == 1.0
                if (m_volL == 0.0)
                    break;
                m_volL = std::max(m_volL - 0.01, 0.0);
                args.clear();
                args.push_back("0");
                args.push_back(std::to_string(m_volL * m_volMaster * (m_mute ? 0.0 : 1.0)));
                route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            }
            break;
        case 'q':  // Balance Left
            if (m_chNum == 1)
                break;
            if (m_volL < 1.0) {
                m_volL = std::min(m_volL + 0.01, 1.0);
                args.clear();
                args.push_back("0");
                args.push_back(std::to_string(m_volL * m_volMaster * (m_mute ? 0.0 : 1.0)));
                route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            } else { // m_volL == 1.0
                if (m_volR == 0.0)
                    break;
                m_volR = std::max(m_volR - 0.01, 0.0);
                args.clear();
                args.push_back("1");
                args.push_back(std::to_string(m_volR * m_volMaster * (m_mute ? 0.0 : 1.0)));
                route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            }
            break;
        case 'w':  // Balance Mid
            if (m_chNum == 1)
                break;
            if (m_volL == 1.0 && m_volR == 1.0)
                break;
            m_volL = m_volR = 1.0;
            args.clear();
            args.push_back("0");
            args.push_back(std::to_string(m_volL * m_volMaster * (m_mute ? 0.0 : 1.0)));
            if (m_chNum == 2) {
                args.push_back("1");
                args.push_back(std::to_string(m_volR * m_volMaster * (m_mute ? 0.0 : 1.0)));
            }
            route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            break;
        case 'd':  // Mute/Unmute
            m_mute = !m_mute;
            args.clear();
            args.push_back("0");
            args.push_back(std::to_string(m_volL * m_volMaster * (m_mute ? 0.0 : 1.0)));
            if (m_chNum == 2) {
                args.push_back("1");
                args.push_back(std::to_string(m_volR * m_volMaster * (m_mute ? 0.0 : 1.0)));
            }
            route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            break;
        case 'a':  // Volume Down
            if (m_volMaster == 0.0)
                break;
            m_volMaster = std::max(m_volMaster - 0.01, 0.0);
            m_mute = (m_volMaster == 0.0);
            args.clear();
            args.push_back("0");
            args.push_back(std::to_string(m_volL * m_volMaster));
            if (m_chNum == 2) {
                args.push_back("1");
                args.push_back(std::to_string(m_volR * m_volMaster));
            }
            route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            break;
        case 's':  // Volume Up
            if (m_volMaster == 1.0)
                break;
            m_volMaster = std::min(m_volMaster + 0.01, 1.0);
            m_mute = (m_volMaster == 0.0);
            args.clear();
            args.push_back("0");
            args.push_back(std::to_string(m_volL * m_volMaster));
            if (m_chNum == 2) {
                args.push_back("1");
                args.push_back(std::to_string(m_volR * m_volMaster));
            }
            route->SetParameter(blkGain, BLKGAIN_PARAMID_GAIN, args);
            break;
        default:
            break;
        }
    }

    lark::Lark::Instance().DeleteRoute(route);

    CONSOLE_PRINT("");

    return 0;
}

void Player::OnStarted(lark::Route *route)
{
    m_routeStatus = "PLAYING";
    this->RefreshDisplay();
}

void Player::OnStopped(lark::Route *route)
{
    m_routeStatus = "STOPPED";
    this->RefreshDisplay();
}

int main(int argc, char *argv[])
{
    Player player;
    return player.Go(argc, argv);
}
