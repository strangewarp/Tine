

// Check whether savefiles exist, and create whichever ones don't exist
void createFiles() {

	char name[8]; // Will hold a given filename

	for (byte i = 0; i < 48; i++) { // For every song-slot...

		sendRow(0, i); // Send the top LED-row a binary value showing which file is about to be created

		getFilename(name, i); // Get the filename that corresponds to this song-slot

		if (sd.exists(name)) { continue; } // If the file exists, skip the file-creation process for this filename

		file.createContiguous(sd.vwd(), name, FILE_BYTES); // Create a contiguous file
		file.close(); // Close the newly-created file

		file.open(name, O_WRITE); // Open the file explicitly in WRITE mode

		file.seekSet(FILE_BPM_BYTE); // Go to the BPM-byte
		file.write(byte(DEFAULT_BPM)); // Write the default BPM value

		for (byte j = FILE_SQS_START; j <= FILE_SQS_END; j++) { // For every seq-size byte in the header...
			file.seekSet(j); // Go to that byte's position
			file.write(byte(DEFAULT_SEQ_SIZE)); // Write the default sequence-size value
		}

		file.close(); // Close the file

	}

}

// Get the name of a target song-slot's savefile (format: "000.DAT" etc.)
void getFilename(char source[], byte fnum) {
	source[0] = 48;
	source[1] = (char(floor(fnum / 10)) % 10) + 48;
	source[2] = char((fnum % 10) + 48);
	source[3] = 46;
	source[4] = 68;
	source[5] = 65;
	source[6] = 84;
	source[7] = 0;
}

// Update a given byte within a given song-file.
// This is used only to update various bytes in the header, so "byte pos" is acceptable.
void updateFileByte(byte pos, byte b) {
	file.seekSet(pos); // Go to the given insert-point
	file.write(b); // Write the new byte into the file
	file.sync(); // Make sure the changes are recorded
}

// Check whether a given header-byte is a duplicate, and if not, update it in the savefile.
// This is used only to update various bytes in the header, so "byte pos" is acceptable.
void updateNonMatch(byte pos, byte b) {
	file.seekSet(pos); // Go to the corresponding byte in the savefile
	if (file.read() != b) { // Read the header-byte. If it isn't the same as the given byte-value...
		updateFileByte(pos, b); // Put the local header-byte into its corresponding savefile header-byte
	}
}

// Update the header-data-byte for the current seq: both SEQ SIZE (bits 0-4) and ACTIVE ON LOAD (bit 6).
//   (bits 5 and 7 [0-indexed] are intentionally stripped, for the savefile)
void updateSeqHeader() {
	updateNonMatch(FILE_SQS_START + RECORDSEQ, STATS[RECORDSEQ] & 95);
}

// Update the current seq's CHAIN-byte in the savefile, if it has been changed
void updateSavedChain() {
	updateNonMatch(FILE_CHAIN_START + RECORDSEQ, CHAIN[RECORDSEQ]);
}

// Send a given header-block's special MIDI-commands to the MIDI-OUT apparatus
void sendHeaderBlock(byte buf[], byte hpos) {

	file.seekSet(hpos); // Navigate to the "SEND ON EXIT" block in the header
	file.read(buf, 48); // Grab the entire block into the "sbuf" buffer

	byte c = 0; // Will track how many special command slots are filled
	while ((c < 48) && buf[c]) { // While fewer than 16 slots have been checked, and the current slot is full...
		c += 3; // Increase the number of filled special command slots, by their number of bytes (3 bytes per slot)
	}

	if (c) { // If any special command slots are filled...
		Serial.write(buf, c); // Send the special commands to MIDI-OUT immediately
	}

}

// Put the current prefs-related global vars into a given buffer
void makePrefBuf(byte buf[]) {
	buf[0] = 0; // reserved
	buf[1] = OCTAVE;
	buf[2] = VELO;
	buf[3] = HUMANIZE;
	buf[4] = CHAN;
	buf[5] = QRESET;
	buf[6] = QUANTIZE;
	buf[7] = DURATION;
	buf[8] = DURHUMANIZE;
	buf[9] = OFFSET;
	buf[10] = SONG;
	buf[11] = 0; // reserved
	buf[12] = GRIDCONFIG;
	buf[13] = RPTVELO;
	buf[14] = RPTSWEEP;
	buf[15] = ARPMODE;
	buf[16] = ARPREFRESH;
}

// Get the prefs-file's filename out of PROGMEM
void getPrefsFilename(char pn[]) {
	for (byte i = 0; i < 6; i++) { // For each letter in the filename...
		pn[i] = pgm_read_byte_near(PREFS_FILENAME + i); // Get that letter out of PROGMEM
	}
}

// Write the current relevant global vars into PRF.DAT
void writePrefs() {

	file.close(); // Temporarily close the current song-file

	byte buf[PREFS_ITEMS_2]; // Make a buffer that will contain all the prefs
	makePrefBuf(buf); // Fill it with all the relevant pref values

	byte cbuf[PREFS_ITEMS_2]; // Make a buffer for checking the old prefs-contents

	char pn[6]; // Will contain the prefs-file's filename
	getPrefsFilename(pn); // Get the prefs-file's filename out of PROGMEM

	file.open(pn, O_RDWR); // Open the prefs-file in read-write mode

	file.seekSet(0); // Ensure we're on the first byte of the file
	file.read(cbuf, PREFS_ITEMS_1); // Read the old prefs-values

	for (byte i = 0; i < PREFS_ITEMS_1; i++) { // For every byte in the prefs-file...
		if (buf[i] != cbuf[i]) { // If the new byte doesn't match the old byte...
			file.seekSet(i); // Go to the byte's location
			file.write(buf[i]); // Replace the old byte
		}
	}

	file.close(); // Close the prefs-file

	char name[8];
	getFilename(name, SONG); // Get the name of the current song-file

	file.open(name, O_RDWR); // Reopen the current song-file

	file.seekSet(FILE_BPM_BYTE); // Go to the BPM-byte's location
	if (file.read() != BPM) { // If the BPM-byte doesn't match the current BPM...
		updateFileByte(FILE_BPM_BYTE, BPM); // Update the BPM-byte in the song's savefile
	}

}

// Load the contents of the preferences file ("PRF.DAT"), and put its contents into global variables
void loadPrefs() {

	byte buf[PREFS_ITEMS_2]; // Make a buffer that will contain all the prefs

	char pn[6]; // Will contain the prefs-file's filename
	getPrefsFilename(pn); // Get the prefs-file's filename out of PROGMEM

	if (!sd.exists(pn)) { // If the prefs-file doesn't exist, create the file

		makePrefBuf(buf); // Fill the prefs-data buffer with the current user-prefs

		file.createContiguous(sd.vwd(), pn, PREFS_ITEMS_1); // Create a contiguous prefs-file
		file.close(); // Close the newly-created file

		file.open(pn, O_WRITE); // Create a prefs file and open it in write-mode
		file.write(buf, PREFS_ITEMS_1); // Write the pref vars into the file

	} else { // Else, if the prefs-file does exist...

		file.open(pn, O_READ); // Open the prefs-file in read-mode
		file.read(buf, PREFS_ITEMS_1); // Read all the prefs-bytes into the buffer

		// Assign the buffer's-worth of pref-bytes to their respective global-vars
		// reserved = buf[0];
		OCTAVE = buf[1];
		VELO = buf[2];
		HUMANIZE = buf[3];
		CHAN = buf[4];
		QRESET = buf[5];
		QUANTIZE = buf[6];
		DURATION = buf[7];
		DURHUMANIZE = buf[8];
		OFFSET = buf[9];
		SONG = buf[10];
		// reserved = buf[11];
		GRIDCONFIG = buf[12];
		RPTVELO = buf[13];
		RPTSWEEP = buf[14];
		ARPMODE = buf[15];
		ARPREFRESH = buf[16];

	}

	file.close(); // Close the prefs file, regardless of whether it already exists or was just created

}

// Load a given song, or create its savefile if it doesn't exist
void loadSong(byte slot) {

	byte sbuf[49]; // Create a buffer for incoming ACTIVE ON EXIT / ACTIVE ON LOAD bytes

	if (file.isOpen()) { // If a savefile is already open...

		sendHeaderBlock(sbuf, FILE_ONEXIT_START); // Send the SEND ON EXIT header-block's special MIDI-commands to the MIDI-OUT apparatus

		file.close(); // Close the current savefile

	}

	resetAll(); // Reset all sequence-data (this leaves the GLOBAL CUE the same)

	char name[8]; // Create a buffer to hold the new savefile's filename
	getFilename(name, slot); // Get the name of the target song-slot's savefile

	file.open(name, O_RDWR); // Open the new savefile

	// There is rarely a bug where the BPM is set to an erroneous value (this can occur on startup for unknown reasons),
	// so we have to compensate for that while loading BPM from a savefile.
	// At the same time, we will compensate for files where the BPM value has been edited to fall outside the valid BPM-range.
	file.seekSet(FILE_BPM_BYTE); // Go to the BPM-byte location
	BPM = file.read(); // Read it
	// If this is an erroneous value that falls outside of the valid BPM range...
	if ((BPM < BPM_LIMIT_LOW) || (word(BPM) > BPM_LIMIT_HIGH)) {
		BPM = DEFAULT_BPM; // Set the BPM to the default value
	}

	file.seekSet(FILE_SQS_START); // Go to the file-header's SEQ-SIZE block
	file.read(STATS, 48); // Read every seq's new size bits, and ACTIVE ON LOAD bit, directly into each seq's STATS byte

	file.seekSet(FILE_CHAIN_START); // Go to the file-header's CHAIN block
	file.read(CHAIN, 48); // Read every seq's CHAIN-data

  // This block of code will activate any seqs that have been flagged as ACTIVE ON LOAD in the file's header-block.
	for (byte i = 0; i < 48; i++) { // For each seq...
		if (STATS[i] & 64) { // If the seq is flagged ACTIVE ON LOAD...
			POS[i] = CUR32 % seqLen(i); // Set the seq's POSITION to match the GLOBAL CUE POINT, or wrap it if the seq-length is short
      STATS[i] |= 128; // Activate the sequence
		}
	}

	updateTickSize(); // Update the internal tick-size (in microseconds) to match the new BPM value

	SONG = slot; // Set the currently-active SONG-position to the given save-slot

	PAGE = 0; // Reset the PAGE status to PAGE A

	writePrefs(); // Write the current relevant global vars into PRF.DAT. This is near the end of the function so the new SONG value is stored as well 

	sendHeaderBlock(sbuf, FILE_ONLOAD_START); // Send the SEND ON LOAD header-block's special MIDI-commands to the MIDI-OUT apparatus	

	LOADHOLD = 18000; // Give LOADHOLD around a second of decay, so the song-number is displayed for that long

	TO_UPDATE = 255; // Flag entire GUI for an LED-update

}
