/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2021  Dawid Buchwald
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* $Id$ */

/*
 * Based on pymcuprog
 * See https://github.com/microchip-pic-avr-tools/pymcuprog
 */

#include "ac_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>

#include "avrdude.h"
#include "libavrdude.h"
#include "updi_link.h"
#include "updi_constants.h"
#include "updi_state.h"

static void msleep(int tms)
{
    usleep(tms * 1000);
}

static int updi_physical_open(PROGRAMMER* pgm, int baudrate, unsigned long cflags)
{
  serial_recv_timeout = 100;
  union pinfo pinfo;

  pinfo.serialinfo.baud = baudrate;
  pinfo.serialinfo.cflags = cflags;

  avrdude_message(MSG_DEBUG, "%s: Opening serial port...\n", progname);

  if (serial_open(pgm->port, pinfo, &pgm->fd)==-1) {

    avrdude_message(MSG_DEBUG, "%s: Serial port open failed!\n", progname);
    return -1;
  }

  /*
   * drain any extraneous input
   */
  serial_drain(&pgm->fd, 0);

  return 0;
}

static void updi_physical_close(PROGRAMMER* pgm)
{
  serial_close(&pgm->fd);
  pgm->fd.ifd = -1;
}

static int updi_physical_send(PROGRAMMER * pgm, unsigned char * buf, size_t len)
{
  size_t i;
  int rv;

  avrdude_message(MSG_DEBUG, "%s: Sending %lu bytes [", progname, len);
  for (i=0; i<len; i++) {
    avrdude_message(MSG_DEBUG, "0x%02x", buf[i]);
    if (i<len-1) {
      avrdude_message(MSG_DEBUG, ", ");
    }
  }
  avrdude_message(MSG_DEBUG, "]\n");

  rv = serial_send(&pgm->fd, buf, len);
  serial_recv(&pgm->fd, buf, len);
  return rv;
}

static int updi_physical_recv(PROGRAMMER * pgm, unsigned char * buf, size_t len)
{
  size_t i;
  int rv;

  rv = serial_recv(&pgm->fd, buf, len);
  if (rv < 0) {
    avrdude_message(MSG_DEBUG,
      "%s: serialupdi_recv(): programmer is not responding\n",
      progname);
    return -1;
  }

  avrdude_message(MSG_DEBUG, "%s: Received %lu bytes [", progname, len);
  for (i=0; i<len; i++) {
    avrdude_message(MSG_DEBUG, "0x%02x", buf[i]);
    if (i<len-1) {
      avrdude_message(MSG_DEBUG, ", ");
    }
  }
  avrdude_message(MSG_DEBUG, "]\n");

  return len;
}

static int updi_physical_send_double_break(PROGRAMMER * pgm)
{
  unsigned char buffer[1];

  avrdude_message(MSG_DEBUG, "%s: Sending double break\n", progname);

  updi_physical_close(pgm);

  if (updi_physical_open(pgm, 300, SERIAL_8E1)==-1) {

    return -1;
  }

  buffer[0] = UPDI_BREAK;

  serial_send(&pgm->fd, buffer, 1);
  serial_recv(&pgm->fd, buffer, 1);

  msleep(100);

  buffer[0] = UPDI_BREAK;

  serial_send(&pgm->fd, buffer, 1);
  serial_recv(&pgm->fd, buffer, 1);

  updi_physical_close(pgm);

  return updi_physical_open(pgm, pgm->baudrate? pgm->baudrate: 115200, SERIAL_8E2);
}

int updi_physical_sib(PROGRAMMER * pgm, unsigned char * buffer, uint8_t size)
{
/*
    def sib(self):
        """
        System information block is just a string coming back from a SIB command
        """
        self.send([
            constants.UPDI_PHY_SYNC,
            constants.UPDI_KEY | constants.UPDI_KEY_SIB | constants.UPDI_SIB_32BYTES])
        return self.ser.readline()
*/
  unsigned char send_buffer[2];

  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_KEY | UPDI_KEY_SIB | UPDI_SIB_32BYTES;

  if (updi_physical_send(pgm, send_buffer, 2) < 0) {
    avrdude_message(MSG_DEBUG, "%s: SIB request send failed\n", progname);
    return -1;
  }

  return updi_physical_recv(pgm, buffer, size);
}

int updi_link_open(PROGRAMMER * pgm) 
{
  if (updi_physical_open(pgm, pgm->baudrate? pgm->baudrate: 115200, SERIAL_8E2) < 0) {
    return -1;
  }
  return updi_physical_send_double_break(pgm);
}

void updi_link_close(PROGRAMMER * pgm)
{
  updi_physical_close(pgm);
}

static int updi_link_init_session_parameters(PROGRAMMER * pgm) 
{
/*
    def _init_session_parameters(self):
        """
        Set the inter-byte delay bit and disable collision detection
        """
        self.stcs(constants.UPDI_CS_CTRLB, 1 << constants.UPDI_CTRLB_CCDETDIS_BIT)
        self.stcs(constants.UPDI_CS_CTRLA, 1 << constants.UPDI_CTRLA_IBDLY_BIT)
*/
  if (updi_link_stcs(pgm, UPDI_CS_CTRLB, 1 << UPDI_CTRLB_CCDETDIS_BIT) < 0) {
    return -1;
  }

  if (updi_link_stcs(pgm, UPDI_CS_CTRLA, 1 << UPDI_CTRLA_IBDLY_BIT) < 0) {
    return -1;
  }

  return 0;
}

static int updi_link_check(PROGRAMMER * pgm)
{
/*
    def _check_datalink(self):
        """
        Check UPDI by loading CS STATUSA
        """
        try:
            if self.ldcs(constants.UPDI_CS_STATUSA) != 0:
                self.logger.info("UPDI init OK")
                return True
        except PymcuprogError:
            self.logger.warning("Check failed")
            return False
        self.logger.info("UPDI not OK - reinitialisation required")
        return False
*/
  int result;
  uint8_t value;
  result = updi_link_ldcs(pgm, UPDI_CS_STATUSA, &value);
  if (result < 0) {
    avrdude_message(MSG_DEBUG, "%s: Check failed\n", progname);
    return -1;
  } else {
    if (value > 0) {
      avrdude_message(MSG_DEBUG, "%s: UDPI init OK\n", progname);
      return 0;
    } else {
      avrdude_message(MSG_DEBUG, "%s: UDPI not OK - reinitialisation required\n", progname);
      return -1;
    }
  }
}


int updi_link_init(PROGRAMMER * pgm)
{
/*
    def init_datalink(self):
        """
        Init DL layer
        """
        self._init_session_parameters()
        # Check
        if not self._check_datalink():
            # Send double break if all is not well, and re-check
            self.updi_phy.send_double_break()
            self._init_session_parameters()
            if not self._check_datalink():
                raise PymcuprogError("UPDI initialisation failed")
*/
  if (updi_link_init_session_parameters(pgm) < 0) {
    avrdude_message(MSG_DEBUG, "%s: Session initialisation failed\n", progname);
    return -1;
  }

  if (updi_link_check(pgm) < 0) {
    avrdude_message(MSG_DEBUG, "%s: Datalink not active, resetting...\n", progname);
    if (updi_physical_send_double_break(pgm) < 0) {
      avrdude_message(MSG_DEBUG, "%s: Datalink initialisation failed\n", progname);
      return -1;
    }
    if (updi_link_init_session_parameters(pgm) < 0) {
      avrdude_message(MSG_DEBUG, "%s: Session initialisation failed\n", progname);
      return -1;
    }
    if (updi_link_check(pgm) < 0) {
      avrdude_message(MSG_DEBUG, "%s: Restoring datalink failed\n", progname);
      return -1;
    }
  }
  return 0;
}

int updi_link_ldcs(PROGRAMMER * pgm, uint8_t address, uint8_t * value) 
{
/*
    def ldcs(self, address):
        """
        Load data from Control/Status space

        :param address: address to load
        """
        self.logger.debug("LDCS from 0x%02X", address)
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_LDCS | (address & 0x0F)])
        response = self.updi_phy.receive(self.LDCS_RESPONSE_BYTES)
        numbytes_received = len(response)
        if numbytes_received != self.LDCS_RESPONSE_BYTES:
            raise PymcuprogError("Unexpected number of bytes in response: "
                                 "{} byte(s) expected {} byte(s)".format(numbytes_received, self.LDCS_RESPONSE_BYTES))

        return response[0]
*/
  unsigned char buffer[2];
  int result;
  avrdude_message(MSG_DEBUG, "%s: LDCS from 0x%02X\n", progname, address);
  buffer[0]=UPDI_PHY_SYNC;
  buffer[1]=UPDI_LDCS | (address & 0x0F);
  if (updi_physical_send(pgm, buffer, 2) < 0) {
    avrdude_message(MSG_DEBUG, "%s: LDCS send operation failed\n", progname);
    return -1;
  }
  result = updi_physical_recv(pgm, buffer, 1);
  if (result != 1) {
    if (result >= 0) {
      avrdude_message(MSG_DEBUG, "%s: Incorrect response size, received %d instead of %d bytes\n", progname, result, 1);
    }
    return -1;
  }
  * value = buffer[0];
  return 0;
}

int updi_link_stcs(PROGRAMMER * pgm, uint8_t address, uint8_t value)
{
/*
    def stcs(self, address, value):
        """
        Store a value to Control/Status space

        :param address: address to store to
        :param value: value to write
        """
        self.logger.debug("STCS to 0x%02X", address)
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_STCS | (address & 0x0F), value])
*/
  unsigned char buffer[3];
  avrdude_message(MSG_DEBUG, "%s: STCS 0x%02X to address 0x%02X\n", progname, value, address);
  buffer[0] = UPDI_PHY_SYNC;
  buffer[1] = UPDI_STCS | (address & 0x0F);
  buffer[2] = value;
  return updi_physical_send(pgm, buffer, 3);
}

int updi_link_ld_ptr_inc(PROGRAMMER * pgm, unsigned char * buffer, uint16_t size)
{
/*
    def ld_ptr_inc(self, size):
        """
        Loads a number of bytes from the pointer location with pointer post-increment
 
        :param size: number of bytes to load
        :return: values read
        """
        self.logger.debug("LD8 from ptr++")
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_LD | constants.UPDI_PTR_INC |
                            constants.UPDI_DATA_8])
        return self.updi_phy.receive(size)
*/
  unsigned char send_buffer[2];
  avrdude_message(MSG_DEBUG, "%s: LD8 from ptr++\n", progname);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_LD | UPDI_PTR_INC | UPDI_DATA_8;
  if (updi_physical_send(pgm, send_buffer, 2) < 0) {
    avrdude_message(MSG_DEBUG, "%s: LD_PTR_INC send operation failed\n", progname);
    return -1;
  }
  return updi_physical_recv(pgm, buffer, size);
}

int updi_link_ld_ptr_inc16(PROGRAMMER * pgm, unsigned char * buffer, uint16_t words)
{
/*
    def ld_ptr_inc16(self, words):
        """
        Load a 16-bit word value from the pointer location with pointer post-increment

        :param words: number of words to load
        :return: values read
        """
        self.logger.debug("LD16 from ptr++")
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_LD | constants.UPDI_PTR_INC |
                            constants.UPDI_DATA_16])
        return self.updi_phy.receive(words << 1)
*/
  unsigned char send_buffer[2];
  avrdude_message(MSG_DEBUG, "%s: LD16 from ptr++\n", progname);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_LD | UPDI_PTR_INC | UPDI_DATA_16;
  if (updi_physical_send(pgm, send_buffer, 2) < 0) {
    avrdude_message(MSG_DEBUG, "%s: LD_PTR_INC send operation failed\n", progname);
    return -1;
  }
  return updi_physical_recv(pgm, buffer, words << 2);
}

int updi_link_st_ptr_inc(PROGRAMMER * pgm, unsigned char * buffer, uint16_t size)
{
/*
    def st_ptr_inc(self, data):
        """
        Store data to the pointer location with pointer post-increment

        :param data: data to store
        """
        self.logger.debug("ST8 to *ptr++")
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_ST | constants.UPDI_PTR_INC | constants.UPDI_DATA_8,
                            data[0]])
        response = self.updi_phy.receive(1)

        if len(response) != 1 or response[0] != constants.UPDI_PHY_ACK:
            raise PymcuprogError("ACK error with st_ptr_inc")

        num = 1
        while num < len(data):
            self.updi_phy.send([data[num]])
            response = self.updi_phy.receive(1)

            if len(response) != 1 or response[0] != constants.UPDI_PHY_ACK:
                raise PymcuprogError("Error with st_ptr_inc")
            num += 1
*/
  unsigned char send_buffer[3];
  unsigned char recv_buffer[1];
  int response;
  int num = 1;
  avrdude_message(MSG_DEBUG, "%s: ST8 to *ptr++\n", progname);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_ST | UPDI_PTR_INC | UPDI_DATA_8;
  send_buffer[2] = buffer[0];
  if (updi_physical_send(pgm, send_buffer, 3) < 0) {
    avrdude_message(MSG_DEBUG, "%s: ST_PTR_INC send operation failed\n", progname);
    return -1;
  }

  response = updi_physical_recv(pgm, recv_buffer, 1);

  if (response != 1 || recv_buffer[0] != UPDI_PHY_ACK) {
    avrdude_message(MSG_DEBUG, "%s: ACK was expected but not received\n", progname);
    return -1;
  }

  while (num < size) {
    send_buffer[0]=buffer[num];
    if (updi_physical_send(pgm, send_buffer, 1) < 0) {
      avrdude_message(MSG_DEBUG, "%s: ST_PTR_INC data send operation failed\n", progname);
      return -1;
    }
    response = updi_physical_recv(pgm, recv_buffer, 1);

    if (response != 1 || recv_buffer[0] != UPDI_PHY_ACK) {
      avrdude_message(MSG_DEBUG, "%s: Data ACK was expected but not received\n", progname);
      return -1;
    }
    num++;
  }

  return 0;
}

int updi_link_st_ptr_inc16(PROGRAMMER * pgm, unsigned char * buffer, uint16_t words)
{
/*
    def st_ptr_inc16(self, data):
        """
        Store a 16-bit word value to the pointer location with pointer post-increment

        :param data: data to store
        """
        self.logger.debug("ST16 to *ptr++")
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_ST | constants.UPDI_PTR_INC |
                            constants.UPDI_DATA_16, data[0], data[1]])
        response = self.updi_phy.receive(1)

        if len(response) != 1 or response[0] != constants.UPDI_PHY_ACK:
            raise PymcuprogError("ACK error with st_ptr_inc16")

        num = 2
        while num < len(data):
            self.updi_phy.send([data[num], data[num + 1]])
            response = self.updi_phy.receive(1)

            if len(response) != 1 or response[0] != constants.UPDI_PHY_ACK:
                raise PymcuprogError("Error with st_ptr_inc16")
            num += 2
*/
  unsigned char send_buffer[4];
  unsigned char recv_buffer[1];
  int response;
  int num = 2;
  avrdude_message(MSG_DEBUG, "%s: ST16 to *ptr++\n", progname);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_ST | UPDI_PTR_INC | UPDI_DATA_16;
  send_buffer[2] = buffer[0];
  send_buffer[3] = buffer[1];
  if (updi_physical_send(pgm, send_buffer, 4) < 0) {
    avrdude_message(MSG_DEBUG, "%s: ST_PTR_INC16 send operation failed\n", progname);
    return -1;
  }

  response = updi_physical_recv(pgm, recv_buffer, 1);

  if (response != 1 || recv_buffer[0] != UPDI_PHY_ACK) {
    avrdude_message(MSG_DEBUG, "%s: ACK was expected but not received\n", progname);
    return -1;
  }

  while (num < words) {
    send_buffer[0]=buffer[num];
    send_buffer[1]=buffer[num+1];
    if (updi_physical_send(pgm, send_buffer, 2) < 0) {
      avrdude_message(MSG_DEBUG, "%s: ST_PTR_INC data send operation failed\n", progname);
      return -1;
    }
    response = updi_physical_recv(pgm, recv_buffer, 1);

    if (response != 1 || recv_buffer[0] != UPDI_PHY_ACK) {
      avrdude_message(MSG_DEBUG, "%s: Data ACK was expected but not received\n", progname);
      return -1;
    }
    num+=2;
  }

  return 0;
}

int updi_link_st_ptr_inc16_RSD(PROGRAMMER * pgm, unsigned char * buffer, uint16_t words, int blocksize) {
/*
    def st_ptr_inc16_RSD(self, data, blocksize):
        """
        Store a 16-bit word value to the pointer location with pointer post-increment
        :param data: data to store
        :blocksize: max number of bytes being sent -1 for all.
                    Warning: This does not strictly honor blocksize for values < 6
                    We always glob together the STCS(RSD) and REP commands.
                    But this should pose no problems for compatibility, because your serial adapter can't deal with 6b chunks,
                    none of pymcuprog would work!
        """
        self.logger.debug("ST16 to *ptr++ with RSD, data length: 0x%03X in blocks of:  %d", len(data), blocksize)

        #for performance we glob everything together into one USB transfer....
        repnumber= ((len(data) >> 1) -1)
        data = [*data, *[constants.UPDI_PHY_SYNC, constants.UPDI_STCS | constants.UPDI_CS_CTRLA, 0x06]]

        if blocksize == -1 :
            # Send whole thing at once stcs + repeat + st + (data + stcs)
            blocksize = 3 + 3 + 2 + len(data)
        num = 0
        firstpacket = []
        if blocksize < 10 :
            # very small block size - we send pair of 2-byte commands first.
            firstpacket = [*[constants.UPDI_PHY_SYNC, constants.UPDI_STCS | constants.UPDI_CS_CTRLA, 0x0E],
                            *[constants.UPDI_PHY_SYNC, constants.UPDI_REPEAT | constants.UPDI_REPEAT_BYTE, (repnumber & 0xFF)]]
            data = [*[constants.UPDI_PHY_SYNC, constants.UPDI_ST | constants.UPDI_PTR_INC |constants.UPDI_DATA_16], *data]
            num = 0
        else:
            firstpacket = [*[constants.UPDI_PHY_SYNC, constants.UPDI_STCS | constants.UPDI_CS_CTRLA , 0x0E],
                            *[constants.UPDI_PHY_SYNC, constants.UPDI_REPEAT | constants.UPDI_REPEAT_BYTE, (repnumber & 0xFF)],
                            *[constants.UPDI_PHY_SYNC, constants.UPDI_ST | constants.UPDI_PTR_INC | constants.UPDI_DATA_16],
                            *data[:blocksize - 8]]
            num = blocksize - 8
        self.updi_phy.send( firstpacket )

        # if finite block size, this is used.
        while num < len(data):
            data_slice = data[num:num+blocksize]
            self.updi_phy.send(data_slice)
            num += len(data_slice)
*/
  avrdude_message(MSG_DEBUG, "%s: ST16 to *ptr++ with RSD, data length: 0x%03X in blocks of: %d\n", progname, words * 2, blocksize);

  unsigned int temp_buffer_size = 3 + 3 + 2 + (words * 2) + 3;
  unsigned int num=0;
  unsigned char* temp_buffer = malloc(temp_buffer_size);

  if (temp_buffer == 0) {
    avrdude_message(MSG_DEBUG, "%s: Allocating temporary buffer failed\n", progname);
    return -1;
  }

  if (blocksize == -1) {
    blocksize = temp_buffer_size;
  }

  temp_buffer[0] = UPDI_PHY_SYNC;
  temp_buffer[1] = UPDI_STCS | UPDI_CS_CTRLA;
  temp_buffer[2] = 0x0E;
  temp_buffer[3] = UPDI_PHY_SYNC;
  temp_buffer[4] = UPDI_REPEAT | UPDI_REPEAT_BYTE;
  temp_buffer[5] = (words - 1) & 0xFF;
  temp_buffer[6] = UPDI_PHY_SYNC;
  temp_buffer[7] = UPDI_ST | UPDI_PTR_INC | UPDI_DATA_16;

  memcpy(temp_buffer + 8, buffer, words * 2);

  temp_buffer[temp_buffer_size-3] = UPDI_PHY_SYNC;
  temp_buffer[temp_buffer_size-2] = UPDI_STCS | UPDI_CS_CTRLA;
  temp_buffer[temp_buffer_size-1] = 0x06;

  if (blocksize < 10) {
    if (updi_physical_send(pgm, temp_buffer, 6) < 0) {
      avrdude_message(MSG_DEBUG, "%s: Failed to send first package\n", progname);
      free(temp_buffer);
      return -1;
    }
    num = 6;
  } 

  while (num < temp_buffer_size) {
    int next_package_size;

    if (num + blocksize > temp_buffer_size) {
      next_package_size = temp_buffer_size - num;
    } else {
      next_package_size = blocksize;
    }

    if (updi_physical_send(pgm, temp_buffer + num, next_package_size) < 0) {
      avrdude_message(MSG_DEBUG, "%s: Failed to send package\n", progname);
      free(temp_buffer);
      return -1;
    }

    num+=next_package_size;
  }
  free(temp_buffer);
  return 0;
}

int updi_link_repeat(PROGRAMMER * pgm, uint16_t repeats)
{
/*
    def repeat(self, repeats):
        """
        Store a value to the repeat counter

        :param repeats: number of repeats requested
        """
        self.logger.debug("Repeat %d", repeats)
        if (repeats - 1) > constants.UPDI_MAX_REPEAT_SIZE:
            self.logger.error("Invalid repeat count of %d", repeats)
            raise Exception("Invalid repeat count!")
        repeats -= 1
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_REPEAT | constants.UPDI_REPEAT_BYTE,
                            repeats & 0xFF])
*/
  unsigned char buffer[3];
  avrdude_message(MSG_DEBUG, "%s: Repeat %d\n", progname, repeats);
  if ((repeats - 1) > UPDI_MAX_REPEAT_SIZE) {
    avrdude_message(MSG_DEBUG, "%s: Invalid repeat count of %d\n", progname, repeats);
    return -1;
  }
  repeats-=1;
  buffer[0] = UPDI_PHY_SYNC;
  buffer[1] = UPDI_REPEAT | UPDI_REPEAT_BYTE;
  buffer[2] = repeats & 0xFF;
  return updi_physical_send(pgm, buffer, 3);
}

int updi_link_read_sib(PROGRAMMER * pgm, unsigned char * buffer, uint16_t size)
{
/*
    def read_sib(self):
        """
        Read the SIB
        """
        return self.updi_phy.sib()
*/
  return updi_physical_sib(pgm, buffer, size);
}

int updi_link_key(PROGRAMMER * pgm, unsigned char * buffer, uint8_t size_type, uint16_t size)
{
/*
    def key(self, size, key):
        """
        Write a key
 
        :param size: size of key (0=64B, 1=128B, 2=256B)
        :param key: key value
        """
        self.logger.debug("Writing key")
        if len(key) != 8 << size:
            raise PymcuprogError("Invalid KEY length!")
        self.updi_phy.send([constants.UPDI_PHY_SYNC, constants.UPDI_KEY | constants.UPDI_KEY_KEY | size])
        self.updi_phy.send(list(reversed(list(key))))
*/
  unsigned char send_buffer[2];
  unsigned char reversed_key[256];
  int index;
  avrdude_message(MSG_DEBUG, "%s: UPDI writing key\n", progname);
  if (size != (8 << size_type)) {
    avrdude_message(MSG_DEBUG, "%s: Invalid key length\n", progname);
    return -1;
  }
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_KEY | UPDI_KEY_KEY | size_type;
  if (updi_physical_send(pgm, send_buffer, 2) < 0) {
    avrdude_message(MSG_DEBUG, "%s: UPDI key send message failed\n", progname);
    return -1;
  }
  /* reverse key contents */
  for (index=0; index<size; index++) {
    reversed_key[index] = buffer[size-index-1];
  }
  return updi_physical_send(pgm, reversed_key, size);
}

int updi_link_ld(PROGRAMMER * pgm, uint32_t address, uint8_t * value)
{
/*
    def ld(self, address):
        """
        Load a single byte direct from a 24-bit address

        :param address: address to load from
        :return: value read
        """
        self.logger.info("LD from 0x{0:06X}".format(address))
        self.updi_phy.send(
            [constants.UPDI_PHY_SYNC, constants.UPDI_LDS | constants.UPDI_ADDRESS_24 | constants.UPDI_DATA_8,
             address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF])
        return self.updi_phy.receive(1)[0]
*/
  unsigned char send_buffer[5];
  unsigned char recv_buffer[1];
  avrdude_message(MSG_DEBUG, "%s: LD from 0x%06X\n", progname, address);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_LDS | UPDI_DATA_8 | (updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16);
  send_buffer[2] = address & 0xFF;
  send_buffer[3] = (address >> 8) & 0xFF;
  send_buffer[4] = (address >> 16) & 0xFF;
  if (updi_physical_send(pgm, send_buffer, updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? 5 : 4) < 0) {
    avrdude_message(MSG_DEBUG, "%s: LD operation send failed\n", progname);
    return -1;
  }
  if (updi_physical_recv(pgm, recv_buffer, 1) < 0) {
    avrdude_message(MSG_DEBUG, "%s: LD operation recv failed\n", progname);
    return -1;
  }
  * value = recv_buffer[0];
  return 0;
}

int updi_link_ld16(PROGRAMMER * pgm, uint32_t address, uint16_t * value)
{
/*
    def ld16(self, address):
        """
        Load a 16-bit word directly from a 24-bit address

        :param address: address to load from
        :return: values read
        """
        self.logger.info("LD from 0x{0:06X}".format(address))
        self.updi_phy.send(
            [constants.UPDI_PHY_SYNC, constants.UPDI_LDS | constants.UPDI_ADDRESS_24 | constants.UPDI_DATA_16,
             address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF])
        return self.updi_phy.receive(2)
*/
  unsigned char send_buffer[5];
  unsigned char recv_buffer[2];
  avrdude_message(MSG_DEBUG, "%s: LD16 from 0x%06X\n", progname, address);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_LDS | UPDI_DATA_16 | (updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16);
  send_buffer[2] = address & 0xFF;
  send_buffer[3] = (address >> 8) & 0xFF;
  send_buffer[4] = (address >> 16) & 0xFF;
  if (updi_physical_send(pgm, send_buffer, updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? 5 : 4) < 0) {
    avrdude_message(MSG_DEBUG, "%s: LD16 operation send failed\n", progname);
    return -1;
  }
  if (updi_physical_recv(pgm, recv_buffer, 2) < 0) {
    avrdude_message(MSG_DEBUG, "%s: LD16 operation recv failed\n", progname);
    return -1;
  }
  * value = (recv_buffer[0] << 8 | recv_buffer[1]);
  return 0;
}

static int updi_link_st_data_phase(PROGRAMMER * pgm, unsigned char * buffer, uint8_t size)
{
/*
    def _st_data_phase(self, values):
        """
        Performs data phase of transaction:
        * receive ACK
        * send data

        :param values: bytearray of value(s) to send
        """
        response = self.updi_phy.receive(1)
        if len(response) != 1 or response[0] != constants.UPDI_PHY_ACK:
            raise PymcuprogError("Error with st")

        self.updi_phy.send(values)
        response = self.updi_phy.receive(1)
        if len(response) != 1 or response[0] != constants.UPDI_PHY_ACK:
            raise PymcuprogError("Error with st")
*/
  unsigned char recv_buffer[1];
  if (updi_physical_recv(pgm, recv_buffer, 1) < 0) {
    avrdude_message(MSG_DEBUG, "%s: UPDI data phase recv failed on first ACK\n", progname);
    return -1;
  }
  if (recv_buffer[0] != UPDI_PHY_ACK) {
    avrdude_message(MSG_DEBUG, "%s: UPDI data phase expected first ACK\n", progname);
    return -1;
  }
  if (updi_physical_send(pgm, buffer, size) < 0) {
    avrdude_message(MSG_DEBUG, "%s: UPDI data phase send failed\n", progname);
    return -1;
  }
  if (updi_physical_recv(pgm, recv_buffer, 1) < 0) {
    avrdude_message(MSG_DEBUG, "%s: UPDI data phase recv failed on second ACK\n", progname);
    return -1;
  }
  if (recv_buffer[0] != UPDI_PHY_ACK) {
    avrdude_message(MSG_DEBUG, "%s: UPDI data phase expected second ACK\n", progname);
    return -1;
  }
  return 0;
}

int updi_link_st(PROGRAMMER * pgm, uint32_t address, uint8_t value)
{
/*
    def st(self, address, value):
        """
        Store a single byte value directly to a 24-bit address

        :param address: address to write to
        :param value: value to write
        """
        self.logger.info("ST to 0x{0:06X}".format(address))
        self.updi_phy.send(
            [constants.UPDI_PHY_SYNC, constants.UPDI_STS | constants.UPDI_ADDRESS_24 | constants.UPDI_DATA_8,
             address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF])
        return self._st_data_phase([value & 0xFF])
*/
  unsigned char send_buffer[5];
  avrdude_message(MSG_DEBUG, "%s: ST to 0x%06X\n", progname, address);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_STS | UPDI_DATA_8 | (updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16);
  send_buffer[2] = address & 0xFF;
  send_buffer[3] = (address >> 8) & 0xFF;
  send_buffer[4] = (address >> 16) & 0xFF;
  if (updi_physical_send(pgm, send_buffer, updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? 5 : 4) < 0) {
    avrdude_message(MSG_DEBUG, "%s: ST operation send failed\n", progname);
    return -1;
  }
  send_buffer[0] = value;
  return updi_link_st_data_phase(pgm, send_buffer, 1);
}

int updi_link_st16(PROGRAMMER * pgm, uint32_t address, uint16_t value)
{
/*
    def st16(self, address, value):
        """
        Store a 16-bit word value directly to a 24-bit address

        :param address: address to write to
        :param value: value to write
        """
        self.logger.info("ST to 0x{0:06X}".format(address))
        self.updi_phy.send(
            [constants.UPDI_PHY_SYNC, constants.UPDI_STS | constants.UPDI_ADDRESS_24 | constants.UPDI_DATA_16,
             address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF])
        return self._st_data_phase([value & 0xFF, (value >> 8) & 0xFF])
*/
  unsigned char send_buffer[5];
  avrdude_message(MSG_DEBUG, "%s: ST16 to 0x%06X\n", progname, address);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_STS | UPDI_DATA_16 | (updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16);
  send_buffer[2] = address & 0xFF;
  send_buffer[3] = (address >> 8) & 0xFF;
  send_buffer[4] = (address >> 16) & 0xFF;
  if (updi_physical_send(pgm, send_buffer, updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? 5 : 4) < 0) {
    avrdude_message(MSG_DEBUG, "%s: ST16 operation send failed\n", progname);
    return -1;
  }
  send_buffer[0] = value & 0xFF;
  send_buffer[1] = (value >> 8) & 0xFF;
  return updi_link_st_data_phase(pgm, send_buffer, 2);
}

int updi_link_st_ptr(PROGRAMMER * pgm, uint32_t address)
{
/*
    def st_ptr(self, address):
        """
        Set the pointer location

        :param address: address to write
        """
        self.logger.info("ST to ptr")
        self.updi_phy.send(
            [constants.UPDI_PHY_SYNC, constants.UPDI_ST | constants.UPDI_PTR_ADDRESS | constants.UPDI_DATA_24,
             address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF])
        response = self.updi_phy.receive(1)
        if len(response) != 1 or response[0] != constants.UPDI_PHY_ACK:
            raise PymcuprogError("Error with st_ptr")
*/
  unsigned char send_buffer[5];
  unsigned char recv_buffer[1];
  avrdude_message(MSG_DEBUG, "%s: ST_PTR to 0x%06X\n", progname, address);
  send_buffer[0] = UPDI_PHY_SYNC;
  send_buffer[1] = UPDI_STS | UPDI_ST | UPDI_PTR_ADDRESS | (updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? UPDI_DATA_24 : UPDI_DATA_16);
  send_buffer[2] = address & 0xFF;
  send_buffer[3] = (address >> 8) & 0xFF;
  send_buffer[4] = (address >> 16) & 0xFF;
  if (updi_physical_send(pgm, send_buffer, updi_get_datalink_mode(pgm) == UPDI_LINK_MODE_24BIT ? 5 : 4) < 0) {
    avrdude_message(MSG_DEBUG, "%s: ST_PTR operation send failed\n", progname);
    return -1;
  }
  if (updi_physical_recv(pgm, recv_buffer, 1) < 0) {
    avrdude_message(MSG_DEBUG, "%s: UPDI ST_PTR recv failed on ACK\n", progname);
    return -1;
  }
  if (recv_buffer[0] != UPDI_PHY_ACK) {
    avrdude_message(MSG_DEBUG, "%s: UPDI ST_PTR expected ACK\n", progname);
    return -1;
  }
  return 0;
}
