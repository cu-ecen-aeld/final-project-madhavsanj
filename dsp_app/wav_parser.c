/*
File: wav_parser.c
Author: Madhav Appanaboyina

References:
    - libsndfile Documentation:
    http://www.mega-nerd.com/libsndfile/api.html

    - libsndfile Examples:
    http://www.mega-nerd.com/libsndfile/examples.html

    - WAV File Format Specification:
    http://soundfile.sapp.org/doc/WaveFormat/

    - SoundTouch Audio Processing Library:
    https://codeberg.org/soundtouch/soundtouch

    Concepts covered in assignments:
    - File handling and parsing structured data(A3)
    - Buffer-based data processing (A5)

Brief:
    - Uses libsndfile to open and parse WAV files
    - Validates CLI input
    - Uses SF_INFO to extract metadata
    - Prints sample rate, channels, frames
    - Handles errors
    - Closes the file that was parsed
*/


#include <stdio.h>
#include <stdlib.h>
#include <sndfile.h>

int main(int argc, char *argv[]) {
    // To validate Command Line Arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_audio.wav>\n", argv[0]);
        return EXIT_FAILURE;
    }

    //Store input file name
    const char *infilename = argv[1];
    SF_INFO sfinfo;
    
    // Initializes sfinfo before passing it to sf_open
    sfinfo.format = 0; 

    // Opens the WAV file in READ mode
    SNDFILE *infile = sf_open(infilename, SFM_READ, &sfinfo);
    if (!infile) {
        fprintf(stderr, "ERROR: Could not open file '%s'\n", infilename);
        fprintf(stderr, "libsndfile error: %s\n", sf_strerror(infile));
        return EXIT_FAILURE;
    }

    // Extract and Print Metadata
    printf("--- WAV File Parsed Successfully ---\n");
    printf("File Name   : %s\n", infilename);
    printf("Sample Rate : %d Hz\n", sfinfo.samplerate);
    printf("Channels    : %d\n", sfinfo.channels);
    printf("Frames      : %ld\n", sfinfo.frames);

    // Clean up memory
    sf_close(infile);

    return EXIT_SUCCESS;
}
