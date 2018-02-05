

/*

		Steles is a MIDI sequencer for the "Tine" hardware.
		THIS CODE IS UNDER DEVELOPMENT AND DOESN'T DO ANYTHING!
		Copyright (C) 2016-2018, C.D.M. Rørmose (sevenplagues@gmail.com).

		This program is free software; you can redistribute it and/or modify
		it under the terms of the GNU General Public License as published by
		the Free Software Foundation; either version 2 of the License, or
		(at your option) any later version.

		This program is distributed in the hope that it will be useful,
		but WITHOUT ANY WARRANTY; without even the implied warranty of
		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
		GNU General Public License for more details.

		You should have received a copy of the GNU General Public License along
		with this program; if not, write to the Free Software Foundation, Inc.,
		51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
		
*/



// Define statements:
//   These values may need to be changed in the course of programming/debugging,
//   but will always stay the same at runtime.
#define FILE_BYTES 393265UL // Number of bytes in each seq-file
#define SCANRATE 7000 // Amount of time between keystroke-scans, in microseconds
#define GESTDECAY 450000UL // Amount of time between gesture-decay ticks, in microseconds



// Program-space (PROGMEM) library
#include <avr/pgmspace.h>

// MAX7219/MAX7221 LED-matrix library
#include <LedControl.h>

// Serial Peripheral Interface library
#include <SPI.h>

// SD-card data-storage library (requires SPI.h)
#include <SdFat.h>



// UI vars
unsigned long BUTTONS = 0; // Tracks which of the 30 buttons are currently pressed; each button has an on/off bit
byte GESTURE[5]; // Tracks currently-active button-gesture events
byte PAGE = 0; // Tracks currently-active page of sequences
byte BLINK = 0; // When an LED-BLINK is active, this will count down to 0
byte TO_UPDATE = 0; // Tracks which rows of LEDs should be updated at the end of a given tick

// Timing vars
unsigned long ABSOLUTETIME = 0; // Absolute time elapsed: wraps around after reaching its limit
unsigned long ELAPSED = 0; // Time elapsed since last tick
word KEYELAPSED = 0; // Time elapsed since last keystroke-scan
unsigned long GESTELAPSED = 0; // Time elapsed since last gesture-decay
unsigned long TICKSIZE = 6250; // Size of the current tick, in microseconds; tick = 60000000 / (bpm * 96)

// Recording vars
byte LOADMODE = 0; // Tracks whether LOAD MODE is active
byte RECORDMODE = 0; // Tracks whether RECORD MODE is active
byte RECORDSEQ = 0; // Sequence currently being recorded into
byte RECORDNOTES = 0; // Tracks whether notes are currently being recorded into a sequence
byte REPEAT = 0; // Toggles whether held-down note-buttons should repeat a NOTE-ON every QUANTIZE ticks, in RECORD-MODE
byte BASENOTE = 0; // Baseline pitch-offset value for RECORD MODE notes
byte OCTAVE = 3; // Octave-offset value for RECORD MODE notes
byte VELO = 127; // Baseline velocity-value for RECORD MODE notes
byte HUMANIZE = 0; // Maximum velocity-humanize value for RECORD MODE notes
byte CHAN = 0; // MIDI-CHAN for RECORD MODE notes
byte QUANTIZE = B00000010; // Time-quantize value for RECORD MODE notes: bits 0-3: 1, 2, 4, 8 (16th-notes)
byte DURATION = B00000100; // Duration value for RECORD MODE notes: bits 0-4: 1, 2, 4, 8, 16 (16th-notes)
byte COPYPOS = 0; // Copy-position within the copy-sequence
byte COPYSIZE = 1; // Number of beats to copy (1, 2, 4, 8, 16, 32)
byte COPYSEQ = 0; // Sequence from which to copy data

// Sequencing vars
byte SONG = 0; // Current song-slot whose data-files are being played
byte PLAYING = 1; // Controls whether the sequences and global tick-counter are iterating
byte DUMMYTICK = 0; // Tracks whether to expect a dummy MIDI CLOCK tick before starting to iterate through sequences
byte CLOCKMASTER = 1; // Toggles whether to generate MIDI CLOCK ticks, or respond to incoming CLOCK ticks from an external device
byte BPM = 80; // Beats-per-minute value: one beat is 96 tempo-ticks
byte TICKCOUNT = 5; // Current global tick, bounded within the size of a 16th-note
byte CUR16 = 127; // Current global sixteenth-note (bounded to 128, or 8 beats)
word GLOBALRAND = 12345; // Global all-purpose semirandom value; gets changed on every tick

// Beat-scattering flags, one per seq.
// bits 0-3: scatter chance
// bits 4-7: scatter distance (0=off; 1,2,4,8 = 8th,4th,half,whole [these can stack with each other])
byte SCATTER[49];

// Cued-command flags, one per seq.
// bit 0: TURN OFF
// bit 1: TURN ON
// bits 2-4: slice 4, 2, 1;
// bits 5-7: cue 4, 2, 1;
byte CMD[49];

// Holds 48 seqs' sizes and activity-flags
// bits 0-5: 1, 2, 4, 8, 16, 32 (= size, in beats)
// bit 6: reserved
// bit 7: on/off flag
byte STATS[49];

// Holds the 48 seqs' internal tick-positions
// bits 0-9: current 16th-note position (0-1023)
// bits 10-15: reserved
word POS[49];

// Holds up to 8 MIDI notes from a given tick,
// (format: MOUT[n*3] = command, pitch, velocity)
// to be sent in a batch at the tick's end
byte MOUT[25];
byte MOUT_COUNT = 0; // Counts the current number of note entries in MOUT

// Note-Sustain data storage
// (format: SUST[n*3] = channel, pitch, duration)
byte SUST[25];
byte SUST_COUNT = 0; // Counts the current number of sustained notes

// Keeps a record of the most recent note-pitch sent to each MIDI channel
// Default value is 60 (middle C), which is replaced when the channel receives its first note
byte RECENT[17] = {60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60};

// MIDI-IN vars
byte INBYTES[4]; // Buffer for incoming MIDI commands
byte INCOUNT = 0; // Number of MIDI bytes received from current incoming command
byte INTARGET = 0; // Number of expected incoming MIDI bytes
byte SYSIGNORE = 0; // Ignores SYSEX messages when toggled



// Typedef for a generic function that takes "column" and "row" arguments.
// This will be used to select RECORD-MODE functions based on which keychords are active.
typedef void (*CmdFunc) (byte col, byte row);

// Tell the compiler that the list of RECORD-MODE commands is sitting in another file (func_cmds.ino),
//   and should be used here too.
// It has to be in that file so that its associated functions get defined before COMMANDS[] gets defined.
extern const CmdFunc COMMANDS[];

// Matches control-row binary button-values to the decimal values that stand for their corresponding functions in COMMANDS
// Note: These binary values are flipped versions of what are displayed in button-key.txt
const byte KEYTAB[] PROGMEM = {
	8,  //  0, 000000:  8, genericCmd (REGULAR NOTE)
	15, //  1, 000001: 15, repeatCmd
	3,  //  2, 000010:  3, chanCmd
	12, //  3, 000011: 12, posCmd
	19, //  4, 000100: 19, veloCmd
	0,  //  5, 000101:  0, ignore
	9,  //  6, 000110:  9, humanizeCmd
	0,  //  7, 000111:  0, ignore
	10, //  8, 001000: 10, octaveCmd
	4,  //  9, 001001:  4, clockCmd
	11, // 10, 001010: 11, pasteCmd
	0,  // 11, 001011:  0, ignore
	14, // 12, 001100: 14, quantizeCmd
	0,  // 13, 001101:  0, ignore
	0,  // 14, 001110:  0, ignore
	17, // 15, 001111: 17, switchCmd
	2,  // 16, 010000:  2, baseCmd
	0,  // 17, 010001:  0, ignore
	18, // 18, 010010: 18, tempoCmd
	0,  // 19, 010011:  0, ignore
	6,  // 20, 010100:  6, copyCmd
	0,  // 21, 010101:  0, ignore
	0,  // 22, 010110:  0, ignore
	0,  // 23, 010111:  0, ignore
	7,  // 24, 011000:  7, durationCmd
	0,  // 25, 011001:  0, ignore
	0,  // 26, 011010:  0, ignore
	0,  // 27, 011011:  0, ignore
	0,  // 28, 011100:  0, ignore
	0,  // 29, 011101:  0, ignore
	0,  // 30, 011110:  0, ignore
	0,  // 31, 011111:  0, ignore
	1,  // 32, 100000:  1, armCmd
	16, // 33, 100001: 16, sizeCmd
	13, // 34, 100010: 13, programChangeCmd
	0,  // 35, 100011:  0, ignore
	5,  // 36, 100100:  5, controlChangeCmd
	0,  // 37, 100101:  0, ignore
	0,  // 38, 100110:  0, ignore
	0,  // 39, 100111:  0, ignore
	8,  // 40, 101000:  8, genericCmd (INTERVAL-UP)
	0,  // 41, 101001:  0, ignore
	0,  // 42, 101010:  0, ignore
	0,  // 43, 101011:  0, ignore
	8,  // 44, 101100:  8, genericCmd (INTERVAL-UP-RANDOM)
	0,  // 45, 101101:  0, ignore
	0,  // 46, 101110:  0, ignore
	0,  // 47, 101111:  0, ignore
	8,  // 48, 110000:  8, genericCmd (INTERVAL-DOWN)
	0,  // 49, 110001:  0, ignore
	0,  // 50, 110010:  0, ignore
	0,  // 51, 110011:  0, ignore
	8,  // 52, 110100:  8, genericCmd (INTERVAL-DOWN-RANDOM)
	0,  // 53, 110101:  0, ignore
	0,  // 54, 110110:  0, ignore
	0,  // 55, 110111:  0, ignore
	0,  // 56, 111000:  0, ignore
	0,  // 57, 111001:  0, ignore
	0,  // 58, 111010:  0, ignore
	0,  // 59, 111011:  0, ignore
	0,  // 60, 111100:  0, ignore (ERASE-WHILE-HELD is handled by other routines)
	0,  // 61, 111101:  0, ignore
	0,  // 62, 111110:  0, ignore
	0,  // 63, 111111:  0, ignore
};



// Long glyph: logo to display on startup
const byte LOGO[] PROGMEM = {
	B01111110, B01111110, B01000110, B01111110,
	B01111110, B01111110, B01100110, B01111110,
	B00011000, B00011000, B01110110, B01100000,
	B00011000, B00011000, B01111110, B01111100,
	B00011000, B00011000, B01101110, B01100000,
	B00011000, B01111110, B01100110, B01111110,
	B00011000, B01111110, B01100010, B01111110
};

// Glyph: BASENOTE
const byte GLYPH_BASENOTE[] PROGMEM = {B00000000, B11101001, B10101101, B11001111, B10101011, B11101001};

// Glyph: BPM
const byte GLYPH_BPM[] PROGMEM = {B00000000, B01010001, B01011011, B01010101, B11010001, B11010001};

// Glyph: CHAN
const byte GLYPH_CHAN[] PROGMEM = {B00000000, B11101000, B10101000, B10001100, B10101010, B11101010};

// Glyph: CLOCK-MASTER
const byte GLYPH_CLOCKMASTER[] PROGMEM = {B00000000, B11101000, B10101000, B10001010, B10101100, B11101010};

// Glyph: CONTROL-CHANGE
const byte GLYPH_CONTROLCHANGE[] PROGMEM = {B00000000, B11101110, B10101010, B10001000, B10101010, B11101110};

// Glyph: DURATION
const byte GLYPH_DURATION[] PROGMEM = {B00000000, B11000000, B10100000, B10101110, B10101000, B11001000};

// Glyph: ERASING
const byte GLYPH_ERASE[] PROGMEM = {B00000000, B11101010, B10001010, B11101010, B10000000, B11101010};

// Glyph: HUMANIZE
const byte GLYPH_HUMANIZE[] PROGMEM = {B00000000, B10100000, B10100000, B11101010, B10101010, B10101110};

// Glyph: LOAD
const byte GLYPH_LOAD[] PROGMEM = {B00000000, B10001010, B10001010, B10001010, B10000000, B11101010};

// Glyph: OCTAVE
const byte GLYPH_OCTAVE[] PROGMEM = {B00000000, B11100000, B10100000, B10101110, B10101000, B11101110};

// Glyph: QUANTIZE
const byte GLYPH_QUANTIZE[] PROGMEM = {B00000000, B11100000, B10100000, B10100101, B10100101, B11110111};

// Glyph: RECORD
const byte GLYPH_RECORD[] PROGMEM = {B11100000, B10010000, B11100000, B10010000, B10010000, B10010000};

// Glyph: REPEAT
const byte GLYPH_REPEAT[] PROGMEM = {B01000000, B01000000, B01000000, B01000000, B11000000, B11000000};

// Glyph: REPEAT (ARMED)
const byte GLYPH_REPEAT_ARMED[] PROGMEM = {B01000000, B01001000, B01001001, B01001001, B11011011, B11011011};

// Glyph: SWITCH RECORDING-SEQUENCE
const byte GLYPH_RSWITCH[] PROGMEM = {B00000000, B11101110, B10101000, B11000100, B10100010, B10101110};

// Glyph: SD-CARD
const byte GLYPH_SD[] PROGMEM = {B00000000, B11101100, B10001010, B01001010, B00101010, B11101100};

// Glyph: SEQ-SIZE
const byte GLYPH_SIZE[] PROGMEM = {B00000000, B11101110, B10000010, B01000100, B00101000, B11101110};

// Glyph: VELO
const byte GLYPH_VELO[] PROGMEM = {B00000000, B10010000, B10010000, B01010000, B00110000, B00010000};

// Interval button glyphs: RANDOM, DOWN, UP
const byte GLYPH_RANDOM[] PROGMEM = {B11110000, B10010000, B00110000, B00100000, B00000000, B00100000};
const byte GLYPH_DOWN[] PROGMEM = {B00000000, B00000000, B00000110, B00000110, B00001111, B00000110};
const byte GLYPH_UP[] PROGMEM = {B00000110, B00001111, B00000110, B00000110, B00000000, B00000000};

// Number multiglyph: holds all numbers used for file-page display (0-6)
const byte MULTIGLYPH_NUMBERS[] PROGMEM = {
	B00111100, B00100100, B00100100, B00100100, B00100100, B00111100, // 0
	B00001000, B00011000, B00001000, B00001000, B00001000, B00001000, // 1
	B00111100, B00100100, B00001000, B00010000, B00010000, B00111100, // 2
	B00111100, B00000100, B00011100, B00000100, B00100100, B00111100, // 3
	B00100100, B00100100, B00111100, B00000100, B00000100, B00000100, // 4
	B00111100, B00100000, B00111100, B00000100, B00100100, B00111100, // 5
	B00111100, B00100000, B00111100, B00100100, B00100100, B00111100  // 6
};






SdFat sd; // Initialize SdFat object
SdFile file; // Initialize an SdFile File object, to control default data read/write processes

LedControl lc = LedControl(5, 7, 6, 1); // Initialize the object that controls the MAX7219's LED-grid



void setup() {

	// Set all the keypad's row-pins to INPUT_PULLUP mode, and all its column-pins to OUTPUT mode
	DDRC = 0;
	PORTC = 255;
	DDRD |= B00011100;
	DDRB |= B00000011;

	// Power up ledControl to full brightness
	lc.shutdown(0, false);
	lc.setIntensity(0, 15);

	// Initialize the SD-card at full speed, or throw a visible error message if no SD-card is inserted
	if (!sd.begin(10, SPI_FULL_SPEED)) {
		lc.setRow(0, 2, B11101110);
		lc.setRow(0, 3, B10001001);
		lc.setRow(0, 4, B11101001);
		lc.setRow(0, 5, B00101001);
		lc.setRow(0, 6, B00101010);
		lc.setRow(0, 7, B11101100);
		sd.initErrorHalt();
	}

	createFiles(); // Check whether the savefiles exist, and if they don't, then create them

	loadSong(SONG); // Load the default song-slot

	TO_UPDATE = 255; // Cue all GUI rows for an initial update

	// updateGUI(); // ...And perform the update right now
	// Note: this would lead the cue-row to briefly flash its 8th LED,
	// so keep it commented out, and just wait for the first call to updateGUI() from within loop().

	Serial.begin(31250); // Start serial comms at the MIDI baud rate
	
	ABSOLUTETIME = micros(); // Make sure ABSOLUTETIME matches the current time, so tempo doesn't have an initial jerk

}


void loop() {

	parseRawMidi(); // Parse incoming MIDI

	updateTimer(); // Update the global timer

	updateGUI(); // Update the GUI, if applicable

	updateGlobalRand(); // Update the global semirandom value

}

