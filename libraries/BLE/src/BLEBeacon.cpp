/*
 * BLEBeacon.cpp
 *
 *  Created on: Jan 4, 2018
 *      Author: kolban
 *
 *  Modified on: Feb 18, 2025
 *      Author: lucasssvaz (based on kolban's and h2zero's work)
 *      Description: Added support for NimBLE
 */

#include "soc/soc_caps.h"
#if SOC_BLE_SUPPORTED

#include "sdkconfig.h"
#if defined(CONFIG_BLUEDROID_ENABLED) || defined(CONFIG_NIMBLE_ENABLED)

/***************************************************************************
 *                       Common includes and definitions                   *
 ***************************************************************************/

#include "BLEBeacon.h"
#include "esp32-hal-log.h"

#define ENDIAN_CHANGE_U16(x) ((((x) & 0xFF00) >> 8) + (((x) & 0xFF) << 8))

/***************************************************************************
 *                              Common functions                           *
 ***************************************************************************/

BLEBeacon::BLEBeacon() {
  m_beaconData.manufacturerId = 0x4c00;
  m_beaconData.subType = 0x02;
  m_beaconData.subTypeLength = 0x15;
  m_beaconData.major = 0;
  m_beaconData.minor = 0;
  m_beaconData.signalPower = 0;
  memset(m_beaconData.proximityUUID, 0, sizeof(m_beaconData.proximityUUID));
}

String BLEBeacon::getData() {
  return String((char *)&m_beaconData, sizeof(m_beaconData));
}

uint16_t BLEBeacon::getMajor() {
  return m_beaconData.major;
}

uint16_t BLEBeacon::getManufacturerId() {
  return m_beaconData.manufacturerId;
}

uint16_t BLEBeacon::getMinor() {
  return m_beaconData.minor;
}

BLEUUID BLEBeacon::getProximityUUID() {
  return BLEUUID(m_beaconData.proximityUUID, 16, true);
}

int8_t BLEBeacon::getSignalPower() {
  return m_beaconData.signalPower;
}

void BLEBeacon::setData(String data) {
  if (data.length() != sizeof(m_beaconData)) {
    log_e("Unable to set the data ... length passed in was %d and expected %d", data.length(), sizeof(m_beaconData));
    return;
  }
  memcpy(&m_beaconData, data.c_str(), sizeof(m_beaconData));
}

void BLEBeacon::setMajor(uint16_t major) {
  m_beaconData.major = ENDIAN_CHANGE_U16(major);
}

void BLEBeacon::setManufacturerId(uint16_t manufacturerId) {
  m_beaconData.manufacturerId = ENDIAN_CHANGE_U16(manufacturerId);
}

void BLEBeacon::setMinor(uint16_t minor) {
  m_beaconData.minor = ENDIAN_CHANGE_U16(minor);
}

void BLEBeacon::setSignalPower(int8_t signalPower) {
  m_beaconData.signalPower = signalPower;
}

void BLEBeacon::setProximityUUID(BLEUUID uuid) {
  uuid = uuid.to128();
#if defined(CONFIG_BLUEDROID_ENABLED)
  memcpy(m_beaconData.proximityUUID, uuid.getNative()->uuid.uuid128, 16);
#elif defined(CONFIG_NIMBLE_ENABLED)
  memcpy(m_beaconData.proximityUUID, uuid.getNative()->u128.value, 16);
#endif
}

#endif /* CONFIG_BLUEDROID_ENABLED || CONFIG_NIMBLE_ENABLED */
#endif /* SOC_BLE_SUPPORTED */
