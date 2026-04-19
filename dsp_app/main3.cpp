/*
File: main3.cpp
Author: Madhav Appanaboyina
Issues covered: #5 - Thread-Safe Circular Buffer Integration
                #6 - Shared DSP Configuration via IPC
                #7 - SoundTouch DSP Integration
                #8 - Connect parsed socket commands to the SoundTouch DSP processing engine
                     (Focus of this code file)

References:
    - libsndfile Documentation:
    http://www.mega-nerd.com/libsndfile/api.html

    - ALSA PCM API Documentation:
    https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html

    - POSIX Threads (pthreads) Documentation:
    https://man7.org/linux/man-pages/man7/pthreads.7.html

    - SoundTouch Audio Processing Library:
    https://codeberg.org/soundtouch/soundtouch

    - Beej's Guide to Network Programming:
    https://beej.us/guide/bgnet/

    - Producer Consumer Problem in C:
    https://www.geeksforgeeks.org/c/producer-consumer-problem-in-c/

    Concepts covered in assignments:
    - Multithreading and synchronization (A6)
    - Circular buffer / data buffering (A5)
    - File I/O and streaming data (A3)
    - Socket programming and networking (A5/A6)
    - Real-time data handling and concurrency

Brief:
    - Implements multithreaded WAV playback using libsndfile and ALSA with real-time DSP
      processing using SoundTouch and runtime control via a TCP socket server
    - Completes the audio pipeline by creating a link between parsed socket commands
      and the DSP engine 
    - Now, these commands actually affect live audio output by updating
      SoundTouch tempo and pitch settings at runtime
    - Producer thread reads audio frames from WAV file into buffer
    - Consumer thread processes audio through SoundTouch DSP and writes to ALSA PCM device
    - Control thread runs a TCP server to receive runtime commands (set tempo/pitch)
    - Uses a thread-safe circular (ring) buffer to decouple file I/O and playback
    - Consumer thread reads the latest shared tempo/pitch values before processing each chunk
    - Dynamically applies updated DSP settings to SoundTouch while playback is ongoing
    - Uses mutexes and condition variables for synchronization (not_full, not_empty)
    - Handles buffer wrap-around using head/tail indexing
    - Prevents underruns and overruns using bounded buffer logic
    - Detects EOF and ensures graceful thread termination
    - Initializes and configures ALSA playback parameters and DSP engine
    - Cleans up resources (threads, buffer, ALSA, DSP, file, sockets) before exit

Note: SoundTouch is a native C++ library, and its C wrapper is incomplete/unreliable in our Buildroot setup.
      Missing functions like 16-bit processing APIs, for example, caused linking issues which made writing this file in
      C++ the correct choice.

*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sndfile.h>
#include <alsa/asoundlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <soundtouch/SoundTouch.h> // Changed to Native C++ Header

#define PCM_DEVICE "default"
#define CHUNK_FRAMES 4096
#define RING_BUFFER_CHUNKS 10
#define PORT 9000
#define BUFFER_SIZE 1024

// IPC Shared State (Issue #6)
//ALl threads can see this shared state as this struct lives in the global RAM
typedef struct {
    float tempo;
    float pitch;
    pthread_mutex_t lock; // Mutex to protect shared state from concurrent access.
} DSPConfig;

DSPConfig shared_config;

// Circular Buffer Structure
//This data structure allows asynchronous data transfer Producer and Consumer threads
typedef struct {
    short *data;
    int head;  // Write pointer index
    int tail;  // Read pointer inder
    int count;  // Current volume of frmaes in the buffer
    int max_frames;  // Frame capacity of the buffer
    int channels;  // No. of Audio Channels
    bool eof_reached; // This flag indicates completion of file reading

    pthread_mutex_t lock;
    pthread_cond_t not_empty; // Condition variable to signal data availability
    pthread_cond_t not_full; // To signal Space availability
} AudioRingBuffer;

AudioRingBuffer ring_buf;
SNDFILE *infile;
snd_pcm_t *pcm_handle;
soundtouch::SoundTouch *pSoundTouch; // Changed to Native C++ DSP Object

// PRODUCER THREAD
//Function of this thread - read audio frames from the file system and enqueue them, blocks when the buffer reaches maximum capacity
void *producer_thread_func(void *arg) {
    // Added (short *) cast for C++ compatibility
    short *temp_buf = (short *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));
    sf_count_t frames_read;

    // Loops until the end of the input file is reached
    while ((frames_read = sf_readf_short(infile, temp_buf, CHUNK_FRAMES)) > 0) {
        pthread_mutex_lock(&ring_buf.lock);

        //Block thread execution if the buffer lacks sufficient space
        while (ring_buf.count + frames_read > ring_buf.max_frames) {
            pthread_cond_wait(&ring_buf.not_full, &ring_buf.lock);
        }

        // Write data chunk to the ring buffer, handling index wrap-around
        int total_samples = frames_read * ring_buf.channels;
        for (int i = 0; i < total_samples; i++) {
            ring_buf.data[ring_buf.head] = temp_buf[i];
            ring_buf.head = (ring_buf.head + 1) % (ring_buf.max_frames * ring_buf.channels);
        }

        ring_buf.count += frames_read;
        pthread_cond_signal(&ring_buf.not_empty); //Signal the consumer thread that new data is available for processing
        pthread_mutex_unlock(&ring_buf.lock);
    }

    pthread_mutex_lock(&ring_buf.lock);
    ring_buf.eof_reached = true;
    pthread_cond_broadcast(&ring_buf.not_empty);
    pthread_mutex_unlock(&ring_buf.lock);

    free(temp_buf);
    printf("[Producer] Finished reading file.\n");
    return NULL;
}


// CONSUMER THREAD
// Function of this thread -Dequeue audio frames from the circular buffer and write them to the LSA PCM hardware driver
void *consumer_thread_func(void *arg) {
    // 16-bit Integer buffers for the ring buffer and ALSA
    short *temp_buf = (short *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));
    short *dsp_out_buf = (short *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));

    // 32-bit Float buffers for SoundTouch DSP processing
    float *float_in_buf = (float *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(float));
    float *float_out_buf = (float *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(float));

    while (1) {
        pthread_mutex_lock(&ring_buf.lock);
        while (ring_buf.count < CHUNK_FRAMES && !ring_buf.eof_reached) { //Block thread execution if the buffer is empty and active streaming is ongoing
            pthread_cond_wait(&ring_buf.not_empty, &ring_buf.lock);
        }

        if (ring_buf.count == 0 && ring_buf.eof_reached) {
            pthread_mutex_unlock(&ring_buf.lock);
 
            // Flush any remaining buffered samples out of the DSP engine
            pSoundTouch->flush();
            int flushed_frames;
            while ((flushed_frames = pSoundTouch->receiveSamples(float_out_buf, CHUNK_FRAMES)) > 0) {
                
                // Convert flushed floats back to 16-bit integers for ALSA
                int total_flushed_samples = flushed_frames * ring_buf.channels;
                for (int i = 0; i < total_flushed_samples; i++) {
                    float val = float_out_buf[i] * 32768.0f;
                    if (val > 32767.0f) val = 32767.0f; // Hard clipping protection
                    if (val < -32768.0f) val = -32768.0f;
                    dsp_out_buf[i] = (short)val;
                }
                
                snd_pcm_writei(pcm_handle, dsp_out_buf, flushed_frames);
            }
            break; 
        }

        int frames_to_pull = (ring_buf.count < CHUNK_FRAMES) ? ring_buf.count : CHUNK_FRAMES;
        int total_samples = frames_to_pull * ring_buf.channels;

        for (int i = 0; i < total_samples; i++) {
            temp_buf[i] = ring_buf.data[ring_buf.tail];
            ring_buf.tail = (ring_buf.tail + 1) % (ring_buf.max_frames * ring_buf.channels);
        }

        ring_buf.count -= frames_to_pull;
        pthread_cond_signal(&ring_buf.not_full);
        pthread_mutex_unlock(&ring_buf.lock);

        // --- DSP PIPELINE ---
 
        // 1. Safely grab the latest network commands
        pthread_mutex_lock(&shared_config.lock);
        float current_tempo = shared_config.tempo;
        float current_pitch = shared_config.pitch;
        pthread_mutex_unlock(&shared_config.lock);

        // 2. Update DSP Engine dynamically
        pSoundTouch->setTempo(current_tempo);
        pSoundTouch->setPitch(current_pitch);

        // 3. DSP BRIDGE IN: Convert 16-bit ints to 32-bit floats (-1.0 to 1.0)
        for (int i = 0; i < total_samples; i++) {
            float_in_buf[i] = (float)temp_buf[i] / 32768.0f;
        }

        // 4. Push normalized float frames to the DSP
        pSoundTouch->putSamples(float_in_buf, frames_to_pull);

        // 5. Continuously drain processed float frames from the DSP
        int processed_frames;
        while ((processed_frames = pSoundTouch->receiveSamples(float_out_buf, CHUNK_FRAMES)) > 0) {
            
            // 6. DSP BRIDGE OUT: Convert 32-bit floats back to 16-bit ints for ALSA
            int processed_samples = processed_frames * ring_buf.channels;
            for (int i = 0; i < processed_samples; i++) {
                float val = float_out_buf[i] * 32768.0f;
                if (val > 32767.0f) val = 32767.0f; // Hard clipping protection
                if (val < -32768.0f) val = -32768.0f;
                dsp_out_buf[i] = (short)val;
            }

            // 7. Write the converted data to the ALSA hardware loop
            snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, dsp_out_buf, processed_frames);
            if (frames_written < 0) {
                snd_pcm_prepare(pcm_handle); 
            }
        }
    }

    free(temp_buf);
    free(dsp_out_buf);
    free(float_in_buf);
    free(float_out_buf);
    printf("[Consumer] Finished playing audio.\n");
    return NULL;
}

// CONTROL THREAD (Socket Server)
// Function - Maintain a listening TCP socket to receive string commands, parse them,
// and safely update the global DSP configuration variables.
void *control_thread_func(void *arg) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    // Socket initialization and allocation
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("[Control] Socket failed");
        return NULL;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("[Control] Bind failed");
        return NULL;
    }
    if (listen(server_fd, 3) < 0) {
        perror("[Control] Listen failed");
        return NULL;
    }

    printf("[Control] Server listening on Port %d\n", PORT);

    // Keep accepting incoming TCP client connections until file playback reaches EOF
    while (!ring_buf.eof_reached) {
        // Accept a new client connection on the listening socket
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        // If accept fails, skip this iteration and keep the server alive
        if (new_socket < 0) continue;

        // Read the incoming command string from the connected client into the buffer
        int valread = read(new_socket, buffer, BUFFER_SIZE);
        if (valread > 0) {
            char param[50];
            // Stores the numeric value associated with the command
            float value;

            // Parse the incoming string (e.g., "set tempo 1.2")
            if (sscanf(buffer, "set %s %f", param, &value) == 2) {
                // Safely update the shared state
                pthread_mutex_lock(&shared_config.lock);
                if (strcmp(param, "tempo") == 0) {
                    shared_config.tempo = value;
                    printf("[Control] Tempo updated to %.2f\n", value);
                } else if (strcmp(param, "pitch") == 0) {
                    shared_config.pitch = value;
                    printf("[Control] Pitch updated to %.2f\n", value);
                } else {
                    printf("[Control] Unknown parameter: %s\n", param);
                }
                pthread_mutex_unlock(&shared_config.lock);
            }
            memset(buffer, 0, BUFFER_SIZE);
        }
        close(new_socket);
    }
    close(server_fd);
    return NULL;
}


// --- MAIN ROUTINE ---
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_audio.wav>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Initialize Shared DSP Config
    shared_config.tempo = 1.0f;
    shared_config.pitch = 1.0f;
    pthread_mutex_init(&shared_config.lock, NULL);

    // Opens WAV file and extracts structural metadata
    SF_INFO sfinfo;
    sfinfo.format = 0;
    infile = sf_open(argv[1], SFM_READ, &sfinfo);
    if (!infile) return EXIT_FAILURE;

    // Initialize and configure the ALSA PCM Hardware Interface
    if (snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0) return EXIT_FAILURE;
    snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, sfinfo.channels, sfinfo.samplerate, 1, 500000);

    // Allocate memory and initialize synchronization for the Circular Buffer
    ring_buf.max_frames = CHUNK_FRAMES * RING_BUFFER_CHUNKS;
    ring_buf.channels = sfinfo.channels;
    ring_buf.data = (short *)malloc(ring_buf.max_frames * ring_buf.channels * sizeof(short)); // Added cast
    ring_buf.head = ring_buf.tail = ring_buf.count = 0;
    ring_buf.eof_reached = false;

    pthread_mutex_init(&ring_buf.lock, NULL);
    pthread_cond_init(&ring_buf.not_empty, NULL);
    pthread_cond_init(&ring_buf.not_full, NULL);

    // --- INITIALIZE DSP ENGINE (Using native C++ initialization) ---
    pSoundTouch = new soundtouch::SoundTouch();
    pSoundTouch->setSampleRate(sfinfo.samplerate);
    pSoundTouch->setChannels(sfinfo.channels);
    pSoundTouch->setTempo(1.0f);
    pSoundTouch->setPitch(1.0f);


    printf("--- Starting Multithreaded Audio Engine ---\n");

    // Release concurrent execution threads
    pthread_t producer, consumer, control;
    pthread_create(&producer, NULL, producer_thread_func, NULL);
    pthread_create(&consumer, NULL, consumer_thread_func, NULL);
    pthread_create(&control, NULL, control_thread_func, NULL);

    // Blocks main execution until the primary operations threads temrinate
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    // Note: The control thread is a daemon thread in this design.
    // It will cleanly die when the main process exits after playback finishes.

    // Execute graceful shutdown of hardware peripherals, memory, and synchronization variables

    delete pSoundTouch; // Free C++ DSP memory
    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    sf_close(infile);
    free(ring_buf.data);
    pthread_mutex_lock(&ring_buf.lock); // Lock before destroy as best practice
    pthread_mutex_unlock(&ring_buf.lock);
    pthread_mutex_destroy(&ring_buf.lock);
    pthread_cond_destroy(&ring_buf.not_empty);
    pthread_cond_destroy(&ring_buf.not_full);
    pthread_mutex_destroy(&shared_config.lock);

    printf("--- System Shut Down Cleanly ---\n");
    return EXIT_SUCCESS;
}







