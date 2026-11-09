/**
 * Copyright (c) 2011-2025 Bill Greiman
 * This file is part of the SdFat library for SD memory cards.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// HDDSynthPro's copy of SdFat's PIO SDIO driver (SdFat 2.3.1,
// src/SdCard/PioSdio/PioSdioCard.*, MIT licence above), as sdio::Card, used
// by the PCB build (pcb_drive) instead of the library's. SdFat mounts it like
// any card (sd_card.cpp's CardFs). What's changed from the library's, and why
// (docs/pio-allocation.md; each change is marked "HDDSynthPro:"):
//   - status() and cardCMD6() end a read or write in progress first: the
//     driver leaves a multi-sector read open between calls and takes the next
//     sector from it without a command, so after a CMD13 in the middle the
//     library's returned other sectors' data with a good CRC and no error
//     (found on the PCB, 8 Nov 2026).
//   - It claims only the state machines it uses (0 and 1), and frees only
//     those, where the library's claimed all four and freed all four, other
//     users' included.
//   - No debug printing (USE_DEBUG_MODE 0): the library's printed to Serial on
//     errors, ignoring core 0's rule of printing only when USB has room.
//   - No input synchroniser bypass: the library's shifted by the GPIO number,
//     past bit 31 for the PCB's pins (34-39), so it never set any; this says
//     so. Bypassing is for 50 MHz, which isn't used.
#pragma once
#include <SdFat.h>  // SdCardInterface, PioSdioConfig, cid_t...

namespace sdio {

//------------------------------------------------------------------------------
/**
 * \class CmdRsp_t
 * \brief SD command/response type.
 */
class CmdRsp_t {
 public:
  /**
   * \param[in] idx_ Command index.
   * \param[in] rsp_ Response type.
   */
  CmdRsp_t(uint8_t idx_, uint8_t rsp_) : idx(idx_), rsp(rsp_) {}
  uint8_t idx;  ///< Command index.
  uint8_t rsp;  ///< Response type.
};
//------------------------------------------------------------------------------
/**
 * \class Card
 * \brief Raw SDIO access to SD and SDHC flash memory cards.
 */
class Card : public SdCardInterface {
 public:
  Card() = default;  // cppcheck-suppress uninitMemberVar
  /** Initialize the SD card.
   * \param[in] config SDIO card configuration.
   * \return true for success or false for failure.
   */
  bool begin(PioSdioConfig config);
  /** CMD6 Switch mode: Check Function Set Function.
   * \param[in] arg CMD6 argument.
   * \param[out] status return status data.
   *
   * \return true for success or false for failure.
   */
  bool cardCMD6(uint32_t arg, uint8_t* status) final;
  /** Disable an SDIO card.
   * not implemented.
   */
  void end() final;

#ifndef DOXYGEN_SHOULD_SKIP_THIS
  uint32_t __attribute__((error("use sectorCount()"))) cardSize();
#endif  // DOXYGEN_SHOULD_SKIP_THIS
  /** Erase a range of sectors.
   *
   * \param[in] firstSector The address of the first sector in the range.
   * \param[in] lastSector The address of the last sector in the range.
   *
   * \note This function requests the SD card to do a flash erase for a
   * range of sectors.  The data on the card after an erase operation is
   * either 0 or 1, depends on the card vendor.  The card must support
   * single sector erase.
   *
   * \return true for success or false for failure.
   */
  bool erase(Sector_t firstSector, Sector_t lastSector) final;
  /**
   * \return code for the last error. See SdCardInfo.h for a list of error
   * codes.
   */
  uint8_t errorCode() const final;
  /** \return error data for last error. */
  uint32_t errorData() const final;
  /** \return error line for last error. Tmp function for debug. */
  uint32_t errorLine() const;
  /**
   * Check for busy with CMD13.
   *
   * \return true if busy else false.
   */
  bool isBusy() final;
  /** \return the SD clock frequency in kHz. */
  uint32_t kHzSdClk();
  /**
   * Read a 512 byte sector from an SD card.
   *
   * \param[in] sector Logical sector to be read.
   * \param[out] dst Pointer to the location that will receive the data.
   * \return true for success or false for failure.
   */
  bool readSector(Sector_t sector, uint8_t* dst) final;
  /**
   * Read multiple 512 byte sectors from an SD card.
   *
   * \param[in] sector Logical sector to be read.
   * \param[in] ns Number of sectors to be read.
   * \param[out] dst Pointer to the location that will receive the data.
   * \return true for success or false for failure.
   */
  bool readSectors(Sector_t sector, uint8_t* dst, size_t ns) final;
  /**
   * Read a card's CID register. The CID contains card identification
   * information such as Manufacturer ID, Product name, Product serial
   * number and Manufacturing date.
   *
   * \param[out] cid pointer to area for returned data.
   *
   * \return true for success or false for failure.
   */
  bool readCID(cid_t* cid) final;
  /**
   * Read a card's CSD register. The CSD contains Card-Specific Data that
   * provides information regarding access to the card's contents.
   *
   * \param[out] csd pointer to area for returned data.
   *
   * \return true for success or false for failure.
   */
  bool readCSD(csd_t* csd) final;
  /** Read one data sector in a multiple sector read sequence
   *
   * \param[out] dst Pointer to the location for the data to be read.
   *
   * \return true for success or false for failure.
   */
  bool readData(uint8_t* dst);
  /** Read OCR register.
   *
   * \param[out] ocr Value of OCR register.
   * \return true for success or false for failure.
   */
  bool readOCR(uint32_t* ocr) final;
  /** Read SCR register.
   *
   * \param[out] scr Value of SCR register.
   * \return true for success or false for failure.
   */
  bool readSCR(scr_t* scr) final;
  /** Return the 64 byte SD Status register.
   * \param[out] sds location for 64 status bytes.
   * \return true for success or false for failure.
   */
  bool readSDS(sds_t* sds) final;
  /** Start a read multiple sectors sequence.
   *
   * \param[in] sector Address of first sector in sequence.
   *
   * \note This function is used with readData() and readStop() for optimized
   * multiple sector reads.
   *
   * \return true for success or false for failure.
   */
  bool readStart(Sector_t sector);
  /** End a read multiple sectors sequence.
   *
   * \return true for success or false for failure.
   */
  bool readStop();
  /** \return SDIO card status. */
  uint32_t status() final;
  /**
   * Determine the size of an SD flash memory card.
   *
   * \return The number of 512 byte data sectors in the card
   *         or zero if an error occurs.
   */
  Sector_t sectorCount() final;
  /**
   *  Send CMD12 to stop read or write.
   *
   * \param[in] blocking If true, wait for command complete.
   *
   * \return true for success or false for failure.
   */
  bool stopTransmission(bool blocking);
  /** \return success if sync successful. Not for user apps. */
  bool syncDevice() final;
  /** Return the card type: SD V1, SD V2 or SDHC
   * \return 0 - SD V1, 1 - SD V2, or 3 - SDHC.
   */
  uint8_t type() const final;
  /**
   * Writes a 512 byte sector to an SD card.
   *
   * \param[in] sector Logical sector to be written.
   * \param[in] src Pointer to the location of the data to be written.
   * \return true for success or false for failure.
   */
  bool writeSector(Sector_t sector, const uint8_t* src) final;
  /**
   * Write multiple 512 byte sectors to an SD card.
   *
   * \param[in] sector Logical sector to be written.
   * \param[in] ns Number of sectors to be written.
   * \param[in] src Pointer to the location of the data to be written.
   * \return true for success or false for failure.
   */
  bool writeSectors(Sector_t sector, const uint8_t* src, size_t ns) final;
  /** Write one data sector in a multiple sector write sequence.
   * \param[in] src Pointer to the location of the data to be written.
   * \return true for success or false for failure.
   */
  bool writeData(const uint8_t* src);
  /** Start a write multiple sectors sequence.
   *
   * \param[in] sector Address of first sector in sequence.
   *
   * \note This function is used with writeData() and writeStop()
   * for optimized multiple sector writes.
   *
   * \return true for success or false for failure.
   */
  bool writeStart(Sector_t sector);

  /** End a write multiple sectors sequence.
   *
   * \return true for success or false for failure.
   */
  bool writeStop();

 private:
  //----------------------------------------------------------------------------
  bool cardAcmd(uint32_t rca, CmdRsp_t cmdRsp, uint32_t arg);
  bool cardCommand(CmdRsp_t cmd, uint32_t arg, void* rsp = nullptr);
  uint32_t cardStatus();  // HDDSynthPro: CMD13 as it stands, for the driver's own checks
  void pioConfig(float clkDiv);
  void pioEnd();
  bool pioInit();
  void powerUpClockCycles();
  bool readData(void* dst, size_t count);
  void setSdErrorCode(uint8_t code, uint32_t line) {
    m_errorCode = code;
    m_errorLine = line;
  }
  static const uint8_t IDLE_STATE = 0;
  static const uint8_t READ_STATE = 1;
  static const uint8_t WRITE_STATE = 2;
  Sector_t m_curSector = 0;
  uint8_t m_curState = IDLE_STATE;
  uint m_cardRsp;
  uint m_errorCode;
  uint m_errorLine;
  bool m_highCapacity;
  bool m_initDone = false;
  uint m_ocr;
  bool m_version2;
  uint m_rca;
  cid_t m_cid;
  csd_t m_csd;
  scr_t m_scr;
  sds_t m_sds;

  float m_clkDiv = 0;
  uint m_clkPin = 63u;   // PIN_SDIO_UNDEFINED;
  uint m_cmdPin = 63u;   // PIN_SDIO_UNDEFINED;
  uint m_dat0Pin = 63u;  // PIN_SDIO_UNDEFINED;
  PIO m_pio = nullptr;
  int m_sm0 = -1;
  int m_sm1 = -1;
  int m_cmdRspOffset = -1;
  pio_sm_config m_cmdConfig;
  int m_rdDataOffset = -1;
  pio_sm_config m_rdDataConfig;
  int m_rdClkOffset = -1;
  pio_sm_config m_rdClkConfig;
  int m_wrDataOffset = -1;
  pio_sm_config m_wrDataConfig;
  int m_wrRespOffset = -1;
  pio_sm_config m_wrRespConfig;
};

}  // namespace sdio
