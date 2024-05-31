#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>

// int32_t bytes2num(const char* bytes, size_t size) {
//     int32_t ret = 0;
//     memcpy(&ret, bytes, size);

//     return ret;
// }

// void num2bytes(char* dest, int32_t num, size_t size) {
//     memcpy(dest, &num, size);
// }



size_t convert_16_to_32(const char* data, char* dest, size_t size, bool use_resample) {
    size_t samples = size / sizeof(int16_t);

    if (use_resample) samples = ceil(size / (float)sizeof(int16_t));

    size_t newsize = samples * sizeof(int32_t);

    int16_t* buf16 = new int16_t[samples];
    int32_t* buf32 = new int32_t[samples];

    memcpy(buf16, data, size);

    for (size_t i = 0; i < samples; i++) buf32[i] = ((int32_t)buf16[i]) << 16;

    memcpy(dest, buf32, newsize); 

    delete[] buf16;
    delete[] buf32;
    return newsize;
}

size_t convert_32_to_16(const char* data, char* dest, size_t size, bool use_resample) {
    size_t samples = size / sizeof(int32_t);
    
    if (use_resample) samples = ceil(size / (float)sizeof(int32_t));

    size_t newsize = samples * sizeof(int16_t);

    int32_t* buf32 = new int32_t[samples];
    int16_t* buf16 = new int16_t[samples];

    memcpy(buf32, data, size);

    for (size_t i = 0; i < samples; i++) buf16[i] = (int16_t)(buf32[i] >> 16);

    memcpy(dest, buf16, newsize); 

    delete[] buf32;
    delete[] buf16;
    return newsize;
}

void volume_convert(const char* buffer, char* dest, size_t size, int volume) {
    int16_t* buf = (int16_t*)buffer;
    int16_t* dbuf = (int16_t*)dest;

    for (size_t i = 0; i < size / sizeof(int16_t); i++) dbuf[i] = buf[i] / 100 * volume;
}

void volume_convert32(const char* buffer, char* dest, size_t size, int volume) {
    int32_t* buf = (int32_t*)buffer;
    int32_t* dbuf = (int32_t*)dest;

    for (size_t i = 0; i < size / sizeof(int32_t); i++) dbuf[i] = buf[i] / 100 * volume;
}

void sound_mix(const char* buffer, const char* buffer2, char* dest, size_t size) {
    int16_t* buf1 = (int16_t*)buffer;
    int16_t* buf2 = (int16_t*)buffer2;
    int16_t* dbuf = (int16_t*)dest;

    for (size_t i = 0; i < size / sizeof(int16_t); i++) {
        int16_t sample1 = buf1[i];
        int16_t sample2 = buf2[i];

        if (sample1 < 0 && sample2 < 0)
        dbuf[i] = (sample1 + sample2) - (sample1 * sample2) / std::numeric_limits<int16_t>::min();
        
        else if (sample1 > 0 && sample2 > 0)
        dbuf[i] = (sample1 + sample2) - (sample1 * sample2) / std::numeric_limits<int16_t>::max();
        
        else dbuf[i] = sample1 + sample2;
    }
}

void sound_mix32(const char* buffer, const char* buffer2, char* dest, size_t size) {
    int32_t sample1;
    int32_t sample2;
    int32_t mixed_sample;
    int pos = 0;

    while (pos < size) {
        memcpy(&sample1, &buffer[pos], 2);
        memcpy(&sample2, &buffer2[pos], 2);

        if (sample1 < 0 && sample2 < 0)
        mixed_sample = (sample1 + sample2) - (sample1 * sample2) / std::numeric_limits<int32_t>::min();
        
        else if (sample1 > 0 && sample2 > 0)
        mixed_sample = (sample1 + sample2) - (sample1 * sample2) / std::numeric_limits<int32_t>::max();
        
        else mixed_sample = sample1 + sample2;

        memcpy(&dest[pos], &mixed_sample, 2);
        pos += 2;
    }
}

size_t convert_mono_to_stereo(const char* buffer, char* dest, size_t size) {
    size_t newsize = size * 2;
    
    size_t samples = size / sizeof(int16_t);
    size_t newsamples = samples * 2;

    int16_t* buf = new int16_t[samples];
    int16_t* dbuf = new int16_t[samples * 2];

    memcpy(buf, buffer, size);

    for (size_t i = 0, j = 0; i < samples; i++) {
        dbuf[j++] = buf[i];
        dbuf[j++] = buf[i];
    }

    memcpy(dest, dbuf, newsize);

    delete[] buf;
    delete[] dbuf;
    return newsize;
}

size_t convert_mono_to_stereo32(const char* buffer, char* dest, size_t size) {
    size_t newsize = size * 2;
    
    size_t samples = size / sizeof(int32_t);
    size_t newsamples = samples * 2;

    int32_t* buf = new int32_t[samples];
    int32_t* dbuf = new int32_t[samples * 2];

    memcpy(buf, buffer, size);

    for (size_t i = 0, j = 0; i < samples; i++) {
        dbuf[j++] = buf[i];
        dbuf[j++] = buf[i];
    }

    memcpy(dest, dbuf, newsize);

    delete[] buf;
    delete[] dbuf;
    return newsize;
}

size_t convert_stereo_to_mono(const char* buffer, char* dest, size_t size) {
    size_t newsize = size / 2;

    size_t samples = size / sizeof(int16_t);
    size_t newsamples = samples / 2;

    int16_t* buf = new int16_t[samples];
    int16_t* dbuf = new int16_t[newsamples];

    int16_t* chbuf1 = new int16_t[newsamples];
    int16_t* chbuf2 = new int16_t[newsamples];

    memcpy(buf, buffer, size);

    for (size_t i = 0, j = 0; i < samples;) {
        chbuf1[j] = buf[i++];
        chbuf2[j++] = buf[i++];
    }

    sound_mix((char*)chbuf1, (char*)chbuf2, (char*)dbuf, newsize);

    memcpy(dest, dbuf, newsize);

    delete[] buf;
    delete[] dbuf;
    delete[] chbuf1;
    delete[] chbuf2;
    return newsize;
}

size_t convert_stereo_to_mono32(const char* buffer, char* dest, size_t size) {
    size_t newsize = size / 2;

    size_t samples = size / sizeof(int32_t);
    size_t newsamples = samples / 2;

    int32_t* buf = new int32_t[samples];
    int32_t* dbuf = new int32_t[newsamples];

    int32_t* chbuf1 = new int32_t[newsamples];
    int32_t* chbuf2 = new int32_t[newsamples];

    memcpy(buf, buffer, size);

    for (size_t i = 0, j = 0; i < samples;) {
        chbuf1[j] = buf[i++];
        chbuf2[j++] = buf[i++];
    }

    sound_mix((char*)chbuf1, (char*)chbuf2, (char*)dbuf, newsize);

    memcpy(dest, dbuf, newsize);

    delete[] buf;
    delete[] dbuf;
    delete[] chbuf1;
    delete[] chbuf2;
    return newsize;
}