///*!   \file thumbdv.c
// *    \brief Functions required to communicate and decode packets from ThumbDV
// *
// *    \copyright  Copyright 2012-2014 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 26-MAY-2015
// *    \author     Ed Gonzalez
// *
// *
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2014 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include "common.h"
#include "datatypes.h"
#include "hal_buffer.h"

#include "vita_output.h"
#include "thumbDV.h"


#define DV3000_TTY              "/dev/ttyAMA0"
#define DV3000_VERSION          "2014-04-23"

#define AMBE3000_HEADER_LEN     4U
#define AMBE3000_START_BYTE     0x61U

#define AMBE3000_SPEECHD_HEADER_LEN 3U

#define BUFFER_LENGTH           400U
#define THUMBDV_MAX_PACKET_LEN  2048U

static pthread_t _read_thread;
BOOL _readThreadAbort = FALSE;

static void delay(unsigned int delay) {
    struct timespec tim, tim2;
    tim.tv_sec = 0;
    tim.tv_nsec = delay * 1000UL;
    nanosleep(&tim, &tim2);
};

static void dump(char *text, unsigned char *data, unsigned int length)
{
    unsigned int offset = 0U;
    unsigned int i;

    fputs(text, stdout);
    fputc('\n', stdout);

    while (length > 0U) {
        unsigned int bytes = (length > 16U) ? 16U : length;

        output( "%04X:  ", offset);

        for (i = 0U; i < bytes; i++)
            fprintf(stdout, "%02X ", data[offset + i]);

        for (i = bytes; i < 16U; i++)
            fputs("   ", stdout);

        fputs("   *", stdout);

        for (i = 0U; i < bytes; i++) {
            unsigned char c = data[offset + i];

            if (isprint(c))
                fputc(c, stdout);
            else
                fputc('.', stdout);
        }

        fputs("*\n", stdout);

        offset += 16U;

        if (length >= 16U)
            length -= 16U;
        else
            length = 0U;
    }
}

static void thumbDV_writeSerial(int serial_fd, const unsigned char * buffer, uint32 bytes)
{
    int32 n = 0;
    errno = 0;
    n = write(serial_fd , buffer, bytes);
    if (n != bytes) {
        output(ANSI_RED "Could not write to serial port. errno = %d\n", errno);
        return;
    }

}

int thumbDV_openSerial(const char * tty_name)
{
    struct termios tty;
    int fd;

    /* TODO: Sanitize tty_name */

    fd = open(tty_name, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        output("ThumbDV: error when opening %s, errno=%d\n", tty_name, errno);
        return fd;
    }

    if (tcgetattr(fd, &tty) != 0) {
        output( "ThumbDV: error %d from tcgetattr\n", errno);
        return -1;
    }

    cfsetospeed(&tty, B230400);
    cfsetispeed(&tty, B230400);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;

    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    tty.c_cflag |= (CLOCAL | CREAD);

    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        output("ThumbDV: error %d from tcsetattr\n", errno);
        return -1;
    }

    output("opened %s - fd = %d\n", tty_name, fd);
    return fd;
}

int thumbDV_processSerial(int serial_fd)
{
    unsigned char buffer[BUFFER_LENGTH];
    unsigned int respLen;
    unsigned int offset;
    ssize_t len;

    errno = 0;
    len = read(serial_fd, buffer, 1);
    if (len != 1) {
        output(ANSI_RED "ThumbDV: error when reading from the serial port, len = %d, errno=%d\n" ANSI_WHITE, len, errno);
        return 0;
    }

    if (buffer[0U] != AMBE3000_START_BYTE) {
        output(ANSI_RED "ThumbDV: unknown byte from the DV3000, 0x%02X\n" ANSI_WHITE, buffer[0U]);
        return 1;
    }

    offset = 0U;
    while (offset < (AMBE3000_HEADER_LEN - 1U)) {
        len = read(serial_fd, buffer + 1U + offset, AMBE3000_HEADER_LEN - 1 - offset);

        if (len == 0)
            delay(5UL);

        offset += len;
    }

    respLen = buffer[1U] * 256U + buffer[2U];

    offset = 0U;
    while (offset < respLen) {
        len = read(serial_fd, buffer + AMBE3000_HEADER_LEN + offset, respLen - offset);

        if (len == 0)
            delay(5UL);

        offset += len;
    }

    respLen += AMBE3000_HEADER_LEN;


    dump("Serial data", buffer, respLen);

    return 0;
}

int thumbDV_encode(int serial_fd, short * speech_in, short * speech_out, uint8 num_of_samples )
{
    unsigned char packet[THUMBDV_MAX_PACKET_LEN];
    uint16 speech_d_bytes = num_of_samples * sizeof(uint16);  /* Should be 2 times the number of samples */



    /* Calculate length of packet NOT including the full header just the type field*/
    uint16 length = 1;
    /* Includes Channel Field and SpeechD Field Header */
    length += AMBE3000_SPEECHD_HEADER_LEN;
    length += speech_d_bytes;

    /* Will be used to write fields into packet */
    unsigned char * idx = &packet[0];

    *(idx++) = AMBE3000_START_BYTE;
    /* Length split into two bytes */
    *(idx++) = length >> 8;
    *(idx++) = length & 0xFF;
    /* SPEECHD Type */
    *(idx++) = 0x02;
    /* Channel0 Identifier */
    *(idx++) = 0x40;
    /* SPEEECHD Identifier */
    *(idx++) = 0x00;
    /* SPEECHD No of Samples */
    *(idx++) = num_of_samples;

    output("Num of Samples = 0x%X\n", num_of_samples);

    *idx = '\0';
    output("Encode Packet Header = ");
    unsigned char * p = &packet[0];
    uint32 i = 0;
    for ( i = 0 ; i < 7 ; i++ ){

        output("%02X ",*p);
        p++;
    }
    output("\n");

    memcpy(idx, speech_in, speech_d_bytes);

    /* +3 needed for first 3 fields of header */
    thumbDV_writeSerial(serial_fd, packet, length + 3);

    return num_of_samples;

}

static void* _thumbDV_readThread(void* param)
{
    int topFd;
    fd_set fds;
    int ret;

    int serial_fd = *(int*)param;
    topFd = serial_fd + 1;

    output("Serial FD = %d in thumbDV_readThread(). TopFD = %d\n", serial_fd, topFd);

    struct timeval timeout;

    while ( !_readThreadAbort ) {
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        FD_ZERO(&fds);
        FD_SET(serial_fd, &fds);

        output("Going into select\n");

        errno = 0;
        ret = select(topFd, &fds, NULL, NULL, &timeout);
        if (ret < 0) {
            fprintf(stderr, "ThumbDV: error from select, errno=%d\n", errno);
        }
        output("Checking if our socket was set in fdx\n");
        if (FD_ISSET(serial_fd, &fds)) {
            ret = thumbDV_processSerial(serial_fd);
        }
    }

    output(ANSI_YELLOW "thumbDV_readThread has exited\n" ANSI_WHITE);
    return 0;
}

void thumbDV_init(const char * serial_device_name, int * serial_fd)
{
    *serial_fd = thumbDV_openSerial("/dev/ttyUSB0");

    pthread_create(&_read_thread, NULL, &_thumbDV_readThread, serial_fd);

    struct sched_param fifo_param;
    fifo_param.sched_priority = 30;
    pthread_setschedparam(_read_thread, SCHED_FIFO, &fifo_param);


    unsigned char reset[5] = { 0x61, 0x00, 0x01, 0x00, 0x33 };
    //unsigned char dstar_mode[17] = {0x61, 0x00, 0x0c, 0x00, 0x0a, 0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48};
    //unsigned char get_prodID[5] = {0x61, 0x00, 0x01, 0x00, 0x30 };
    //unsigned char get_version[5] = {0x61, 0x00, 0x01, 0x00, 0x31};

    thumbDV_writeSerial(*serial_fd, reset, 5);
//    thumbDV_writeSerial(*serial_fd, get_prodID, 5 );
//    /thumbDV_writeSerial(*serial_fd, get_version, 5);
//    thumbDV_writeSerial(*serial_fd, dstar_mode, 17);

}
