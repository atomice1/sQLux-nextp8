/**
 * DS1307 Real-Time Clock Model for sQLux
 * 
 * Emulates a DS1307 RTC connected via I2C
 * Responds to I2C master transactions through MMIO registers
 */

#include "i2c_rtc.h"
#include <string.h>
#include <stdio.h>

/* DS1307 Configuration */
#define DS1307_I2C_ADDR     0x68        /* 7-bit I2C address */
#define DS1307_ADDR_WRITE   0xD0        /* Address + Write bit */
#define DS1307_ADDR_READ    0xD1        /* Address + Read bit */
#define DS1307_MEM_SIZE     64          /* Total memory: 64 bytes */
#define DS1307_RTC_REGS     7           /* RTC registers: 0x00-0x06 */

/* RTC Register Offsets */
#define REG_SECONDS         0x00
#define REG_MINUTES         0x01
#define REG_HOURS           0x02
#define REG_DAY             0x03
#define REG_DATE            0x04
#define REG_MONTH           0x05
#define REG_YEAR            0x06
#define REG_CONTROL         0x07

/* Register Bit Masks */
#define SECONDS_CH_BIT      0x80        /* Clock Halt bit */
#define HOURS_12_24_BIT     0x40        /* 12/24 hour mode */
#define HOURS_AM_PM_BIT     0x20        /* AM/PM in 12-hour mode */

/* I2C State Machine States */
typedef enum {
    I2C_IDLE,           /* Waiting for START condition */
    I2C_ADDRESS,        /* Receiving slave address */
    I2C_REG_ADDR,       /* Receiving register address */
    I2C_WRITE_DATA,     /* Receiving data to write */
    I2C_READ_DATA,      /* Sending data to master */
    I2C_ERROR           /* Error state (NACK) */
} i2c_state_t;

/* DS1307 Internal State */
typedef struct {
    uint8_t memory[DS1307_MEM_SIZE];    /* RTC registers + RAM */
    uint8_t reg_ptr;                    /* Current register pointer */
    i2c_state_t state;                  /* I2C state machine */
    bool is_selected;                   /* Device selected by address */
    bool is_reading;                    /* Current transaction is read */
    uint8_t data_in;                    /* Data received from master (buffered) */
    uint8_t data_out;                   /* Data to send to master */
    unsigned busy;                      /* Transaction in progress */
    bool error;                         /* ACK error flag */
    time_t last_update;                 /* Last time RTC was updated */
    uint16_t next_ctrl;                 /* Next control register value */
    bool pending_ctrl;                  /* Is there a pending control write */
    uint8_t last_data_in;               /* Latched data byte from last I2C transaction */
    bool last_rw;                       /* Latched rw bit from last I2C transaction */
} ds1307_state_t;

/* Global DS1307 state */
static ds1307_state_t ds1307;

/* Helper Functions */

/**
 * Convert binary to BCD
 */
static uint8_t bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}

/**
 * Convert BCD to binary
 */
static uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

/**
 * Update RTC registers from system time
 */
static void update_rtc_registers(void) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    if (tm_info == NULL) {
        return;
    }
    
    /* Only update if clock is not halted (CH bit clear) */
    if (ds1307.memory[REG_SECONDS] & SECONDS_CH_BIT) {
        return;
    }
    
    /* Convert to BCD and store in registers */
    ds1307.memory[REG_SECONDS] = bin_to_bcd(tm_info->tm_sec) & 0x7F;
    ds1307.memory[REG_MINUTES] = bin_to_bcd(tm_info->tm_min) & 0x7F;
    
    /* Store in 24-hour format */
    ds1307.memory[REG_HOURS] = bin_to_bcd(tm_info->tm_hour) & 0x3F;
    
    /* Day of week (1-7, Sunday=1) */
    ds1307.memory[REG_DAY] = (tm_info->tm_wday == 0) ? 7 : tm_info->tm_wday;
    
    /* Date, month, year */
    ds1307.memory[REG_DATE] = bin_to_bcd(tm_info->tm_mday) & 0x3F;
    ds1307.memory[REG_MONTH] = bin_to_bcd(tm_info->tm_mon + 1) & 0x1F;
    
    /* Year (0-99 for 2000-2099) */
    int year = tm_info->tm_year + 1900;
    ds1307.memory[REG_YEAR] = bin_to_bcd(year % 100);
    
    ds1307.last_update = now;
}

/**
 * Process I2C byte based on current state
 * Called when a byte has been sent/received on the I2C bus
 * Note: The I2C master hardware handles device addressing (0x68),
 * so this model only sees register addresses and data.
 */
static void process_i2c_transaction(bool is_write) {
    if (is_write) {
        /* Master is writing a byte to us */
        uint8_t byte = ds1307.data_in;
        
        switch (ds1307.state) {
            case I2C_IDLE:
                /* First write after START - this is the register address */
                if (byte < DS1307_MEM_SIZE) {
                    ds1307.reg_ptr = byte;
                    ds1307.state = I2C_WRITE_DATA;
                    ds1307.error = false;
                    printf("DS1307: Set register pointer to 0x%02X\n", byte);
                } else {
                    /* Invalid register address */
                    ds1307.error = true;
                    ds1307.state = I2C_ERROR;
                    printf("DS1307: Error - Invalid register address 0x%02X (max 0x%02X)\n", 
                           byte, DS1307_MEM_SIZE - 1);
                }
                break;
                
            case I2C_WRITE_DATA:
                /* Receiving data to write */
                if (ds1307.reg_ptr < DS1307_MEM_SIZE) {
                    ds1307.memory[ds1307.reg_ptr] = byte;
                    printf("DS1307: Write 0x%02X to register 0x%02X\n", byte, ds1307.reg_ptr);
                    
                    /* Auto-increment register pointer */
                    ds1307.reg_ptr = (ds1307.reg_ptr + 1) % DS1307_MEM_SIZE;
                    ds1307.error = false;
                } else {
                    ds1307.error = true;
                    ds1307.state = I2C_ERROR;
                    printf("DS1307: Error - Write to invalid register 0x%02X\n", ds1307.reg_ptr);
                }
                break;
                
            case I2C_READ_DATA:
                /* Should not receive write data during read transaction */
                ds1307.error = true;
                ds1307.state = I2C_ERROR;
                printf("DS1307: Error - Received write data during read transaction\n");
                break;
                
            case I2C_ERROR:
                /* Stay in error state */
                break;
                
            default:
                ds1307.state = I2C_IDLE;
                break;
        }
    } else {
        /* Master is reading a byte from us */
        ds1307.state = I2C_READ_DATA;
        /* Prepare next byte for subsequent read */
        if (ds1307.reg_ptr < DS1307_MEM_SIZE) {
            ds1307.data_out = ds1307.memory[ds1307.reg_ptr];
            printf("DS1307: Read 0x%02X from register 0x%02X\n", ds1307.data_out, ds1307.reg_ptr);
            ds1307.reg_ptr = (ds1307.reg_ptr + 1) % DS1307_MEM_SIZE;
            ds1307.error = false;
        } else {
            ds1307.error = true;
            printf("DS1307: Error - Read from invalid register 0x%02X\n", ds1307.reg_ptr);
        }
    }
}

/* Public API Implementation */

void i2c_rtc_init(void) {
    /* Clear all memory */
    memset(&ds1307, 0, sizeof(ds1307));
    
    /* Initialize to idle state */
    ds1307.state = I2C_IDLE;
    ds1307.is_selected = false;
    ds1307.is_reading = false;
    ds1307.busy = 0;
    ds1307.error = false;
    ds1307.reg_ptr = 0;
    ds1307.data_in = 0;
    ds1307.data_out = 0;
    ds1307.last_data_in = 0;
    ds1307.last_rw = false;
    
    /* Set initial time from system */
    update_rtc_registers();
    
    /* Clear CH bit to start the clock */
    ds1307.memory[REG_SECONDS] &= ~SECONDS_CH_BIT;
    
    printf("DS1307 RTC initialized\n");
}

void i2c_rtc_update(void) {
    /* Update RTC registers once per second */
    time_t now = time(NULL);
    if (now != ds1307.last_update) {
        update_rtc_registers();
    }
}

void i2c_rtc_write_data(uint8_t value) {
    /* Buffer the data byte to be sent on the I2C bus
     * The actual transaction happens when control register is written */
    ds1307.data_in = value;
}

uint8_t i2c_rtc_read_data(void) {
    /* Return the data byte prepared by the state machine */
    return ds1307.data_out;
}

void i2c_rtc_write_ctrl(uint8_t value) {
    if (ds1307.pending_ctrl) {
        /* Previous control not yet processed */
        printf("DS1307: Warning - Previous control write not yet processed\n");
    }
    printf("DS1307: Control write 0x%02X (pending)\n", value);
    ds1307.next_ctrl = value;
    ds1307.pending_ctrl = true;
}

static void i2c_rtc_process_pending_ctrl(void) {
    bool ena = (ds1307.next_ctrl & 0x01) != 0;
    bool rw = (ds1307.next_ctrl & 0x02) != 0;
    ds1307.pending_ctrl = false;
    
    if (!ena) {
        /* ENA=0: STOP condition - end transaction and reset to idle */
        if (ds1307.busy) {
            printf("DS1307: BUSY 1->0 (STOP)\n");
        }
        ds1307.state = I2C_IDLE;
        ds1307.busy = 0;
        return;
    }
    
    /* ENA=1: Start or continue I2C transaction */
    if (!ds1307.busy) {
        printf("DS1307: BUSY 0->1 (START/CONTINUE)\n");
    }
    ds1307.busy = 2;

    /* If command matches previous (same data_in and rw), auto-increment reg_ptr
     * before issuing the transaction. Otherwise reset state to IDLE so the
     * next byte is treated as a fresh transaction. */
    if (ds1307.data_in == ds1307.last_data_in && rw == ds1307.last_rw) {
        printf("DS1307: Repeat command - reg_ptr auto-incremented to 0x%02X\n", ds1307.reg_ptr);
    } else {
        ds1307.state = I2C_IDLE;
    }

    if (rw) {
        /* Read operation - master wants to read from slave */
        printf("DS1307: Read operation, state=%d\n", ds1307.state);
        process_i2c_transaction(false);
    } else {
        /* Write operation - master is sending a byte to slave */
        printf("DS1307: Write operation, state=%d, data=0x%02X\n", ds1307.state, ds1307.data_in);
        process_i2c_transaction(true);
    }

    /* Latch last command info for next control evaluation */
    ds1307.last_data_in = ds1307.data_in;
    ds1307.last_rw = rw;
}

uint8_t i2c_rtc_read_status(void) {
    /* Return status bits */
    uint8_t status = 0;

    printf("DS1307: BUSY=%u\n", ds1307.busy);
    if (ds1307.busy) {
        status |= 0x01;  /* Busy bit */
    }
    
    if (ds1307.error) {
        status |= 0x02;  /* Error bit */
    }

    if (ds1307.busy > 0) {
        ds1307.busy--;
        if (ds1307.busy == 0)
            printf("DS1307: BUSY 1->0 (transaction complete)\n");
    } else if (!ds1307.pending_ctrl && ds1307.state == I2C_READ_DATA) {
        // After read transaction, set pending control to read next byte
        printf("DS1307: Setting pending control for next read\n");
        ds1307.pending_ctrl = true;
    } else {
        if (ds1307.pending_ctrl) {
            printf("DS1307: Processing pending control write\n");
            i2c_rtc_process_pending_ctrl();
        }
    }

    return status;
}

void i2c_rtc_reset(void) {
    ds1307.state = I2C_IDLE;
    ds1307.is_selected = false;
    ds1307.busy = 0;
    ds1307.error = false;
    ds1307.pending_ctrl = false;
    ds1307.last_data_in = 0;
    ds1307.last_rw = false;
}
