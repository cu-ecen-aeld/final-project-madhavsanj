/*
File: alsa_playback.c
Author: Madhav Appanaboyina

References:
    - ALSA API Documentation:
    https://www.alsa-project.org/alsa-doc/alsa-lib/

    - SoundTouch Audio Processing Library:
    https://codeberg.org/soundtouch/soundtouch

    - WAV File Format Specification:
    http://soundfile.sapp.org/doc/WaveFormat/

    Concepts covered in assignments:
    - Buffer management and synchronization(A5 & A6)
    - File descriptor-based I/O handling(A3)

Brief:
    - Streams audio data to the ALSA device using snd_pcm_writei().
    - Handles buffer underrun conditions and recovers playback.
    - Uses blocking playback mode to ensure continuous audio streaming.
    - Manages audio buffer allocation and chunk-based processing.
    - Cleans up ALSA handles and allocated resources after playback.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sndfile.h>
#include <alsa/asoundlib.h>


// Defines the hardware device (your USB dongle)
#define PCM_DEVICE "default"


int main(int argc, char *argv[]) {
    // To validate Command Line Arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_audio.wav>\n", argv[0]);
        return EXIT_FAILURE;
    }


    const char *infilename = argv[1];
    SF_INFO sfinfo;
    sfinfo.format = 0; 


    // Opens the WAV file in READ mode
    SNDFILE *infile = sf_open(infilename, SFM_READ, &sfinfo);
    if (!infile) {
        fprintf(stderr, "ERROR: Could not open file '%s'\n", infilename);
        return EXIT_FAILURE;
    }


    //  Opens the ALSA PCM device for playback
    snd_pcm_t *pcm_handle;
    if (snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "ERROR: Could not open ALSA device\n");
        sf_close(infile);
        return EXIT_FAILURE;
    }


    // sets the hardware parameters using audio metadata obtained from parsing(sfinfo)
    // This maps sfinfo data directly to the hardware config
    snd_pcm_set_params(pcm_handle,
                       SND_PCM_FORMAT_S16_LE,
                       SND_PCM_ACCESS_RW_INTERLEAVED,
                       sfinfo.channels,
                       sfinfo.samplerate,
                       1,       //To allow software resampling
                       500000); // 0.5s latency buffer -> to store 0.5s of audio data in the buffer


    // Extracts and orints the metadata
    printf("--- Starting Playback ---\n");
    printf("File Name   : %s\n", infilename);
    printf("Sample Rate : %d Hz\n", sfinfo.samplerate);
    printf("Channels    : %d\n", sfinfo.channels);


    // Playback Loop (Reading and Playing)
    // allocating a small buffer to stream the data to the soundcard
    short *buffer = malloc(4096 * sizeof(short) * sfinfo.channels);
    sf_count_t frames_read;


    while ((frames_read = sf_readf_short(infile, buffer, 4096)) > 0) {
        snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, buffer, frames_read);


        // Handles buffer underruns (stuttering prevention)
        if (frames_written < 0) {
            snd_pcm_prepare(pcm_handle);
        }
    }


    // Cleans up memory and hardware 
    printf("Playback Finished.\n");
    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    sf_close(infile);
    free(buffer);


    return EXIT_SUCCESS;
}
