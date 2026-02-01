#ifndef I2C_RTC_H
#define I2C_RTC_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/**
 * DS1307 Real-Time Clock Model
 * 
 * This module emulates a DS1307 RTC connected via I2C.
 * It responds to I2C transactions through MMIO registers.
 * 
 * Memory Map:
 *   0x00-0x06: RTC registers (seconds, minutes, hours, day, date, month, year)
 *   0x07:      Control register
 *   0x08-0x3F: General purpose RAM (56 bytes)
 * 
 * I2C Address: 0x68 (7-bit)
 *
 * (C) 2026 Chris January
 * GPL-3 with exception for sQLux linking
 */

/**
 * Initialize the DS1307 RTC model
 * Sets initial time to current system time
 */
void i2c_rtc_init(void);

/**
 * Update the RTC time
 * Should be called periodically to keep the RTC synchronized
 * with real time (e.g., once per frame)
 */
void i2c_rtc_update(void);

/**
 * Handle write to I2C data register (0x800021)
 * Called when the emulated CPU writes to the I2C data output register
 * 
 * @param value: The byte written to the data register
 */
void i2c_rtc_write_data(uint8_t value);

/**
 * Handle read from I2C data register (0x800021)
 * Called when the emulated CPU reads from the I2C data input register
 * 
 * @return: The byte read from the data register
 */
uint8_t i2c_rtc_read_data(void);

/**
 * Handle write to I2C control register (0x800023)
 * Called when the emulated CPU writes to the I2C control register
 * Controls the I2C transaction state machine
 * 
 * @param value: Control bits (bit[1]=rw, bit[0]=ena)
 */
void i2c_rtc_write_ctrl(uint8_t value);

/**
 * Handle read from I2C control/status register (0x800023)
 * Called when the emulated CPU reads from the I2C status register
 * 
 * @return: Status bits (bit[1]=err, bit[0]=busy)
 */
uint8_t i2c_rtc_read_status(void);

/**
 * Reset the I2C transaction state
 * Called when a STOP condition is issued or on error recovery
 */
void i2c_rtc_reset(void);

#endif /* I2C_RTC_H */
