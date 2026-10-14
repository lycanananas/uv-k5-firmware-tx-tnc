#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app/aprs_tx.h"
#include "bsp/dp32g030/dma.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "driver/uart.h"
#include "frequencies.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/ui.h"

#define APRS_LINE_MAX       160u
#define APRS_FRAME_MAX      180u
#define APRS_INFO_MAX       128u
#define APRS_PATH_MAX         2u
#define APRS_TXDELAY_FLAGS   40u
#define APRS_TAIL_FLAGS       3u
#define APRS_BIT_CYCLES   40000u
#define APRS_TX_LIMIT_CYCLES (48000000u * 30u)
#define APRS_MARK             12389u
#define APRS_SPACE            22714u
#define APRS_TONE_GAIN           66u
#define APRS_VHF_MIN       14400000u
#define APRS_VHF_MAX       14600000u
#define APRS_UHF_MIN       43000000u
#define APRS_UHF_MAX       44000000u
#define APRS_EEPROM_ADDRESS     0x1FF8u
#define APRS_BUSY_RSSI             50u
#define APRS_BUSY_SAMPLE_MS       100u
#define APRS_BUSY_TIMEOUT_MS    10000u

#define SYST_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYST_VAL  (*(volatile uint32_t *)0xE000E018u)

typedef struct {
	uint32_t previous;
	uint32_t elapsed;
	uint32_t next;
	bool space;
	uint8_t ones;
} APRS_Clock_t;

static uint8_t gUARTReadIndex;
static char gLine[APRS_LINE_MAX];
static uint8_t gLineLength;
static bool gDiscardLine;
static const char *gStatus = "READY";

static bool APRS_IsFrequencyAllowed(uint32_t frequency)
{
	return (frequency >= APRS_VHF_MIN && frequency <= APRS_VHF_MAX) ||
		       (frequency >= APRS_UHF_MIN && frequency <= APRS_UHF_MAX);
}

static void APRS_FormatFrequency(uint32_t frequency, char *text)
{
	const uint32_t megahertz = frequency / 100000u;
	const uint32_t kilohertz = (frequency % 100000u) / 100u;

	text[0] = (char)('0' + (megahertz / 100u));
	text[1] = (char)('0' + ((megahertz / 10u) % 10u));
	text[2] = (char)('0' + (megahertz % 10u));
	text[3] = '.';
	text[4] = (char)('0' + (kilohertz / 100u));
	text[5] = (char)('0' + ((kilohertz / 10u) % 10u));
	text[6] = (char)('0' + (kilohertz % 10u));
	text[7] = '\0';
}

static void APRS_Display(void)
{
	char frequency[8];

	APRS_FormatFrequency(gTxVfo->pTX->Frequency, frequency);
	UI_DisplayClear();
	memset(gStatusLine, 0, sizeof(gStatusLine));
	UI_PrintStringSmallBold("APRS TX TNC", 0, 0, 0);
	UI_DisplayFrequency(frequency, 16, 2, false);
	UI_PrintStringSmallNormal(gStatus, 0, 0, 6);
	ST7565_BlitStatusLine();
	ST7565_BlitFullScreen();
}

static void APRS_SaveFrequency(uint32_t frequency)
{
	uint8_t record[8] = {
		'A', 'Q', 'R', 'G',
		(uint8_t)frequency,
		(uint8_t)(frequency >> 8),
		(uint8_t)(frequency >> 16),
		(uint8_t)(frequency >> 24)
	};

	EEPROM_WriteBuffer(APRS_EEPROM_ADDRESS, record);
}

static uint32_t APRS_LoadFrequency(void)
{
	uint8_t record[8];

	EEPROM_ReadBuffer(APRS_EEPROM_ADDRESS, record, sizeof(record));
	const uint32_t frequency = (uint32_t)record[4] |
		((uint32_t)record[5] << 8) |
		((uint32_t)record[6] << 16) |
		((uint32_t)record[7] << 24);
	if (record[0] == 'A' && record[1] == 'Q' && record[2] == 'R' && record[3] == 'G' && APRS_IsFrequencyAllowed(frequency))
		return frequency;

	return 14480000u;
}

static void APRS_SetFrequency(uint32_t frequency)
{
	if (!APRS_IsFrequencyAllowed(frequency)) {
		gStatus = "FREQ OUT OF RANGE";
		return;
	}

	gTxVfo->freq_config_RX.Frequency = frequency;
	gTxVfo->freq_config_TX.Frequency = frequency;
	gTxVfo->Band = FREQUENCY_GetBand(frequency);
	RADIO_ConfigureSquelchAndOutputPower(gTxVfo);
	gTxVfo->SquelchOpenRSSIThresh = 0;
	gTxVfo->SquelchCloseRSSIThresh = 0;
	gTxVfo->SquelchOpenNoiseThresh = 127;
	gTxVfo->SquelchCloseNoiseThresh = 127;
	gTxVfo->SquelchOpenGlitchThresh = 255;
	gTxVfo->SquelchCloseGlitchThresh = 255;
	APRS_SaveFrequency(frequency);
	gStatus = "READY";
	RADIO_SetupRegisters(true);
}

static void APRS_SetupVFO(void)
{
	gEeprom.TX_VFO = 0;
	gEeprom.RX_VFO = 0;
	gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
	gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
	RADIO_SelectVfos();

	gTxVfo->CHANNEL_SAVE = FREQ_CHANNEL_FIRST + BAND3_137MHz;
	gTxVfo->pRX = &gTxVfo->freq_config_RX;
	gTxVfo->pTX = &gTxVfo->freq_config_TX;
	gTxVfo->TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
	gTxVfo->Modulation = MODULATION_FM;
	gTxVfo->freq_config_RX.CodeType = CODE_TYPE_OFF;
	gTxVfo->freq_config_TX.CodeType = CODE_TYPE_OFF;
	APRS_SetFrequency(APRS_LoadFrequency());
}

static uint8_t APRS_DMAWriteIndex(void)
{
	return (uint8_t)(DMA_CH0->ST & 0xFFu);
}

static uint16_t APRS_FCS(const uint8_t *data, uint8_t length)
{
	uint16_t crc = 0xFFFFu;

	for (uint8_t index = 0; index < length; index++) {
		crc ^= data[index];
		for (uint8_t bit = 0; bit < 8; bit++)
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
	}

	return crc ^ 0xFFFFu;
}

static bool APRS_EncodeAddress(const char *text, uint8_t length, bool last, uint8_t *address)
{
	uint8_t callLength = 0;
	uint8_t ssid = 0;
	uint8_t index = 0;

	while (index < length && text[index] != '-') {
		char character = text[index++];
		if (character >= 'a' && character <= 'z')
			character = (char)(character - ('a' - 'A'));
		if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) || callLength == 6u)
			return false;
		address[callLength++] = (uint8_t)(character << 1);
	}

	if (callLength == 0u)
		return false;
	while (callLength < 6u)
		address[callLength++] = (uint8_t)(' ' << 1);

	if (index < length) {
		index++;
		if (index == length)
			return false;
		while (index < length) {
			if (text[index] < '0' || text[index] > '9')
				return false;
			ssid = (uint8_t)(ssid * 10u + (uint8_t)(text[index++] - '0'));
			if (ssid > 15u)
				return false;
		}
	}

	address[6] = (uint8_t)(0x60u | (ssid << 1) | (last ? 1u : 0u));
	return true;
}

static bool APRS_BuildFrame(const char *line, uint8_t length, uint8_t *frame, uint8_t *frameLength)
{
	uint8_t separator = 0;
	uint8_t infoStart = 0;
	uint8_t addressEnd;
	uint8_t output;
	uint8_t pathCount = 0;

	while (separator < length && line[separator] != '>')
		separator++;
	if (separator == 0u || separator == length)
		return false;

	infoStart = (uint8_t)(separator + 1u);
	while (infoStart < length && line[infoStart] != ':')
		infoStart++;
	if (infoStart == separator + 1u || infoStart == length || length - infoStart - 1u > APRS_INFO_MAX)
		return false;

	addressEnd = (uint8_t)(separator + 1u);
	while (addressEnd < infoStart && line[addressEnd] != ',')
		addressEnd++;
	if (addressEnd == separator + 1u ||
		!APRS_EncodeAddress(line + separator + 1u, (uint8_t)(addressEnd - separator - 1u), false, frame) ||
		!APRS_EncodeAddress(line, separator, addressEnd == infoStart, frame + 7u))
		return false;

	output = 14u;
	while (addressEnd < infoStart) {
		const uint8_t pathStart = (uint8_t)(addressEnd + 1u);
		addressEnd = pathStart;
		while (addressEnd < infoStart && line[addressEnd] != ',')
			addressEnd++;
		if (addressEnd == pathStart || pathCount++ >= APRS_PATH_MAX ||
			!APRS_EncodeAddress(line + pathStart, (uint8_t)(addressEnd - pathStart), addressEnd == infoStart, frame + output))
			return false;
		output = (uint8_t)(output + 7u);
	}

	frame[output++] = 0x03u;
	frame[output++] = 0xF0u;
	memcpy(frame + output, line + infoStart + 1u, length - infoStart - 1u);
	output = (uint8_t)(output + length - infoStart - 1u);
	const uint16_t fcs = APRS_FCS(frame, output);
	frame[output++] = (uint8_t)fcs;
	frame[output++] = (uint8_t)(fcs >> 8);
	*frameLength = output;
	return true;
}

static bool APRS_ParseQRGCommand(const char *line, uint8_t length, uint32_t *frequency)
{
	uint32_t value = 0;

	if (length != 12u || memcmp(line, "!QRG ", 5u) != 0 || line[8] != '.')
		return false;
	for (uint8_t index = 5; index < length; index++) {
		if (index == 8u)
			continue;
		if (line[index] < '0' || line[index] > '9')
			return false;
		value = value * 10u + (uint8_t)(line[index] - '0');
	}

	*frequency = value * 100u;
	return true;
}

static void APRS_ClockStart(APRS_Clock_t *clock)
{
	clock->previous = SYST_VAL;
	clock->elapsed = 0;
	clock->next = APRS_BIT_CYCLES;
	clock->space = false;
	clock->ones = 0;
}

static uint32_t APRS_ClockNow(APRS_Clock_t *clock)
{
	const uint32_t value = SYST_VAL;
	clock->elapsed += value <= clock->previous ? clock->previous - value : clock->previous + SYST_LOAD + 1u - value;
	clock->previous = value;
	return clock->elapsed;
}

static void APRS_SetTone(bool space)
{
	BK4819_WriteRegister(BK4819_REG_71, space ? APRS_SPACE : APRS_MARK);
	BK4819_WriteRegister(BK4819_REG_70, (uint16_t)(BK4819_REG_70_ENABLE_TONE1 | (APRS_TONE_GAIN << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN)));
}

static bool APRS_SendBit(APRS_Clock_t *clock, uint8_t bit)
{
	while ((int32_t)(APRS_ClockNow(clock) - clock->next) < 0) {
		if (clock->elapsed >= APRS_TX_LIMIT_CYCLES)
			return false;
	}
	if (clock->elapsed >= APRS_TX_LIMIT_CYCLES)
		return false;
	if (!bit) {
		clock->space = !clock->space;
		APRS_SetTone(clock->space);
	}
	clock->next += APRS_BIT_CYCLES;
	return true;
}

static bool APRS_SendByte(APRS_Clock_t *clock, uint8_t value, bool stuff)
{
	for (uint8_t bit = 0; bit < 8; bit++) {
		const uint8_t valueBit = value & 1u;
		value >>= 1;
		if (!APRS_SendBit(clock, valueBit))
			return false;
		if (stuff) {
			if (!valueBit)
				clock->ones = 0;
			else if (++clock->ones == 5u) {
				if (!APRS_SendBit(clock, 0))
					return false;
				clock->ones = 0;
			}
		}
	}
	return true;
}

static void APRS_WaitForClearChannel(void)
{
	uint16_t busyFor = 0;
	bool busyDisplayed = false;

	while (busyFor < APRS_BUSY_TIMEOUT_MS) {
		if (BK4819_GetRSSI() < APRS_BUSY_RSSI)
			return;
		if (!busyDisplayed) {
			gStatus = "CHANNEL BUSY";
			APRS_Display();
			busyDisplayed = true;
		}
		SYSTEM_DelayMs(APRS_BUSY_SAMPLE_MS);
		busyFor += APRS_BUSY_SAMPLE_MS;
	}
}

static void APRS_Transmit(const uint8_t *frame, uint8_t length)
{
	APRS_Clock_t clock;
	bool sent = true;

	APRS_WaitForClearChannel();
	gStatus = "SENDING!!";
	APRS_Display();
	RADIO_SetTxParameters();
	BK4819_WriteRegister(BK4819_REG_51, 0);
	SYSTEM_DelayMs(50);
	APRS_SetTone(false);
	APRS_ClockStart(&clock);

	for (uint8_t index = 0; index < APRS_TXDELAY_FLAGS && sent; index++)
		sent = APRS_SendByte(&clock, 0x7Eu, false);
	for (uint8_t index = 0; index < length && sent; index++)
		sent = APRS_SendByte(&clock, frame[index], true);
	for (uint8_t index = 0; index < APRS_TAIL_FLAGS && sent; index++)
		sent = APRS_SendByte(&clock, 0x7Eu, false);

	BK4819_EnterTxMute();
	BK4819_WriteRegister(BK4819_REG_70, 0);
	RADIO_SetupRegisters(true);
	gStatus = sent ? "SENT" : "TX TIMEOUT";
	APRS_Display();
}

static void APRS_HandleLine(void)
{
	uint8_t frame[APRS_FRAME_MAX];
	uint8_t frameLength;
	uint32_t frequency;

	if (APRS_ParseQRGCommand(gLine, gLineLength, &frequency)) {
		APRS_SetFrequency(frequency);
		APRS_Display();
		return;
	}

	if (!APRS_BuildFrame(gLine, gLineLength, frame, &frameLength)) {
		gStatus = "INVALID FRAME";
		APRS_Display();
		return;
	}
	APRS_Transmit(frame, frameLength);
}

static void APRS_ReadUART(void)
{
	const uint8_t writeIndex = APRS_DMAWriteIndex();

	while (gUARTReadIndex != writeIndex) {
		const char character = (char)UART_DMA_Buffer[gUARTReadIndex++];
		if (character == '\r' || character == '\n') {
			if (gLineLength && !gDiscardLine)
				APRS_HandleLine();
			gLineLength = 0;
			gDiscardLine = false;
			continue;
		}
		if (gLineLength + 1u >= sizeof(gLine)) {
			gDiscardLine = true;
			continue;
		}
		if (!gDiscardLine)
			gLine[gLineLength++] = character;
	}
}

void APRS_TX_Run(void)
{
	APRS_SetupVFO();
	APRS_Display();

	while (true) {
		APRS_ReadUART();
		SYSTEM_DelayMs(10);
	}
}