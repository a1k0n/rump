/**********************************************************************
 * main.c - Main firmware (ATmega16/ATmega32 version)                 *
 * Version 1.00                                                       *
 **********************************************************************
 * rump is copyright (C) 2008 Chris Lee <clee@mg8.org>                *
 * based on c64key, copyright (C) 2006-2007 Mikkel Holm Olsen         *
 * based on HID-Test by Christian Starkjohann, Objective Development  *
 **********************************************************************
 * rump (Real USB Model-M PCB) is Free Software; you can redistribute *
 * and/or modify it under the terms of the OBDEV lice,nse, as found   *
 * in the license.txt file.                                           *
 *                                                                    *
 * rump is distributed in the hope that it will be useful, but        *
 * WITHOUT ANY WARRANTY; without even the implied warranty of         *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the      *
 * OBDEV license for further details.                                 *
 **********************************************************************/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <string.h>

/* Now included from the makefile */
//#include "keycodes.h"

#include "usbdrv.h"
#define DEBUG_LEVEL 0
#include "oddebug.h"

/* Hardware documentation:
 * ATmega-16 @12.000 MHz
 *
 * XT1..XT2: 12MHz X-tal
 * PA0..PA7: Keyboard matrix row0..row7 (pins J4.1 -> J4.8)
 * PC0..PC7: Keyboard matrix row16..row8 (pins J4.16 -> J4.9)
 * PB0..PB7: Keyboard matrix col0..col7 (pins J3.1 -> J3.8)
 * PD0     : D- USB negative (needs appropriate zener-diode and resistors)
 * PD2/INT0: D+ USB positive (needs appropriate zener-diode and resistors)
 *
 * USB Connector:
 * -------------
 *  1 (red)    +5V
 *  2 (white)  DATA-
 *  3 (green)  DATA+
 *  4 (black)  GND
 *
 *
 *                       +---[1K5r]---- +5V
 *                       |
 *      USB              |                          ATmega-16
 *                       |
 *      (D-)-------+-----+-------------[68r]------- PD0
 *                 |
 *      (D+)-------|-----+-------------[68r]------- PD2/INT0
 *                 |     |
 *                 _     _
 *                 ^     ^  2 x 3.6V
 *                 |     |  zener to GND
 *                 |     |
 *                GND   GND
 */

/* The LED states */
#define LED_NUM     0x01
#define LED_CAPS    0x02
#define LED_SCROLL  0x04
#define LED_COMPOSE 0x08
#define LED_KANA    0x10


/* USB report descriptor (length is defined in usbconfig.h)
   This has been changed to conform to the USB keyboard boot protocol */
const char usbHidReportDescriptor[USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH] PROGMEM = {
	0x05, 0x01,            // USAGE_PAGE (Generic Desktop)
	0x09, 0x06,            // USAGE (Keyboard)
	0xa1, 0x01,            // COLLECTION (Application)
	0x05, 0x07,            //   USAGE_PAGE (Keyboard)
	0x19, 0xe0,            //   USAGE_MINIMUM (Keyboard LeftControl)
	0x29, 0xe7,            //   USAGE_MAXIMUM (Keyboard Right GUI)
	0x15, 0x00,            //   LOGICAL_MINIMUM (0)
	0x25, 0x01,            //   LOGICAL_MAXIMUM (1)
	0x75, 0x01,            //   REPORT_SIZE (1)
	0x95, 0x08,            //   REPORT_COUNT (8)
	0x81, 0x02,            //   INPUT (Data,Var,Abs)
	0x95, 0x01,            //   REPORT_COUNT (1)
	0x75, 0x08,            //   REPORT_SIZE (8)
	0x81, 0x03,            //   INPUT (Cnst,Var,Abs)
	0x95, 0x05,            //   REPORT_COUNT (5)
	0x75, 0x01,            //   REPORT_SIZE (1)
	0x05, 0x08,            //   USAGE_PAGE (LEDs)
	0x19, 0x01,            //   USAGE_MINIMUM (Num Lock)
	0x29, 0x05,            //   USAGE_MAXIMUM (Kana)
	0x91, 0x02,            //   OUTPUT (Data,Var,Abs)
	0x95, 0x01,            //   REPORT_COUNT (1)
	0x75, 0x03,            //   REPORT_SIZE (3)
	0x91, 0x03,            //   OUTPUT (Cnst,Var,Abs)
	0x95, 0x06,            //   REPORT_COUNT (6)
	0x75, 0x08,            //   REPORT_SIZE (8)
	0x15, 0x00,            //   LOGICAL_MINIMUM (0)
	0x25, 0x65,            //   LOGICAL_MAXIMUM (101)
	0x05, 0x07,            //   USAGE_PAGE (Keyboard)
	0x19, 0x00,            //   USAGE_MINIMUM (Reserved (no event indicated))
	0x29, 0x65,            //   USAGE_MAXIMUM (Keyboard Application)
	0x81, 0x00,            //   INPUT (Data,Ary,Abs)
	0xc0                   // END_COLLECTION
};

/* This buffer holds the last values of the scanned keyboard matrix */
static unsigned short bitbuf[8];

/* The ReportBuffer contains the USB report sent to the PC */
static uchar reportBuffer[8];    /* buffer for HID reports */
static uchar previousReport[8];
static uchar idleRate;           /* in 4 ms units */
static uchar protocolVer = 1;    /* 0 is boot protocol, 1 is report protocol */

static void hardwareInit(void) {
	/*
	 * Theory of operation:
	 * Initially, all keyboard scan columns are set as inputs, but with
	 * pullups.  This causes them to be "weak" +5 outputs.  To scan, we set
	 * one row as a low output (with other rows being inputs with no
	 * pullups), causing it to "win" over the weak outputs.
	 */
	PORTA = 0xFF;   /* Port A = J4 pins 1-8 - enable pull-up */
	DDRA  = 0x00;   /* Port A is input */

	PORTB = 0x00;   /* Port B = J3 pins 1-8 - no pull-up */
	DDRB  = 0x00;   /* Port B is input */

	PORTC = 0xFF;   /* Port C = J4 pins 9-16 - enable pull-up */
	DDRC  = 0x00;   /* Port C is input */

	/* PORTD: USB I/O on PD0/PD2, LED current source on PD5..PD7, pull up
         * others which are N/C */
	PORTD = 0x1a;   /* 0001 1010 bin: USB reset on PD0 / PD2 */
	DDRD  = 0x05;   /* 0000 0101 bin: these pins are for USB output */

	/* USB Reset by device only required on Watchdog Reset */
	_delay_ms(10);   /* delay >10ms for USB reset */

	DDRD = 0x00;    /* 0000 0000 bin: remove USB reset condition */
	/* configure timer 0 for a rate of 12M/(1024 * 256) = 45.78 Hz (~22ms) */
	TCCR0 = 5;      /* timer 0 prescaler: 1024 */
}

/* This function scans the entire keyboard, debounces the keys, and
   if a key change has been found, a new report is generated, and the
   function returns true to signal the transfer of the report. */
static uchar scankeys(void) {
	unsigned short activeRows;
	uchar activeCols;
	uchar reportIndex = 1; /* First available report entry is 2 */
	static uchar debounce = 5;

	/* Scan all eight columns: PORTB->matrix->PINA|PINC */
        /* Note: PORTB is wired backwards as well in the surface-mount design at
         * http://www.circuits.io/circuits/923, so the scanned column mask is
         * now reversed as well */
	for (uchar col = 0, colmask = 0x80; col < 8; ++col, colmask >>= 1) {
		DDRB = colmask;
		_delay_us(30);
		unsigned short data = PINA | (PINC<<8);
		if (data != bitbuf[col]) debounce = 10;
		bitbuf[col] = data;
	}
	DDRB = 0x00;

	if (debounce == 0) // Nothing's changed.
		return 0;

	if (debounce > 0) { // Something's changed, but we're still settling.
		if (--debounce)
			return 0;
	}

	activeRows = 0;
	activeCols = 0;
	/* Clear report buffer */
	memset(reportBuffer, 0, sizeof(reportBuffer));

	/* Process all rows for key-codes */
	unsigned short rowmask;
	uchar row;
	for (uchar col = 0, colmask = 1; col < 8; ++col, colmask <<= 1) {
		unsigned short data = bitbuf[col];

		/* Anything on this column? - if not, skip it */
		if (0xFFFF == data) { continue; }

		for (row = 0, rowmask = 1; row < NUMROWS; ++row, rowmask <<= 1, data >>= 1) {
			/* If no key detected, jump to the next column */
			if (data & 1) { continue; }

			activeRows |= rowmask;
			activeCols |= colmask;

			/* Read keyboard map */
			uchar key = pgm_read_byte(&keymap[row][col]);
                        if (key == 0) { continue; }

			/* Is this a modifier key? */
			if (key > KEY_Modifiers) {
				reportBuffer[0] |= 1<<(key - (KEY_Modifiers + 1));
				continue;
			}

			/* Too many keycodes - rollOver */
			if (++reportIndex < sizeof(reportBuffer)) {
				/* Set next available entry */
				reportBuffer[reportIndex] = key;
			} else if (reportIndex == sizeof(reportBuffer)) {
				memset(reportBuffer + 2, KEY_errorRollOver, sizeof(reportBuffer) - 2);
				/* continue decoding to get modifiers */
			}
		}
	}

	/* Ghost-key prevention, seems to actually work! */
	if (reportBuffer[5]) {
		uchar numRows, numCols;

		for (numRows = 0; activeRows; numRows++) {
			activeRows &= (activeRows - 1);
		}

		for (numCols = 0; activeCols; numCols++) {
			activeCols &= (activeCols - 1);
		}

		if ((numRows + numCols) < reportIndex) {
			// This should imply that a ghost key event has
			// happened, so drop the current report
			return 0;
		}
	}

	uchar changed = 0;
	for (uchar i = 0; i < sizeof(reportBuffer); i++) {
		if (previousReport[i] != reportBuffer[i]) {
			changed = 1;
			previousReport[i] = reportBuffer[i];
		}
	}
	return changed;
}

uchar expectReport = 0;
uchar LEDstate = 0;

uchar usbFunctionSetup(uchar data[8]) {
	usbRequest_t *rq = (void *)data;
	usbMsgPtr = reportBuffer;

	if ((rq->bmRequestType & USBRQ_TYPE_MASK) != USBRQ_TYPE_CLASS)
		return 0;

	switch (rq->bRequest) {
		case USBRQ_HID_GET_IDLE:
			usbMsgPtr = &idleRate;
			return 1;
		case USBRQ_HID_SET_IDLE:
			idleRate = rq->wValue.bytes[1];
			return 0;
		case USBRQ_HID_GET_REPORT:
			return sizeof(reportBuffer);
		case USBRQ_HID_SET_REPORT:
			if (rq->wLength.word == 1)
				expectReport = 1;
			return expectReport == 1 ? 0xFF : 0;
		case USBRQ_HID_GET_PROTOCOL:
			if (rq->wValue.bytes[1] < 1)
				protocolVer = rq->wValue.bytes[1];
			return 0;
		case USBRQ_HID_SET_PROTOCOL:
			usbMsgPtr = &protocolVer;
			return 1;
		default:
			return 0;
	}
}


uchar usbFunctionWrite(uchar *data, uchar len) {
	if ((expectReport) && (len == 1)) {
		/* Get the state of all 5 LEDs */
		LEDstate = data[0];
#if 0
		/* Our LEDs happened to be mapped on PD5..PD7 corresponding to
		 * LEDstate0..2; setting the direction register to an output
		 * will turn them on accordingly */
		DDRD = (DDRD & 0x1f) | ((LEDstate&7) << 5);
#else
		// my hacked-up prototype boards swapped some things around
		// num lock - PD7
		// scroll lock - PD6
		// caps lock - PD5
		uchar new_DDRD = DDRD & 0x1f;
		if (LEDstate & LED_NUM)
			new_DDRD |= 1<<7;
		if (LEDstate & LED_CAPS)
			new_DDRD |= 1<<5;
		if (LEDstate & LED_SCROLL)
			new_DDRD |= 1<<6;
                DDRD = new_DDRD;
#endif
	}
	expectReport = 0;
	return 0x01;
}

int main(void) {
	uchar updateNeeded = 0;
	uchar idleCounter = 0;

	memset(reportBuffer, 0, sizeof(reportBuffer));
	memset(previousReport, 0, sizeof(previousReport));
	memset(bitbuf, 0xff, sizeof(bitbuf));

	wdt_enable(WDTO_2S); /* Enable watchdog timer 2s */
	hardwareInit(); /* Initialize hardware (I/O) */

	odDebugInit();

	usbInit(); /* Initialize USB stack processing */
	sei(); /* Enable global interrupts */

	/* Main loop */
	for (;;) {
		/* Reset the watchdog */
		wdt_reset();
		/* Poll the USB stack */
		usbPoll();

		/* Scan the keyboard for changes */
		updateNeeded |= scankeys();

		/* Check timer if we need periodic reports */
		if (TIFR & (1 << TOV0)) {
			/* Reset flag */
			TIFR = 1 << TOV0;
			/* Do we need periodic reports? */
			if (idleRate != 0) {
				if (idleCounter > 4) {
					/* Yes, but not yet */
					/* 22 ms in units of 4 ms */
					idleCounter -= 5;
				} else {
					/* Yes, it is time now */
					updateNeeded = 1;
					idleCounter = idleRate;
				}
			}
		}

		/* If an update is needed, send the report */
		if(updateNeeded && usbInterruptIsReady()) {
			updateNeeded = 0;
			usbSetInterrupt(reportBuffer, sizeof(reportBuffer));
		}
	}
	return 0;
}
