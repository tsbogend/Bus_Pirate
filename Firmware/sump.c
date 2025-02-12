/*
 * This file is part of the Bus Pirate project
 * (http://code.google.com/p/the-bus-pirate/).
 *
 * Written and maintained by the Bus Pirate project.
 *
 * To the extent possible under law, the project has
 * waived all copyright and related or neighboring rights to Bus Pirate. This
 * work is published from United States.
 *
 * For details see: http://creativecommons.org/publicdomain/zero/1.0/.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sump.h"

#ifdef BP_ENABLE_SUMP_SUPPORT

#include "base.h"
#include "core.h"
#include "uart.h"

/*
 * SUMP command set - taken from http://www.sump.org/projects/analyzer/protocol/
 * With additions taken from
 * http://dangerousprototypes.com/docs/The_Logic_Sniffer's_extended_SUMP_protocol
 *
 * @TODO: Add commands 0x0F, 0x9E, 0x9F from the extended SUMP protocol?
 * @TODO: Add "Set trigger configuration" command (0xC2, 0xC6, 0xCA, 0xCE).
 * @TODO: Check why samples are sent out backwards.
 * @TODO: Implement continuous sampling.
 * @TODO: Remove sump_command_t.left and turn the structure into two separate
 *        fields.
 */

/**
 * Resets the device.
 *
 * Should be sent 5 times when the receiver status is unknown. (It could be
 * waiting for up to four bytes of pending long command data.)
 */
#define SUMP_RESET 0x00

/**
 * Arms the trigger.
 */
#define SUMP_RUN 0x01

/**
 * Asks for device identification.
 *
 * The device will respond with four bytes. The first three ("SLA") identify
 * the device. The last one identifies the protocol version which is currently
 * either "0" or "1"
 */
#define SUMP_ID 0x02

/**
 * Get metadata.
 *
 * In response, the device sends a series of 1-byte keys, followed by data
 * pertaining to that key. The series ends with the key 0x00. The system can be
 * extended with new keys as more data needs to be reported.
 *
 * Type 0 Keys (null-terminated string, UTF-8 encoded):
 *
 * 0x00 Not used, key means end of metadata
 * 0x01 Device name (e.g. "Openbench Logic Sniffer v1.0", "Bus Pirate v3b")
 * 0x02 Version of the FPGA firmware
 * 0x03 Ancillary version (PIC firmware)
 *
 * Type 1 Keys (32-bit unsigned integer):
 *
 * 0x20 Number of usable probes
 * 0x21 Amount of sample memory available (bytes)
 * 0x22 Amount of dynamic memory available (bytes)
 * 0x23 Maximum sample rate (Hz)
 * 0x24 Protocol version (see below) [*]
 *
 * Type 2 Keys (8-bit unsigned integer):
 *
 * 0x40 Number of usable probes (short)
 * 0x41 Protocol version (short)
 *
 * [*]
 *
 * The protocol version key holds a 4-stage version, one per byte, where the
 * MSB holds the major version number. As of the first release to support this
 * metadata command, the protocol version should be 2. This would be encoded
 * as 0x00000002.
 */
#define SUMP_DESC 0x04

/**
 * Put transmitter out of pause mode.
 *
 * It will continue to transmit captured data if any is pending. This command
 * is being used for XON/XOFF flow control.
 */
#define SUMP_XON 0x11

/**
 * Put transmitter in pause mode.
 *
 * It will stop transmitting captured data. This command is being used for
 * XON/XOFF flow control.
 */
#define SUMP_XOFF 0x13

/**
 * Set Divider.
 *
 * When x is written, the sampling frequency is set to f = clock / (x + 1)
 *
 *          LSB                  MSB
 * 10000000 XXXXXXXXXXXXXXXXXXXXXXXX????????
 *          ||||||||||||||||||||||||
 *          ++++++++++++++++++++++++----------- Divider
 */
#define SUMP_DIV 0x80

/**
 * Set Read & Delay Count.
 *
 * Read Count is the number of samples (divided by four) to read back from
 * memory and sent to the host computer. Delay Count is the number of samples
 * (divided by four) to capture after the trigger fired. A Read Count bigger
 * than the Delay Count means that data from before the trigger match will be
 * read back. This data will only be valid if the device was running long
 * enough before the trigger matched.
 *
 *          LSB          MSB LSB          MSB
 * 10000001 XXXXXXXXXXXXXXXX YYYYYYYYYYYYYYYY
 *          |||||||||||||||| ||||||||||||||||
 *          |||||||||||||||| ++++++++++++++++--- Delay Count
 *          ++++++++++++++++-------------------- Read Count
 */
#define SUMP_CNT 0x81

/**
 * Set Flags.
 *
 * Sets the following flags:
 *
 * - demux: Enables the demux input module. (Filter must be off.)
 * - filter: Enables the filter input module. (Demux must be off.)
 * - channel groups: Disable channel group. Disabled groups are excluded from
 *                   data transmissions. This can be used to speed up transfers.
 *                   There are four groups, each represented by one bit.
 *                   Starting with the least significant bit of the channel
 *                   group field channels are assigned as follows: 0-7, 8-15,
 *                   16-23, 24-31
 * - external: Selects the clock to be used for sampling. If set to 0, the
 *             internal clock divided by the configured divider is used, and if
 *             set to 1, the external clock will be used. (filter and demux are
 *             only available with internal clock)
 * - inverted: When set to 1, the external clock will be inverted before being
 *             used. The inversion causes a delay that may cause problems at
 *             very high clock rates. This option only has an effect with
 *             external set to 1.
 *
 * 10000010 ABCCCCDE ????????????????????????
 *          ||||||||
 *          |||||||+---------------------------- Demux (1: Enable)
 *          ||||||+----------------------------- Filter (1: Enable)
 *          ||++++------------------------------ Channel Groups (1: Disable)
 *          |+---------------------------------- External (1: Enable)
 *          +----------------------------------- Inverted (1: Enable)
 */
#define SUMP_FLAGS 0x82

/**
 * Set Trigger Values.
 *
 * Defines which trigger values must match. In parallel mode each bit
 * represents one channel, in serial mode each bit represents one of the last
 * 32 samples of the selected channel. The opcodes refer to stage 0-3 in the
 * order given below. (Protocol version 0 only supports stage 0.)
 *
 *          LSB                          MSB
 * 1100xx00 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 *          ||||||||||||||||||||||||||||||||
 *          ++++++++++++++++++++++++++++++++--- Trigger Mask
 */
#define SUMP_TRIG 0xC0

/**
 * Set trigger mask.
 *
 * Defines which values individual bits must have. In parallel mode each bit
 * represents one channel, in serial mode each bit represents one of the last
 * 32 samples of the selected channel. The opcodes refer to stage 0-3 in the
 * order given above. (Protocol version 0 only supports stage 0.)
 *
 *          LSB                          MSB
 * 1100xx01 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 *          ||||||||||||||||||||||||||||||||
 *          ++++++++++++++++++++++++++++++++--- Trigger Mask
 */
#define SUMP_TRIG_VALS 0xC1
#define SUMP_TRIG_CONF 0xC2

/**
 * Not used, key means end of metadata.
 */
#define SUMP_METADATA_END 0x00

/**
 * Device name (e.g. "Openbench Logic Sniffer v1.0", "Bus Pirate v3b").
 */
#define SUMP_METADATA_DEVICE_NAME 0x01

/**
 * Version of the FPGA firmware.
 */
#define SUMP_METADATA_FPGA_VERSION 0x02

/**
 * Ancillary version (PIC firmware).
 */
#define SUMP_METADATA_ANCILLARY_VERSION 0x03

/**
 * Number of usable probes.
 */
#define SUMP_METADATA_USABLE_PROBES_NUMBER 0x20

/**
 * Amount of sample memory available (bytes).
 */
#define SUMP_METADATA_SAMPLE_MEMORY_AVAILABLE 0x21

/**
 * Amount of dynamic memory available (bytes).
 */
#define SUMP_METADATA_DYNAMIC_MEMORY_AVAILABLE 0x22

/**
 * Maximum sample rate (Hz).
 */
#define SUMP_METADATA_MAXIMUM_SAMPLE_RATE 0x23

/**
 * Protocol version.
 */
#define SUMP_METADATA_PROTOCOL_VERSION 0x24

/**
 * Number of usable probes (short).
 */
#define SUMP_METADATA_USABLE_PROBES_SHORT_NUMBER 0x40

/**
 * Protocol version (short).
 */
#define SUMP_METADATA_PROTOCOL_SHORT_VERSION 0x41

/**
 * Default timer period value for polling probes.
 */
#define BP_DEFAULT_TIMER_PERIOD 0x00000640

/**
 * How much memory is allocated for samples, in bytes.
 */
#define BP_SUMP_SAMPLE_MEMORY_SIZE BP_TERMINAL_BUFFER_SIZE

/* Maximum sample count with RLE enabled */
#define BP_SUMP_MAX_SAMPLES	((uint32_t)BP_SUMP_SAMPLE_MEMORY_SIZE / 2 * 128)

/**
 * The highest sample rate for the Bus Pirate to sample data at, in Hz.
 */
#define BP_SUMP_MAXIMUM_SAMPLE_RATE 1000000

/**
 * SUMP protocol version the Bus Pirate supports.
 */
#define BP_SUMP_PROTOCOL_VERSION 2

#ifdef BUSPIRATEV4
#define BP_SUMP_DEVICE_NAME	'B', 'P', 'v', '4', '\0'
#define BP_SUMP_PROBES_COUNT	6
#define READ_IOPORT		IOPOR
#define IOBITS			(AUX + MOSI + CLK + MISO + CS + AUX2)
#elif defined(BUSPIRATEV3)
#define BP_SUMP_DEVICE_NAME	'B', 'P', 'v', '3', '\0'
#define BP_SUMP_PROBES_COUNT	5
#define READ_IOPORT		(PORTB >> 6)
#define IOBITS			(AUX + MOSI + CLK + MISO + CS)
#error "Invalid or unknown Bus Pirate version!"
#endif				/* BUSPIRATEV4 || BUSPIRATEV3 */

#define BP_SUMP_CHANNEL_MASK	((1 << BP_SUMP_PROBES_COUNT) - 1)

/**
 * SUMP metadata information for the Bus Pirate device.
 */
static const uint8_t SUMP_METADATA[] = {
	/* Device name. */

	SUMP_METADATA_DEVICE_NAME,
	BP_SUMP_DEVICE_NAME,

	/* Sample memory (4096 bytes). */

	SUMP_METADATA_SAMPLE_MEMORY_AVAILABLE,
	(uint8_t) ((uint32_t) BP_SUMP_MAX_SAMPLES >> 24),
	(uint8_t) (((uint32_t) BP_SUMP_MAX_SAMPLES >> 16) & 0xFF),
	(uint8_t) (((uint32_t) BP_SUMP_MAX_SAMPLES >> 8) & 0xFF),
	(uint8_t) ((uint32_t) BP_SUMP_MAX_SAMPLES & 0xFF),

	/* Sample rate (1MHz). */

	SUMP_METADATA_MAXIMUM_SAMPLE_RATE,
	(uint8_t) ((uint32_t) BP_SUMP_MAXIMUM_SAMPLE_RATE >> 24),
	(uint8_t) (((uint32_t) BP_SUMP_MAXIMUM_SAMPLE_RATE >> 16) & 0xFF),
	(uint8_t) (((uint32_t) BP_SUMP_MAXIMUM_SAMPLE_RATE >> 8) & 0xFF),
	(uint8_t) ((uint32_t) BP_SUMP_MAXIMUM_SAMPLE_RATE & 0xFF),

	/* Number of probes (5). */

	SUMP_METADATA_USABLE_PROBES_SHORT_NUMBER, BP_SUMP_PROBES_COUNT,

	/* Protocol version (v2). */

	SUMP_METADATA_PROTOCOL_SHORT_VERSION, BP_SUMP_PROTOCOL_VERSION,

	SUMP_METADATA_END
};

/**
 * SUMP_ID response buffer, advertise ourselves as a Logic Sniffer.
 *
 * According to the specifications on sump.org, this should either be 'SLA0' or
 * 'SLA1', however both Logic Sniffer (http://ols.lxtreme.nl) and
 * sigrok (https://sigrok.org) will not handle the device otherwise.
 */
static const uint8_t SUMP_DEVICE_ID[] = { '1', 'A', 'L', 'S' };

extern bus_pirate_configuration_t bus_pirate_configuration;

/**
 * How many bytes should be acquired in the next sampling operation, in bytes.
 */
static uint32_t samples_to_acquire;

static uint8_t sump_trigger_mask;
static uint8_t sump_trigger_val;
static bool sump_rle_mode;

/**
 * Resets the device to start another buffer acquisition.
 */
static void sump_reset(void)
{
	/* Switch LED off. */
	BP_LEDMODE = OFF;

	/* Switch pull-ups off for all pins. */
	CNPU1 = 0;
	CNPU2 = 0;

	/* Stop timer #4. */
	T4CON = 0;

	/* Setup timer periods. */
	PR5 = HI16(BP_DEFAULT_TIMER_PERIOD);
	PR4 = LO16(BP_DEFAULT_TIMER_PERIOD);

	/* Default to acquire a full buffer. */
	samples_to_acquire = BP_SUMP_MAX_SAMPLES;
	sump_trigger_mask = 0;
	sump_trigger_val = 0;
	sump_rle_mode = false;
}

/**
 * Acquires data from the probes and sends it out to the controlling software.
 *
 * This function will acquire up to BP_SUMP_MAX_SAMPLES, by using the
 * value read from the samples_to_acquire variable.  Calling this function
 * where the sampler is not armed will not trigger any action.
 *
 * To avoid rewriting interrupt vectors with the bootloader, this firmware
 * currently uses polling to read the trigger and timer.  A final version
 * should use interrupts after lots of testing.
 *
 * @return true if data acquisition was performed, false otherwise.
 */
static bool sump_acquire_samples(void)
{
	uint8_t last_sample, sample, count;
	uint8_t *bufptr, *endptr;
	uint32_t i;

	bufptr = bus_pirate_configuration.terminal_input;

	/* Turn the LED on. */
	BP_LEDMODE = ON;

	/* Clear timer #5 holding register. */
	TMR5HLD = 0;

	/* Setup timer #4 counter. */
	TMR4 = 0;

	/* Timer #4 counter will be 32 bits wide. */
	T4CON = 0b1000;

	/* wait for trigger */
	while ((READ_IOPORT & sump_trigger_mask) != sump_trigger_val) {
		/* check for abort CMDs */
		if (user_serial_ready_to_read()) {
			switch (user_serial_read_byte())
			case SUMP_RESET:
				return true;
		}
	}

	/* Take samples. */

	/* Start timer #4. */
	T4CONbits.TON = ON;

	/* Clear timer #4 interrupt flag. */
	IFS1bits.T5IF = OFF;

	/* Capture samples into the terminal buffer. */
	if (sump_rle_mode) {
		endptr = bufptr + BP_SUMP_SAMPLE_MEMORY_SIZE - 1;
		last_sample = 0xff; /* impossible value */
		count = 0;
		i = 0;

		for (i = 0; i < samples_to_acquire && bufptr < endptr; i++) {
			sample = READ_IOPORT & BP_SUMP_CHANNEL_MASK;

			if (sample != last_sample) {
				if (count)
					*bufptr++ = count | 0x80;

				*bufptr++ = sample;
				last_sample = sample;
				count = 0;
			} else {
				if (++count == 127) {
					*bufptr++ = count | 0x80;
					count = 0;
					last_sample = 0xff;
				}
			}
			/* Wait for timer4 interrupt to trigger. */
			while (IFS1bits.T5IF == OFF) {
			}

			/* Clear timer #4 interrupt flag. */
			IFS1bits.T5IF = OFF;
		}
		if (count)
			*bufptr++ = count | 0x80;
	} else {
		for (i = 0; i < samples_to_acquire; i++) {
			*bufptr++ = READ_IOPORT & BP_SUMP_CHANNEL_MASK;

			/* Wait for timer4 interrupt to trigger. */
			while (IFS1bits.T5IF == OFF) {
			}

			/* Clear timer #4 interrupt flag. */
			IFS1bits.T5IF = OFF;
		}
	}

	/* Stop timer #4. */
	T4CON = OFF;

	/* Write captured samples out. */
	while (bufptr > bus_pirate_configuration.terminal_input)
		user_serial_transmit_character(*--bufptr);

	/* Reset the analyzer state. */
	sump_reset();

	/* Acquisition complete. */
	return true;
}

static void sump_read_cmd_param(uint8_t *param)
{
	size_t i;

	for (i = 0; i < 4; i++)
		*param++ = user_serial_read_byte();
}

/**
 * Processes the given byte as a part of a SUMP command.
 *
 * @param[in] input_byte the byte to process.
 * @return true if a SUMP_RESET command was received, false otherwise.
 */
static bool sump_handle_command_byte(unsigned char input_byte)
{
	uint8_t param[4];
	uint32_t period;

	/* check for long command */
	if (input_byte & 0x80)
		sump_read_cmd_param(param);

	switch (input_byte) {
	case SUMP_RESET:
		/* Reset the analyzer. */
		sump_reset();
		break;

	case SUMP_ID:
		/* Send the device identification buffer. */
		bp_write_buffer(SUMP_DEVICE_ID, sizeof(SUMP_DEVICE_ID));
		break;

	case SUMP_RUN:
		sump_acquire_samples();
		break;

	case SUMP_DESC:
		/* Send device description. */
		bp_write_buffer(SUMP_METADATA, sizeof(SUMP_METADATA));
		break;

	case SUMP_XON:
	case SUMP_XOFF:
		/* Start/Stop and resume are not supported yet. */
		break;

	case SUMP_TRIG:
		sump_trigger_mask = param[0] & BP_SUMP_CHANNEL_MASK;
		break;

	case SUMP_TRIG_VALS:
		sump_trigger_val = param[0] & BP_SUMP_CHANNEL_MASK;
		break;

	case SUMP_TRIG_CONF:
		/* @TODO: Fill this? */
		break;

	case SUMP_FLAGS:
		sump_rle_mode = (param[1] & 1) ? true : false;
		break;

	case SUMP_CNT:
		/* Read requested samples buffer size. */
		samples_to_acquire = param[1] << 8 | param[0];
		samples_to_acquire++;
		samples_to_acquire *= 4;

		/* Clamp sample counter if more bytes are requested. */
		if (samples_to_acquire > BP_SUMP_MAX_SAMPLES) {
			samples_to_acquire = BP_SUMP_MAX_SAMPLES;
		}
		break;

	case SUMP_DIV:
		/*
		 * Read the 24-bits period value and rescale from SUMP's
		 * own 100MHz frequency range to the internal 16MIPs
		 * range.
		 */
		period = ((((uint32_t)param[2] << 16) +
			    ((uint32_t)param[1] << 8) +
			    ((uint32_t)param[0]) + 1) * 4) / 25;

		/* Round down if needed. */
		if (period > 0x10) {
			period -= 0x10;
		} else {
			period = 1;
		}

		/* Set timer period. */
		PR5 = HI16(period);
		PR4 = LO16(period);
		break;
	default:
		/* unknown command, ignore */
		break;
	}
	return false;
}

void enter_sump_mode(void)
{
	/* Set probing channels to INPUT mode. */
	IODIR |= IOBITS;

	/* Reset the analyzer state. */
	sump_reset();

	/* Sampling action. */
	for (;;) {
		if (sump_handle_command_byte(user_serial_read_byte()))
			return;
	}
}

#endif				/* BP_ENABLE_SUMP_SUPPORT */
