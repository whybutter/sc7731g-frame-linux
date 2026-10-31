// SPDX-License-Identifier: MIT
// bt_raw_c3.h -- the C3 as a known BLE transmitter and a raw scanner for the
// VT10101's raw-radio LE experiment (docs/bluetooth.md bt4).
//
// ESPHome only builds and boots it; esp32_ble brings up the controller and
// Bluedroid, then bt_raw_start() takes the GAP callback over (the config has
// no other BLE component that needs it) and drives advertising and scanning
// directly, so every byte on air is ours:
// - advertising: static random address C3:BE:12:00:00:01, ADV_NONCONN_IND,
//   channel 37 only, every 100 ms, payload 02 01 06 | 08 FF FF FF 'B' 'E'
//   'R' 'Y' 'L' -- fixed, so its 64 on-air bits after the preamble are known;
// - scanning (passive, all channels): every advert that looks like the
//   Timemore scale (OUI C8:47:8C, "TIMEMORE" in it, or company 0xFFFF) is
//   logged raw: address, address type, event type, RSSI and the AD bytes.
#pragma once
#include <cstring>
#include "esp_gap_ble_api.h"
#include "esp_log.h"

namespace bt_raw {

static const char *TAG = "btraw";
static const uint8_t RAND_ADDR[6] = {0xC3, 0xBE, 0x12, 0x00, 0x00, 0x01};
static uint8_t ADV[] = {0x02, 0x01, 0x06,
                        0x08, 0xFF, 0xFF, 0xFF, 'B', 'E', 'R', 'Y', 'L'};
static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0xA0,  // 100 ms, the legacy minimum for non-connectable
    .adv_int_max = 0xA0,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_37,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};
static esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};
static uint32_t adv_count, seen;
static bool adv_enabled = true;

static bool interesting(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param &r) {
  if (r.bda[0] == 0xC8 && r.bda[1] == 0x47 && r.bda[2] == 0x8C) return true;
  const uint8_t *d = r.ble_adv;
  int n = r.adv_data_len + r.scan_rsp_len;
  for (int i = 0; i + 8 <= n; i++)
    if (!memcmp(d + i, "TIMEMORE", 8)) return true;
  for (int i = 0; i + 2 < n;) {  // AD structures: len, type, data
    uint8_t len = d[i];
    if (!len || i + len >= n + 1) break;
    if (d[i + 1] == 0xFF && len >= 3 && d[i + 2] == 0xFF && d[i + 3] == 0xFF) return true;
    i += len + 1;
  }
  return false;
}

static void gap_cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p) {
  switch (ev) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      if (adv_enabled)
        esp_ble_gap_start_advertising(&adv_params);
      break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      ESP_LOGI(TAG, "advertising %s: C3:BE:12:00:00:01 nonconn ch37 100ms",
               p->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? "on" : "FAILED");
      break;
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      esp_ble_gap_start_scanning(0);
      break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      ESP_LOGI(TAG, "scanning %s", p->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? "on" : "FAILED");
      break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      auto &r = p->scan_rst;
      if (r.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || !interesting(r)) break;
      char hex[2 * 62 + 1];
      int n = r.adv_data_len + r.scan_rsp_len;
      for (int i = 0; i < n && i < 62; i++) sprintf(hex + 2 * i, "%02x", r.ble_adv[i]);
      hex[2 * (n < 62 ? n : 62)] = 0;
      ESP_LOGI(TAG, "scale? %02X:%02X:%02X:%02X:%02X:%02X type=%d evt=%d rssi=%d adv=%d rsp=%d %s",
               r.bda[0], r.bda[1], r.bda[2], r.bda[3], r.bda[4], r.bda[5], r.ble_addr_type,
               r.ble_evt_type, r.rssi, r.adv_data_len, r.scan_rsp_len, hex);
      seen++;
      break;
    }
    default:
      break;
  }
}

// the web server's "advertising" switch: the on/off control for the frame's
// hit test (a word that moves only while this is on is the correlator)
inline void set_advertising(bool on) {
  adv_enabled = on;
  if (on)
    esp_ble_gap_start_advertising(&adv_params);
  else
    esp_ble_gap_stop_advertising();
  ESP_LOGI(TAG, "advertising %s (switch)", on ? "on" : "off");
}

inline void start() {
  esp_ble_gap_register_callback(gap_cb);
  esp_ble_gap_set_rand_addr((uint8_t *) RAND_ADDR);
  esp_ble_gap_config_adv_data_raw(ADV, sizeof(ADV));
  esp_ble_gap_set_scan_params(&scan_params);
}

}  // namespace bt_raw
