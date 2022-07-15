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
#include <klogging.h>
#include <unistd.h>
#include <termios.h>
#include <fstream>
#include <mutex>
#include <cstring>

static const char *__version = "0.2";

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

class Player;

class WavFile : public lark::DataProducer {
public:
    WavFile(Player *player) : m_player(player)
    {
        memset(&m_header, 0, sizeof(m_header));
    }
    int Open(const char *wavFileName);
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
    long m_pcmBytes = 0;
    struct wav_header m_header;
    size_t m_sampleSize = 0;
    std::mutex m_mutex;
    Player *m_player;
};

int WavFile::Open(const char *wavFileName)
{
    m_fin.open(wavFileName, std::ifstream::binary);
    if (!m_fin) {
        CONSOLE_PRINT("Unable to open %s", wavFileName);
        return -1;
    }

    if (!m_fin.read((char *)&m_header, sizeof(m_header))) {
        CONSOLE_PRINT("Unable to read riff/wave header");
        return -1;
    }

    if ((m_header.riff_id != ID_RIFF) ||
            (m_header.riff_fmt != ID_WAVE)) {
        CONSOLE_PRINT("Not a riff/wave header");
        return -1;
    }

    if (m_header.audio_format != FORMAT_PCM) {
        CONSOLE_PRINT("Not PCM format");
        return -1;
    }

    if (memcmp(&m_header.data_id, "data", 4) != 0) {
        CONSOLE_PRINT("No data chunk");
        return -1;
    }

    if (m_header.num_channels > 2) {
        CONSOLE_PRINT("Can't support %u channels", m_header.num_channels);
        CONSOLE_PRINT("Mono and stereo are supported");
        return -1;
    }

    m_sampleSize = m_header.bits_per_sample / 8 * m_header.num_channels;

	m_fin.seekg(0, std::ios::end);
    long cur = m_fin.tellg();
	m_pcmBytes = cur - sizeof(struct wav_header);

    m_fin.seekg(sizeof(struct wav_header), std::ios::beg);

    return 0;
}

void WavFile::SeekToBegin()
{
    std::lock_guard<std::mutex> _l(m_mutex);
    m_fin.clear();
    m_fin.seekg(sizeof(struct wav_header), std::ios::beg);
}

class Player : public lark::Route::Callbacks {
public:
    Player() : m_wav(this) { }
    int Go(int argc, char *argv[]);

    inline void RefreshDisplay(long progress) const
    {
        static long s_progress;

        if (progress >= 0)
            s_progress = progress;
        char prog[8];
        if (s_progress == 0 || s_progress == 10000) {
            snprintf(prog, sizeof(prog), "%5lu%%", s_progress / 100);
        } else {
            snprintf(prog, sizeof(prog), "%2lu.%02lu%%", s_progress / 100, s_progress % 100);
        }

        if (m_chNum == 2) {
            STATUS_PRINT("L-CH VOLUME: %-8g R-CH VOLUME: %-8g %-10s   PITCH: %-8g  TEMPO: %-8g    %-7s %s ",
                m_volL * m_volMaster, m_volR * m_volMaster, m_mute ? "MUTED" : "", m_pitch, m_tempo, StateString(), prog);
        } else {
            STATUS_PRINT("MONO-CH VOLUME: %-8g                    %-10s   PITCH: %-8g  TEMPO: %-8g    %-7s %s ",
                         m_volMaster, m_mute ? "MUTED" : "", m_pitch, m_tempo, StateString(), prog);
        }
    }

private:
    virtual void OnStarted() override;
    virtual void OnStopped(lark::Route::StopReason reason) override;

    inline const char *StateString() const
    {
        static const char *tbl[] = {
            "STOPPED", "PLAYING"
        };
        return tbl[m_state];
    }

    WavFile m_wav;

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

    enum State { STOPPED, PLAYING };
    State m_state = STOPPED;

    std::mutex m_mutex;
};

int WavFile::Produce(void *data, lark::samples_t samples, bool blocking, int64_t *timestamp)
{
    if (timestamp)
        *timestamp = -1;

    long cur = m_fin.tellg();
    m_player->RefreshDisplay((cur - sizeof(struct wav_header)) * 10000 / m_pcmBytes);

    std::lock_guard<std::mutex> _l(m_mutex);
    if (m_fin.read((char *)data, m_sampleSize * samples))
        return samples;
    else {
        m_player->RefreshDisplay(10000);
        return lark::E_EOF;
    }
}

int Player::Go(int argc, char *argv[])
{
    if (argc < 2) {
        CONSOLE_PRINT(
            "kplay - A WAV File Player with Real-Time Sound Tuning | Version %s\n"
            "\n"
            "Copyright (C) 2022  Kui Wang\n"
            "\n"
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
            "-t TEMPO                   The initial tempo (default 1.0)", __version);
        return 0;
    }

    std::string savingFile;
    Output output = PORTAUDIO;
    for (int ch = -1; (ch = getopt(argc, argv, "o:s:rv:p:t:")) != -1; ) {
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
                CONSOLE_PRINT("Supported -o arguments: portaudio, alsa, tinyalsa, stdout, null");
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

    int ret = m_wav.Open(argv[optind]);
    if (ret < 0)
        return ret;

    const struct wav_header &header = m_wav.Header();
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
    lark::Lark &lk = lark::Lark::Instance();
    KLOG_DISABLE_OPTIONS(KLOGGING_TO_STDOUT | KLOGGING_TO_STDERR);

    // Create the playback route named RouteA
    lark::Route *route = lk.NewRoute("RouteA", this);
    if (!route) {
        CONSOLE_PRINT("Failed to create route");
        return -1;
    }

    // Create RouteA's blocks
    const char *soFileName = "libblkstreamin" SUFFIX;
    lark::Parameters args;
    lark::DataProducer *producer = &m_wav;
    producer->SetBlocking(true);
    args.clear();
    args.push_back(std::to_string((unsigned long)producer));
    lark::Block *blkStreamIn = route->NewBlock(soFileName, true, false, args);
    if (!blkStreamIn) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        lk.DeleteRoute(route);
        return -1;
    }

    soFileName = "libblkfadein" SUFFIX;
    lark::Block *blkFadeIn = route->NewBlock(soFileName, false, false);
    if (!blkFadeIn) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        lk.DeleteRoute(route);
        return -1;
    }
    args.clear();
    args.push_back(std::to_string(0.5)); // 0.5s to fade in
    route->SetParameter(blkFadeIn, BLKFADEIN_PARAMID_FADING_TIME, args);

    soFileName = "libblkgain" SUFFIX;
    lark::Block *blkGain = route->NewBlock(soFileName, false, false);
    if (!blkGain) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        lk.DeleteRoute(route);
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
            lk.DeleteRoute(route);
            return -1;
        }

        soFileName = "libblkinterleave" SUFFIX;
        blkInterleave = route->NewBlock(soFileName, false, false);
        if (!blkInterleave) {
            CONSOLE_PRINT("Failed to new a block from %s", soFileName);
            lk.DeleteRoute(route);
            return -1;
        }
    }

    soFileName = "libblkformatadapter" SUFFIX;
    lark::Block *blkFormatAdapter = route->NewBlock(soFileName, false, false);
    if (!blkFormatAdapter) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        lk.DeleteRoute(route);
        return -1;
    }

    soFileName = "libblksoundtouch" SUFFIX;
    lark::Block *blkSoundTouch = route->NewBlock(soFileName, false, false);
    if (!blkSoundTouch) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        lk.DeleteRoute(route);
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
        lk.DeleteRoute(route);
        return -1;
    }

    soFileName = "libblkfadeout" SUFFIX;
    lark::Block *blkFadeOut = route->NewBlock(soFileName, false, false);
    if (!blkFadeOut) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        lk.DeleteRoute(route);
        return -1;
    }
    args.clear();
    args.push_back(std::to_string(0.2)); // 0.2s to fade out
    route->SetParameter(blkFadeOut, BLKFADEOUT_PARAMID_FADING_TIME, args);

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
        lk.DeleteRoute(route);
        return -1;
    }
    if (!blkOutput) {
        CONSOLE_PRINT("Failed to new a block from %s", soFileName);
        lk.DeleteRoute(route);
        return -1;
    }

    // Create RouteA's links
    if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkStreamIn, 0, blkFormatAdapter, 0)) {
        CONSOLE_PRINT("Failed to new a link");
        lk.DeleteRoute(route);
        return -1;
    }
    if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkFormatAdapter, 0, blkFadeIn, 0)) {
        CONSOLE_PRINT("Failed to new a link");
        lk.DeleteRoute(route);
        return -1;
    }
    if (m_chNum == 2) { // stereo
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkFadeIn, 0, blkDeinterleave, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkDeinterleave, 0, blkGain, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkDeinterleave, 1, blkGain, 1)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkGain, 0, blkInterleave, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkGain, 1, blkInterleave, 1)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkInterleave, 0, blkSoundTouch, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
    } else { // mono
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkFadeIn, 0, blkGain, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, lark::SampleFormat_FLOAT, 1, frameSizeInSamples, blkGain, 0, blkSoundTouch, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
    }
    if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkSoundTouch, 0, blkFadeOut, 0)) {
        CONSOLE_PRINT("Failed to new a link");
        lk.DeleteRoute(route);
        return -1;
    }
    if (!route->NewLink(rate, lark::SampleFormat_FLOAT, m_chNum, frameSizeInSamples, blkFadeOut, 0, blkFormatAdapter1, 0)) {
        CONSOLE_PRINT("Failed to new a link");
        lk.DeleteRoute(route);
        return -1;
    }

    if (savingFile != "") {
        soFileName = "libblkfilewriter" SUFFIX;
        args.clear();
        args.push_back(savingFile);
        lark::Block *blkFileWriter = route->NewBlock(soFileName, false, true, args);
        if (!blkFileWriter) {
            CONSOLE_PRINT("Failed to new a block from %s", soFileName);
            lk.DeleteRoute(route);
            return -1;
        }

        soFileName = "libblkduplicator" SUFFIX;
        lark::Block *blkDuplicator = route->NewBlock(soFileName, false, false);
        if (!blkDuplicator) {
            CONSOLE_PRINT("Failed to new a block from %s", soFileName);
            lk.DeleteRoute(route);
            return -1;
        }

        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkFormatAdapter1, 0, blkDuplicator, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkDuplicator, 0, blkOutput, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkDuplicator, 1, blkFileWriter, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
    } else {
        if (!route->NewLink(rate, format, m_chNum, frameSizeInSamples, blkFormatAdapter1, 0, blkOutput, 0)) {
            CONSOLE_PRINT("Failed to new a link");
            lk.DeleteRoute(route);
            return -1;
        }
    }

    if (m_chNum == 2) {
        CONSOLE_PRINT(
            "*************************************************************************************************************\n"
            "* [q] Balance Left   [w] Balance Mid  [e] Balance Right  [r] Pitch High   [t] Tempo Fast    |   K P L A Y   *\n"
            "* [a] Volume Down    [s] Volume Up    [d] Mute/Unmute    [f] Pitch Low    [g] Tempo Slow    | P O W E R E D *\n"
            "* [z] Seek to Begin  [x] Play/Stop    [c] Exit           [v] Pitch Reset  [b] Tempo Reset   | B Y   L A R K *\n"
            "*************************************************************************************************************");
    } else {
        CONSOLE_PRINT(
            "*************************************************************************************************************\n"
            "*                                                        [r] Pitch High   [t] Tempo Fast    |   K P L A Y   *\n"
            "* [a] Volume Down    [s] Volume Up    [d] Mute/Unmute    [f] Pitch Low    [g] Tempo Slow    | P O W E R E D *\n"
            "* [z] Seek to Begin  [x] Play/Stop    [c] Exit           [v] Pitch Reset  [b] Tempo Reset   | B Y   L A R K *\n"
            "*************************************************************************************************************");
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
        lk.DeleteRoute(route);
        return -1;
    }

    while (1) {
        this->RefreshDisplay(-1);

        int key = getchar();

        if (key == 'c') // Exit
            break;

        switch (key) {
        case 'z':  // Seek to Begin
            m_wav.SeekToBegin();
            break;

        case 'x': { // Play/Stop
            bool toPlay = false;

            m_mutex.lock();
            if (m_state == STOPPED) {
                toPlay = true;
            } else if (m_state == PLAYING) {
                toPlay = false;
            }
            m_mutex.unlock();
            if (toPlay) {
                route->Start();
            } else {
                args.clear();
                route->SetParameter(blkFadeOut, BLKFADEOUT_PARAMID_TRIGGER_FADING, args);
            }
            break;
        }

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

    lk.DeleteRoute(route);

    CONSOLE_PRINT("");

    return 0;
}

void Player::OnStarted()
{
    // Triggered from user
    // No lock needed
    m_state = PLAYING;
    this->RefreshDisplay(-1);
}

void Player::OnStopped(lark::Route::StopReason reason)
{
    if (reason != lark::Route::USER_STOP) { // Triggered from lark route
        m_mutex.lock();
    } else { // Triggered from user
        // No lock needed
    }

    m_state = STOPPED;
    this->RefreshDisplay(-1);

    if (reason != lark::Route::USER_STOP) { // Triggered from lark route
        m_wav.SeekToBegin();
        m_mutex.unlock();
    } else { // Triggered from user
        // No unlock needed
    }
}

int main(int argc, char *argv[])
{
    Player player;
    return player.Go(argc, argv);
}
