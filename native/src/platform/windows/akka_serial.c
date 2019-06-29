/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2003, 2004  Martin J. Thomas  <mthomas@rhrk.uni-kl.de>
 * Copyright (C) 2006  Joerg Wunsch <j@uriah.heep.sax.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id$ */

/*
 * Native Win32 serial interface for avrdude.
 */

#include "akka_serial_win.h"

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)

#include <windows.h>
#include <stdio.h>
#include <ctype.h>   /* for isprint */

#include "serial_win.h"

long serial_recv_timeout = 5000; /* ms */

char * progname = "Akka Serial";
static bool debug = true;

void serial_debug(bool value)
{
	debug = value;
}

char debugBuffer[255];

static void print_debug(const char* const msg, int en)
{
	if (debug) {
		if (errno == 0) {
			fprintf(stderr, "%s", msg);
		}
		else {
			fprintf(stderr, "%s: %d\n", msg, en);
		}
		fflush(stderr);
	}
}

static void print_debug_port(const char* const msg,const char* port, int en)
{
	if (debug) {
		if (errno == 0) {
			fprintf(stderr, "%s for port \"%s\"", msg, port);
		}
		else {
			fprintf(stderr, "%s for port \"%s\": %d\n", msg, port, en);
		}
		fflush(stderr);
	}
}

#define DATA_CANCEL 0xffffffff

#define W32SERBUFSIZE 1024

#define strncasecmp(x,y,z) _strnicmp(x,y,z)

struct baud_mapping {
  long baud;
  DWORD speed;
};

//contains file descriptors used in managing a serial port
struct serial_config {
	int port_fd; // file descriptor of serial port

				 /* a pipe is used to abort a serial read by writing something into the
				 * write end of the pipe */
	int pipe_read_fd; // file descriptor, read end of pipe
	int pipe_write_fd; // file descriptor, write end of pipe
	union filedescriptor fd;
};


/* HANDLE hComPort=INVALID_HANDLE_VALUE; */

static struct baud_mapping baud_lookup_table [] = {
  { 1200,   CBR_1200 },
  { 2400,   CBR_2400 },
  { 4800,   CBR_4800 },
  { 9600,   CBR_9600 },
  { 19200,  CBR_19200 },
  { 38400,  CBR_38400 },
  { 57600,  CBR_57600 },
  { 115200, CBR_115200 },
  { 0,      0 }                 /* Terminator. */
};

static DWORD serial_baud_lookup(long baud)
{
  struct baud_mapping *map = baud_lookup_table;

  while (map->baud) {
    if (map->baud == baud)
      return map->speed;
    map++;
  }

  /*
   * If a non-standard BAUD rate is used, issue
   * a warning (if we in debug) and return the raw rate
   */
  if (debug)
      fprintf(stderr, "%s: serial_baud_lookup(): Using non-standard baud rate: %ld",
              progname, baud);

  return baud;
}


static BOOL serial_w32SetTimeOut(HANDLE hComPort, DWORD timeout) // in ms
{
	COMMTIMEOUTS ctmo;
	ZeroMemory (&ctmo, sizeof(COMMTIMEOUTS));
	ctmo.ReadIntervalTimeout = timeout;
	ctmo.ReadTotalTimeoutMultiplier = timeout;
	ctmo.ReadTotalTimeoutConstant = timeout;

	return SetCommTimeouts(hComPort, &ctmo);
}

static int ser_setspeed(union filedescriptor *fd, long baud, int char_size, int parity, bool two_stop_bits)
{
	DCB dcb;
	HANDLE hComPort = (HANDLE)fd->pfd;

	ZeroMemory (&dcb, sizeof(DCB));
	dcb.DCBlength = sizeof(DCB);
	dcb.BaudRate = serial_baud_lookup (baud);
	dcb.fBinary = 1;
	dcb.fDtrControl = DTR_CONTROL_DISABLE;
	dcb.fRtsControl = RTS_CONTROL_DISABLE;

	switch (char_size) {
	case 5: dcb.ByteSize = 5; break;
	case 6: dcb.ByteSize = 6; break;
	case 7: dcb.ByteSize = 7; break;
	case 8: dcb.ByteSize = 8; break;
	default:
		close(fd);
		print_debug("Invalid character size", 0);
		return -E_INVALID_SETTINGS;
	}
	

	/* set parity */
	switch (parity) {
	case PARITY_NONE: dcb.Parity = PARITY_NONE; break;
	case PARITY_ODD: dcb.Parity = PARITY_ODD; break;
	case PARITY_EVEN: dcb.Parity = PARITY_EVEN; break;
	default:
		close(fd);
		print_debug("Invalid parity", 0);
		return -E_INVALID_SETTINGS;
	}


	dcb.StopBits = two_stop_bits ? TWOSTOPBITS : ONESTOPBIT;

	if (!SetCommState(hComPort, &dcb))
		return -E_INVALID_SETTINGS;

	return 0;
}

int serial_open(
	const char* const port_name,
	int baud,
	int char_size,
	bool two_stop_bits,
	int parity,
	struct serial_config** serial)
{

	struct serial_config* s = malloc(sizeof(s));
	
	char portName1[100];

	strcpy(portName1,port_name);
	ser_open(portName1, baud,char_size,two_stop_bits,parity, &s->fd);
	(*serial) = s;
	return 0;
}

///NOTE serial_open sets the filedescriptor
static int ser_open(char* port, long baud, int char_size, bool two_stop_bits, int parity, union filedescriptor *fdp)
{
	LPVOID lpMsgBuf;
	HANDLE hComPort=INVALID_HANDLE_VALUE;
	char *newname = 0;

	/*
	 * If the port is of the form "net:<host>:<port>", then
	 * handle it as a TCP connection to a terminal server.
	 *
	 * This is curently not implemented for Win32.
	 */
	if (strncmp(port, "net:", strlen("net:")) == 0) {
		print_debug("ser_open(): network connects are currently not implemented for Win32 environments", errno);
		return -E_IO;
	}

	if (strncasecmp(port, "com", strlen("com")) == 0) {

	    // prepend "\\\\.\\" to name, required for port # >= 10
	    newname = malloc(strlen("\\\\.\\") + strlen(port) + 1);

	    if (newname == 0) {
			print_debug("ser_open(): out of memory", errno);
    		exit(1);
	    }
	    strcpy(newname, "\\\\.\\");
	    strcat(newname, port);

	    port = newname;
	}

	hComPort = CreateFile(port, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hComPort == INVALID_HANDLE_VALUE) {
		FormatMessage( 
			FORMAT_MESSAGE_ALLOCATE_BUFFER | 
			FORMAT_MESSAGE_FROM_SYSTEM | 
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			GetLastError(),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
			(LPTSTR) &lpMsgBuf,
			0,
			NULL);

		_snprintf_s(debugBuffer, _TRUNCATE, "ser_open(): can't open device \"%s\": %s", port, (char*)lpMsgBuf);
		print_debug(debugBuffer, errno);

		LocalFree( lpMsgBuf );
		return -E_IO;
	}

	if (!SetupComm(hComPort, W32SERBUFSIZE, W32SERBUFSIZE))
	{
		CloseHandle(hComPort);
		print_debug_port("ser_open() : can't set buffers",port, errno);
		return -E_IO;
	}

        fdp->pfd = (void *)hComPort;

		int setSpeedErr = ser_setspeed(fdp, baud, char_size, parity, two_stop_bits);

	if (setSpeedErr != 0)
	{
		CloseHandle(hComPort);
		print_debug_port("ser_open() : can't set com-state", port, errno);


		return setSpeedErr;
	}

	if (!serial_w32SetTimeOut(hComPort,0))
	{
		CloseHandle(hComPort);
		print_debug_port("ser_open() :  can't set initial timeout", port, errno);
		return -E_IO;
	}

	if (newname != 0) {
	    free(newname);
	}
	return 0;
}


static int ser_close(union filedescriptor *fd)
{
	HANDLE hComPort=(HANDLE)fd->pfd;
	if (hComPort != INVALID_HANDLE_VALUE)
		CloseHandle (hComPort);

	hComPort = INVALID_HANDLE_VALUE;

	return 0;
}

int serial_close(struct serial_config* const serial)
{
	ser_close(&serial->fd);
	free(serial);

	return 0;
}

static int ser_set_dtr_rts(union filedescriptor *fd, int is_on)
{
	HANDLE hComPort=(HANDLE)fd->pfd;

	if (is_on) {
		EscapeCommFunction(hComPort, SETDTR);
		EscapeCommFunction(hComPort, SETRTS);
	} else {
		EscapeCommFunction(hComPort, CLRDTR);
		EscapeCommFunction(hComPort, CLRRTS);
	}
	return 0;
}

//return number of bytes sent
int serial_write(struct serial_config* const serial, char* const data, size_t size)
{
	return ser_send(&serial->fd,data,size);
}

static int ser_send(union filedescriptor *fd, char const * buf, size_t buflen)
{
	size_t len = buflen;
	unsigned char c='\0';
	DWORD written;
	DWORD writtenReturn;
        unsigned char * b = buf;

	HANDLE hComPort=(HANDLE)fd->pfd;

	if (hComPort == INVALID_HANDLE_VALUE) {
		print_debug("serial_write(): port not open",errno); 
		return -E_IO;
	}

	if (!len)
  return 0;


	if (debug)
	{
		fprintf(stderr, "%s: Send: ", progname);

		while (len) {
			c = *b;
			if (isprint(c)) {
				fprintf(stderr, "%c ", c);
			}
			else {
				fprintf(stderr, ". ");
			}
			fprintf(stderr, "[%02x] ", c);
			b++;
			len--;
		}
		fprintf(stderr, "\n");
	}
	
	serial_w32SetTimeOut(hComPort,500);

	if (!WriteFile (hComPort, buf, buflen, &written, NULL)) {
		print_debug("ser_send() : write error : sorry no info avail", errno);

		return -E_IO;
	}
	writtenReturn = written;

	if (written != buflen) {
		print_debug("ser_send() : size/send mismatch", errno);
		return -E_IO;
	}

	//return number of bytes sent
	return writtenReturn;
}


int serial_cancel_read(struct serial_config* const serial)
{
	print_debug("called undefined akka_serial.c function: serial_cancel_read", errno);

	//int data = DATA_CANCEL;

	////write to pipe to wake up any blocked read thread (self-pipe trick)
	//if (write(serial->pipe_write_fd, &data, 1) < 0) {
	//	print_debug("Error writing to pipe during read cancel", errno);
	//	return -E_IO;
	//}


	return 0;
}

//return number of bytes read
int serial_read(struct serial_config* const serial, unsigned char* const buf, size_t buflen)
{
//	return ser_recv(&serial->fd, &buffer, size);
//}

//static int ser_recv(union filedescriptor *fd, unsigned char * buf, size_t buflen)
//{
	unsigned char c;
	unsigned char * p = buf;
	union filedescriptor * fd = &serial->fd;
	DWORD read;
	DWORD readReturn;

	HANDLE hComPort=(HANDLE)fd->pfd;
	
	if (hComPort == INVALID_HANDLE_VALUE) {
		print_debug("ser_read(): port not open",errno);
		return -E_IO;
	}

	serial_w32SetTimeOut(hComPort, serial_recv_timeout);
	
	if (!ReadFile(hComPort, buf, buflen, &read, NULL)) {
		LPVOID lpMsgBuf;
		FormatMessage( 
			FORMAT_MESSAGE_ALLOCATE_BUFFER | 
			FORMAT_MESSAGE_FROM_SYSTEM | 
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			GetLastError(),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
			(LPTSTR) &lpMsgBuf,
			0,
			NULL 	);
		fprintf(stderr, "%s: ser_recv(): read error: %s\n",
			      progname, (char*)lpMsgBuf);
		LocalFree( lpMsgBuf );
		return -E_IO;
	}

	readReturn = read;

	/* time out detected */
	if (read == 0) {
			print_debug("ser_recv() : programmer is not responding",errno);
		return -E_IO;
	}

	p = buf;

	if (debug)
	{
		fprintf(stderr, "%s: Recv: ", progname);

		while (read) {
			c = *p;
			if (isprint(c)) {
				fprintf(stderr, "%c ", c);
			}
			else {
				fprintf(stderr, ". ");
			}
			fprintf(stderr, "[%02x] ", c);

			p++;
			read--;
		}
		fprintf(stderr, "\n");
	}

	//return number of bytes read
  return readReturn;
}



static int ser_drain(union filedescriptor *fd, int display)
{
	// int rc;
	unsigned char buf[10];
	BOOL readres;
	DWORD read;

	HANDLE hComPort=(HANDLE)fd->pfd;

  	if (hComPort == INVALID_HANDLE_VALUE) {
		print_debug("ser_drain(): port not open",errno); 
		return -E_IO;
	}

	serial_w32SetTimeOut(hComPort,250);
  
	if (display) {
		fprintf(stderr, "drain>");
	}

	while (1) {
		readres=ReadFile(hComPort, buf, 1, &read, NULL);
		if (!readres) {
			LPVOID lpMsgBuf;
			FormatMessage( 
				FORMAT_MESSAGE_ALLOCATE_BUFFER | 
				FORMAT_MESSAGE_FROM_SYSTEM | 
				FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				GetLastError(),
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
				(LPTSTR) &lpMsgBuf,
				0,
				NULL 	);
			fprintf(stderr, "%s: ser_drain(): read error: %s\n",
					  progname, (char*)lpMsgBuf);
			LocalFree( lpMsgBuf );
			return -E_IO;
		}

		if (read) { // data avail
			if (display) fprintf(stderr, "%02x ", buf[0]);
		}
		else { // no more data
			if (display) fprintf(stderr, "<drain\n");
			break;
		}
	} // while
  return 0;
}

struct serial_device serial_serdev =
{
  .open = ser_open,
  .setspeed = ser_setspeed,
  .close = ser_close,
  .send = ser_send,
  //.recv = ser_recv,
  .drain = ser_drain,
  .set_dtr_rts = ser_set_dtr_rts,
  .flags = SERDEV_FL_CANSETSPEED,
};

struct serial_device *serdev = &serial_serdev;

#endif /* WIN32NATIVE */ 
