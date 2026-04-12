/*
File: main1.c
Author: Madhav Appanaboyina
Issues covered: #5 - Thread-Safe Circular Buffer Integration

References:
    - libsndfile Documentation:
    http://www.mega-nerd.com/libsndfile/api.html

    - ALSA PCM API Documentation:
    https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html

    - POSIX Threads (pthreads) Documentation:
    https://man7.org/linux/man-pages/man7/pthreads.7.html

    - Producer-Consumer Problem (Condition Variables):
    https://en.wikipedia.org/wiki/Producer%E2%80%93consumer_problem

    Concepts covered in assignments:
    - Multithreading and synchronization (A6)
    - Circular buffer / data buffering (A5)
    - File I/O and streaming data (A3)
    - Real-time data handling and concurrency

Brief:
    - Implements multithreaded WAV playback using libsndfile and ALSA
    - Uses a thread-safe circular (ring) buffer to decouple file I/O and playback
    - Producer thread reads audio frames from WAV file into buffer
    - Consumer thread pulls frames from buffer and writes to ALSA PCM device
    - Uses mutexes and condition variables to handle synchronization (not_full, not_empty)
    - Handles buffer wrap-around using head/tail indexing
    - Prevents underruns and overruns using bounded buffer logic
    - Detects EOF and ensures graceful thread termination
    - Initializes and configures ALSA playback parameters
    - Cleans up resources (threads, buffer, ALSA, file) before exit
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sndfile.h>
#include <alsa/asoundlib.h>

#define PCM_DEVICE "default"
#define CHUNK_FRAMES 4096
#define RING_BUFFER_CHUNKS 10 // Buffer holds 10 chunks to prevent underruns

// --- Circular Buffer Structure ---
typedef struct {
    short *data;            // Array holding the PCM data
    int head;               // Write index
    int tail;               // Read index
    int count;              // Current number of frames in buffer
    int max_frames;         // Total frame capacity
    int channels;           // Number of audio channels
    bool eof_reached;       // Flag to signal producer is done
    
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} AudioRingBuffer;

// Global structs for the threads to share
AudioRingBuffer ring_buf;
SNDFILE *infile;
snd_pcm_t *pcm_handle;


// --- PRODUCER THREAD (Reads from WAV, writes to Buffer) ---
void *producer_thread_func(void *arg) {
    short *temp_buf = malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));
    sf_count_t frames_read;

    while ((frames_read = sf_readf_short(infile, temp_buf, CHUNK_FRAMES)) > 0) {
        pthread_mutex_lock(&ring_buf.lock);

        // Wait if the buffer is full (Spurious wakeup protection)
        while (ring_buf.count + frames_read > ring_buf.max_frames) {
            pthread_cond_wait(&ring_buf.not_full, &ring_buf.lock);
        }

        // Write data to the ring buffer (handling wrap-around)
        int total_samples = frames_read * ring_buf.channels;
        for (int i = 0; i < total_samples; i++) {
            ring_buf.data[ring_buf.head] = temp_buf[i];
            ring_buf.head = (ring_buf.head + 1) % (ring_buf.max_frames * ring_buf.channels);
        }
        
        ring_buf.count += frames_read;

        // Signal consumer that data is available
        pthread_cond_signal(&ring_buf.not_empty);
        pthread_mutex_unlock(&ring_buf.lock);
    }

    // EOF Reached
    pthread_mutex_lock(&ring_buf.lock);
    ring_buf.eof_reached = true;
    pthread_cond_broadcast(&ring_buf.not_empty); // Wake up consumer if it's waiting
    pthread_mutex_unlock(&ring_buf.lock);

    free(temp_buf);
    printf("[Producer] Finished reading file.\n");
    return NULL;
}


// --- CONSUMER THREAD (Reads from Buffer, writes to ALSA) ---
void *consumer_thread_func(void *arg) {
    short *temp_buf = malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));

    while (1) {
        pthread_mutex_lock(&ring_buf.lock);

        // Wait if buffer is empty AND we haven't reached EOF
        while (ring_buf.count < CHUNK_FRAMES && !ring_buf.eof_reached) {
            pthread_cond_wait(&ring_buf.not_empty, &ring_buf.lock);
        }

        // Break loop if buffer is empty and EOF is reached
        if (ring_buf.count == 0 && ring_buf.eof_reached) {
            pthread_mutex_unlock(&ring_buf.lock);
            break; 
        }

        // Determine how many frames to pull (handle the last small chunk at EOF)
        int frames_to_pull = (ring_buf.count < CHUNK_FRAMES) ? ring_buf.count : CHUNK_FRAMES;
        int total_samples = frames_to_pull * ring_buf.channels;

        // Read data from ring buffer (handling wrap-around)
        for (int i = 0; i < total_samples; i++) {
            temp_buf[i] = ring_buf.data[ring_buf.tail];
            ring_buf.tail = (ring_buf.tail + 1) % (ring_buf.max_frames * ring_buf.channels);
        }

        ring_buf.count -= frames_to_pull;

        // Signal producer that space freed up
        pthread_cond_signal(&ring_buf.not_full);
        pthread_mutex_unlock(&ring_buf.lock);

        // Write to ALSA (Outside the mutex to prevent blocking the producer!)
        snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, temp_buf, frames_to_pull);
        if (frames_written < 0) {
            snd_pcm_prepare(pcm_handle); // Recover from underrun
        }
    }

    free(temp_buf);
    printf("[Consumer] Finished playing audio.\n");
    return NULL;
}


// --- MAIN ROUTINE ---
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_audio.wav>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 1. Open WAV File
    SF_INFO sfinfo;
    sfinfo.format = 0;
    infile = sf_open(argv[1], SFM_READ, &sfinfo);
    if (!infile) {
        fprintf(stderr, "ERROR: Could not open WAV file.\n");
        return EXIT_FAILURE;
    }

    // 2. Initialize ALSA
    if (snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "ERROR: Could not open ALSA device.\n");
        sf_close(infile);
        return EXIT_FAILURE;
    }
    snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                       sfinfo.channels, sfinfo.samplerate, 1, 500000);

    // 3. Initialize Ring Buffer
    ring_buf.max_frames = CHUNK_FRAMES * RING_BUFFER_CHUNKS;
    ring_buf.channels = sfinfo.channels;
    ring_buf.data = malloc(ring_buf.max_frames * ring_buf.channels * sizeof(short));
    ring_buf.head = 0;
    ring_buf.tail = 0;
    ring_buf.count = 0;
    ring_buf.eof_reached = false;
    
    pthread_mutex_init(&ring_buf.lock, NULL);
    pthread_cond_init(&ring_buf.not_empty, NULL);
    pthread_cond_init(&ring_buf.not_full, NULL);

    printf("--- Starting Multithreaded Audio Engine ---\n");

    // 4. Spawn Threads
    pthread_t producer, consumer;
    pthread_create(&producer, NULL, producer_thread_func, NULL);
    pthread_create(&consumer, NULL, consumer_thread_func, NULL);

    // 5. Wait for threads to finish
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    // 6. Clean up Hardware and Memory
    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    sf_close(infile);
    free(ring_buf.data);
    pthread_mutex_destroy(&ring_buf.lock);
    pthread_cond_destroy(&ring_buf.not_empty);
    pthread_cond_destroy(&ring_buf.not_full);

    printf("--- System Shut Down Cleanly ---\n");
    return EXIT_SUCCESS;
}

