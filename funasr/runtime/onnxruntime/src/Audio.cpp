#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <assert.h>

#include "Audio.h"
#include "precomp.h"

using namespace std;

// see http://soundfile.sapp.org/doc/WaveFormat/
// Note: We assume little endian here
struct WaveHeader {
  bool Validate() const {
    //                 F F I R
    if (chunk_id != 0x46464952) {
      printf("Expected chunk_id RIFF. Given: 0x%08x\n", chunk_id);
      return false;
    }
    //               E V A W
    if (format != 0x45564157) {
      printf("Expected format WAVE. Given: 0x%08x\n", format);
      return false;
    }

    if (subchunk1_id != 0x20746d66) {
      printf("Expected subchunk1_id 0x20746d66. Given: 0x%08x\n",
                       subchunk1_id);
      return false;
    }

    if (subchunk1_size != 16) {  // 16 for PCM
      printf("Expected subchunk1_size 16. Given: %d\n",
                       subchunk1_size);
      return false;
    }

    if (audio_format != 1) {  // 1 for PCM
      printf("Expected audio_format 1. Given: %d\n", audio_format);
      return false;
    }

    if (num_channels != 1) {  // we support only single channel for now
      printf("Expected single channel. Given: %d\n", num_channels);
      return false;
    }
    if (byte_rate != (sample_rate * num_channels * bits_per_sample / 8)) {
      return false;
    }

    if (block_align != (num_channels * bits_per_sample / 8)) {
      return false;
    }

    if (bits_per_sample != 16) {  // we support only 16 bits per sample
      printf("Expected bits_per_sample 16. Given: %d\n",
                       bits_per_sample);
      return false;
    }
    return true;
  }

  // See https://en.wikipedia.org/wiki/WAV#Metadata and
  // https://www.robotplanet.dk/audio/wav_meta_data/riff_mci.pdf
  void SeekToDataChunk(std::istream &is) {
    //                              a t a d
    while (is && subchunk2_id != 0x61746164) {
      // const char *p = reinterpret_cast<const char *>(&subchunk2_id);
      // printf("Skip chunk (%x): %c%c%c%c of size: %d\n", subchunk2_id, p[0],
      //        p[1], p[2], p[3], subchunk2_size);
      is.seekg(subchunk2_size, std::istream::cur);
      is.read(reinterpret_cast<char *>(&subchunk2_id), sizeof(int32_t));
      is.read(reinterpret_cast<char *>(&subchunk2_size), sizeof(int32_t));
    }
  }

  int32_t chunk_id;
  int32_t chunk_size;
  int32_t format;
  int32_t subchunk1_id;
  int32_t subchunk1_size;
  int16_t audio_format;
  int16_t num_channels;
  int32_t sample_rate;
  int32_t byte_rate;
  int16_t block_align;
  int16_t bits_per_sample;
  int32_t subchunk2_id;    // a tag of this chunk
  int32_t subchunk2_size;  // size of subchunk2
};
static_assert(sizeof(WaveHeader) == WAV_HEADER_SIZE, "");

class AudioWindow {
  private:
    int *window;
    int in_idx;
    int out_idx;
    int sum;
    int window_size = 0;

  public:
    AudioWindow(int window_size) : window_size(window_size)
    {
        window = (int *)calloc(sizeof(int), window_size + 1);
        in_idx = 0;
        out_idx = 1;
        sum = 0;
    };
    ~AudioWindow(){
        free(window);
    };
    int put(int val)
    {
        sum = sum + val - window[out_idx];
        window[in_idx] = val;
        in_idx = in_idx == window_size ? 0 : in_idx + 1;
        out_idx = out_idx == window_size ? 0 : out_idx + 1;
        return sum;
    };
};

AudioFrame::AudioFrame(){};
AudioFrame::AudioFrame(int len) : len(len)
{
    start = 0;
};
AudioFrame::~AudioFrame(){};
int AudioFrame::set_start(int val)
{
    start = val < 0 ? 0 : val;
    return start;
};

int AudioFrame::set_end(int val, int max_len)
{

    float num_samples = val - start;
    float frame_length = 400;
    float frame_shift = 160;
    float num_new_samples =
        ceil((num_samples - frame_length) / frame_shift) * frame_shift + frame_length;

    end = start + num_new_samples;
    len = (int)num_new_samples;
    if (end > max_len)
        printf("frame end > max_len!!!!!!!\n");
    return end;
};

int AudioFrame::get_start()
{
    return start;
};

int AudioFrame::get_len()
{
    return len;
};

int AudioFrame::disp()
{
    printf("not imp!!!!\n");

    return 0;
};

Audio::Audio(int data_type) : data_type(data_type)
{
    speech_buff = NULL;
    speech_data = NULL;
    align_size = 1360;
}

Audio::Audio(int data_type, int size) : data_type(data_type)
{
    speech_buff = NULL;
    speech_data = NULL;
    align_size = (float)size;
}

Audio::~Audio()
{
    if (speech_buff != NULL) {
        free(speech_buff);
        
    }

    if (speech_data != NULL) {
        
        free(speech_data);
    }
}

void Audio::disp()
{
    printf("Audio time is %f s. len is %d\n", (float)speech_len / model_sample_rate,
           speech_len);
}

float Audio::get_time_len()
{
    return (float)speech_len / model_sample_rate;
}

void Audio::wavResample(int32_t sampling_rate, const float *waveform,
                          int32_t n)
{
    printf(
          "Creating a resampler:\n"
          "   in_sample_rate: %d\n"
          "   output_sample_rate: %d\n",
          sampling_rate, static_cast<int32_t>(model_sample_rate));
    float min_freq =
        std::min<int32_t>(sampling_rate, model_sample_rate);
    float lowpass_cutoff = 0.99 * 0.5 * min_freq;

    int32_t lowpass_filter_width = 6;
    //FIXME
    //auto resampler = new LinearResample(
    //      sampling_rate, model_sample_rate, lowpass_cutoff, lowpass_filter_width);
    auto resampler = std::make_unique<LinearResample>(
          sampling_rate, model_sample_rate, lowpass_cutoff, lowpass_filter_width);
    std::vector<float> samples;
    resampler->Resample(waveform, n, true, &samples);
    //reset speech_data
    speech_len = samples.size();
    if (speech_data != NULL) {
        free(speech_data);
    }
    speech_data = (float*)malloc(sizeof(float) * speech_len);
    memset(speech_data, 0, sizeof(float) * speech_len);
    copy(samples.begin(), samples.end(), speech_data);
}

bool Audio::loadwav(const char *filename, int32_t* sampling_rate)
{
    WaveHeader header;
    if (speech_data != NULL) {
        free(speech_data);
    }
    if (speech_buff != NULL) {
        free(speech_buff);
    }
    
    offset = 0;
    std::ifstream is(filename, std::ifstream::binary);
    is.read(reinterpret_cast<char *>(&header), sizeof(header));
    if(!is){
        fprintf(stderr, "Failed to read %s\n", filename);
        return false;
    }
    
    *sampling_rate = header.sample_rate;
    // header.subchunk2_size contains the number of bytes in the data.
    // As we assume each sample contains two bytes, so it is divided by 2 here
    speech_len = header.subchunk2_size / 2;
    speech_buff = (int16_t *)malloc(sizeof(int16_t) * speech_len);

    if (speech_buff)
    {
        memset(speech_buff, 0, sizeof(int16_t) * speech_len);
        is.read(reinterpret_cast<char *>(speech_buff), header.subchunk2_size);
        if (!is) {
            fprintf(stderr, "Failed to read %s\n", filename);
            return false;
        }
        speech_data = (float*)malloc(sizeof(float) * speech_len);
        memset(speech_data, 0, sizeof(float) * speech_len);

        float scale = 1;
        if (data_type == 1) {
            scale = 32768;
        }
        for (int32_t i = 0; i != speech_len; ++i) {
            speech_data[i] = (float)speech_buff[i] / scale;
        }

        //resample
        if(*sampling_rate != model_sample_rate){
            wavResample(*sampling_rate, speech_data, speech_len);
        }

        AudioFrame* frame = new AudioFrame(speech_len);
        frame_queue.push(frame);

        return true;
    }
    else
        return false;
}

bool Audio::loadwav(const char* buf, int nFileLen, int32_t* sampling_rate)
{
    WaveHeader header;
    if (speech_data != NULL) {
        free(speech_data);
    }
    if (speech_buff != NULL) {
        free(speech_buff);
    }
    offset = 0;

    std::memcpy(&header, buf, sizeof(header));

    *sampling_rate = header.sample_rate;
    speech_len = header.subchunk2_size / 2;
    speech_buff = (int16_t *)malloc(sizeof(int16_t) * speech_len);
    if (speech_buff)
    {
        memset(speech_buff, 0, sizeof(int16_t) * speech_len);
        memcpy((void*)speech_buff, (const void*)(buf + WAV_HEADER_SIZE), speech_len * sizeof(int16_t));

        speech_data = (float*)malloc(sizeof(float) * speech_len);
        memset(speech_data, 0, sizeof(float) * speech_len);

        float scale = 1;
        if (data_type == 1) {
            scale = 32768;
        }

        for (int32_t i = 0; i != speech_len; ++i) {
            speech_data[i] = (float)speech_buff[i] / scale;
        }
        
        //resample
        if(*sampling_rate != model_sample_rate){
            wavResample(*sampling_rate, speech_data, speech_len);
        }

        AudioFrame* frame = new AudioFrame(speech_len);
        frame_queue.push(frame);

        return true;
    }
    else
        return false;
}

bool Audio::loadpcmwav(const char* buf, int nBufLen, int32_t* sampling_rate)
{
    if (speech_data != NULL) {
        free(speech_data);
    }
    if (speech_buff != NULL) {
        free(speech_buff);
    }
    offset = 0;

    speech_len = nBufLen / 2;
    speech_buff = (int16_t*)malloc(sizeof(int16_t) * speech_len);
    if (speech_buff)
    {
        memset(speech_buff, 0, sizeof(int16_t) * speech_len);
        memcpy((void*)speech_buff, (const void*)buf, speech_len * sizeof(int16_t));

        speech_data = (float*)malloc(sizeof(float) * speech_len);
        memset(speech_data, 0, sizeof(float) * speech_len);

        float scale = 1;
        if (data_type == 1) {
            scale = 32768;
        }

        for (int32_t i = 0; i != speech_len; ++i) {
            speech_data[i] = (float)speech_buff[i] / scale;
        }
        
        //resample
        if(*sampling_rate != model_sample_rate){
            wavResample(*sampling_rate, speech_data, speech_len);
        }

        AudioFrame* frame = new AudioFrame(speech_len);
        frame_queue.push(frame);
        return true;

    }
    else
        return false;
}

bool Audio::loadpcmwav(const char* filename, int32_t* sampling_rate)
{
    if (speech_data != NULL) {
        free(speech_data);
    }
    if (speech_buff != NULL) {
        free(speech_buff);
    }
    offset = 0;

    FILE* fp;
    fp = fopen(filename, "rb");
    if (fp == nullptr)
        return false;
    fseek(fp, 0, SEEK_END);
    uint32_t nFileLen = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    speech_len = (nFileLen) / 2;
    speech_buff = (int16_t*)malloc(sizeof(int16_t) * speech_len);
    if (speech_buff)
    {
        memset(speech_buff, 0, sizeof(int16_t) * speech_len);
        int ret = fread(speech_buff, sizeof(int16_t), speech_len, fp);
        fclose(fp);

        speech_data = (float*)malloc(sizeof(float) * speech_len);
        memset(speech_data, 0, sizeof(float) * speech_len);

        float scale = 1;
        if (data_type == 1) {
            scale = 32768;
        }
        for (int32_t i = 0; i != speech_len; ++i) {
            speech_data[i] = (float)speech_buff[i] / scale;
        }

        //resample
        if(*sampling_rate != model_sample_rate){
            wavResample(*sampling_rate, speech_data, speech_len);
        }

        AudioFrame* frame = new AudioFrame(speech_len);
        frame_queue.push(frame);
    
        return true;
    }
    else
        return false;

}

int Audio::fetch_chunck(float *&dout, int len)
{
    if (offset >= speech_align_len) {
        dout = NULL;
        return S_ERR;
    } else if (offset == speech_align_len - len) {
        dout = speech_data + offset;
        offset = speech_align_len;
        // 临时解决 
        AudioFrame *frame = frame_queue.front();
        frame_queue.pop();
        delete frame;

        return S_END;
    } else {
        dout = speech_data + offset;
        offset += len;
        return S_MIDDLE;
    }
}

int Audio::fetch(float *&dout, int &len, int &flag)
{
    if (frame_queue.size() > 0) {
        AudioFrame *frame = frame_queue.front();
        frame_queue.pop();

        dout = speech_data + frame->get_start();
        len = frame->get_len();
        delete frame;
        flag = S_END;
        return 1;
    } else {
        return 0;
    }
}

void Audio::padding()
{

    float num_samples = speech_len;
    float frame_length = 400;
    float frame_shift = 160;
    float num_frames = floor((num_samples + (frame_shift / 2)) / frame_shift);
    float num_new_samples = (num_frames - 1) * frame_shift + frame_length;
    float num_padding = num_new_samples - num_samples;
    float num_left_padding = (frame_length - frame_shift) / 2;
    float num_right_padding = num_padding - num_left_padding;

    float *new_data = (float *)malloc(num_new_samples * sizeof(float));
    int i;
    int tmp_off = 0;
    for (i = 0; i < num_left_padding; i++) {
        int ii = num_left_padding - i - 1;
        new_data[i] = speech_data[ii];
    }
    tmp_off = num_left_padding;
    memcpy(new_data + tmp_off, speech_data, speech_len * sizeof(float));
    tmp_off += speech_len;

    for (i = 0; i < num_right_padding; i++) {
        int ii = speech_len - i - 1;
        new_data[tmp_off + i] = speech_data[ii];
    }
    free(speech_data);
    speech_data = new_data;
    speech_len = num_new_samples;

    AudioFrame *frame = new AudioFrame(num_new_samples);
    frame_queue.push(frame);
    frame = frame_queue.front();
    frame_queue.pop();
    delete frame;
}

#define UNTRIGGERED 0
#define TRIGGERED   1

#define SPEECH_LEN_5S  (16000 * 5)
#define SPEECH_LEN_10S (16000 * 10)
#define SPEECH_LEN_20S (16000 * 20)
#define SPEECH_LEN_30S (16000 * 30)

/*
void Audio::split()
{
    VadInst *handle = WebRtcVad_Create();
    WebRtcVad_Init(handle);
    WebRtcVad_set_mode(handle, 2);
    int window_size = 10;
    AudioWindow audiowindow(window_size);
    int status = UNTRIGGERED;
    int offset = 0;
    int fs = 16000;
    int step = 480;

    AudioFrame *frame;

    frame = frame_queue.front();
    frame_queue.pop();
    delete frame;
    frame = NULL;

    while (offset < speech_len - step) {
        int n = WebRtcVad_Process(handle, fs, speech_buff + offset, step);
        if (status == UNTRIGGERED && audiowindow.put(n) >= window_size - 1) {
            frame = new AudioFrame();
            int start = offset - step * (window_size - 1);
            frame->set_start(start);
            status = TRIGGERED;
        } else if (status == TRIGGERED) {
            int win_weight = audiowindow.put(n);
            int voice_len = (offset - frame->get_start());
            int gap = 0;
            if (voice_len < SPEECH_LEN_5S) {
                offset += step;
                continue;
            } else if (voice_len < SPEECH_LEN_10S) {
                gap = 1;
            } else if (voice_len < SPEECH_LEN_20S) {
                gap = window_size / 5;
            } else {
                gap = window_size / 2;
            }

            if (win_weight < gap) {
                status = UNTRIGGERED;
                offset = frame->set_end(offset, speech_align_len);
                frame_queue.push(frame);
                frame = NULL;
            }
        }
        offset += step;
    }

    if (frame != NULL) {
        frame->set_end(speech_len, speech_align_len);
        frame_queue.push(frame);
        frame = NULL;
    }
    WebRtcVad_Free(handle);
}
*/