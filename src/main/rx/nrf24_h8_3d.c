/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

// This file borrows heavily from project Deviation,
// see http://deviationtx.com

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>
#include "build_config.h"

#ifdef USE_RX_H8_3D

#include "drivers/rx_nrf24l01.h"
#include "drivers/rx_xn297.h"
#include "drivers/system.h"

#include "rx/nrf24.h"
#include "rx/nrf24_h8_3d.h"


/*
 * Deviation transmitter sends 345 bind packets, then starts sending data packets.
 * Packets are send at rate of at least one every 4 milliseconds, ie at least 250Hz.
 * This means binding phase lasts 1.4 seconds, the transmitter then enters the data phase.
 * Other transmitters may vary but should have similar characteristics.
 */


/*
 * H8_3D Protocol
 * No auto acknowledgment
 * Payload size is 20, static
 * Data rate is 1Kbps
 * Bind Phase
 * uses address {0xab,0xac,0xad,0xae,0xaf}, converted by XN297 to {0x41, 0xbd, 0x42, 0xd4, 0xc2}
 * hops between 4 channels
 * Data Phase
 * uses same address as bind phase
 * hops between 4 channels generated from txId received in bind packets
 *
 */
#define RC_CHANNEL_COUNT    14

#define FLAG_FLIP       0x01
#define FLAG_RATE_MID   0x02
#define FLAG_RATE_HIGH  0x04
#define FLAG_HEADLESS   0x10 // RTH + headless on H8, headless on JJRC H20
#define FLAG_RTH        0x20 // 360° flip mode on H8 3D, RTH on JJRC H20
#define FLAG_PICTURE    0x40 // on payload[18]
#define FLAG_VIDEO      0x80 // on payload[18]
#define FLAG_CAMERA_UP  0x04 // on payload[18]
#define FLAG_CAMERA_DOWN 0x08 // on payload[18]

typedef enum {
    STATE_BIND = 0,
    STATE_DATA
} protocol_state_t;

STATIC_UNIT_TESTED protocol_state_t protocolState;

#define H8_3D_PROTOCOL_PAYLOAD_SIZE   20
STATIC_UNIT_TESTED uint8_t payloadSize;

#define CRC_LEN 2
#define RX_TX_ADDR_LEN     5
//STATIC_UNIT_TESTED uint8_t rxTxAddr[RX_TX_ADDR_LEN] = {0xc4, 0x57, 0x09, 0x65, 0x21};
STATIC_UNIT_TESTED uint8_t rxTxAddrXN297[RX_TX_ADDR_LEN] = {0x41, 0xbd, 0x42, 0xd4, 0xc2}; // converted XN297 address
#define TX_ID_LEN 4
STATIC_UNIT_TESTED uint8_t txId[TX_ID_LEN];

// radio channels for frequency hopping
#define H8_3D_RF_CHANNEL_COUNT 4
STATIC_UNIT_TESTED uint8_t h8_3dRfChannelCount = H8_3D_RF_CHANNEL_COUNT;
STATIC_UNIT_TESTED uint8_t h8_3dRfChannelIndex;
STATIC_UNIT_TESTED uint8_t h8_3dRfChannels[H8_3D_RF_CHANNEL_COUNT];
#define H8_3D_RF_BIND_CHANNEL_START 0x06
#define H8_3D_RF_BIND_CHANNEL_END 0x26

#define DATA_HOP_TIMEOUT 5000 // 5ms
#define BIND_HOP_TIMEOUT 1000 // 1ms, to find the bind channel as quickly as possible
static uint32_t hopTimeout = BIND_HOP_TIMEOUT;
static uint32_t timeOfLastHop;

STATIC_UNIT_TESTED bool h8_3dCheckBindPacket(const uint8_t *payload)
{
    bool bindPacket = false;
    if ((payload[5] == 0x00) && (payload[6] == 0x00) && (payload[7] == 0x01)) {
        const uint32_t checkSumTxId = (payload[1] + payload[2] + payload[3] + payload[4]) & 0xff;
        if (checkSumTxId == payload[8]) {
            bindPacket = true;
            txId[0] = payload[1];
            txId[1] = payload[2];
            txId[2] = payload[3];
            txId[3] = payload[4];
        }
    }
    return bindPacket;
}

STATIC_UNIT_TESTED uint16_t h8_3dConvertToPwm(uint8_t val, int16_t _min, int16_t _max)
{
#define PWM_RANGE (PWM_RANGE_MAX - PWM_RANGE_MIN)

    int32_t ret = val;
    const int32_t range = _max - _min;
    ret = PWM_RANGE_MIN + ((ret - _min) * PWM_RANGE)/range;
    return (uint16_t)ret;
}

void h8_3dSetRcDataFromPayload(uint16_t *rcData, const uint8_t *payload)
{
    rcData[NRF24_ROLL] = h8_3dConvertToPwm(payload[12], 0xbb, 0x43); // aileron
    rcData[NRF24_PITCH] = h8_3dConvertToPwm(payload[11], 0x43, 0xbb); // elevator
    rcData[NRF24_THROTTLE] = h8_3dConvertToPwm(payload[9], 0, 0xff); // throttle
    const int8_t yawByte = payload[10]; // rudder
    rcData[NRF24_YAW] = yawByte >= 0 ? h8_3dConvertToPwm(yawByte, -0x3c, 0x3c) : h8_3dConvertToPwm(yawByte, 0xbc, 0x44);

    const uint8_t flags = payload[17];
    const uint8_t flags2 = payload[18];
    if (flags & FLAG_RATE_HIGH) {
        rcData[RC_CHANNEL_RATE] = PWM_RANGE_MAX;
    } else if (flags & FLAG_RATE_MID) {
        rcData[RC_CHANNEL_RATE] = PWM_RANGE_MIDDLE;
    } else {
        rcData[RC_CHANNEL_RATE] = PWM_RANGE_MIN;
    }

    rcData[RC_CHANNEL_FLIP] = flags & FLAG_FLIP ? PWM_RANGE_MAX : PWM_RANGE_MIN;
    rcData[RC_CHANNEL_PICTURE] = flags2 & FLAG_PICTURE ? PWM_RANGE_MAX : PWM_RANGE_MIN;
    rcData[RC_CHANNEL_VIDEO] = flags2 & FLAG_VIDEO ? PWM_RANGE_MAX : PWM_RANGE_MIN;
    rcData[RC_CHANNEL_HEADLESS] = flags & FLAG_HEADLESS ? PWM_RANGE_MAX : PWM_RANGE_MIN;
    rcData[RC_CHANNEL_RTH] = flags & FLAG_RTH ? PWM_RANGE_MAX : PWM_RANGE_MIN;

    if (flags2 & FLAG_CAMERA_UP) {
        rcData[NRF24_AUX7] = PWM_RANGE_MAX;
    } else if (flags2 & FLAG_CAMERA_DOWN) {
        rcData[NRF24_AUX7] = PWM_RANGE_MIN;
    } else {
        rcData[NRF24_AUX7] = PWM_RANGE_MIDDLE;
    }
    rcData[NRF24_AUX8] = h8_3dConvertToPwm(payload[14], 0x10, 0x30);
    rcData[NRF24_AUX9] = h8_3dConvertToPwm(payload[15], 0x30, 0x10);
    rcData[NRF24_AUX10] = h8_3dConvertToPwm(payload[16], 0x10, 0x30);
}

static void h8_3dHopToNextChannel(void)
{
    ++h8_3dRfChannelIndex;
    if (protocolState == STATE_BIND) {
        if (h8_3dRfChannelIndex > H8_3D_RF_BIND_CHANNEL_END) {
            h8_3dRfChannelIndex = H8_3D_RF_BIND_CHANNEL_START;
        }
        NRF24L01_SetChannel(h8_3dRfChannelIndex);
    } else {
        if (h8_3dRfChannelIndex >= h8_3dRfChannelCount) {
            h8_3dRfChannelIndex = 0;
        }
        NRF24L01_SetChannel(h8_3dRfChannels[h8_3dRfChannelIndex]);
    }
}

// The hopping channels are determined by the txId
void h8_3dSetHoppingChannels(const uint8_t* txId)
{
#ifdef XXX
    for (int ii = 0; ii < H8_3D_RF_CHANNEL_COUNT; ++ii) {
        h8_3dRfChannels[ii] = 0x06 + (0x0f * ii) + ((txId[ii] >> 4) + (txId[ii] & 0x0f)) % 0x0f;
    }
#else
    h8_3dRfChannels[0] = 0x06 + ((txId[0]>>4) + (txId[0] & 0x0f)) % 0x0f;
    h8_3dRfChannels[1] = 0x15 + ((txId[1]>>4) + (txId[1] & 0x0f)) % 0x0f;
    h8_3dRfChannels[2] = 0x24 + ((txId[2]>>4) + (txId[2] & 0x0f)) % 0x0f;
    h8_3dRfChannels[3] = 0x33 + ((txId[3]>>4) + (txId[3] & 0x0f)) % 0x0f;
#endif
}

void h8_3dSetBound(const uint8_t* txId)
{
    protocolState = STATE_DATA;
    h8_3dSetHoppingChannels(txId);
    hopTimeout = DATA_HOP_TIMEOUT;
    timeOfLastHop = micros();
    h8_3dRfChannelIndex = 0;
    NRF24L01_SetChannel(h8_3dRfChannels[0]);
}

bool crcOK(uint16_t crc, const uint8_t *payload)
{
    if (payload[payloadSize - CRC_LEN] != (crc >> 8)) {
        return false;
    }
    if (payload[payloadSize - CRC_LEN + 1] != (crc & 0xff)) {
        return false;
    }
    return true;
}

/*
 * This is called periodically by the scheduler.
 * Returns NRF24L01_RECEIVED_DATA if a data packet was received.
 */
nrf24_received_t h8_3dDataReceived(uint8_t *payload)
{
    nrf24_received_t ret = NRF24_RECEIVED_NONE;
    switch (protocolState) {
    case STATE_BIND:
        if (NRF24L01_ReadPayloadIfAvailable(payload, payloadSize)) {
            const uint16_t crc = XN297_UnscramblePayload(payload, payloadSize - CRC_LEN, rxTxAddrXN297);
            if (crcOK(crc, payload)) {
                const bool bindPacket = h8_3dCheckBindPacket(payload);
                if (bindPacket) {
                    ret = NRF24_RECEIVED_BIND;
                    h8_3dSetBound(txId);
                }
            }
        }
        break;
    case STATE_DATA:
        // read the payload, processing of payload is deferred
        if (NRF24L01_ReadPayloadIfAvailable(payload, payloadSize)) {
            const uint16_t crc = XN297_UnscramblePayload(payload, payloadSize - CRC_LEN, rxTxAddrXN297);
            if (crcOK(crc, payload)) {
                ret = NRF24_RECEIVED_DATA;
            }
        }
        break;
    }
    const uint32_t timeNowUs = micros();
    if ((ret == NRF24_RECEIVED_DATA) || (timeNowUs > timeOfLastHop + hopTimeout)) {
        h8_3dHopToNextChannel();
        timeOfLastHop = timeNowUs;
    }
    return ret;
}

void h8_3dNrf24Init(nrf24_protocol_t protocol, uint32_t nrf24_id)
{
    UNUSED(protocol);
    protocolState = STATE_BIND;

    NRF24L01_Initialize(0); // sets PWR_UP, no CRC - hardware CRC not used for XN297
    NRF24L01_Setup();

    NRF24L01_WriteReg(NRF24L01_06_RF_SETUP, NRF24L01_06_RF_SETUP_RF_DR_1Mbps | NRF24L01_06_RF_SETUP_RF_PWR_n12dbm);
    // RX_ADDR for pipes P1-P5 are left at default values
    NRF24L01_WriteRegisterMulti(NRF24L01_0A_RX_ADDR_P0, rxTxAddrXN297, RX_TX_ADDR_LEN);
    if (nrf24_id == 0) {
        h8_3dRfChannelIndex = H8_3D_RF_BIND_CHANNEL_START;
        NRF24L01_SetChannel(H8_3D_RF_BIND_CHANNEL_START);
    } else {
        h8_3dSetBound((uint8_t*)&nrf24_id);
    }

    payloadSize = H8_3D_PROTOCOL_PAYLOAD_SIZE + CRC_LEN; // payload + 2 bytes CRC
    NRF24L01_WriteReg(NRF24L01_11_RX_PW_P0, payloadSize); // payload + 2 bytes CRC

    NRF24L01_SetRxMode(); // enter receive mode to start listening for packets
}

void h8_3dInit(const rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig)
{
    rxRuntimeConfig->channelCount = RC_CHANNEL_COUNT;
    h8_3dNrf24Init((nrf24_protocol_t)rxConfig->nrf24rx_protocol, 0);
}
#endif
