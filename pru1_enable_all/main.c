/*
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *
 *	* Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the
 *	  distribution.
 *
 *	* Neither the name of Texas Instruments Incorporated nor the names of
 *	  its contributors may be used to endorse or promote products derived
 *	  from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rsc_types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pru_cfg.h>
#include <pru_ctrl.h>
#include <pru_intc.h>
#include <pru_rpmsg.h>
#include "resource_table_1.h"

// Host-1 Interrupt sets bit 31 in register R31
#define HOST_INT ((uint32_t) 1 << 31)

// The PRU-ICSS system events used for RPMsg are defined in the Linux device tree
// PRU1 uses system event 18 (To ARM) and 19 (From ARM)

#define TO_ARM_HOST 18
#define FROM_ARM_HOST 19

#define PIN_ENABLE 8 // P8_27, LCD_VSYNC
#define PIN_MICROSTEP_0 10 // P8_28, LCD_PCLK
#define PIN_MICROSTEP_1 9 // P8_29, LCD_HSYNC
#define PIN_MICROSTEP_2 11 // P8_30, LCD_DE
#define PIN_DIR_LEFT 2 // P8_43, LCD_DATA2
#define PIN_STEP_LEFT 3 // P8_44, LCD_DATA3
#define PIN_DIR_RIGHT 0 // P8_45, LCD_DATA0
#define PIN_STEP_RIGHT 1 // P8_46, LCD_DATA1

/*
 * Using the name 'rpmsg-client-sample' will probe the RPMsg sample driver
 * found at linux-x.y.z/samples/rpmsg/rpmsg_client_sample.c
 *
 * Using the name 'rpmsg-pru' will probe the rpmsg_pru driver found
 * at linux-x.y.z/drivers/rpmsg/rpmsg_pru.c
 */
#define CHAN_NAME "rpmsg-pru"
#define CHAN_DESC "Channel 31"
#define CHAN_PORT 31

// Used to make sure the Linux drivers are ready for RPMsg communication; Found at linux-x.y.z/include/uapi/linux/virtio_config.h
#define VIRTIO_CONFIG_S_DRIVER_OK 4

// How long to wait between frames before shutting down (1s = 200M cycles)
#define SHUTDOWN_WATCHDOG_TIMER (200 * 1000 * 1000)

volatile register uint32_t __R30;
volatile register uint32_t __R31;

struct DataFrame {
	uint8_t enabled;
	// 1/microstep (1 - full step, 4 - 1/4 step and so on)
	uint8_t microstep;
	uint8_t directionLeft;
	uint8_t directionRight;
	uint32_t speedLeft;
	uint32_t speedRight;
};

uint8_t payload[RPMSG_BUF_SIZE];

void main() {
	// Set 1 on all pins
	__R31 = __R31 | (1 << PIN_ENABLE);
	__R31 = __R31 | (1 << PIN_MICROSTEP_0);
	__R31 = __R31 | (1 << PIN_MICROSTEP_1);
	__R31 = __R31 | (1 << PIN_MICROSTEP_2);
	__R31 = __R31 | (1 << PIN_DIR_LEFT);
	__R31 = __R31 | (1 << PIN_STEP_LEFT);
	__R31 = __R31 | (1 << PIN_DIR_RIGHT);
	__R31 = __R31 | (1 << PIN_STEP_RIGHT);

	// Allow OCP master port access by the PRU so the PRU can read external memories
	CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;
	// Clear the status of the PRU-ICSS system event that the ARM will use to 'kick' us
	CT_INTC.SICR_bit.STS_CLR_IDX = FROM_ARM_HOST;

	// Make sure the Linux drivers are ready for RPMsg communication
	volatile uint8_t *status;
	status = &resourceTable.rpmsg_vdev.status;
	while (!(*status & VIRTIO_CONFIG_S_DRIVER_OK));

	// Initialize the RPMsg transport structure
	struct pru_rpmsg_transport transport;
	pru_rpmsg_init(&transport, &resourceTable.rpmsg_vring0, &resourceTable.rpmsg_vring1, TO_ARM_HOST, FROM_ARM_HOST);

	// Create the RPMsg channel between the PRU and ARM user space using the transport structure.
	while (pru_rpmsg_channel(RPMSG_NS_CREATE, &transport, CHAN_NAME, CHAN_DESC, CHAN_PORT) != PRU_RPMSG_SUCCESS);

	uint16_t src, dst, len;
	struct DataFrame* received;
	received = (struct DataFrame*) malloc(sizeof(struct DataFrame));

	while (1) {
		// Check bit 30 of register R31 to see if the ARM has kicked us
		if (__R31 & HOST_INT) {
			// Clear the event status
			CT_INTC.SICR_bit.STS_CLR_IDX = FROM_ARM_HOST;
			// Receive all available messages, multiple messages can be sent per kick
			while (pru_rpmsg_receive(&transport, &src, &dst, payload, &len) == PRU_RPMSG_SUCCESS) {
				received = (struct DataFrame*) payload;

				if (received->enabled) {
					__R31 = __R31 & ~(1 << PIN_ENABLE);
					__R31 = __R31 & ~(1 << PIN_MICROSTEP_0);
					__R31 = __R31 & ~(1 << PIN_MICROSTEP_1);
					__R31 = __R31 & ~(1 << PIN_MICROSTEP_2);
					__R31 = __R31 & ~(1 << PIN_DIR_LEFT);
					__R31 = __R31 & ~(1 << PIN_STEP_LEFT);
					__R31 = __R31 & ~(1 << PIN_DIR_RIGHT);
					__R31 = __R31 & ~(1 << PIN_STEP_RIGHT);
				} else {
					__R31 = __R31 | (1 << PIN_ENABLE);
					__R31 = __R31 | (1 << PIN_MICROSTEP_0);
					__R31 = __R31 | (1 << PIN_MICROSTEP_1);
					__R31 = __R31 | (1 << PIN_MICROSTEP_2);
					__R31 = __R31 | (1 << PIN_DIR_LEFT);
					__R31 = __R31 | (1 << PIN_STEP_LEFT);
					__R31 = __R31 | (1 << PIN_DIR_RIGHT);
					__R31 = __R31 | (1 << PIN_STEP_RIGHT);
				}
			}
		}
	}
}
