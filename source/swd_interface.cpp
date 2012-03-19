#include "swd_interface.h"

#include "libs/error/error_stack.h"
#include "libs/log/log_default.h"

#include <ftdi.h>

#include <unistd.h>

using namespace Err;
using namespace Log;

/*
 * Many of the MPSSE commands expect either an 8- or 16-bit count.  To get the
 * most out of those bits, it encodes a count N as N-1.  These macros produce
 * the individual bytes of the adjusted count.
 */
#define FTH(n) ((((n) - 1) >> 8) & 0xFF)  // High 8 bits
#define FTL(n) (((n) - 1) & 0xFF)         // Low 8 bits


namespace {

/*
 * Maps FT232H I/O pins to SWD signals and protocol states.  The mapping
 * is fixed for now.
 */
enum PinStates {
  //                           RST  SWDI  SWDO  SWDCLK
  kStateIdle        = 0x9,  //  1    0     0      1
  kStateResetTarget = 0x1,  //  0    0     0      1
  kStateResetSWD    = 0xB,  //  1    0     1      1
};

/*
 * Pin directions for read and write -- used with PinStates above.
 */
enum PinDirs {
  //                  RST  SWDI  SWDO  SWDCLK
  kDirWrite = 0xB,  // 1    0     1      1
  kDirRead  = 0x9,  // 1    0     0      1
};


uint8_t const kSWDHeaderStart = 1 << 0;

uint8_t const kSWDHeaderAP = 1 << 1;
uint8_t const kSWDHeaderDP = 0 << 1;

uint8_t const kSWDHeaderRead  = 1 << 2;
uint8_t const kSWDHeaderWrite = 0 << 2;

uint8_t const kSWDHeaderParity = 1 << 5;
uint8_t const kSWDHeaderPark = 1 << 7;

uint8_t swd_request(int address, bool debug_port, bool write) {
  bool parity = debug_port ^ write;

  uint8_t request = kSWDHeaderStart
                  | (debug_port ? kSWDHeaderDP : kSWDHeaderAP)
                  | (write ? kSWDHeaderWrite : kSWDHeaderRead)
                  | ((address & 0x3) << 3)
                  | kSWDHeaderPark;

  // Incorporate address into parity
  switch (address & 0x3) {
    case 0:
    case 3:
      // Even number of ones, no change required.
      break;

    case 1:
    case 2:
      parity ^= 1;
      break;
  }

  if (parity) request |= kSWDHeaderParity;

  return request;
}

bool swd_parity(uint32_t data) {
  uint32_t t = data;
  t ^= t >> 16;
  t ^= t >> 8;
  t ^= t >> 4;
  t ^= t >> 2;
  t ^= t >> 1;

  return t & 1;
}

Error mpsse_transaction(ftdi_context *ftdi,
                        uint8_t *command, size_t command_count,
                        uint8_t *response, size_t response_count,
                        int timeout) {
  size_t count = 0;

  CheckEQ(ftdi_write_data(ftdi, command, command_count), (int) command_count);

  for (int i = 0; i < timeout; ++i) {
    count += CheckP(ftdi_read_data(ftdi, response + count,
                                         response_count - count));

    if (count >= response_count) {
      debug(2, "Response took %d attempts.", i);
      return success;
    }

    usleep(1000);
  }

  return Err::timeout;
}

Error swd_read(ftdi_context *ftdi, int addr, bool debug_port, uint32_t *data) {
  uint8_t commands[] = {
    // Send SWD request byte
    MPSSE_DO_WRITE | MPSSE_LSB | MPSSE_BITMODE, FTL(8),
        swd_request(addr, debug_port, false),

    // Release the bus and clock out a turnaround bit.
    SET_BITS_LOW, kStateIdle, kDirRead,
    CLK_BITS, FTL(1),

    // Read in the response, data, and parity bitfields.
    MPSSE_DO_READ | MPSSE_READ_NEG | MPSSE_LSB | MPSSE_BITMODE, FTL(3),
    MPSSE_DO_READ | MPSSE_READ_NEG | MPSSE_LSB, FTL(4), FTH(4),
    MPSSE_DO_READ | MPSSE_READ_NEG | MPSSE_LSB | MPSSE_BITMODE, FTL(2),

    // Take the bus back and clock out a turnaround bit.
    SET_BITS_LOW, kStateIdle, kDirWrite,
    CLK_BITS, FTL(1),
  };

  uint8_t response[6];
  // response[0]: the three-bit response, MSB-justified.
  // response[4:1]: the 32-bit response word.
  // response[5]: the parity bit in bit 6, turnaround (ignored) in bit 7.
  Check(mpsse_transaction(ftdi, commands, sizeof(commands),
                                response, sizeof(response),
                                1000));

  uint8_t ack = response[0] >> 5;
  CheckEQ(ack, 0x1);  // Require an OK response from the target.

  // Check for parity error.
  uint32_t data_temp = response[1]
                     | response[2] << 8
                     | response[3] << 16
                     | response[4] << 24;
  uint8_t parity = (response[5] >> 6) & 1;
  CheckEQ(parity, swd_parity(data_temp));
  
  // All is well!
  *data = data_temp;
  return success;
}

Error swd_write(ftdi_context *ftdi, int addr, bool debug_port, uint32_t data) {
  uint8_t commands[] = {
    // Write request byte.
    MPSSE_DO_WRITE | MPSSE_LSB | MPSSE_BITMODE, FTL(8),
        swd_request(addr, debug_port, true),
    // Release the bus and clock out a turnaround bit.
    SET_BITS_LOW, kStateIdle, kDirRead,
    CLK_BITS, FTL(1),

    // Read response.
    MPSSE_DO_READ | MPSSE_READ_NEG | MPSSE_LSB | MPSSE_BITMODE, FTL(3),

    // Take the bus back and clock out a turnaround bit.
    SET_BITS_LOW, kStateIdle, kDirWrite,
    CLK_BITS, FTL(1),

    // Send the data word.
    MPSSE_DO_WRITE | MPSSE_LSB, FTL(4), FTH(4),
    (data >>  0) & 0xFF,
    (data >>  8) & 0xFF,
    (data >> 16) & 0xFF,
    (data >> 24) & 0xFF,
    // Send the parity bit.
    MPSSE_DO_WRITE | MPSSE_LSB | MPSSE_BITMODE, FTL(1),
        swd_parity(data) ? 0xFF : 0x00,
  };

  uint8_t response[1];
  Check(mpsse_transaction(ftdi, commands, sizeof(commands),
                                response, sizeof(response),
                                1000));
  
  uint8_t ack = response[0] >> 5;
  CheckEQ(ack, 0x1);  // Require OK response.

  return success;
}

}  // un-named namespace for implementation factors


SWDInterface::SWDInterface(ftdi_context *ftdi) : _ftdi(ftdi) {}


Error SWDInterface::initialize() {
  Check(reset_target());
  Check(reset_swd());

  uint32_t idcode;
  Check(read_dp_idcode(&idcode));

  uint32_t version = idcode >> 28;
  uint32_t partno = (idcode >> 12) & 0xFFFF;
  uint32_t designer = (idcode >> 1) & 0x7FF;

  debug(1, "Debug Port IDCODE = %08X", idcode);
  debug(1, "  Version:  %X", version);
  debug(1, "  Part:     %X", partno);
  debug(1, "  Designer: %X", designer);

  return success;
}


Error SWDInterface::reset_target() {
  uint8_t commands[] = { SET_BITS_LOW, 0 /* set below */, kDirWrite };

  commands[1] = kStateResetTarget;
  CheckEQ(ftdi_write_data(_ftdi, commands, sizeof(commands)),
          sizeof(commands));

  usleep(20000);

  commands[1] = kStateIdle;
  CheckEQ(ftdi_write_data(_ftdi, commands, sizeof(commands)),
          sizeof(commands));

  return success;
}


Error SWDInterface::reset_swd() {
  uint8_t commands[] = {
    SET_BITS_LOW, kStateResetSWD, kDirWrite,
    CLK_BYTES, FTL(6), FTH(6),
    CLK_BITS, FTL(2),
    SET_BITS_LOW, kStateIdle, kDirWrite,
    CLK_BITS, FTL(1),
  };

  CheckEQ(ftdi_write_data(_ftdi, commands, sizeof(commands)),
          sizeof(commands));

  return success;
}


Error SWDInterface::read_dp(DebugRegister addr, uint32_t *data) {
  return swd_read(_ftdi, addr, true, data);
}

Error SWDInterface::write_dp(DebugRegister addr, uint32_t data) {
  return swd_write(_ftdi, addr, true, data);
}


/*
 * Implementation of utilities.
 */

Error SWDInterface::read_dp_idcode(uint32_t *data) {
  return read_dp(kDPIDCODE, data);
}

Error SWDInterface::write_dp_abort(uint32_t data) {
  return write_dp(kDPABORT, data);
}

Error SWDInterface::read_dp_ctrlstat_wcr(uint32_t *data) {
  return read_dp(kDPCTRLSTAT, data);
}

Error SWDInterface::write_dp_ctrlstat_wcr(uint32_t data) {
  return write_dp(kDPCTRLSTAT, data);
}

Error SWDInterface::write_dp_select(uint32_t data) {
  return write_dp(kDPSELECT, data);
}

Error SWDInterface::read_dp_resend(uint32_t *data) {
  return read_dp(kDPRESEND, data);
}

Error SWDInterface::read_dp_rdbuff(uint32_t *data) {
  return read_dp(kDPRDBUFF, data);
}

