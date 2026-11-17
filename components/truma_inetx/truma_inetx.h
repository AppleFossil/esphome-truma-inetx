#pragma once

#include "version.h"

#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

#include <map>
#include <string>
#include <vector>

#include "truma_core.h"

namespace esphome {
namespace truma_inetx {

namespace espbt = esphome::esp32_ble_tracker;

class TrumaInetX;

// Anything that wants bus values: sensors, switches, the climate entity...
class TrumaListener {
 public:
  virtual ~TrumaListener() = default;
  void set_parent(TrumaInetX *parent) { this->parent_ = parent; }
  void set_key(const std::string &topic, const std::string &param, uint16_t address) {
    this->topic_ = topic;
    this->param_ = param;
    this->address_ = address;
  }
  // Status-only entities (connected, pairing, status text) set a type instead.
  void set_status_type(uint8_t type) { this->status_type_ = type; }

  virtual bool wants(uint16_t src, const std::string &topic, const std::string &param) const {
    return !this->topic_.empty() && topic == this->topic_ && param == this->param_ &&
           (this->address_ == 0 || this->address_ == src);
  }
  virtual void on_value(uint16_t src, const truma_inetx_proto::CborValue &value) {}
  virtual void on_status() {}
  // Re-publish from the bus (after a reconnect).
  virtual void refresh();

 protected:
  TrumaInetX *parent_{nullptr};
  std::string topic_;
  std::string param_;
  uint16_t address_{0};
  uint8_t status_type_{0};
};

enum StatusType : uint8_t {
  STATUS_NONE = 0,
  STATUS_CONNECTED = 1,
  STATUS_PAIRED = 2,
  STATUS_PAIRING_MODE = 3,
  STATUS_TEXT = 4,
  STATUS_VERSION = 6,
  STATUS_LAST_DISCONNECT = 7,
};

// The ESPHome BLE client, kept thin: it only decides which advertisement to
// connect to and forwards every event to the hub. It is a separate component
// because BLEClientBase switches its own loop() off while idle.
class TrumaBleClient : public esp32_ble_client::BLEClientBase {
 public:
  void set_hub(TrumaInetX *hub) { this->hub_ = hub; }
  bool parse_device(const espbt::ESPBTDevice &device) override;
  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
  // Look a characteristic up in every discovered service (the service UUID
  // differs between panel models).
  esp32_ble_client::BLECharacteristic *find_characteristic(const espbt::ESPBTUUID &chr, bool log_services);

 protected:
  TrumaInetX *hub_{nullptr};
};

class TrumaInetX : public Component, public truma_inetx_proto::TransportIO {
 public:
  TrumaInetX();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_client(TrumaBleClient *client) { this->client_ = client; }
  void set_user_name(const std::string &name) { this->session_.identity.user_name = name; }
  void set_pairing_timeout(uint32_t ms) { this->pairing_timeout_ = ms; }
  void set_log_parameters(bool b) { this->session_.log_params = b; }
  void set_privacy(bool b) { this->privacy_ = b; }
  void set_german(bool b) { this->german_ = b; }
  bool german() const { return this->german_; }
  // Pick the text for the configured language.
  const char *tr(const char *en, const char *de) const { return this->german_ ? de : en; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
#endif
  void register_listener(TrumaListener *l) { this->listeners_.push_back(l); }

  // --- user actions (buttons / lambdas) ----------------------------------
  void start_pairing();

  void unpair();
  void reconnect();
  void dump_bus();
  void request_measurements() { this->session_.request_measurements(); }
  bool write_param(const std::string &topic, const std::string &param, int32_t value, uint16_t address = 0);
  bool write_param_text(const std::string &topic, const std::string &param, const std::string &value,
                        uint16_t address = 0);

  // --- state for entities ------------------------------------------------
  const truma_inetx_proto::CborValue *get_value(const std::string &topic, const std::string &param, uint16_t address = 0) const {
    return this->session_.bus.get(topic, param, address);
  }
  bool is_connected() const { return this->session_.running(); }
  bool is_paired() const { return !this->bonds_.empty(); }
  bool is_pairing() const { return this->pairing_; }
  const std::string &status_text() const { return this->status_; }
  const std::string &last_disconnect() const { return this->last_disconnect_; }

  // --- called by TrumaBleClient ------------------------------------------
  bool accept_advertisement(const espbt::ESPBTDevice &device);
  void connect_declined();
  void on_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
  void on_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

  // --- truma_inetx_proto::TransportIO -------------------------------------------------
  bool gatt_write(truma_inetx_proto::Channel ch, const uint8_t *data, size_t len, bool with_response) override;

 protected:
  enum class Link : uint8_t { NO_BLE, WAIT_ADV, CONNECTING, ENCRYPTING, SUBSCRIBING, SESSION };
  // Pairing candidates: an armed panel advertises without a name, an idle one
  // puts its name in the scan response. Tracked per (rotating) address.
  struct Candidate {
    uint32_t first_seen;
    uint32_t last_seen;
    uint8_t count;
    bool named;
    uint32_t retry_after;  // after a refusal/failure, wait before trying again
  };
  struct Bond {
    uint8_t addr[6];
    uint8_t irk[16];
    bool has_irk;
    uint8_t id_addr[6];
    uint8_t id_type;
  };

  void ble_ready_();
  void load_bonds_();
  void remove_bonds_();
  bool is_our_panel_(const uint8_t addr[6], const std::string &name) const;
  void set_link_(Link l);
  void try_subscribe_();
  esp32_ble_client::BLECharacteristic *find_char_(const char *uuid);
  void register_notify_(uint16_t char_handle);
  void write_cccd_(uint16_t char_handle);
  void link_up_();
  void link_down_(const char *reason);
  // Restart the Truma session over the existing BLE link. The panel does not
  // accept a new connection after a disconnect until it is power-cycled, so
  // the ESP never drops a working link on its own.
  void soft_restart_(const char *reason);
  void record_disconnect_(uint8_t hci_reason);
  std::string now_text_();
  const char *session_state_text_() const;
  void drop_link_(const char *reason);
  void update_status_();
  void notify_status_();

  TrumaBleClient *client_{nullptr};
  truma_inetx_proto::Transport transport_;
  truma_inetx_proto::Session session_;
  std::vector<TrumaListener *> listeners_;
  ESPPreferenceObject pref_;
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

  bool ble_ready_done_{false};
  std::vector<Bond> bonds_;
  bool pairing_{false};
  bool privacy_{false};
  bool german_{false};
  bool status_is_countdown_{false};
  std::string last_disconnect_{"-"};
  bool soft_restart_pending_{false};
  uint32_t soft_restarts_{0};
  uint32_t session_since_{0};
  std::map<uint64_t, Candidate> candidates_;
  uint64_t current_addr_{0};
  std::string current_name_;
  std::string panel_name_;  // name of the bonded panel, persisted
  ESPPreferenceObject name_pref_;
  uint32_t pairing_started_{0};
  uint32_t last_nameless_at_{0};
  // Diagnostics for the pairing window
  uint16_t seen_named_{0};
  uint16_t seen_nameless_{0};
  std::map<uint64_t, uint32_t> adv_log_;  // last time each Truma address was logged
  // Addresses of our panel: when each was last heard, and when a connect to it
  // last failed. After a bond/disconnect the panel can keep advertising an
  // address it no longer accepts connections on ("phantom", 0x3E forever)
  // while it also advertises a live one -- so rotate away from failures.
  std::map<uint64_t, uint32_t> ours_seen_;
  std::map<uint64_t, uint32_t> fail_at_;
  uint64_t last_failed_addr_{0};
  uint32_t pairing_until_{0};
  uint32_t pairing_timeout_{120000};

  Link link_{Link::NO_BLE};
  uint32_t link_since_{0};
  uint32_t next_connect_ok_{0};
  uint8_t failures_{0};
  uint8_t auth_failures_{0};
  uint32_t last_unpaired_log_{0};
  bool session_was_running_{false};

  bool encrypted_{false};
  bool services_ready_{false};
  uint16_t cmd_handle_{0};
  uint16_t data_w_handle_{0};
  uint16_t data_r_handle_{0};
  uint16_t notify_pending_{0};     // characteristic whose CCCD we are waiting for
  uint16_t cccd_pending_{0};       // its descriptor handle
  uint32_t notify_since_{0};
  bool cccd_written_by_us_{false};
  uint8_t notify_done_{0};

  std::string status_;
  uint32_t last_status_check_{0};
  bool last_connected_{false};
  bool last_paired_{false};
  bool last_pairing_{false};
};

}  // namespace truma_inetx
}  // namespace esphome
