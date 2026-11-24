#include "truma_inetx.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/esp32_ble/ble.h"

#include <esp_attr.h>
#include <esp_bt.h>
#include <esp_idf_version.h>
#include <mbedtls/aes.h>

#include <cstdio>
#include <cstring>
#include <strings.h>

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx";

// Counts restarts done to reload a lost key (survives a software restart, not a power cut).
static const uint32_t KEY_RELOAD_MAGIC = 0x7E1C0A42;
static RTC_NOINIT_ATTR uint32_t key_reload_magic;
static RTC_NOINIT_ATTR uint32_t key_reload_count;
// More restarts in a row than this and the key is really gone: pair again by hand.
static const uint32_t KEY_RELOAD_MAX = 2;

// Time without a session-level answer before a half-open link is dropped.
static const uint32_t LINK_SETUP_TIMEOUT = 30000;
// How long to wait for the stack to write a CCCD before doing it ourselves.
static const uint32_t CCCD_FALLBACK_MS = 3000;
// Reconnect backoff after failed attempts (a successful session resets it).
static const uint32_t BACKOFF_MS[] = {2000, 5000, 15000, 30000, 45000};

static void core_log(int level, const char *msg) {
  switch (level) {
    case 0:
      ESP_LOGE(TAG, "%s", msg);
      break;
    case 1:
      ESP_LOGW(TAG, "%s", msg);
      break;
    case 2:
      ESP_LOGI(TAG, "%s", msg);
      break;
    case 3:
      ESP_LOGD(TAG, "%s", msg);
      break;
    default:
      ESP_LOGV(TAG, "%s", msg);
      break;
  }
}

static void aes128_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

static void addr_from_u64(uint64_t a, uint8_t out[6]) {
  for (int i = 0; i < 6; i++)
    out[i] = static_cast<uint8_t>(a >> (40 - 8 * i));
}

static std::string addr_str(const uint8_t a[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

static std::string uuid_str(const uint8_t b[16], bool upper) {
  char buf[37];
  snprintf(buf, sizeof(buf),
           upper ? "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X"
                 : "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return buf;
}

// BLEClientBase::register_for_notify() exists in current ESPHome (it keeps the
// GATT cache alive until the registration completes); older releases need the
// raw call.
template<typename C> static auto register_notify(C *c, uint16_t h, int) -> decltype(c->register_for_notify(h)) {
  return c->register_for_notify(h);
}
template<typename C> static esp_err_t register_notify(C *c, uint16_t h, long) {
  return esp_ble_gattc_register_for_notify(c->get_gattc_if(), c->get_remote_bda(), h);
}

struct IdentityPref {
  uint32_t magic;
  uint8_t muid[16];
  uint8_t uuid[16];
};
static const uint32_t IDENTITY_MAGIC = 0x54524D31;  // "TRM1"

// ---------------------------------------------------------------------------
// TrumaListener

bool TrumaListener::wants(uint16_t src, const std::string &topic, const std::string &param) const {
  if (this->topic_.empty() || topic != this->topic_ || param != this->param_)
    return false;
  if (this->instance_ != 0)
    return this->parent_ != nullptr && this->parent_->nth_publisher(this->topic_, this->instance_) == src;
  return this->address_ == 0 || this->address_ == src;
}

void TrumaListener::refresh() {
  if (this->parent_ == nullptr || this->topic_.empty())
    return;
  uint16_t addr = this->address_;
  if (this->instance_ != 0) {
    addr = this->parent_->nth_publisher(this->topic_, this->instance_);
    if (addr == 0)
      return;
  }
  const truma_inetx_proto::CborValue *v = this->parent_->get_value(this->topic_, this->param_, addr);
  if (v != nullptr)
    this->on_value(addr, *v);
}

uint16_t TrumaInetX::nth_publisher(const std::string &topic, uint8_t n) const {
  const std::string prefix = topic + ".";
  uint8_t seen = 0;
  for (auto &d : this->session_.bus.devices) {  // std::map: ascending address
    auto it = d.second.params.lower_bound(prefix);
    if (it != d.second.params.end() && it->first.compare(0, prefix.size(), prefix) == 0) {
      if (++seen == n)
        return d.first;
    }
  }
  return 0;
}

uint8_t TrumaInetX::count_publishers_(const std::string &topic) const {
  uint8_t n = 0;
  while (n < 255 && this->nth_publisher(topic, n + 1) != 0)
    n++;
  return n;
}

// ---------------------------------------------------------------------------
// TrumaBleClient

bool TrumaBleClient::parse_device(const espbt::ESPBTDevice &device) {
  if (this->state() != espbt::ClientState::IDLE)
    return false;
  if (this->hub_ == nullptr || !this->hub_->accept_advertisement(device))
    return false;
  // The panel's address rotates, so the target is set per advertisement.
  this->set_address(device.address_uint64());
  if (BLEClientBase::parse_device(device))
    return true;
  this->hub_->connect_declined();
  return false;
}

bool TrumaBleClient::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param) {
  if (!BLEClientBase::gattc_event_handler(event, gattc_if, param))
    return false;
  if (this->hub_ != nullptr)
    this->hub_->on_gattc_event(event, gattc_if, param);
  return true;
}

void TrumaBleClient::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  BLEClientBase::gap_event_handler(event, param);
  if (this->hub_ != nullptr)
    this->hub_->on_gap_event(event, param);
}

esp32_ble_client::BLECharacteristic *TrumaBleClient::find_characteristic(const espbt::ESPBTUUID &chr,
                                                                        bool log_services) {
  esp32_ble_client::BLECharacteristic *found = nullptr;
  for (auto *svc : this->services_) {
    if (log_services) {
      char buf[48];
      svc->uuid.to_str(buf);
      ESP_LOGD(TAG, "Panel service %s (handles 0x%04X-0x%04X)", buf, svc->start_handle, svc->end_handle);
    }
    auto *c = svc->get_characteristic(chr);  // also parses the service's characteristics
    if (found == nullptr)
      found = c;
    if (log_services) {
      for (auto *ch : svc->characteristics) {
        char cbuf[48];
        ch->uuid.to_str(cbuf);
        ESP_LOGD(TAG, "  characteristic %s (handle 0x%04X, properties 0x%02X)", cbuf, ch->handle, ch->properties);
      }
    }
  }
  return found;
}

// ---------------------------------------------------------------------------
// TrumaInetX

TrumaInetX::TrumaInetX() : transport_(this), session_(&transport_) {}

void TrumaInetX::setup() {
  truma_inetx_proto::set_log_fn(core_log);
  ESP_LOGI(TAG, "truma_inetx version " TRUMA_INETX_VERSION);

  // A stable app identity, created once and kept across reboots.
  this->pref_ = global_preferences->make_preference<IdentityPref>(fnv1_hash("truma_inetx_identity_v1"), true);
  IdentityPref id{};
  if (!this->pref_.load(&id) || id.magic != IDENTITY_MAGIC) {
    id.magic = IDENTITY_MAGIC;
    for (int i = 0; i < 16; i += 4) {
      uint32_t a = random_uint32(), b = random_uint32();
      memcpy(&id.muid[i], &a, 4);
      memcpy(&id.uuid[i], &b, 4);
    }
    // RFC 4122 version 4 / variant bits
    id.muid[6] = (id.muid[6] & 0x0F) | 0x40;
    id.muid[8] = (id.muid[8] & 0x3F) | 0x80;
    id.uuid[6] = (id.uuid[6] & 0x0F) | 0x40;
    id.uuid[8] = (id.uuid[8] & 0x3F) | 0x80;
    this->pref_.save(&id);
    global_preferences->sync();
  }
  this->name_pref_ = global_preferences->make_preference<char[32]>(fnv1_hash("truma_inetx_panel_name_v1"), true);
  {
    char buf[32] = {0};
    if (this->name_pref_.load(&buf)) {
      buf[31] = 0;
      this->panel_name_ = buf;
    }
  }
  this->session_.identity.muid = uuid_str(id.muid, true);
  this->session_.identity.uuid = uuid_str(id.uuid, false);

  this->session_.unix_time = [this]() -> uint32_t {
#ifdef USE_TIME
    if (this->time_ != nullptr) {
      auto now = this->time_->now();
      if (now.is_valid())
        return static_cast<uint32_t>(now.timestamp);
    }
#endif
    return 0;
  };
  this->session_.on_update = [this](uint16_t src, const std::string &topic, const std::string &param,
                                    const truma_inetx_proto::CborValue &v) {
    if (src == truma_inetx_proto::DEV_PANEL && topic == "TimeAndDate" && param == "Time")
      this->check_panel_time_(v);
    char key[8];
    snprintf(key, sizeof(key), "%04X", src);
    // A newer value within the window replaces the held one (last one wins).
    this->pending_updates_[std::string(key) + topic + "." + param] = PendingUpdate{src, topic, param, v, millis() + 800};
  };
  this->session_.on_ready = [this]() {
    this->failures_ = 0;
    this->auth_failures_ = 0;
    this->session_was_running_ = true;
    ESP_LOGI(TAG, "Connected to the Truma panel (bus address 0x%04X)", this->session_.assigned_addr());
    this->request_robust_conn_params_();
    if (this->time_sync_ && !this->time_rejected_)
      this->set_timeout("time_sync", 15000, [this]() { this->auto_sync_time_(); });
    for (auto *l : this->listeners_)
      l->refresh();
    this->notify_status_();
  };
  this->session_.on_failed = [this](const char *reason) { this->soft_restart_(reason); };
  this->transport_.on_fatal = [this](const char *reason) { this->soft_restart_(reason); };
  // Publish the initial status (and the firmware version) once at boot.
  this->set_timeout(1000, [this]() { this->notify_status_(); });
}

void TrumaInetX::dump_config() {
  ESP_LOGCONFIG(TAG, "Truma iNet X (direct BLE), version " TRUMA_INETX_VERSION ":");
  ESP_LOGCONFIG(TAG, "  ESP-IDF: %s", esp_get_idf_version());
  if (this->startup_delay_ms_ > 0)
    ESP_LOGCONFIG(TAG, "  Startup delay: %u s", static_cast<unsigned>(this->startup_delay_ms_ / 1000));
  if (this->disconnect_on_shutdown_)
    ESP_LOGCONFIG(TAG, "  Disconnect on shutdown: YES");
  ESP_LOGCONFIG(TAG, "  User name: %s", this->session_.identity.user_name.c_str());
  ESP_LOGCONFIG(TAG, "  App MUID: %s", this->session_.identity.muid.c_str());
  ESP_LOGCONFIG(TAG, "  Pairing window: %us", static_cast<unsigned>(this->pairing_timeout_ / 1000));
  ESP_LOGCONFIG(TAG, "  LE privacy: %s", YESNO(this->privacy_));
  ESP_LOGCONFIG(TAG, "  Language: %s", this->german_ ? "de" : "en");
  ESP_LOGCONFIG(TAG, "  Supervision timeout: %u ms", static_cast<unsigned>(this->supervision_timeout_ms_));
  ESP_LOGCONFIG(TAG, "  Bonded panels: %u (name '%s')", static_cast<unsigned>(this->bonds_.size()),
                this->panel_name_.c_str());
  for (auto &b : this->bonds_) {
    ESP_LOGCONFIG(TAG, "    %s (IRK: %s, identity address %s type %u, address matches IRK: %s)",
                  addr_str(b.addr).c_str(), b.has_irk ? "yes" : "no", addr_str(b.id_addr).c_str(), b.id_type,
                  b.has_irk && truma_inetx_proto::rpa_matches(b.addr, b.irk, aes128_ecb) ? "yes" : "no");
  }
  ESP_LOGCONFIG(TAG, "  Listeners: %u", static_cast<unsigned>(this->listeners_.size()));
}

void TrumaInetX::ble_ready_() {
  // Just Works bonding (the panel has no display/keyboard dialogue), and ask
  // for the panel's identity key so its rotating address can be recognised.
  // The panel always pairs "Just Works" (no code). Keyboard/display + MITM are
  // still offered: that is the setting the long-term tests ran with.
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
  esp_ble_io_cap_t iocap = ESP_IO_CAP_KBDISP;
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
  uint8_t key_size = 16;
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
  // Ask for the panel's identity key (IRK): the panel moves to a new random
  // address after every disconnect, and the IRK is how the stack finds the
  // bond (and the encryption key) again under the new address.
  //
  // Known limitation (measured 2026-11-14 on an ESP32-C3): on chips whose
  // controller keeps the resolving list (ESP32-C3/S3/C6/H2), Bluedroid dials a
  // bonded peer by its identity address and leaves the address matching to
  // the controller; with the iNet X panel that never connects. The classic
  // ESP32 resolves addresses in the host and dials the address the panel
  // actually advertises -- like the ESPHome Bluetooth proxy, which works.
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
  // LE privacy: connect from a resolvable private address like a phone does.
  // Off by default; see the 'privacy' option.
  esp_ble_gap_config_local_privacy(this->privacy_);
  ESP_LOGI(TAG, "LE privacy %s", this->privacy_ ? "on (private address)" : "off (public address)");

  // Maximum transmit power. The default is +9 dBm; small boards such as the
  // ESP32-C3 Super Mini have weak antennas, and a connection request the panel
  // only half hears shows up as 0x3E (connection failed to be established).
  // Pick the highest level the chip accepts: 15 is +20 dBm on ESP32-C3/S3,
  // the classic ESP32 tops out at 7 (+9 dBm) and refuses anything above.
  esp_err_t txerr = ESP_FAIL;
  for (int lvl = 15; lvl >= 0; lvl--) {
    txerr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, static_cast<esp_power_level_t>(lvl));
    if (txerr == ESP_OK) {
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, static_cast<esp_power_level_t>(lvl));
      ESP_LOGD(TAG, "BLE TX power level %d", lvl);
      break;
    }
  }
  ESP_LOGI(TAG, "BLE TX power set to maximum (%s)", txerr == ESP_OK ? "ok" : "failed");

  this->load_bonds_();
  if (this->bonds_.empty()) {
    ESP_LOGW(TAG, "Not paired with a Truma panel yet. Press the 'Pair' button, then put the panel into "
                  "add-device mode.");
  } else {
    ESP_LOGI(TAG, "Paired with %u panel(s); waiting for its advertisement", static_cast<unsigned>(this->bonds_.size()));
  }
  this->set_link_(Link::WAIT_ADV);
}

void TrumaInetX::load_bonds_() {
  this->bonds_.clear();
  int n = esp_ble_get_bond_device_num();
  if (n <= 0)
    return;
  std::vector<esp_ble_bond_dev_t> list(n);
  if (esp_ble_get_bond_device_list(&n, list.data()) != ESP_OK)
    return;
  for (int i = 0; i < n; i++) {
    Bond b{};
    memcpy(b.addr, list[i].bd_addr, 6);
    b.has_irk = (list[i].bond_key.key_mask & ESP_LE_KEY_PID) != 0;
    memcpy(b.irk, list[i].bond_key.pid_key.irk, 16);
    memcpy(b.id_addr, list[i].bond_key.pid_key.static_addr, 6);
    b.id_type = list[i].bond_key.pid_key.addr_type;
    this->bonds_.push_back(b);
  }
}

void TrumaInetX::remove_bonds_() {
  this->panel_name_.clear();
  char empty[32] = {0};
  this->name_pref_.save(&empty);
  this->load_bonds_();
  for (auto &b : this->bonds_) {
    ESP_LOGI(TAG, "Removing bond %s", addr_str(b.addr).c_str());
    esp_ble_remove_bond_device(b.addr);
  }
  this->bonds_.clear();
}

bool TrumaInetX::is_our_panel_(const uint8_t addr[6], const std::string &name) const {
  for (auto &b : this->bonds_) {
    if (memcmp(b.addr, addr, 6) == 0)
      return true;
    if (b.has_irk && truma_inetx_proto::rpa_matches(addr, b.irk, aes128_ecb))
      return true;
    if (b.has_irk && memcmp(b.id_addr, addr, 6) == 0)
      return true;
  }
  // The panel's name carries part of its identity address ("Truma iNetX-55A77E"),
  // so it is unique enough to recognise the panel if its address ever changes.
  return !this->bonds_.empty() && !this->panel_name_.empty() && name == this->panel_name_;
}

void TrumaInetX::set_link_(Link l) {
  this->link_ = l;
  this->link_since_ = millis();
}

void TrumaInetX::flush_updates_(uint32_t now, bool all) {
  for (auto it = this->pending_updates_.begin(); it != this->pending_updates_.end();) {
    if (!all && static_cast<int32_t>(now - it->second.due) < 0) {
      ++it;
      continue;
    }
    const PendingUpdate u = std::move(it->second);
    it = this->pending_updates_.erase(it);
    for (auto *l : this->listeners_)
      if (l->wants(u.src, u.topic, u.param))
        l->on_value(u.src, u.value);
    // A new publisher of an instance-selected topic (say the second gas bottle)
    // can shift which device is "1" and "2": re-publish those entities.
    auto ip = this->instance_publishers_.find(u.topic);
    if (ip != this->instance_publishers_.end()) {
      const uint8_t count = this->count_publishers_(u.topic);
      if (count != ip->second) {
        ip->second = count;
        for (auto *l : this->listeners_)
          if (l->instance() != 0 && l->topic() == u.topic)
            l->refresh();
      }
    }
  }
}

void TrumaInetX::loop() {
  const uint32_t now = millis();
  if (!this->pending_updates_.empty())
    this->flush_updates_(now, false);

  if (!this->ble_ready_done_) {
    if (esp32_ble::global_ble != nullptr && esp32_ble::global_ble->is_active()) {
      this->ble_ready_done_ = true;
      this->ble_ready_();
    } else {
      this->update_status_();
      return;
    }
  }

  if (this->pairing_ && static_cast<int32_t>(now - this->pairing_until_) >= 0 && this->link_ == Link::WAIT_ADV) {
    this->pairing_ = false;
    if (this->seen_named_ == 0 && this->seen_nameless_ == 0) {
      ESP_LOGW(TAG, "Pairing window closed: no Truma panel was heard at all. Is it switched on, in range, and not "
                    "connected to another device?");
    } else if (this->seen_nameless_ == 0) {
      ESP_LOGW(TAG, "Pairing window closed: a Truma panel was heard %u times, but always with its name -- it was "
                    "never in add-device mode while the window was open.",
               this->seen_named_);
    } else {
      ESP_LOGW(TAG, "Pairing window closed without a successful pairing (%u nameless / %u named advertisements)",
               this->seen_nameless_, this->seen_named_);
    }
  }

  // Link supervision: nothing below the session layer has a deadline of its
  // own, and a link that is up but carries nothing must not sit there.
  switch (this->link_) {
    case Link::CONNECTING:
    case Link::ENCRYPTING:
    case Link::SUBSCRIBING:
      if (now - this->link_since_ > LINK_SETUP_TIMEOUT) {
        this->drop_link_("link setup did not complete in time");
      } else if (this->client_->state() == espbt::ClientState::IDLE && now - this->link_since_ > 3000) {
        this->link_down_("connection attempt ended");
      }
      break;
    default:
      break;
  }

  // The stack normally writes the CCCD itself after register_for_notify; if
  // it has not done so, do it here.
  if (this->link_ == Link::SUBSCRIBING && this->notify_pending_ != 0 && !this->cccd_written_by_us_ &&
      now - this->notify_since_ > CCCD_FALLBACK_MS) {
    ESP_LOGD(TAG, "Writing CCCD for handle 0x%04X ourselves", this->notify_pending_);
    this->write_cccd_(this->notify_pending_);
  }

  this->transport_.loop(now);
  this->session_.loop(now);

  if (now - this->last_status_check_ > 500) {
    this->last_status_check_ = now;
    this->update_status_();
  }
}

// ---------------------------------------------------------------------------
// Advertisements

bool TrumaInetX::accept_advertisement(const espbt::ESPBTDevice &device) {
  if (!this->ble_ready_done_ || this->link_ != Link::WAIT_ADV)
    return false;
  const uint32_t now = millis();
  uint8_t addr[6];
  addr_from_u64(device.address_uint64(), addr);
  const std::string name = device.get_name().empty() ? std::string()
                                                     : std::string(device.get_name().c_str(), device.get_name().size());

  // Panels advertise different UUIDs from Truma's own range (fc3100xx-...):
  // 0000, 0001, 0002 and 0006 have been seen. Accept the whole range, like the
  // Home Assistant integration does -- and, like it, also accept the panel by
  // its name, because not every address it puts on air carries the UUID.
  bool has_uuid = false;
  for (auto &uuid : device.get_service_uuids()) {
    char buf[48];
    uuid.to_str(buf);
    if (strncasecmp(buf, "FC31", 4) == 0 && strlen(buf) == 36 &&
        strcasecmp(buf + 8, "-F3B2-11E8-8EB2-F2801F1B9FD1") == 0) {
      has_uuid = true;
      break;
    }
  }
  const bool has_name = strncasecmp(name.c_str(), "Truma iNetX", 11) == 0;
  bool is_identity = false;
  for (auto &b : this->bonds_)
    is_identity |= b.has_irk && memcmp(b.id_addr, addr, 6) == 0;
  if (!has_uuid && !has_name && !is_identity)
    return false;

  {
    // One line per Truma address every 10 s: what the ESP actually hears.
    auto &last = this->adv_log_[device.address_uint64()];
    if (last == 0 || now - last > 10000) {
      last = now;
      ESP_LOGD(TAG, "Truma advertisement from %s, RSSI %d, name '%s', service UUID %s%s", addr_str(addr).c_str(),
               device.get_rssi(), name.c_str(), has_uuid ? "yes" : "no", is_identity ? ", identity address" : "");
    }
    if (this->adv_log_.size() > 32)
      this->adv_log_.clear();
  }
  if (this->pairing_) {
    if (name.empty())
      this->seen_nameless_++;
    else
      this->seen_named_++;
  }
  const bool waiting = static_cast<int32_t>(now - this->next_connect_ok_) < 0;

  bool ok = false;
  if (this->pairing_) {
    if (waiting)
      return false;
    // Only a panel in add-device mode accepts a new bond. Nameless panels are
    // preferred (some models drop the name when armed, which keeps a
    // neighbour's idle panel out of the way); otherwise named ones are tried.
    const uint64_t key = device.address_uint64();
    if (this->candidates_.size() > 32 && this->candidates_.find(key) == this->candidates_.end()) {
      for (auto it = this->candidates_.begin(); it != this->candidates_.end();) {
        if (now - it->second.last_seen > 30000)
          it = this->candidates_.erase(it);
        else
          ++it;
      }
    }
    auto &c = this->candidates_[key];
    if (c.count == 0)
      c.first_seen = now;
    c.last_seen = now;
    if (c.count < 255)
      c.count++;
    if (name.empty()) {
      this->last_nameless_at_ = now;
    } else {
      c.named = true;
    }
    if (c.retry_after != 0 && static_cast<int32_t>(now - c.retry_after) < 0) {
      // tried recently and it failed; give it a moment
    } else if (!c.named) {
      // Some panels drop their name in add-device mode; such a one is
      // certainly the armed panel.
      ok = true;
      ESP_LOGI(TAG, "Pairing: found a Truma panel without name at %s (RSSI %d), connecting", addr_str(addr).c_str(),
               device.get_rssi());
    } else if (now - this->pairing_started_ > 5000 &&
               (this->last_nameless_at_ == 0 || now - this->last_nameless_at_ > 5000)) {
      // Other panels keep their name while in add-device mode. With no
      // nameless panel around, try the named one; a panel that is not armed
      // simply refuses, and is retried a few seconds later.
      ok = true;
      ESP_LOGI(TAG, "Pairing: trying Truma panel '%s' at %s (RSSI %d)", name.c_str(), addr_str(addr).c_str(),
               device.get_rssi());
    }
  } else if (!this->bonds_.empty()) {
    const uint64_t key = device.address_uint64();
    if (is_identity || this->is_our_panel_(addr, name)) {
      this->ours_seen_[key] = now;
      if (this->ours_seen_.size() > 16) {
        for (auto it = this->ours_seen_.begin(); it != this->ours_seen_.end();)
          it = (now - it->second > 120000) ? this->ours_seen_.erase(it) : std::next(it);
      }
      auto f = this->fail_at_.find(key);
      const bool failed_recently = f != this->fail_at_.end() && now - f->second < 90000;
      // Is another address of our panel on air that has not failed lately?
      bool alternative = false;
      for (auto &o : this->ours_seen_) {
        if (o.first == key || now - o.second > 30000)
          continue;
        auto of = this->fail_at_.find(o.first);
        if (of == this->fail_at_.end() || now - of->second >= 90000)
          alternative = true;
      }
      if (failed_recently && alternative) {
        // A phantom: leave it, the live address will come by.
      } else if (waiting && !(this->last_failed_addr_ != 0 && key != this->last_failed_addr_ && !failed_recently &&
                                now - this->link_since_ > 1000)) {
        // backing off -- unless this is a different address than the one that
        // just failed, which is worth a try right away
      } else if (now < this->startup_delay_ms_) {
        // startup_delay: give the panel time to drop the link of before the restart
        if (now - this->last_startup_wait_log_ > 10000 || this->last_startup_wait_log_ == 0) {
          this->last_startup_wait_log_ = now;
          ESP_LOGI(TAG, "Panel seen, waiting %u s after start before connecting (startup_delay)",
                   static_cast<unsigned>((this->startup_delay_ms_ - now + 999) / 1000));
        }
      } else {
        ok = true;
        ESP_LOGD(TAG, "Our panel advertises at %s (RSSI %d%s), connecting", addr_str(addr).c_str(), device.get_rssi(),
                 failed_recently ? ", failed before" : "");
      }
    } else if (now - this->last_unpaired_log_ > 60000) {
      this->last_unpaired_log_ = now;
      ESP_LOGD(TAG, "Ignoring another Truma panel at %s (not ours)", addr_str(addr).c_str());
    }
  } else if (!waiting && now - this->last_unpaired_log_ > 30000) {
    this->last_unpaired_log_ = now;
    ESP_LOGW(TAG, "A Truma panel is in range (%s) but this device is not paired. Press 'Pair', then put the panel "
                  "into add-device mode.",
             addr_str(addr).c_str());
  }
  if (ok) {
    this->current_addr_ = device.address_uint64();
    this->current_name_ = name;
    this->set_link_(Link::CONNECTING);
  }
  return ok;
}

void TrumaInetX::connect_declined() {
  // The BLE client was not ready for this advertisement; no attempt was made.
  if (this->link_ == Link::CONNECTING)
    this->set_link_(Link::WAIT_ADV);
}

// ---------------------------------------------------------------------------
// GATT / GAP events

void TrumaInetX::on_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
  const uint32_t now = millis();
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Connection to the panel failed, status=%d", param->open.status);
        this->link_down_("connect failed");
        break;
      }
      ESP_LOGD(TAG, "Link up, starting encryption");
      this->fail_at_.erase(this->current_addr_);
      this->last_failed_addr_ = 0;
      this->transport_.reset();
      this->encrypted_ = false;
      this->services_ready_ = false;
      this->set_link_(Link::ENCRYPTING);
      // Encrypt before touching any characteristic: a CCCD write that races
      // ahead of encryption fails and the link is torn down. With a bond this
      // re-uses the stored key, without one it runs Just Works pairing.
      esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT);
      break;
    }
    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK) {
        ESP_LOGD(TAG, "MTU %u", param->cfg_mtu.mtu);
        this->transport_.set_mtu(param->cfg_mtu.mtu);
      }
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      this->services_ready_ = true;
      this->try_subscribe_();
      break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle != this->notify_pending_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        this->drop_link_("could not register for notifications");
        break;
      }
      this->notify_since_ = now;
      break;
    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (this->link_ != Link::SUBSCRIBING || this->notify_pending_ == 0)
        break;
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Enabling notifications failed, status=%d", param->write.status);
        this->drop_link_("could not enable notifications");
        break;
      }
      this->notify_pending_ = 0;
      this->cccd_pending_ = 0;
      this->notify_done_++;
      if (this->notify_done_ == 1) {
        this->register_notify_(this->data_r_handle_);
      } else {
        this->link_up_();
      }
      break;
    }
    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == this->cmd_handle_) {
        this->transport_.on_cmd_notify(param->notify.value, param->notify.value_len, now);
      } else if (param->notify.handle == this->data_r_handle_) {
        this->transport_.on_data_notify(param->notify.value, param->notify.value_len, now);
      }
      break;
    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.handle == this->cmd_handle_ || param->write.handle == this->data_w_handle_)
        this->transport_.on_write_complete(param->write.status == ESP_GATT_OK, now);
      break;
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGD(TAG, "Disconnected, reason=0x%02X", param->disconnect.reason);
      if (this->link_ == Link::SESSION)
        this->record_disconnect_(param->disconnect.reason);
      this->link_down_("disconnected");
      break;
    case ESP_GATTC_CLOSE_EVT:
      if (this->link_ != Link::WAIT_ADV && this->link_ != Link::NO_BLE)
        this->link_down_("closed");
      break;
    default:
      break;
  }
}

void TrumaInetX::on_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
      // The base class answers requests for the address it dialled; after
      // address resolution the request can carry the identity address.
      if (this->link_ == Link::ENCRYPTING &&
          memcmp(param->ble_security.ble_req.bd_addr, this->client_->get_remote_bda(), 6) != 0)
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      // The panel pairs without a code ("Just Works"); never seen in practice.
      ESP_LOGW(TAG, "Pairing: the panel asks for a code -- not supported, declining");
      esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, false, 0);
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      ESP_LOGW(TAG, "Pairing: enter the code %06u on the panel", static_cast<unsigned>(param->ble_security.key_notif.passkey));
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      ESP_LOGI(TAG, "Pairing: panel shows %06u -- confirming", static_cast<unsigned>(param->ble_security.key_notif.passkey));
      esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      if (this->link_ != Link::ENCRYPTING)
        break;
      auto &a = param->ble_security.auth_cmpl;
      if (!a.success) {
        this->auth_failures_++;
        if (this->pairing_) {
          auto it = this->candidates_.find(this->current_addr_);
          if (it != this->candidates_.end())
            it->second.retry_after = millis() + 8000;
        }
        ESP_LOGW(TAG, "Encryption/pairing with the panel failed (reason 0x%02X)%s", a.fail_reason,
                 this->bonds_.empty() ? " -- is the panel in add-device mode?"
                                      : " -- if this repeats, the panel has forgotten this device: pair again");
        // The stack can drop the panel's key from RAM after a link loss ("Deleting unpaired
        // device"); it then tries to pair from scratch and the panel answers "pairing not
        // supported". The key is still in flash, so a restart loads it again.
        if (a.fail_reason == ESP_AUTH_SMP_PAIR_NOT_SUPPORT && !this->pairing_) {
          this->load_bonds_();
          if (key_reload_magic != KEY_RELOAD_MAGIC)
            key_reload_count = 0;
          if (!this->bonds_.empty() && key_reload_count < KEY_RELOAD_MAX) {
            key_reload_magic = KEY_RELOAD_MAGIC;
            key_reload_count++;
            ESP_LOGW(TAG, "The Bluetooth stack lost the panel key from RAM, it is still in flash: restarting the ESP "
                          "to load it (%u/%u)",
                     static_cast<unsigned>(key_reload_count), static_cast<unsigned>(KEY_RELOAD_MAX));
            this->drop_link_("authentication failed");
            this->set_timeout("key_reload", 1000, []() { App.safe_reboot(); });
            break;
          }
        }
        this->drop_link_("authentication failed");
        break;
      }
      key_reload_magic = KEY_RELOAD_MAGIC;
      key_reload_count = 0;
      const bool was_pairing = this->pairing_;
      ESP_LOGI(TAG, "Encrypted (auth mode 0x%02X%s)", a.auth_mode,
               (a.auth_mode & ESP_LE_AUTH_REQ_MITM) ? ", authenticated with code" : ", without code");
      this->load_bonds_();
      if (was_pairing) {
        this->pairing_ = false;
        if (!this->current_name_.empty()) {
          this->panel_name_ = this->current_name_;
          char buf[32] = {0};
          strncpy(buf, this->panel_name_.c_str(), sizeof(buf) - 1);
          this->name_pref_.save(&buf);
          global_preferences->sync();
        }
        ESP_LOGI(TAG, "Paired with the Truma panel (%s). The panel may show 'something went wrong' -- that only "
                      "refers to the app-level handshake and can be ignored.",
                 addr_str(a.bd_addr).c_str());
      }
      this->auth_failures_ = 0;
      this->encrypted_ = true;
      this->try_subscribe_();
      break;
    }
    default:
      break;
  }
}

void TrumaInetX::try_subscribe_() {
  if (this->link_ != Link::ENCRYPTING || !this->encrypted_ || !this->services_ready_)
    return;
  auto *cmd = this->find_char_(truma_inetx_proto::CHAR_CMD_UUID);
  auto *dw = this->find_char_(truma_inetx_proto::CHAR_DATA_W_UUID);
  auto *dr = this->find_char_(truma_inetx_proto::CHAR_DATA_R_UUID);
  if (cmd == nullptr || dw == nullptr || dr == nullptr) {
    this->drop_link_("Truma characteristics not found");
    return;
  }
  this->cmd_handle_ = cmd->handle;
  this->data_w_handle_ = dw->handle;
  this->data_r_handle_ = dr->handle;
  this->notify_done_ = 0;
  this->set_link_(Link::SUBSCRIBING);
  ESP_LOGD(TAG, "Encrypted; enabling notifications (CMD 0x%04X, DATA_R 0x%04X)", this->cmd_handle_,
           this->data_r_handle_);
  this->register_notify_(this->cmd_handle_);
}

esp32_ble_client::BLECharacteristic *TrumaInetX::find_char_(const char *uuid) {
  // Search every service the panel offers; log them once per connection.
  const bool log_services = std::strcmp(uuid, truma_inetx_proto::CHAR_CMD_UUID) == 0;
  auto *c = this->client_->find_characteristic(espbt::ESPBTUUID::from_raw(uuid), log_services);
  if (c == nullptr)
    ESP_LOGW(TAG, "Characteristic %s not found on the panel", uuid);
  return c;
}

void TrumaInetX::register_notify_(uint16_t char_handle) {
  this->notify_pending_ = char_handle;
  this->cccd_pending_ = 0;
  this->cccd_written_by_us_ = false;
  this->notify_since_ = millis();
  // The base class tracks the registration and, for this connection type,
  // writes the CCCD itself when the registration completes.
  if (register_notify(this->client_, char_handle, 0) != ESP_OK)
    this->drop_link_("register_for_notify failed");
}

void TrumaInetX::write_cccd_(uint16_t char_handle) {
  this->cccd_written_by_us_ = true;
  esp_bt_uuid_t uuid;
  uuid.len = ESP_UUID_LEN_16;
  uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
  esp_gattc_descr_elem_t result;
  uint16_t count = 1;
  uint16_t descr = char_handle + 1;
  if (esp_ble_gattc_get_descr_by_char_handle(this->client_->get_gattc_if(), this->client_->get_conn_id(), char_handle,
                                             uuid, &result, &count) == ESP_GATT_OK &&
      count > 0)
    descr = result.handle;
  this->cccd_pending_ = descr;
  uint16_t enable = 1;
  esp_ble_gattc_write_char_descr(this->client_->get_gattc_if(), this->client_->get_conn_id(), descr, sizeof(enable),
                                 reinterpret_cast<uint8_t *>(&enable), ESP_GATT_WRITE_TYPE_RSP,
                                 ESP_GATT_AUTH_REQ_NONE);
}

void TrumaInetX::link_up_() {
  this->set_link_(Link::SESSION);
  ESP_LOGD(TAG, "Notifications on, starting session");
  this->session_since_ = millis();
  this->soft_restarts_ = 0;
  this->session_.start(millis());
}

void TrumaInetX::drop_link_(const char *reason) {
  if (this->link_ == Link::WAIT_ADV || this->link_ == Link::NO_BLE)
    return;
  ESP_LOGW(TAG, "Dropping the link: %s", reason);
  this->client_->disconnect();
  this->link_down_(reason);
}

void TrumaInetX::link_down_(const char *reason) {
  if (this->link_ == Link::WAIT_ADV || this->link_ == Link::NO_BLE)
    return;
  const bool was_running = this->session_.running();
  if (this->link_ == Link::CONNECTING && this->current_addr_ != 0) {
    // The link never came up: remember the address so the next attempt can
    // rotate to another one the panel advertises.
    this->fail_at_[this->current_addr_] = millis();
    this->last_failed_addr_ = this->current_addr_;
    if (this->fail_at_.size() > 16)
      this->fail_at_.erase(this->fail_at_.begin());
  }
  this->session_.stop();
  this->transport_.reset();
  this->transport_.set_mtu(23);
  this->encrypted_ = false;
  this->services_ready_ = false;
  this->notify_pending_ = 0;
  this->cmd_handle_ = this->data_w_handle_ = this->data_r_handle_ = 0;
  this->set_link_(Link::WAIT_ADV);

  uint32_t delay;
  if (this->pairing_) {
    // Inside the pairing window: the panel's add-device mode does not last
    // long, so retry quickly (refused addresses are skipped anyway).
    delay = 1000;
  } else if (was_running) {
    ESP_LOGW(TAG, "Connection to the panel lost (%s), reconnecting", reason);
    this->failures_ = 0;
    delay = BACKOFF_MS[0];
  } else {
    const size_t n = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]);
    delay = BACKOFF_MS[this->failures_ < n ? this->failures_ : n - 1];
    if (this->failures_ < 250)
      this->failures_++;
    ESP_LOGD(TAG, "Attempt failed (%s), next try in %us", reason, static_cast<unsigned>(delay / 1000));
  }
  this->next_connect_ok_ = millis() + delay;
  this->notify_status_();
}

// ---------------------------------------------------------------------------
// TransportIO

bool TrumaInetX::gatt_write(truma_inetx_proto::Channel ch, const uint8_t *data, size_t len, bool with_response) {
  const uint16_t handle = ch == truma_inetx_proto::Channel::CMD ? this->cmd_handle_ : this->data_w_handle_;
  if (handle == 0 || this->link_ != Link::SESSION)
    return false;
  auto err = esp_ble_gattc_write_char(this->client_->get_gattc_if(), this->client_->get_conn_id(), handle, len,
                                      const_cast<uint8_t *>(data),
                                      with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
                                      ESP_GATT_AUTH_REQ_NONE);
  return err == ESP_OK;
}

// ---------------------------------------------------------------------------
// Actions

void TrumaInetX::start_pairing() {
  ESP_LOGI(TAG, "Pairing mode for %us: put the panel into add-device mode now (Truma app or panel menu)",
           static_cast<unsigned>(this->pairing_timeout_ / 1000));
  if (this->link_ != Link::WAIT_ADV && this->link_ != Link::NO_BLE)
    this->drop_link_("re-pairing requested");
  if (this->ble_ready_done_)
    this->remove_bonds_();
  this->pairing_ = true;
  this->candidates_.clear();
  this->fail_at_.clear();
  this->ours_seen_.clear();
  this->last_failed_addr_ = 0;
  this->pairing_started_ = millis();
  this->last_nameless_at_ = 0;
  this->seen_named_ = 0;
  this->seen_nameless_ = 0;
  this->adv_log_.clear();
  this->pairing_until_ = millis() + this->pairing_timeout_;
  this->next_connect_ok_ = millis();
  this->failures_ = 0;
  this->notify_status_();
}

void TrumaInetX::unpair() {
  this->pairing_ = false;
  if (this->link_ != Link::WAIT_ADV && this->link_ != Link::NO_BLE)
    this->drop_link_("unpair requested");
  if (this->ble_ready_done_)
    this->remove_bonds_();
  ESP_LOGI(TAG, "Bond removed. Also remove this device from the panel's Bluetooth device list.");
  this->notify_status_();
}

void TrumaInetX::reconnect() {
  this->next_connect_ok_ = millis();
  this->failures_ = 0;
  if (this->link_ == Link::SESSION) {
    // Keep the BLE link: the panel would not let us back in.
    this->soft_restart_("restart requested");
  } else if (this->link_ != Link::WAIT_ADV && this->link_ != Link::NO_BLE) {
    this->drop_link_("reconnect requested");
  }
}

void TrumaInetX::request_robust_conn_params_() {
  if (this->link_ != Link::SESSION)
    return;
  // Keep the interval ESPHome connects with (8.75-11.25 ms): the panel rejects
  // longer ones (status 14, "unacceptable connection interval"). Only the
  // supervision timeout is raised. ESPHome logs a rejection as
  // "Conn param update failed"; success is only logged at VERBOSE level
  // ("Conn params updated", logger: logs: esp32_ble: VERBOSE).
  esp_ble_conn_update_params_t p = {};
  memcpy(p.bda, this->client_->get_remote_bda(), sizeof(esp_bd_addr_t));
  p.min_int = 0x07;
  p.max_int = 0x09;
  p.latency = 0;
  p.timeout = static_cast<uint16_t>(this->supervision_timeout_ms_ / 10);
  ESP_LOGI(TAG, "Requesting supervision timeout %u ms (interval 8.75-11.25 ms)",
           static_cast<unsigned>(this->supervision_timeout_ms_));
  esp_err_t err = esp_ble_gap_update_conn_params(&p);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "esp_ble_gap_update_conn_params failed: %d", err);
  // ESPHome does not pass the result on to components: read back what is in use.
  this->set_timeout("link_params", 5000, [this]() { this->read_link_params_(); });
}

void TrumaInetX::sync_panel_time() {
  this->time_rewrites_ = 0;
  this->write_panel_time_();
}

void TrumaInetX::auto_sync_time_() {
  if (this->time_rejected_)
    return;
  this->sync_panel_time();
  // Again in 24 h (replaced if the session restarts in between).
  this->set_timeout("time_sync", 24UL * 3600UL * 1000UL, [this]() { this->auto_sync_time_(); });
}

// The panel ignores writes to TimeAndDate.SystemTime. Its menu sets the clock
// as text: TimeAndDate.Time "HH:MM" and TimeAndDate.Date "DD.MM.YY".
void TrumaInetX::write_panel_time_() {
#ifdef USE_TIME
  if (this->time_ == nullptr) {
    ESP_LOGW(TAG, "Panel clock: no time_id configured");
    return;
  }
  auto now = this->time_->now();
  if (!now.is_valid()) {
    ESP_LOGW(TAG, "Panel clock: Home Assistant time not available yet");
    return;
  }
  if (!this->session_.running())
    return;
  char hm[8], dmy[12];
  snprintf(hm, sizeof(hm), "%02d:%02d", now.hour, now.minute);
  snprintf(dmy, sizeof(dmy), "%02d.%02d.%02d", now.day_of_month, now.month, now.year % 100);
  ESP_LOGI(TAG, "Panel clock: setting %s %s", dmy, hm);
  bool ok = this->write_param_text("TimeAndDate", "Date", dmy, truma_inetx_proto::DEV_PANEL);
  ok = this->write_param_text("TimeAndDate", "Time", hm, truma_inetx_proto::DEV_PANEL) && ok;
  if (ok) {
    // The panel reports its "HH:MM" every minute; judge the first report that
    // arrives a few seconds after the write.
    this->time_check_from_ = millis() + 3000;
    this->time_check_until_ = millis() + 130000;
  }
#else
  ESP_LOGW(TAG, "Panel clock: no time_id configured");
#endif
}

void TrumaInetX::on_shutdown() {
  if (!this->disconnect_on_shutdown_ || this->client_ == nullptr)
    return;
  if (this->link_ == Link::NO_BLE || this->link_ == Link::WAIT_ADV)
    return;
  ESP_LOGI(TAG, "Restart: disconnecting from the panel first (disconnect_on_shutdown)");
  esp_ble_gap_disconnect(this->client_->get_remote_bda());
  delay(300);  // let the controller send the disconnect before the radio goes down
}

void TrumaInetX::check_panel_time_(const truma_inetx_proto::CborValue &v) {
#ifdef USE_TIME
  const uint32_t ms = millis();
  if (this->time_check_until_ == 0 || static_cast<int32_t>(ms - this->time_check_until_) > 0 ||
      static_cast<int32_t>(ms - this->time_check_from_) < 0)
    return;
  if (this->time_ == nullptr || v.type != truma_inetx_proto::CborValue::TEXT)
    return;
  int hh = 0, mm = 0;
  if (sscanf(v.s.c_str(), "%d:%d", &hh, &mm) != 2)
    return;
  auto now = this->time_->now();
  if (!now.is_valid())
    return;
  int diff = (hh * 60 + mm) - (now.hour * 60 + now.minute);
  while (diff > 720)
    diff -= 1440;
  while (diff < -720)
    diff += 1440;
  this->time_check_until_ = 0;
  if (diff >= -2 && diff <= 2) {
    ESP_LOGI(TAG, "Panel clock: shows %s, matches the local time", v.s.c_str());
    this->time_rejected_ = false;
    return;
  }
  if (this->time_rewrites_ < 1) {
    // Maybe the write raced with the panel's minute report: try once more.
    this->time_rewrites_++;
    ESP_LOGI(TAG, "Panel clock: shows %s, local time %02d:%02d -- trying once more", v.s.c_str(), now.hour, now.minute);
    this->set_timeout("time_rewrite", 2000, [this]() { this->write_panel_time_(); });
    return;
  }
  ESP_LOGW(TAG, "Panel clock: shows %s, local time %02d:%02d -- the panel does not accept the time, "
                "no more automatic attempts (button still works)",
           v.s.c_str(), now.hour, now.minute);
  this->time_rejected_ = true;
#endif
}

void TrumaInetX::read_link_params_() {
  if (this->link_ != Link::SESSION)
    return;
  esp_gap_conn_params_t cp{};
  esp_bd_addr_t bda;
  memcpy(bda, this->client_->get_remote_bda(), sizeof(esp_bd_addr_t));
  if (esp_ble_get_current_conn_params(bda, &cp) != ESP_OK) {
    ESP_LOGW(TAG, "Could not read the link parameters");
    return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), tr("interval %.2f ms, latency %u, timeout %.1f s", "Intervall %.2f ms, Latenz %u, Timeout %.1f s"),
           cp.interval * 1.25f, static_cast<unsigned>(cp.latency), cp.timeout / 100.0f);
  this->link_params_ = buf;
  ESP_LOGI(TAG, "Link parameters in use: %s", buf);
  if (static_cast<uint32_t>(cp.timeout) * 10u + 500u < this->supervision_timeout_ms_)
    ESP_LOGW(TAG, "The panel did not accept the longer supervision timeout");
  this->notify_status_();
}

void TrumaInetX::soft_restart_(const char *reason) {
  if (this->link_ != Link::SESSION) {
    this->drop_link_(reason);
    return;
  }
  if (this->soft_restart_pending_)
    return;
  this->soft_restart_pending_ = true;
  this->soft_restarts_++;
  ESP_LOGW(TAG, "Session problem (%s): restarting the session over the existing link (#%u), not disconnecting",
           reason, static_cast<unsigned>(this->soft_restarts_));
  this->session_.stop();
  this->notify_status_();
  // Deferred: we may be inside a transport/session callback right now.
  this->set_timeout("soft_restart", 3000, [this]() {
    this->soft_restart_pending_ = false;
    if (this->link_ != Link::SESSION)
      return;
    this->transport_.reset();
    this->session_.start(millis());
    this->notify_status_();
  });
}

std::string TrumaInetX::now_text_() {
  char buf[32];
  if (this->time_ != nullptr) {
    auto t = this->time_->now();
    if (t.is_valid()) {
      snprintf(buf, sizeof(buf), "%02d.%02d. %02d:%02d", t.day_of_month, t.month, t.hour, t.minute);
      return buf;
    }
  }
  snprintf(buf, sizeof(buf), "uptime %us", static_cast<unsigned>(millis() / 1000));
  return buf;
}

void TrumaInetX::record_disconnect_(uint8_t hci_reason) {
  const char *why;
  switch (hci_reason) {
    case 0x08:
      why = tr("radio timeout (panel out of reach)", "Funk-Timeout (Panel nicht mehr erreichbar)");
      break;
    case 0x13:
      why = tr("closed by the panel", "vom Panel beendet");
      break;
    case 0x16:
      why = tr("closed by the ESP", "vom ESP beendet");
      break;
    case 0x22:
      why = tr("panel does not answer (LL timeout)", "Panel antwortet nicht (LL timeout)");
      break;
    case 0x3D:
      why = tr("encryption error (MIC)", "Verschluesselungsfehler (MIC)");
      break;
    default:
      why = tr("other reason", "anderer Grund");
      break;
  }
  const uint32_t mins = (millis() - this->session_since_) / 60000;
  char buf[160];
  snprintf(buf, sizeof(buf),
           tr("%s: %s (0x%02X), after %u h %02u min, %u session restarts",
              "%s: %s (0x%02X), nach %u h %02u min, %u Neustarts der Sitzung"),
           this->now_text_().c_str(), why, hci_reason, static_cast<unsigned>(mins / 60),
           static_cast<unsigned>(mins % 60), static_cast<unsigned>(this->soft_restarts_));
  this->last_disconnect_ = buf;
  ESP_LOGW(TAG, "Link lost: %s", buf);
  this->notify_status_();
}

void TrumaInetX::log_raw(uint32_t ms) {
  ESP_LOGI(TAG, "Logging every parameter message in full for %u s", static_cast<unsigned>(ms / 1000));
  this->session_.log_raw_for(ms);
}

void TrumaInetX::dump_bus() {
  ESP_LOGI(TAG, "Bus dump (%u devices):", static_cast<unsigned>(this->session_.bus.devices.size()));
  for (auto &d : this->session_.bus.devices) {
    ESP_LOGI(TAG, "-- 0x%04X (%u parameters)", d.first, static_cast<unsigned>(d.second.params.size()));
    for (auto &p : d.second.params) {
      ESP_LOGI(TAG, "   %s = %s", p.first.c_str(), p.second.value.to_string().c_str());
      delay(2);  // keep the log stream from overflowing
    }
  }
}

bool TrumaInetX::write_param(const std::string &topic, const std::string &param, int32_t value, uint16_t address) {
  return this->session_.write(topic, param, truma_inetx_proto::CborValue::make_int(value), address);
}

bool TrumaInetX::write_param_text(const std::string &topic, const std::string &param, const std::string &value,
                                  uint16_t address) {
  return this->session_.write(topic, param, truma_inetx_proto::CborValue::make_text(value), address);
}

// ---------------------------------------------------------------------------
// Status

void TrumaInetX::update_status_() {
  const uint32_t now = millis();
  std::string s;
  bool countdown = false;
  if (!this->ble_ready_done_) {
    s = tr("Bluetooth starting", "Bluetooth startet");
  } else {
    switch (this->link_) {
      case Link::NO_BLE:
        s = tr("Bluetooth starting", "Bluetooth startet");
        break;
      case Link::WAIT_ADV:
        if (this->pairing_) {
          char buf[112];
          snprintf(buf, sizeof(buf),
                   tr("Pairing mode (%us left): set the panel to add-device mode",
                      "Kopplungsmodus (noch %us): Panel auf \"Neues Geraet hinzufuegen\" stellen"),
                   static_cast<unsigned>((this->pairing_until_ - now) / 1000));
          s = buf;
          countdown = true;
        } else if (this->bonds_.empty()) {
          s = tr("Not paired", "Nicht gekoppelt");
        } else if (this->auth_failures_ >= 3) {
          s = tr("Panel rejects this device: pair again", "Panel lehnt den ESP ab: neu koppeln");
        } else if (static_cast<int32_t>(now - this->next_connect_ok_) < 0) {
          s = tr("Waiting to reconnect", "Warte auf neuen Verbindungsversuch");
        } else {
          s = tr("Searching for panel", "Suche Panel");
        }
        break;
      case Link::CONNECTING:
        s = this->pairing_ ? tr("Pairing: connecting", "Kopplung: verbinde") : tr("Connecting", "Verbinde");
        break;
      case Link::ENCRYPTING:
        s = this->pairing_ ? tr("Pairing: bonding", "Kopplung: tausche Schluessel")
                           : tr("Encrypting", "Verschluessele");
        break;
      case Link::SUBSCRIBING:
        s = tr("Subscribing", "Abonniere");
        break;
      case Link::SESSION:
        if (this->session_.running()) {
          s = tr("Connected", "Verbunden");
        } else {
          s = std::string(tr("Starting session (", "Starte Sitzung (")) + this->session_state_text_() + ")";
        }
        break;
    }
  }
  const bool changed = s != this->status_ || this->last_connected_ != this->is_connected() ||
                       this->last_paired_ != this->is_paired() || this->last_pairing_ != this->pairing_;
  if (!changed)
    return;
  // The pairing countdown changes every second; only report it every 10 s.
  if (s != this->status_ && countdown && this->status_is_countdown_ &&
      ((this->pairing_until_ - now) / 1000) % 10 != 0 && this->last_pairing_ == this->pairing_)
    return;
  this->status_ = s;
  this->status_is_countdown_ = countdown;
  this->notify_status_();
}

const char *TrumaInetX::session_state_text_() const {
  const char *en = this->session_.state_name();
  if (!this->german_)
    return en;
  static const char *const MAP[][2] = {{"registering", "Anmeldung"},  {"subscribing", "Abonnieren"},
                                       {"identifying", "Identifizieren"}, {"discovering", "Parameter abfragen"},
                                       {"measuring", "Messen"},        {"running", "laeuft"},
                                       {"idle", "inaktiv"},            {"failed", "Fehler"}};
  for (auto &m : MAP)
    if (strcmp(m[0], en) == 0)
      return m[1];
  return en;
}

void TrumaInetX::notify_status_() {
  this->last_connected_ = this->is_connected();
  this->last_paired_ = this->is_paired();
  this->last_pairing_ = this->pairing_;
  for (auto *l : this->listeners_)
    l->on_status();
}

}  // namespace truma_inetx
}  // namespace esphome
