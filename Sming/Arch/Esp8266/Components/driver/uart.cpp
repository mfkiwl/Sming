/*
 uart.cpp - esp8266 UART HAL

 Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.
 This file is part of the esp8266 core for Arduino environment.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

 @author 2018 mikee47 <mike@sillyhouse.net>

 Additional features to support flexible transmit buffering and callbacks

 */

/**
 *  UART GPIOs
 *
 * UART0 TX: 1 or 2
 * UART0 RX: 3
 *
 * UART0 SWAP TX: 15
 * UART0 SWAP RX: 13
 *
 *
 * UART1 TX: 7 (NC) or 2
 * UART1 RX: 8 (NC)
 *
 * UART1 SWAP TX: 11 (NC)
 * UART1 SWAP RX: 6 (NC)
 *
 * NC = Not Connected to Module Pads --> No Access
 *
 */
#include <BitManipulations.h>

#include <driver/uart.h>
#include <espinc/uart_register.h>
#include <driver/SerialBuffer.h>
#include <esp_systemapi.h>
#include <Data/Range.h>

/*
 * Parameters relating to RX FIFO and buffer thresholds
 *
 * 'headroom' is the number of characters which may be received before a receive overrun
 * condition occurs and data is lost.
 *
 * For the hardware FIFO, data is processed via interrupt so the headroom can be fairly small.
 * The greater the headroom, the more interrupts will be generated thus reducing efficiency.
 */
#define RX_FIFO_FULL_THRESHOLD 120									  ///< UIFF interrupt when FIFO bytes > threshold
#define RX_FIFO_HEADROOM (UART_RX_FIFO_SIZE - RX_FIFO_FULL_THRESHOLD) ///< Chars between UIFF and UIOF
/*
 * Using a buffer, data is typically processed via task callback so requires additional time.
 * This figure is set to a nominal default which should provide robust operation for most situations.
 * It can be adjusted if necessary via the rx_headroom parameter.
*/
#define DEFAULT_RX_HEADROOM (32 - RX_FIFO_HEADROOM)

namespace
{
int s_uart_debug_nr = UART_NO;

// Get number of characters in receive FIFO
__forceinline uint8_t uart_rxfifo_count(uint8_t nr)
{
	return (READ_PERI_REG(UART_STATUS(nr)) >> UART_RXFIFO_CNT_S) & UART_RXFIFO_CNT;
}

// Get number of characters in transmit FIFO
__forceinline uint8_t uart_txfifo_count(uint8_t nr)
{
	return (READ_PERI_REG(UART_STATUS(nr)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT;
}

// Get available free characters in transmit FIFO
__forceinline uint8_t uart_txfifo_free(uint8_t nr)
{
	return UART_TX_FIFO_SIZE - uart_txfifo_count(nr) - 1;
}

// Return true if transmit FIFO is full
__forceinline bool uart_txfifo_full(uint8_t nr)
{
	return uart_txfifo_count(nr) >= (UART_TX_FIFO_SIZE - 1);
}

// Keep track of interrupt enable state for each UART
uint8_t isrMask;
// Keep a reference to all created UARTS - required because they share an ISR
smg_uart_t* uartInstances[UART_COUNT];

// Registered port callback functions
smg_uart_notify_callback_t notifyCallbacks[UART_COUNT];

/** @brief Invoke a port callback, if one has been registered
 *  @param uart
 *  @param code
 */
void notify(smg_uart_t* uart, smg_uart_notify_code_t code)
{
	auto callback = notifyCallbacks[uart->uart_nr];
	if(callback != nullptr) {
		callback(uart, code);
	}
}

__forceinline bool uart_isr_enabled(uint8_t nr)
{
	return bitRead(isrMask, nr);
}

/** @brief Determine if the given uart is a real uart or a virtual one
 */
__forceinline bool is_physical(int uart_nr)
{
	return (uart_nr >= 0) && (uart_nr < UART_PHYSICAL_COUNT);
}

__forceinline bool is_physical(smg_uart_t* uart)
{
	return uart != nullptr && is_physical(uart->uart_nr);
}

/** @brief If given a virtual uart, obtain the related physical one
 */
smg_uart_t* get_physical(smg_uart_t* uart)
{
	if(uart != nullptr && uart->uart_nr == UART2) {
		uart = uartInstances[UART0];
	}
	return uart;
}

/**
 * @brief service interrupts for a UART
 * @param uart_nr identifies which UART to check
 * @param uart the allocated uart structure, which may be NULL if port hasn't been setup
 */
void IRAM_ATTR handle_uart_interrupt(uint8_t uart_nr, smg_uart_t* uart)
{
	uint32_t usis = READ_PERI_REG(UART_INT_ST(uart_nr));

	// If status is clear there's no interrupt to service on this UART
	if(usis == 0) {
		return;
	}

	/*
	 * If we haven't asked for interrupts on this UART, then disable all interrupt sources for it.
	 *
	 * This happens at startup where we've only initialised one of the UARTS. For example, we initialise
	 * UART1 for debug output but leave UART0 alone. However, the SDK has enabled some interrupt sources
	 * which we're not expecting.
	 *
	 * (Calling uart_detach_all() at startup pre-empts all this.)
	 */
	if(uart == nullptr || !uart_isr_enabled(uart_nr)) {
		WRITE_PERI_REG(UART_INT_ENA(uart_nr), 0);
		return;
	}

	// Value to be passed to callback
	uint32_t status = usis;

	// Deal with the event, unless we're in raw mode
	if(!bitRead(uart->options, UART_OPT_CALLBACK_RAW)) {
		// Rx FIFO full or timeout
		if(usis & (UART_RXFIFO_FULL_INT_ST | UART_RXFIFO_TOUT_INT_ST | UART_RXFIFO_OVF_INT_ST)) {
			size_t read = 0;

			// Read as much data as possible from the RX FIFO into buffer
			if(uart->rx_buffer != nullptr) {
				size_t avail = uart_rxfifo_count(uart_nr);
				size_t space = uart->rx_buffer->getFreeSpace();
				read = (avail <= space) ? avail : space;
				space -= read;
				while(read-- != 0) {
					uint8_t c = READ_PERI_REG(UART_FIFO(uart_nr));
					uart->rx_buffer->writeChar(c);
				}

				// Don't call back until buffer is (almost) full
				if(space > uart->rx_headroom) {
					status &= ~UART_RXFIFO_FULL_INT_ST;
				}
			}

			/*
			 * If the FIFO is full and we didn't read any of the data then need to mask the interrupt out or it'll recur.
			 * The interrupt gets re-enabled by a call to uart_read() or uart_flush()
			 */
			if(usis & UART_RXFIFO_OVF_INT_ST) {
				CLEAR_PERI_REG_MASK(UART_INT_ENA(uart_nr), UART_RXFIFO_OVF_INT_ENA);
			} else if(read == 0) {
				CLEAR_PERI_REG_MASK(UART_INT_ENA(uart_nr), UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA);
			}
		}

		// Unless we replenish TX FIFO, disable after handling interrupt
		if(usis & UART_TXFIFO_EMPTY_INT_ST) {
			// Dump as much data as we can from buffer into the TX FIFO
			if(uart->tx_buffer != nullptr) {
				size_t space = uart_txfifo_free(uart_nr);
				size_t avail = uart->tx_buffer->available();
				size_t count = (avail <= space) ? avail : space;
				while(count-- != 0) {
					WRITE_PERI_REG(UART_FIFO(uart_nr), uart->tx_buffer->readChar());
				}
			}

			// If TX FIFO remains empty then we must disable TX FIFO EMPTY interrupt to stop it recurring.
			if(uart_txfifo_count(uart_nr) == 0) {
				// The interrupt gets re-enabled by uart_write()
				CLEAR_PERI_REG_MASK(UART_INT_ENA(uart_nr), UART_TXFIFO_EMPTY_INT_ENA);
			} else {
				// We've topped up TX FIFO so defer callback until next time
				status &= ~UART_TXFIFO_EMPTY_INT_ST;
			}
		}
	}

	// Keep a note of persistent flags - cleared via uart_get_status()
	uart->status |= status;

	if(status != 0 && uart->callback != nullptr) {
		uart->callback(uart, status);
	}

	// Final step is to clear status flags
	WRITE_PERI_REG(UART_INT_CLR(uart_nr), usis);
}

/** @brief UART interrupt service routine
 *  @note both UARTS share the same ISR, although UART1 only supports transmit
 */
void IRAM_ATTR uart_isr(void* arg)
{
	handle_uart_interrupt(UART0, uartInstances[UART0]);
	handle_uart_interrupt(UART1, uartInstances[UART1]);
}

void uart0_pin_select(unsigned pin)
{
	switch(pin) {
	case 1:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_UART0_TXD);
		break;
	case 2:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_UART0_TXD_BK);
		break;
	case 3:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_UART0_RXD);
		break;
	case 13:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_UART0_CTS);
		break;
	case 15:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_UART0_RTS);
		break;
	}
}

void uart0_pin_restore(unsigned pin)
{
	switch(pin) {
	case 1:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);
		break;
	case 2:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
		break;
	case 3:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_GPIO3);
		break;
	case 13:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13);
		break;
	case 15:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15);
		break;
	}
}

void uart1_pin_select(unsigned pin)
{
	// GPIO7 as TX not possible! See GPIO pins used by UART
	switch(pin) {
	case 2:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_UART1_TXD_BK);
		break;
	}
}

void uart1_pin_restore(const unsigned pin)
{
	switch(pin) {
	case 2:
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
		break;
	}
}

} // namespace

smg_uart_t* smg_uart_get_uart(uint8_t uart_nr)
{
	return (uart_nr < UART_COUNT) ? uartInstances[uart_nr] : nullptr;
}

uint8_t smg_uart_disable_interrupts()
{
	ETS_UART_INTR_DISABLE();
	return isrMask;
}

void smg_uart_restore_interrupts()
{
	if(isrMask != 0) {
		ETS_UART_INTR_ENABLE();
	}
}

bool smg_uart_set_notify(unsigned uart_nr, smg_uart_notify_callback_t callback)
{
	if(uart_nr >= UART_COUNT) {
		return false;
	}

	notifyCallbacks[uart_nr] = callback;
	return true;
}

void smg_uart_set_callback(smg_uart_t* uart, smg_uart_callback_t callback, void* param)
{
	if(uart != nullptr) {
		uart->callback = nullptr; // In case interrupt fires between setting param and callback
		uart->param = param;
		uart->callback = callback;
	}
}

size_t smg_uart_read(smg_uart_t* uart, void* buffer, size_t size)
{
	if(!smg_uart_rx_enabled(uart) || buffer == nullptr || size == 0) {
		return 0;
	}

	notify(uart, UART_NOTIFY_BEFORE_READ);

	size_t read = 0;

	auto buf = static_cast<uint8_t*>(buffer);

	// First read data from RX buffer if in use
	if(uart->rx_buffer != nullptr) {
		while(read < size && !uart->rx_buffer->isEmpty())
			buf[read++] = uart->rx_buffer->readChar();
	}

	// Top up from hardware FIFO
	if(is_physical(uart)) {
		while(read < size && uart_rxfifo_count(uart->uart_nr) != 0) {
			buf[read++] = READ_PERI_REG(UART_FIFO(uart->uart_nr));
		}

		// FIFO full may have been disabled if buffer overflowed, re-enabled it now
		WRITE_PERI_REG(UART_INT_CLR(uart->uart_nr),
					   UART_RXFIFO_FULL_INT_CLR | UART_RXFIFO_TOUT_INT_CLR | UART_RXFIFO_OVF_INT_CLR);
		SET_PERI_REG_MASK(UART_INT_ENA(uart->uart_nr),
						  UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA | UART_RXFIFO_OVF_INT_ENA);
	}

	return read;
}

size_t smg_uart_rx_available(smg_uart_t* uart)
{
	if(!smg_uart_rx_enabled(uart)) {
		return 0;
	}

	smg_uart_disable_interrupts();

	size_t avail = is_physical(uart) ? uart_rxfifo_count(uart->uart_nr) : 0;

	if(uart->rx_buffer != nullptr) {
		avail += uart->rx_buffer->available();
	}

	smg_uart_restore_interrupts();

	return avail;
}

void smg_uart_start_isr(smg_uart_t* uart)
{
	if(!is_physical(uart)) {
		return;
	}

	uint32_t conf1 = 0;
	uint32_t intena = 0;

	if(smg_uart_rx_enabled(uart)) {
		conf1 = (RX_FIFO_FULL_THRESHOLD << UART_RXFIFO_FULL_THRHD_S) | (0x02 << UART_RX_TOUT_THRHD_S) | UART_RX_TOUT_EN;

		/*
		 * There is little benefit in generating interrupts on errors, instead these
		 * should be cleared at the start of a transaction and checked at the end.
		 * See uart_get_status().
		 */
		intena = UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA | UART_BRK_DET_INT_ENA | UART_RXFIFO_OVF_INT_ENA;
	}

	if(smg_uart_tx_enabled(uart)) {
		/*
		 * We can interrupt when TX FIFO is empty; at 1Mbit that gives us 800 CPU
		 * cycles before the last character has actually gone over the wire. Even if
		 * a gap occurs it is unlike to cause any problems. It also makes the callback
		 * more useful, for example if using it for RS485 we'd then want to reverse
		 * transfer direction and begin waiting for a response.
		 */

		// TX FIFO empty threshold
		// conf1 |= (0 << UART_TXFIFO_EMPTY_THRHD_S);
		// TX FIFO empty interrupt only gets enabled via uart_write function()
	}

	WRITE_PERI_REG(UART_CONF1(uart->uart_nr), conf1);
	WRITE_PERI_REG(UART_INT_CLR(uart->uart_nr), 0xffff);
	WRITE_PERI_REG(UART_INT_ENA(uart->uart_nr), intena);

	uint8_t oldmask = isrMask;

	bitSet(isrMask, uart->uart_nr);

	if(oldmask == 0) {
		ETS_UART_INTR_DISABLE();
		ETS_UART_INTR_ATTACH(uart_isr, nullptr);
		ETS_UART_INTR_ENABLE();
	}
}

size_t smg_uart_write(smg_uart_t* uart, const void* buffer, size_t size)
{
	if(!smg_uart_tx_enabled(uart) || buffer == nullptr || size == 0) {
		return 0;
	}

	size_t written = 0;

	auto buf = static_cast<const uint8_t*>(buffer);

	bool isPhysical = is_physical(uart);

	while(written < size) {
		if(isPhysical) {
			// If TX buffer not in use or it's empty then write directly to hardware FIFO
			if(uart->tx_buffer == nullptr || uart->tx_buffer->isEmpty()) {
				while(written < size && !uart_txfifo_full(uart->uart_nr)) {
					WRITE_PERI_REG(UART_FIFO(uart->uart_nr), buf[written++]);
				}
				// Enable TX FIFO EMPTY interrupt
				WRITE_PERI_REG(UART_INT_CLR(uart->uart_nr), UART_TXFIFO_EMPTY_INT_CLR);
				SET_PERI_REG_MASK(UART_INT_ENA(uart->uart_nr), UART_TXFIFO_EMPTY_INT_ENA);
			}
		}

		// Write any remaining data into transmit buffer
		if(uart->tx_buffer != nullptr) {
			while(written < size && uart->tx_buffer->writeChar(buf[written])) {
				++written;
			}
		}

		notify(uart, UART_NOTIFY_AFTER_WRITE);

		if(!bitRead(uart->options, UART_OPT_TXWAIT)) {
			break;
		}
	}

	return written;
}

size_t smg_uart_tx_free(smg_uart_t* uart)
{
	if(!smg_uart_tx_enabled(uart)) {
		return 0;
	}

	smg_uart_disable_interrupts();

	size_t space = is_physical(uart) ? uart_txfifo_free(uart->uart_nr) : 0;
	if(uart->tx_buffer != nullptr) {
		space += uart->tx_buffer->getFreeSpace();
	}

	smg_uart_restore_interrupts();

	return space;
}

void smg_uart_wait_tx_empty(smg_uart_t* uart)
{
	if(!smg_uart_tx_enabled(uart)) {
		return;
	}

	notify(uart, UART_NOTIFY_WAIT_TX);

	if(uart->tx_buffer != nullptr) {
		while(!uart->tx_buffer->isEmpty()) {
			system_soft_wdt_feed();
		}
	}

	if(is_physical(uart)) {
		while(uart_txfifo_count(uart->uart_nr) != 0)
			system_soft_wdt_feed();
	}
}

void smg_uart_set_break(smg_uart_t* uart, bool state)
{
	uart = get_physical(uart);
	if(uart != nullptr) {
		if(state) {
			SET_PERI_REG_MASK(UART_CONF0(uart->uart_nr), UART_TXD_BRK);
		} else {
			CLEAR_PERI_REG_MASK(UART_CONF0(uart->uart_nr), UART_TXD_BRK);
		}
	}
}

uint8_t smg_uart_get_status(smg_uart_t* uart)
{
	uint8_t status = 0;
	if(uart != nullptr) {
		smg_uart_disable_interrupts();
		// Get break/overflow flags from actual uart (physical or otherwise)
		status = uart->status & (UART_BRK_DET_INT_ST | UART_RXFIFO_OVF_INT_ST);
		uart->status = 0;
		// Read raw status register directly from real uart, masking out non-error bits
		uart = get_physical(uart);
		if(uart != nullptr) {
			uint32_t intraw = READ_PERI_REG(UART_INT_RAW(uart->uart_nr));
			intraw &= UART_BRK_DET_INT_ST | UART_RXFIFO_OVF_INT_ST | UART_FRM_ERR_INT_ST | UART_PARITY_ERR_INT_ST;
			status |= intraw;
			// Clear errors
			WRITE_PERI_REG(UART_INT_CLR(uart->uart_nr), status);
		}
		smg_uart_restore_interrupts();
	}
	return status;
}

void smg_uart_flush(smg_uart_t* uart, smg_uart_mode_t mode)
{
	if(uart == nullptr) {
		return;
	}

	bool flushRx = mode != UART_TX_ONLY && uart->mode != UART_TX_ONLY;
	bool flushTx = mode != UART_RX_ONLY && uart->mode != UART_RX_ONLY;

	smg_uart_disable_interrupts();
	if(flushRx && uart->rx_buffer != nullptr) {
		uart->rx_buffer->clear();
	}

	if(flushTx && uart->tx_buffer != nullptr) {
		uart->tx_buffer->clear();
	}

	if(is_physical(uart)) {
		// Clear the hardware FIFOs
		uint32_t flushBits = 0;
		if(flushTx) {
			flushBits |= UART_TXFIFO_RST;
		}
		if(flushRx) {
			flushBits |= UART_RXFIFO_RST;
		}
		SET_PERI_REG_MASK(UART_CONF0(uart->uart_nr), flushBits);
		CLEAR_PERI_REG_MASK(UART_CONF0(uart->uart_nr), flushBits);

		if(flushTx) {
			// Prevent TX FIFO EMPTY interrupts - don't need them until uart_write is called again
			CLEAR_PERI_REG_MASK(UART_INT_ENA(uart->uart_nr), UART_TXFIFO_EMPTY_INT_ENA);
		}

		// If receive overflow occurred then these interrupts will be masked
		if(flushRx) {
			WRITE_PERI_REG(UART_INT_CLR(uart->uart_nr), ~UART_TXFIFO_EMPTY_INT_CLR);
			SET_PERI_REG_MASK(UART_INT_ENA(uart->uart_nr),
							  UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA | UART_RXFIFO_OVF_INT_ENA);
		}
	}

	smg_uart_restore_interrupts();
}

uint32_t smg_uart_set_baudrate_reg(int uart_nr, uint32_t baud_rate)
{
	if(!is_physical(uart_nr) || baud_rate == 0) {
		return 0;
	}

	uint32_t clkdiv = UART_CLK_FREQ / baud_rate;
	WRITE_PERI_REG(UART_CLKDIV(uart_nr), clkdiv);
	// Return the actual baud rate in use
	baud_rate = clkdiv ? UART_CLK_FREQ / clkdiv : 0;
	return baud_rate;
}

uint32_t smg_uart_set_baudrate(smg_uart_t* uart, uint32_t baud_rate)
{
	uart = get_physical(uart);
	if(uart == nullptr) {
		return 0;
	}

	baud_rate = smg_uart_set_baudrate_reg(uart->uart_nr, baud_rate);
	// Store the actual baud rate in use
	uart->baud_rate = baud_rate;
	return baud_rate;
}

uint32_t smg_uart_get_baudrate(smg_uart_t* uart)
{
	uart = get_physical(uart);
	return (uart == nullptr) ? 0 : uart->baud_rate;
}

smg_uart_t* smg_uart_init_ex(const smg_uart_config_t& cfg)
{
	// Already initialised?
	if(smg_uart_get_uart(cfg.uart_nr) != nullptr) {
		return nullptr;
	}

	auto uart = new smg_uart_t;
	if(uart == nullptr) {
		return nullptr;
	}

	memset(uart, 0, sizeof(smg_uart_t));
	uart->uart_nr = cfg.uart_nr;
	uart->mode = cfg.mode;
	uart->options = cfg.options;
	uart->tx_pin = UART_PIN_DEFAULT;
	uart->rx_pin = UART_PIN_DEFAULT;
	uart->rx_headroom = DEFAULT_RX_HEADROOM;

	auto rxBufferSize = cfg.rx_size;
	auto txBufferSize = cfg.tx_size;

	switch(cfg.uart_nr) {
	case UART0:
	case UART2:
		// Virtual uart requires a minimum RAM buffer
		if(cfg.uart_nr == UART2) {
			rxBufferSize += UART_RX_FIFO_SIZE;
			txBufferSize += UART_TX_FIFO_SIZE;
		}

		if(smg_uart_rx_enabled(uart) && !smg_uart_realloc_buffer(uart->rx_buffer, rxBufferSize)) {
			delete uart;
			return nullptr;
		}

		if(smg_uart_tx_enabled(uart) && !smg_uart_realloc_buffer(uart->tx_buffer, txBufferSize)) {
			delete uart->rx_buffer;
			delete uart;
			return nullptr;
		}

		if(cfg.uart_nr == UART2) {
			break;
		}

		// OK, buffers allocated so setup hardware
		smg_uart_detach(cfg.uart_nr);

		if(smg_uart_rx_enabled(uart)) {
			uart->rx_pin = 3;
			uart0_pin_select(uart->rx_pin);
		}

		if(smg_uart_tx_enabled(uart)) {
			uart->tx_pin = (cfg.tx_pin == 2) ? 2 : 1;
			uart0_pin_select(uart->tx_pin);
		}

		CLEAR_PERI_REG_MASK(UART_SWAP_REG, UART_SWAP0);

		WRITE_PERI_REG(UART_CONF0(UART0), cfg.format);
		break;

	case UART1:
		// Note: uart_interrupt_handler does not support RX on UART 1
		if(uart->mode == UART_RX_ONLY) {
			delete uart;
			return nullptr;
		}
		uart->mode = UART_TX_ONLY;

		// Transmit buffer optional
		if(!smg_uart_realloc_buffer(uart->tx_buffer, txBufferSize)) {
			delete uart;
			return nullptr;
		}

		// Setup hardware
		smg_uart_detach(cfg.uart_nr);
		uart->tx_pin = 2;
		uart1_pin_select(uart->tx_pin);
		WRITE_PERI_REG(UART_CONF0(UART1), cfg.format);
		break;

	default:
		// big fail!
		delete uart;
		return nullptr;
	}

	smg_uart_set_baudrate(uart, cfg.baudrate);
	smg_uart_flush(uart);
	uartInstances[cfg.uart_nr] = uart;
	smg_uart_start_isr(uart);

	notify(uart, UART_NOTIFY_AFTER_OPEN);

	return uart;
}

void smg_uart_uninit(smg_uart_t* uart)
{
	if(uart == nullptr) {
		return;
	}

	notify(uart, UART_NOTIFY_BEFORE_CLOSE);

	smg_uart_stop_isr(uart);
	// If debug output being sent to this UART, disable it
	if(uart->uart_nr == s_uart_debug_nr) {
		smg_uart_set_debug(UART_NO);
	}

	switch(uart->uart_nr) {
	case UART0:
		uart0_pin_restore(uart->rx_pin);
		uart0_pin_restore(uart->tx_pin);
		break;
	case UART1:
		uart1_pin_restore(uart->tx_pin);
		break;
	}

	delete uart->rx_buffer;
	delete uart->tx_buffer;
	delete uart;
}

void smg_uart_set_format(smg_uart_t* uart, smg_uart_format_t format)
{
	uart = get_physical(uart);
	if(uart != nullptr) {
		SET_PERI_REG_BITS(UART_CONF0(uart->uart_nr), 0xff, format, 0);
	}
}

bool smg_uart_intr_config(smg_uart_t* uart, const smg_uart_intr_config_t* config)
{
	uart = get_physical(uart);
	if(uart == nullptr || config == nullptr) {
		return false;
	}

	uint32_t conf1{0};
	if(smg_uart_rx_enabled(uart)) {
		if(uart->rx_buffer == nullptr) {
			// Setting this to 0 results in lockup as the interrupt never clears
			uint8_t rxfifo_full_thresh = TRange(1, UART_RXFIFO_FULL_THRHD).clip(config->rxfifo_full_thresh);
			conf1 |= rxfifo_full_thresh << UART_RXFIFO_FULL_THRHD_S;
		} else {
			conf1 |= RX_FIFO_FULL_THRESHOLD << UART_RXFIFO_FULL_THRHD_S;
		}
		uint8_t rx_timeout_thresh = TRange(0, UART_RX_TOUT_THRHD).clip(config->rx_timeout_thresh);
		conf1 |= rx_timeout_thresh << UART_RX_TOUT_THRHD_S;
		conf1 |= UART_RX_TOUT_EN;
	}

	if(smg_uart_tx_enabled(uart)) {
		uint8_t txfifo_empty_intr_thresh = TRange(0, UART_TXFIFO_EMPTY_THRHD).clip(config->txfifo_empty_intr_thresh);
		conf1 |= txfifo_empty_intr_thresh << UART_TXFIFO_EMPTY_THRHD_S;
	}

	WRITE_PERI_REG(UART_CONF1(uart->uart_nr), conf1);
	return true;
}

void smg_uart_swap(smg_uart_t* uart, int tx_pin)
{
	if(uart == nullptr) {
		return;
	}

	switch(uart->uart_nr) {
	case UART0:
		uart0_pin_restore(uart->tx_pin);
		uart0_pin_restore(uart->rx_pin);

		if(uart->tx_pin == 1 || uart->tx_pin == 2 || uart->rx_pin == 3) {
			if(smg_uart_tx_enabled(uart)) {
				uart->tx_pin = 15;
			}

			if(smg_uart_rx_enabled(uart)) {
				uart->rx_pin = 13;
			}

			SET_PERI_REG_MASK(UART_SWAP_REG, UART_SWAP0);
		} else {
			if(smg_uart_tx_enabled(uart)) {
				uart->tx_pin = (tx_pin == 2) ? 2 : 1;
			}

			if(smg_uart_rx_enabled(uart)) {
				uart->rx_pin = 3;
			}

			CLEAR_PERI_REG_MASK(UART_SWAP_REG, UART_SWAP0);
		}

		uart0_pin_select(uart->tx_pin);
		uart0_pin_select(uart->rx_pin);
		break;

	case UART1:
		// Currently no swap possible! See GPIO pins used by UART
		break;

	default:
		break;
	}
}

bool smg_uart_set_tx(smg_uart_t* uart, int tx_pin)
{
	if(uart != nullptr && uart->uart_nr == UART0 && smg_uart_tx_enabled(uart)) {
		uart0_pin_restore(uart->tx_pin);
		uart->tx_pin = (tx_pin == 2) ? 2 : 1;
		uart0_pin_select(uart->tx_pin);
		return true;
	}

	// All other combinations, e.g. GPIO7 as TX not possible! See GPIO pins used by UART
	return false;
}

bool smg_uart_set_pins(smg_uart_t* uart, int tx_pin, int rx_pin)
{
	if(uart == nullptr) {
		return false;
	}

	// Only UART0 allows pin changes
	if(uart->uart_nr != UART0) {
		return false;
	}

	bool res{true};

	if(smg_uart_tx_enabled(uart) && uart->tx_pin != tx_pin) {
		if(rx_pin == 13 && tx_pin == 15) {
			smg_uart_swap(uart, 15);
		} else if(rx_pin == 3 && (tx_pin == 1 || tx_pin == 2)) {
			if(uart->rx_pin != rx_pin) {
				smg_uart_swap(uart, tx_pin);
			} else {
				smg_uart_set_tx(uart, tx_pin);
			}
		} else {
			res = false;
		}
	}

	if(smg_uart_rx_enabled(uart) && uart->rx_pin != rx_pin) {
		if(rx_pin == 13 && tx_pin == 15) {
			smg_uart_swap(uart, 15);
		} else {
			res = false;
		}
	}

	return res;
}

void smg_uart_debug_putc(char c)
{
	smg_uart_t* uart = smg_uart_get_uart(s_uart_debug_nr);
	if(uart != nullptr) {
		smg_uart_write_char(uart, c);
	}
}

void smg_uart_set_debug(int uart_nr)
{
	s_uart_debug_nr = uart_nr;
	system_set_os_print(uart_nr >= 0);
	ets_install_putc1(smg_uart_debug_putc);
}

int smg_uart_get_debug()
{
	return s_uart_debug_nr;
}

void smg_uart_detach(int uart_nr)
{
	if(!is_physical(uart_nr)) {
		return;
	}

	smg_uart_disable_interrupts();
	bitClear(isrMask, uart_nr);
	WRITE_PERI_REG(UART_CONF1(uart_nr), 0);
	WRITE_PERI_REG(UART_INT_CLR(uart_nr), 0xffff);
	WRITE_PERI_REG(UART_INT_ENA(uart_nr), 0);
	smg_uart_restore_interrupts();
}

void smg_uart_detach_all()
{
	smg_uart_disable_interrupts();
	for(unsigned uart_nr = 0; uart_nr < UART_PHYSICAL_COUNT; ++uart_nr) {
		WRITE_PERI_REG(UART_CONF1(uart_nr), 0);
		WRITE_PERI_REG(UART_INT_CLR(uart_nr), 0xffff);
		WRITE_PERI_REG(UART_INT_ENA(uart_nr), 0);
	}
	isrMask = 0;
}
