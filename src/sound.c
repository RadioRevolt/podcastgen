// sound.c
// ******************************
//
// podcastgen
// by Trygve Bertelsen Wiig, 2014
//
// This program detects speech and music
// using the algorithm given in
// http://www.speech.kth.se/prod/publications/files/3437.pdf.
//
// It then removes the music from the input file, and
// fades the speech sections into each other.

#include <sndfile.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <math.h>

#include "sound.h"
#include "main.h"
#include "util.h"
#include "files.h"

const int RMS_FRAME_DURATION = 20; // Length of the RMS calculation frames in milliseconds
const int LONG_FRAME_DURATION = 1000; // Length of the long (averaging) frames in milliseconds
int FRAMES_IN_RMS_FRAME;
int FRAMES_IN_LONG_FRAME;
int RMS_FRAME_COUNT;
int LONG_FRAME_COUNT;
int RMS_FRAMES_IN_LONG_FRAME;

float LOW_ENERGY_COEFFICIENT = 0.20; // see http://ieeexplore.ieee.org/stamp/stamp.jsp?tp=&arnumber=1292679
float UPPER_MUSIC_THRESHOLD = 0.0; // MLER below this value => 1 second frame classified as music

float *calculate_rms(float *rms) {
	float *read_cache = malloc(2*FRAMES_IN_RMS_FRAME*sizeof(float));
	sf_count_t frames_read = 0;

	int frame; // dummy variable for inner loop
	double local_rms; // temporary variable for inner loop

	for (int rms_frame = 0; rms_frame < RMS_FRAME_COUNT; rms_frame++) {
		frames_read = sf_readf_float(source_file, read_cache, FRAMES_IN_RMS_FRAME);
		local_rms = 0;

		for (frame = 0; frame < frames_read; frame++) {
			local_rms += pow(read_cache[frame], 2);
		}

		local_rms = sqrt(local_rms/RMS_FRAME_DURATION);
		rms[rms_frame] = local_rms;
	}

	free(read_cache);
	return rms;
}

float *calculate_features(float *rms, float *mean_rms, float *variance_rms, float *norm_variance_rms, float *mler) {

	int start_rms_frame = 0;
	int current_rms_frame = 0;

	float rms_sum;
	float variance_difference_sum; // sum of (x_i - mu)^2
	float lowthres; // used to compute the MLER value

	for (int long_frame = 0; long_frame < LONG_FRAME_COUNT; long_frame++) {
		// We calculate four features for the 1 second interval:
		// - mean RMS
		// - variance of the RMS values
		// - normalized variance of the RMS values (i.e. variance divided by mean RMS)
		// - Modified Low Energy Ratio (MLER)
		rms_sum = 0;
		variance_difference_sum = 0;
		lowthres = 0;
	
		// Mean RMS
		for (current_rms_frame = start_rms_frame; current_rms_frame < start_rms_frame + RMS_FRAMES_IN_LONG_FRAME; current_rms_frame++) {
			rms_sum += rms[current_rms_frame];
		}
		mean_rms[long_frame] = rms_sum/RMS_FRAMES_IN_LONG_FRAME;

		// Variances and MLER
		lowthres = LOW_ENERGY_COEFFICIENT*mean_rms[long_frame];
		mler[long_frame] = 0;
		for (current_rms_frame = start_rms_frame; current_rms_frame < start_rms_frame + RMS_FRAMES_IN_LONG_FRAME; current_rms_frame++) {
			variance_difference_sum += pow(rms[current_rms_frame] - rms_sum, 2);
			mler[long_frame] += signum(lowthres-rms[current_rms_frame]) + 1;
			variance_difference_sum += pow(rms[current_rms_frame] - rms_sum, 2);
		}
		variance_rms[long_frame] = variance_difference_sum/RMS_FRAMES_IN_LONG_FRAME;
		norm_variance_rms[long_frame] = variance_rms[long_frame]/mean_rms[long_frame];
		mler[long_frame] = mler[long_frame]/(2*RMS_FRAMES_IN_LONG_FRAME);

		logger(INFO, "Seconds: %d\n", long_frame);
		logger(INFO, "Mean: %f\n", mean_rms[long_frame]);
		logger(INFO, "Variance: %f\n", variance_rms[long_frame]);
		logger(INFO, "Normalized variance: %f\n", norm_variance_rms[long_frame]);
		logger(INFO, "MLER: %f\n\n", mler[long_frame]);

		start_rms_frame += RMS_FRAMES_IN_LONG_FRAME;
	}
}

void classify_segments(bool *is_music, float *mler) {
	for (int long_frame = 0; long_frame < LONG_FRAME_COUNT; long_frame++) {
		if (mler[long_frame] <= UPPER_MUSIC_THRESHOLD) {
			is_music[long_frame] = true;
		} else {
			is_music[long_frame] = false;
		}
	}
}

void average_musicness(bool *is_music) {
	bool *music_second_pass = malloc(2*LONG_FRAME_COUNT*sizeof(bool));
	music_second_pass[0] = true;
	music_second_pass[1] = true;
	music_second_pass[2] = true;
	for (int long_frame = 3; long_frame < LONG_FRAME_COUNT-3; long_frame++) {
		music_second_pass[long_frame] = rint((is_music[long_frame-3]+is_music[long_frame-2]+is_music[long_frame-1]+is_music[long_frame]+is_music[long_frame+1]+is_music[long_frame+2]+is_music[long_frame+3])/5.0);
		//music_second_pass[long_frame] = is_music[long_frame];
	}
	music_second_pass[LONG_FRAME_COUNT] = false;
	music_second_pass[LONG_FRAME_COUNT-1] = false;
	music_second_pass[LONG_FRAME_COUNT-2] = false;
	for (int long_frame = 0; long_frame < LONG_FRAME_COUNT; long_frame++) {
		is_music[long_frame] = music_second_pass[long_frame];
	}
	free(music_second_pass);
}

int merge_segments(bool *is_music, segment *merged_segments) {
	// Create segment structs with corresponding start, end and
	// is_music values
	segment *segments = malloc(LONG_FRAME_COUNT*sizeof(segment));
	int current_segment = 0;
	for (int long_frame = 0; long_frame < LONG_FRAME_COUNT; long_frame++) {
		if (long_frame == 0) {
			segments[current_segment].startframe = 0;
			segments[current_segment].endframe = 0;
			segments[current_segment].is_music = true;
		} else if (is_music[long_frame] == segments[current_segment].is_music) {
			segments[current_segment].endframe++;
		} else {
			current_segment++;
			segments[current_segment].startframe = long_frame;
			segments[current_segment].endframe = long_frame;
			segments[current_segment].is_music = is_music[long_frame];
		}
	}

	// Merge smaller segments into larger segments and
	// merge segments of the same type into each other
	int current_merged_segment = 0;
	for (int seg = 0; seg < current_segment; seg++) {
		if (seg == 0) {
			merged_segments[0].startframe = segments[0].startframe;
			merged_segments[0].endframe = segments[0].endframe;
			if (has_intro) {
				merged_segments[0].is_music = false;
			}
		} else if (segments[seg].endframe - segments[seg].startframe < 10) {
			merged_segments[current_merged_segment].endframe = segments[seg].endframe;
		} else if (segments[seg].is_music == merged_segments[current_merged_segment].is_music) {
			merged_segments[current_merged_segment].endframe = segments[seg].endframe;
		} else {
			current_merged_segment++;
			merged_segments[current_merged_segment].startframe = segments[seg].startframe;
			merged_segments[current_merged_segment].endframe = segments[seg].endframe;
			merged_segments[current_merged_segment].is_music = segments[seg].is_music;
		}
	}

	free(segments);

	// Grow speech segments slightly
	const int GROW_BY_BEFORE = 3; // seconds
	const int GROW_BY_AFTER = 3; // seconds
	if (has_intro) {
		merged_segments[0].endframe += GROW_BY_AFTER;
	} else {
		merged_segments[0].endframe -= GROW_BY_BEFORE;
	}
	for (int seg = 1; seg < current_segment; seg++) {
		if (merged_segments[seg].is_music) {
			merged_segments[seg].startframe += GROW_BY_BEFORE;
			merged_segments[seg].endframe -= GROW_BY_AFTER;
		} else {
			merged_segments[seg].startframe -= GROW_BY_BEFORE;
			merged_segments[seg].endframe += GROW_BY_AFTER;
		}
	}

	return current_merged_segment;
}
