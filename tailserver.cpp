// version 2.0.0-alpha
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <chrono>
#include <cstring>
#include <csignal>
#include <limits>
#include <samplerate.h>
#include <soxr.h>
#include "alsaLib.hpp"
#include "cpplibs/ssocket.hpp"
#include "cpplibs/argparse.hpp"
#include "cpplibs/libcbuf.hpp"
#include "utils/sndutils.hpp"
using namespace std;

mutex mgr_mtx;

enum client_state {
    RUNNING,
    STOP,
    PAUSE
};

enum tail_stream_mode_t {
    PLAYBACK,
    CAPTURE,
    CAPTURE_PB
};

struct client_t {
    Socket sock;
    client_state state;
    wav_header_t header;
    tail_stream_mode_t mode;

    CircularBuffer buffer;
    size_t buffer_size = 0;

    int volume = 100;
    
    int capture_pb_id = 0;

    bool drm = false;
};

struct tail_sound_convert_t {
    const char* inbuf;
    char* outbuf;
    int inWidth;
    int outWidth;
    int inChannels;
    int outChannels;
    int inRate;
    int outRate;
    int volume;
    size_t inSize;
};

Socket sockpl;
Socket sockmgr;

PCM pcm_playback;
PCM pcm_capture;

string defaultDevice;
int defaultWidth = 16;
int defaultRate = 48000;
int defaultChannels = 2;
int defaultPeriod = 0;

bool LibSR = false;
bool use_resample = false;

size_t defaultBufferSize;

map<int, client_t*> clients;

bool exit_flag = false;
bool wait_pcm_mtx = false;

void sighandler(int e) {
    exit_flag = true;
    sockpl.close();
    sockmgr.close(); 
}

void tail_pcm_playback_init() {
    pcm_playback.open(defaultDevice, SND_PCM_STREAM_PLAYBACK, 0);
    pcm_playback.setAccess(SND_PCM_ACCESS_RW_INTERLEAVED);
    pcm_playback.setFormat(inttoformat(defaultWidth, 1));
    pcm_playback.setRate(defaultRate);
    pcm_playback.setChannels(defaultChannels); 
    pcm_playback.setBufferSize(1024);
    // pcm_playback.setPeriodSize(256);
    // snd_pcm_hw_params_test_period_size
    pcm_playback.paramsApply();
}

void tail_pcm_playback_reinit() {
    pcm_playback.pcm_exit();
    tail_pcm_playback_init();
}

void tail_pcm_capture_init() {
    pcm_capture.open(defaultDevice, SND_PCM_STREAM_CAPTURE, 0);
    pcm_capture.setAccess(SND_PCM_ACCESS_RW_INTERLEAVED);
    pcm_capture.setFormat(inttoformat(defaultWidth, 1));
    pcm_capture.setRate(defaultRate);
    pcm_capture.setChannels(defaultChannels); 
    pcm_capture.setBufferSize(1024);
    // pcm_capture.setPeriodSize(256);
    // snd_pcm_hw_params_test_period_size
    pcm_capture.paramsApply();
}

void tail_pcm_capture_reinit() {
    pcm_capture.pcm_exit();
    tail_pcm_capture_init();
}

void tail_pcm_init() {
    defaultPeriod = 256;
    defaultBufferSize = defaultPeriod * defaultChannels * (defaultWidth / 8);

    tail_pcm_playback_init();
    tail_pcm_capture_init();

    cout << "Rate: " << defaultRate << endl;
    cout << "Width: " << defaultWidth << endl;
    cout << "Channels: " << defaultChannels << endl;
}

void tail_client_pause(int id) {
    clients[id]->state = PAUSE;
}

void tail_client_resume(int id) {
    clients[id]->state = RUNNING;
}

void tail_client_close(int id) {
    // wait_pcm_mtx = true;
    mgr_mtx.lock();
    // wait_pcm_mtx = false;

    clients[id]->sock.close();

    delete clients[id];
    clients.erase(id);

    mgr_mtx.unlock();
}

bool tail_check_all_pcm_not_running() {
    for (auto i : clients) if (i.second->state == RUNNING) return false;
    return true;
}

size_t tail_snd_width_convert(const char* buf, char* dest, size_t size, int from, int to) {
    if (from == 16 && to == 32) return convert_16_to_32(buf, dest, size, use_resample);
    else if (from == 32 && to == 16) return convert_32_to_16(buf, dest, size, use_resample);

    return size;
}

size_t tail_snd_convert_channels(const char* buf, char* dest, size_t size, int inch, int outch, int width) {
    if (inch < outch && width == 16) return convert_mono_to_stereo(buf, dest, size);
    else if (inch < outch && width == 32) return convert_mono_to_stereo32(buf, dest, size);
    else if (inch > outch && width == 16) return convert_stereo_to_mono(buf, dest, size);
    else if (inch > outch && width == 32) return convert_stereo_to_mono32(buf, dest, size);
    return size;
}

size_t tail_snd_resample_soxr(const char* buffer, char* dest, size_t size, double inputRate, double outputRate, int width) {
    if (inputRate == outputRate) return size;

    size_t isamples = size / (width / 8);

    double rateRatio = outputRate / inputRate;
    size_t osamples = lrint(isamples * rateRatio);

    soxr_datatype_t spec = (width == 32) ? SOXR_INT32_I : SOXR_INT16_I;
    soxr_io_spec_t iospec = soxr_io_spec(spec, spec);
    soxr_quality_spec_t qualityspec = soxr_quality_spec(SOXR_MQ, 0);

    size_t idone, odone;
    soxr_oneshot(inputRate, outputRate, 1, buffer, isamples, &idone, dest, osamples, &odone, &iospec, &qualityspec, nullptr);

    return odone * (defaultWidth / 8);
}

size_t tail_snd_resample_libsamplerate(const char* buffer, char* dest, size_t size, double inputRate, double outputRate) {
    if (inputRate == outputRate) return size;

    size_t isamples = size / sizeof(int16_t);

    double rateRatio = outputRate / inputRate;
    size_t osamples = lrint(isamples * rateRatio);

    int16_t* ibuf = new int16_t[isamples];
    int16_t* obuf = new int16_t[osamples];
    float* ifbuf = new float[isamples];
    float* ofbuf = new float[osamples];

    memcpy(ibuf, buffer, size);
    src_short_to_float_array(ibuf, ifbuf, isamples);

    SRC_DATA data;
    data.data_in = ifbuf;
    data.input_frames = isamples;
    data.data_out = ofbuf;
    data.output_frames = osamples;
    data.src_ratio = rateRatio;

    int error = src_simple(&data, SRC_SINC_BEST_QUALITY, 1);

    if (error) cout << "Resample error: " << src_strerror << endl;

    src_float_to_short_array(ofbuf, obuf, osamples);
    memcpy(dest, obuf, data.output_frames_gen * sizeof(int16_t));

    delete[] ibuf;
    delete[] obuf;
    delete[] ifbuf;
    delete[] ofbuf;

    return data.output_frames_gen * sizeof(int16_t);
}

void tail_snd_volume_convert(const char* buffer, char* dest, size_t size, int volume, int width) {
    if (width == 32) volume_convert32(buffer, dest, size, volume);
    else volume_convert(buffer, dest, size, volume);
}

void tail_snd_mix(const char* buffer, const char* buffer2, char* dest, size_t size) {
    if (defaultWidth == 32) sound_mix32(buffer, buffer2, dest, size);
    else sound_mix(buffer, buffer2, dest, size);
}

size_t tail_snd_convert(tail_sound_convert_t data) {
    size_t buffer_size = defaultBufferSize * (32.0f / defaultWidth) * (2 / defaultChannels);

    if (use_resample) buffer_size *= (384000.0f / defaultRate);

    char* buffer = new char[buffer_size];

    memset(buffer, 0, buffer_size);
    memcpy(buffer, data.inbuf, data.inSize);

    size_t snd_size = data.inSize;

    snd_size = tail_snd_width_convert(buffer, buffer, snd_size, data.inWidth, data.outWidth);
    snd_size = tail_snd_convert_channels(buffer, buffer, snd_size, data.inChannels, data.outChannels, data.outWidth);

    if (use_resample) {
        if (LibSR && data.outWidth == 16) snd_size = tail_snd_resample_libsamplerate(buffer, buffer, snd_size, data.inRate, data.outRate);
        else snd_size = tail_snd_resample_soxr(buffer, buffer, snd_size, data.inRate, data.outRate, data.outWidth);
    }

    tail_snd_volume_convert(buffer, buffer, snd_size, data.volume, data.outWidth);

    memcpy(data.outbuf, buffer, snd_size);

    delete[] buffer;

    return snd_size;
}

void tail_pcm_io_capture_pb_callback(const char* buffer, size_t snd_size, int id) {
    size_t client_buffer_size = defaultBufferSize * (32.0f / defaultWidth) * (2 / defaultChannels);

    if (use_resample) client_buffer_size *= (384000.0f / defaultRate);

    char* client_capture_buffer = new char[client_buffer_size];

    for (auto [_, client] : clients) {
        if (client->mode == CAPTURE_PB && client->capture_pb_id == id && client->state == RUNNING) {
            memset(client_capture_buffer, 0, client_buffer_size);

            tail_sound_convert_t convdata;
            convdata.inbuf = buffer;
            convdata.outbuf = client_capture_buffer;
            convdata.inWidth = defaultWidth;
            convdata.outWidth = client->header.bitsPerSample;
            convdata.inChannels = defaultChannels;
            convdata.outChannels = client->header.numChannels;
            convdata.inRate = defaultRate;
            convdata.outRate = client->header.sampleRate;
            convdata.volume = client->volume;
            convdata.inSize = snd_size;

            snd_size = tail_snd_convert(convdata);

            client->sock.sendmsg(client_capture_buffer, snd_size);
        }
    }

    delete[] client_capture_buffer;
}

void tail_pcm_io_playback() {
    size_t client_buffer_size = defaultBufferSize * (32.0f / defaultWidth) * (2 / defaultChannels);

    if (use_resample) client_buffer_size *= (384000.0f / defaultRate);

    char* mixed_buffer = new char[defaultBufferSize];
    char* client_playback_buffer = new char[client_buffer_size];

    while (!exit_flag) {
        memset(mixed_buffer, 0, defaultBufferSize);

        if (!clients.empty() && !tail_check_all_pcm_not_running()) {
            mgr_mtx.lock();

            for (auto [id, client] : clients) {
                if (client->state != RUNNING) continue;

                if (client->mode == PLAYBACK) {
                    memset(client_playback_buffer, 0, client_buffer_size);

                    if (client->buffer.usage() < client->buffer.size() / 10) {
                        while (client->buffer.usage() < client->buffer.size() / 2) {
                            sockrecv_t snd_data = client->sock.recvmsg();

                            if (!snd_data.size) break;

                            client->buffer.write(snd_data.buffer, snd_data.size);
                            client->sock.send(0);
                        }
                        
                    }

                    if (client->buffer.empty()) continue;

                    size_t snd_size = client->buffer.read(client_playback_buffer, client->buffer_size);

                    tail_sound_convert_t convdata;
                    convdata.inbuf = client_playback_buffer;
                    convdata.outbuf = client_playback_buffer;
                    convdata.inWidth = client->header.bitsPerSample;
                    convdata.outWidth = defaultWidth;
                    convdata.inChannels = client->header.numChannels;
                    convdata.outChannels = defaultChannels;
                    convdata.inRate = client->header.sampleRate;
                    convdata.outRate = defaultRate;
                    convdata.volume = client->volume;
                    convdata.inSize = snd_size;

                    snd_size = tail_snd_convert(convdata);

                    tail_pcm_io_capture_pb_callback(client_playback_buffer, snd_size, id);
                    tail_snd_mix(mixed_buffer, client_playback_buffer, mixed_buffer, snd_size);
                }
            }

            mgr_mtx.unlock();
        }

        tail_pcm_io_capture_pb_callback(mixed_buffer, defaultBufferSize, 0);
        
        try { pcm_playback.writei(mixed_buffer, defaultPeriod); } catch (int e) { tail_pcm_playback_reinit(); }
    }

    delete[] mixed_buffer;
    delete[] client_playback_buffer;

    pcm_playback.drop();
    pcm_playback.pcm_exit();
}

void tail_pcm_io_capture() {
    size_t client_buffer_size = defaultBufferSize * (32.0f / defaultWidth) * (2 / defaultChannels);

    if (use_resample) client_buffer_size *= (384000.0f / defaultRate);

    char* capture_buffer = new char[defaultBufferSize];
    char* client_capture_buffer = new char[client_buffer_size];

    while (!exit_flag) {
        memset(capture_buffer, 0, defaultBufferSize);

        try { pcm_capture.readi(capture_buffer, defaultPeriod); } catch (int e) { tail_pcm_capture_reinit(); }

        if (!clients.empty() && !tail_check_all_pcm_not_running()) {
            mgr_mtx.lock();

            for (auto [id, client] : clients) {
                if (client->state != RUNNING) continue;
                
                if (client->mode == CAPTURE) {
                    memset(client_capture_buffer, 0, client_buffer_size);
                    
                    size_t snd_size = defaultBufferSize;

                    tail_sound_convert_t convdata;
                    convdata.inbuf = capture_buffer;
                    convdata.outbuf = client_capture_buffer;
                    convdata.inWidth = defaultWidth;
                    convdata.outWidth = client->header.bitsPerSample;
                    convdata.inChannels = defaultChannels;
                    convdata.outChannels = client->header.numChannels;
                    convdata.inRate = defaultRate;
                    convdata.outRate = client->header.sampleRate;
                    convdata.volume = client->volume;
                    convdata.inSize = snd_size;

                    snd_size = tail_snd_convert(convdata);

                    client->sock.sendmsg(client_capture_buffer, snd_size);
                }
            }

            mgr_mtx.unlock();
        } 
    }

    delete[] capture_buffer;
    delete[] client_capture_buffer;

    pcm_capture.pcm_exit();
}

void tail_pcm_io_manager(Socket sock, Socket sockd, int client_id) {
    // wait_pcm_mtx = true;
    mgr_mtx.lock();
    // wait_pcm_mtx = false;

    client_t* client = new client_t;
    client->sock = sockd;
    client->state = RUNNING;
    client->header = *((wav_header_t*)sock.recvmsg().buffer);
    client->mode = (tail_stream_mode_t)sock.recvbyte();
    client->volume = sock.recvbyte();
    
    if (client->mode == CAPTURE_PB) {
        client->capture_pb_id = stoi(sock.recvmsg().string);

        if (client->capture_pb_id && clients[client->capture_pb_id]->drm) {
            delete client;
            sock.sendmsg("Error: Unable capture DRM stream.");
            sock.close();
            sockd.close();

            mgr_mtx.unlock();

            return;
        }
    } else client->drm = sock.recvbyte();

    if (client->mode == PLAYBACK) {
        client->buffer_size = defaultBufferSize * ((float)client->header.bitsPerSample / defaultWidth) * ((float)client->header.numChannels / defaultChannels);
        client->buffer.resize(client->buffer_size * 1024);

        if (use_resample) {
            client->buffer.resize(((float)client->header.numChannels / defaultChannels) * (client->header.bitsPerSample / 8) * client->header.sampleRate);
            client->buffer_size *= ((double)client->header.sampleRate / defaultRate);
        }

        sock.sendmsg(to_string(client->buffer.size() / 4));
    }

    clients[client_id] = client;
    mgr_mtx.unlock();

    if (client->mode == PLAYBACK) { if (sock.recv(1).size) while (!client->buffer.empty()) continue; }
    else sock.recvbyte();

    sock.send(0);
    tail_client_close(client_id);
}

int main(int argc, char** argv) {
    ArgumentParser parser(argc, argv);
    parser.add_argument({.flag1 = "-D", .flag2 = "--device"});
    parser.add_argument({.flag1 = "-r", .flag2 = "--rate", .type = ANYINTEGER });
    parser.add_argument({.flag1 = "-w", .flag2 = "--width", .type = ANYINTEGER });
    parser.add_argument({.flag1 = "-a", .flag2 = "--use-alsa", .without_value = true});
    parser.add_argument({.flag1 = "-m", .flag2 = "--mono", .without_value = true});
    parser.add_argument({.flag2 = "--libsamplerate", .without_value = true});
    parser.add_argument({.flag2 = "--resample", .without_value = true});
    auto args = parser.parse();

    defaultDevice = (args["--device"].type != ANYNONE) ? args["--device"].str : (args["--use-alsa"].boolean) ? "plughw:0,0" : "pulse";
    if (args["--rate"].type != ANYNONE) defaultRate = args["--rate"].integer;
    if (args["--width"].type != ANYNONE) defaultWidth = args["--width"].integer;

    LibSR = args["--libsamplerate"].boolean;
    use_resample = args["--resample"].boolean;

    if (args["--mono"].boolean) defaultChannels = 1;

    signal(SIGINT, sighandler);
    // signal(SIGTERM, sighandler);
    // signal(SIGKILL, sighandler);

    sockpl.open(AF_INET, SOCK_STREAM);
    sockpl.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
    sockpl.bind("", 53764);
    sockpl.listen(0);

    sockmgr.open(AF_INET, SOCK_STREAM);
    sockmgr.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
    sockmgr.bind("", 53765);
    sockmgr.listen(0);

    // thread(tail_pcm_device_writer).detach();
    tail_pcm_init();

    thread tail_pcm_io_playback_thread(tail_pcm_io_playback);
    thread tail_pcm_io_capture_thread(tail_pcm_io_capture);
    
    // while (true) thread(manager, sock.saccept().first).detach();

    while (!exit_flag) {
        try {
            pair<Socket, sockaddress_t> plclient = sockpl.accept(); 
            thread(tail_pcm_io_manager, sockmgr.accept().first, plclient.first, plclient.second.port).detach();
            plclient.first.setblocking(false);
        } catch (...) {}
    }

    tail_pcm_io_playback_thread.join();
    tail_pcm_io_capture_thread.join();
}
