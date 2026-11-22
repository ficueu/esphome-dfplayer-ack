#include "dfplayer_ack.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::dfplayer_ack {

static const char *const TAG = "dfplayer_ack";

// Commands sent to the module
static constexpr uint8_t CMD_NEXT = 0x01;
static constexpr uint8_t CMD_PREVIOUS = 0x02;
static constexpr uint8_t CMD_PLAY_FILE = 0x03;
static constexpr uint8_t CMD_VOLUME_UP = 0x04;
static constexpr uint8_t CMD_VOLUME_DOWN = 0x05;
static constexpr uint8_t CMD_SET_VOLUME = 0x06;
static constexpr uint8_t CMD_SET_EQ = 0x07;
static constexpr uint8_t CMD_PLAY_FILE_LOOP = 0x08;
static constexpr uint8_t CMD_SET_DEVICE = 0x09;
static constexpr uint8_t CMD_SLEEP = 0x0A;
static constexpr uint8_t CMD_RESET = 0x0C;
static constexpr uint8_t CMD_START = 0x0D;
static constexpr uint8_t CMD_PAUSE = 0x0E;
static constexpr uint8_t CMD_PLAY_FOLDER = 0x0F;
static constexpr uint8_t CMD_PLAY_MP3 = 0x12;
static constexpr uint8_t CMD_PLAY_ADVERT = 0x13;
static constexpr uint8_t CMD_PLAY_FOLDER_BIG = 0x14;
static constexpr uint8_t CMD_STOP = 0x16;
static constexpr uint8_t CMD_PLAY_FOLDER_LOOP = 0x17;
static constexpr uint8_t CMD_RANDOM = 0x18;
static constexpr uint8_t CMD_SET_REPEAT = 0x19;
static constexpr uint8_t CMD_QUERY_STATUS = 0x42;

// Messages sent by the module
static constexpr uint8_t MSG_USB_INSERTED = 0x3A;  // arg 1 = USB, 2 = TF
static constexpr uint8_t MSG_DEVICE_REMOVED = 0x3B;
static constexpr uint8_t MSG_FINISHED_USB = 0x3C;
static constexpr uint8_t MSG_FINISHED_TF = 0x3D;
static constexpr uint8_t MSG_FINISHED_FLASH = 0x3E;
static constexpr uint8_t MSG_ONLINE = 0x3F;  // sent after power-up / reset
static constexpr uint8_t MSG_ERROR = 0x40;
static constexpr uint8_t MSG_ACK = 0x41;

static constexpr uint32_t QUERY_INTERVAL_MS = 150;
static constexpr uint32_t BUSY_ERROR_MIN_DELAY_MS = 500;
static constexpr uint32_t FINISHED_DEDUPE_MS = 500;

// Command argument as a human reads it: "2/7" for folder commands, plain number otherwise
struct ArgStr {
  char s[12];
  ArgStr(uint8_t cmd, uint16_t arg) {
    if (cmd == 0x0F) {
      snprintf(this->s, sizeof(this->s), "%u/%u", arg >> 8, arg & 0xFF);
    } else if (cmd == 0x14) {
      snprintf(this->s, sizeof(this->s), "%u/%u", arg >> 12, arg & 0x0FFF);
    } else {
      snprintf(this->s, sizeof(this->s), "%u", arg);
    }
  }
};

// "7E FF 06 41 ..." for logs
struct HexFrame {
  char s[format_hex_pretty_size(10)];
  explicit HexFrame(const uint8_t *data) { format_hex_pretty_to(this->s, data, 10, ' '); }
};

// ------------------------------------------------------------------ lifecycle

void DFPlayerAck::setup() {
  this->boot_at_ = millis();
  this->ready_wait_start_ = this->boot_at_;
  if (this->busy_pin_ != nullptr) {
    this->busy_pin_->setup();
    this->playing_ = this->read_busy_playing_();
  }
  if (this->playing_binary_sensor_ != nullptr) {
    this->playing_binary_sensor_->publish_initial_state(this->playing_);
    this->playing_published_ = true;
  }
  publish_(this->retry_count_sensor_, 0);
  publish_(this->failed_count_sensor_, 0);
  publish_(this->reset_count_sensor_, 0);
  publish_(this->last_error_sensor_, 0);
}

void DFPlayerAck::dump_config() {
  static const char *const VERIFY_NAMES[] = {"none", "busy_pin", "query"};
  ESP_LOGCONFIG(TAG,
                "DFPlayer (ACK/retry):\n"
                "  Wait for ACK: %s\n"
                "  ACK timeout: %u ms\n"
                "  Max retries: %u (non-idempotent on ACK timeout: %s)\n"
                "  Retry delay: %u ms\n"
                "  Command interval: %u ms\n"
                "  Playback verification: %s (timeout %u ms)\n"
                "  Init timeout: %u ms\n"
                "  Restore volume/EQ after module reset: %s\n"
                "  Validate RX checksum: %s\n"
                "  Queue size: %u",
                YESNO(this->use_ack_), (unsigned)this->ack_timeout_, this->max_retries_,
                YESNO(this->retry_non_idempotent_), (unsigned)this->retry_delay_, (unsigned)this->command_interval_,
                VERIFY_NAMES[this->verify_mode_], (unsigned)this->verify_timeout_, (unsigned)this->init_timeout_,
                YESNO(this->restore_on_reset_), YESNO(this->validate_checksum_), this->queue_size_);
  LOG_PIN("  BUSY pin: ", this->busy_pin_);
  LOG_SENSOR("  ", "Retry count", this->retry_count_sensor_);
  LOG_SENSOR("  ", "Failed count", this->failed_count_sensor_);
  LOG_SENSOR("  ", "Last error", this->last_error_sensor_);
  LOG_SENSOR("  ", "Reset count", this->reset_count_sensor_);
  LOG_BINARY_SENSOR("  ", "Playing", this->playing_binary_sensor_);
}

void DFPlayerAck::loop() {
  this->read_uart_();

  if (this->busy_pin_ != nullptr) {
    bool p = this->read_busy_playing_();
    if (p && this->state_ != State::IDLE) this->seen_playing_ = true;
    this->set_playing_(p);
  }

  const uint32_t now = millis();
  switch (this->state_) {
    case State::RETRY_WAIT:
      // ACK lost, but playback started/stopped while we were waiting to resend
      if (this->retry_after_ack_timeout_ && this->busy_confirms_(this->queue_.front())) {
        ESP_LOGW(TAG, "%s: no ACK, but BUSY pin confirms it was executed", cmd_to_str(this->queue_.front().cmd));
        this->complete_();
        break;
      }
      [[fallthrough]];
    case State::IDLE:
      if (!this->queue_.empty() && (int32_t)(now - this->next_tx_at_) >= 0) this->transmit_();
      break;
    case State::WAIT_ACK:
      if (now - this->state_since_ >= this->ack_timeout_) this->attempt_failed_(ERR_ACK_TIMEOUT, true);
      break;
    case State::VERIFY:
      this->process_verify_();
      break;
  }
}

// ------------------------------------------------------------------ public commands

void DFPlayerAck::next() { this->enqueue_(CMD_NEXT, 0, Expect::PLAYING, 0); }
void DFPlayerAck::previous() { this->enqueue_(CMD_PREVIOUS, 0, Expect::PLAYING, 0); }
void DFPlayerAck::play_mp3(uint16_t file) { this->enqueue_(CMD_PLAY_MP3, file, Expect::PLAYING, F_IDEMPOTENT); }
void DFPlayerAck::play_file(uint16_t file) { this->enqueue_(CMD_PLAY_FILE, file, Expect::PLAYING, F_IDEMPOTENT); }
void DFPlayerAck::play_file_loop(uint16_t file) {
  this->enqueue_(CMD_PLAY_FILE_LOOP, file, Expect::PLAYING, F_IDEMPOTENT);
}
void DFPlayerAck::play_folder_loop(uint16_t folder) {
  this->enqueue_(CMD_PLAY_FOLDER_LOOP, folder, Expect::PLAYING, F_IDEMPOTENT);
}
void DFPlayerAck::play_folder(uint16_t folder, uint16_t file) {
  if (folder < 100 && file < 256) {
    this->enqueue_(CMD_PLAY_FOLDER, (folder << 8) | file, Expect::PLAYING, F_IDEMPOTENT);
  } else if (folder <= 15 && file <= 3000) {
    this->enqueue_(CMD_PLAY_FOLDER_BIG, (folder << 12) | file, Expect::PLAYING, F_IDEMPOTENT);
  } else {
    ESP_LOGE(TAG, "Cannot play folder %u file %u (folder<100 & file<256, or folder<=15 & file<=3000)", folder, file);
    this->reject_(CMD_PLAY_FOLDER, ERR_OUT_OF_RANGE);
  }
}
// Advert interrupts the current track and resumes it afterwards; BUSY stays low, so no verification.
void DFPlayerAck::play_advert(uint16_t file) { this->enqueue_(CMD_PLAY_ADVERT, file, Expect::NONE, 0); }
void DFPlayerAck::volume_up() { this->enqueue_(CMD_VOLUME_UP, 0, Expect::NONE, 0); }
void DFPlayerAck::volume_down() { this->enqueue_(CMD_VOLUME_DOWN, 0, Expect::NONE, 0); }
void DFPlayerAck::set_volume(uint8_t volume) {
  if (volume > 30) volume = 30;
  this->enqueue_(CMD_SET_VOLUME, volume, Expect::NONE, F_IDEMPOTENT | F_COALESCE);
}
void DFPlayerAck::set_eq(EqPreset preset) {
  this->enqueue_(CMD_SET_EQ, preset, Expect::NONE, F_IDEMPOTENT | F_COALESCE);
}
void DFPlayerAck::set_device(Device device) { this->enqueue_(CMD_SET_DEVICE, device, Expect::NONE, F_IDEMPOTENT); }
void DFPlayerAck::sleep() { this->enqueue_(CMD_SLEEP, 0, Expect::NONE, F_IDEMPOTENT); }
// The module reboots on reset, so its ACK is unreliable. We don't wait for it; instead the
// queue pauses until the module announces itself (0x3F) or init_timeout passes.
void DFPlayerAck::reset() { this->enqueue_(CMD_RESET, 0, Expect::NONE, F_IDEMPOTENT | F_NO_ACK); }
void DFPlayerAck::start() { this->enqueue_(CMD_START, 0, Expect::PLAYING, F_IDEMPOTENT); }
void DFPlayerAck::pause() { this->enqueue_(CMD_PAUSE, 0, Expect::STOPPED, F_IDEMPOTENT); }
void DFPlayerAck::stop() { this->enqueue_(CMD_STOP, 0, Expect::STOPPED, F_IDEMPOTENT); }
void DFPlayerAck::random() { this->enqueue_(CMD_RANDOM, 0, Expect::PLAYING, F_IDEMPOTENT); }
void DFPlayerAck::set_current_track_repeat(bool enable) {
  this->enqueue_(CMD_SET_REPEAT, enable ? 0x00 : 0x01, Expect::NONE, F_IDEMPOTENT);
}

void DFPlayerAck::clear_queue() {
  size_t keep = this->in_flight_() ? 1 : 0;
  size_t dropped = this->queue_.size() > keep ? this->queue_.size() - keep : 0;
  this->queue_.resize(keep);
  if (keep == 0) this->state_ = State::IDLE;
  ESP_LOGD(TAG, "Queue cleared (%u command(s) dropped)", (unsigned)dropped);
}

// ------------------------------------------------------------------ queue

void DFPlayerAck::enqueue_(uint8_t cmd, uint16_t arg, Expect expect, uint8_t flags) {
  // Only the tail may be merged, so ordering relative to other commands never changes.
  if ((flags & F_COALESCE) && !this->queue_.empty()) {
    Command &tail = this->queue_.back();
    bool tail_busy = this->queue_.size() == 1 && this->state_ != State::IDLE;
    if (tail.cmd == cmd && !tail_busy) {
      ESP_LOGV(TAG, "Merging %s %u -> %u", cmd_to_str(cmd), tail.arg, arg);
      tail.arg = arg;
      return;
    }
  }
  if (this->queue_.size() >= this->queue_size_) {
    ESP_LOGE(TAG, "Queue full (%u), dropping %s %s", this->queue_size_, cmd_to_str(cmd), ArgStr(cmd, arg).s);
    this->reject_(cmd, ERR_QUEUE_FULL);
    return;
  }
  ESP_LOGV(TAG, "Queued %s %s (queue %u)", cmd_to_str(cmd), ArgStr(cmd, arg).s, (unsigned)this->queue_.size() + 1);
  this->queue_.push_back(Command{cmd, arg, expect, flags, 0, 0});
}

void DFPlayerAck::reject_(uint8_t cmd, uint8_t reason) {
  this->failed_count_++;
  publish_(this->failed_count_sensor_, this->failed_count_);
  this->set_last_error_(reason);
  this->failed_callback_.call(cmd, reason);
}

void DFPlayerAck::transmit_() {
  Command &c = this->queue_.front();
  const uint32_t now = millis();
  c.attempts++;
  if (c.attempts == 1) c.first_tx_at = now;

  this->retry_after_ack_timeout_ = false;
  this->playing_at_send_ = this->busy_pin_ != nullptr ? this->read_busy_playing_() : this->playing_;
  this->seen_playing_ = this->busy_pin_ != nullptr && this->playing_at_send_;
  this->last_status_ = -1;
  this->query_pending_ = false;
  if (c.expect == Expect::PLAYING) this->finished_dedupe_valid_ = false;

  if (c.attempts == 1) {
    ESP_LOGD(TAG, "-> %s %s", cmd_to_str(c.cmd), ArgStr(c.cmd, c.arg).s);
  } else {
    ESP_LOGW(TAG, "-> %s %s (attempt %u/%u)", cmd_to_str(c.cmd), ArgStr(c.cmd, c.arg).s, c.attempts,
             this->max_retries_ + 1);
  }

  bool wait_ack = this->use_ack_ && !(c.flags & F_NO_ACK);
  this->write_frame_(c.cmd, c.arg, wait_ack);
  this->state_since_ = now;
  this->ack_latency_ = UINT32_MAX;
  if (wait_ack) {
    this->state_ = State::WAIT_ACK;
  } else {
    this->handle_ack_(false);
  }
}

void DFPlayerAck::write_frame_(uint8_t cmd, uint16_t arg, bool feedback) {
  uint8_t buf[10] = {
      0x7E, 0xFF, 0x06, cmd, (uint8_t)(feedback ? 0x01 : 0x00), (uint8_t)(arg >> 8), (uint8_t)(arg & 0xFF),
      0x00, 0x00, 0xEF};
  uint16_t sum = 0;
  for (uint8_t i = 1; i < 7; i++) sum += buf[i];
  uint16_t checksum = (uint16_t)(0 - sum);
  buf[7] = checksum >> 8;
  buf[8] = checksum & 0xFF;
  this->write_array(buf, sizeof(buf));
  this->last_tx_at_ = millis();
  ESP_LOGVV(TAG, "TX %s", HexFrame(buf).s);
}

void DFPlayerAck::handle_ack_(bool from_module) {
  Command &c = this->queue_.front();
  const uint32_t now = millis();
  if (from_module) this->ack_latency_ = now - this->last_tx_at_;

  switch (c.cmd) {
    case CMD_SET_VOLUME:
      this->last_volume_ = c.arg;
      break;
    case CMD_VOLUME_UP:
      if (this->last_volume_ >= 0 && this->last_volume_ < 30) this->last_volume_++;
      break;
    case CMD_VOLUME_DOWN:
      if (this->last_volume_ > 0) this->last_volume_--;
      break;
    case CMD_SET_EQ:
      this->last_eq_ = c.arg;
      break;
    case CMD_RESET:
      this->ready_ = false;
      this->reset_expected_ = true;
      this->ready_wait_start_ = now;
      if (this->busy_pin_ == nullptr) this->set_playing_(false);
      break;
    case CMD_SLEEP:
      if (this->busy_pin_ == nullptr) this->set_playing_(false);
      break;
    default:
      break;
  }

  if (c.expect != Expect::NONE && this->verify_mode_ != VERIFY_NONE) {
    this->state_ = State::VERIFY;
    this->state_since_ = now;
    this->last_status_ = -1;
    this->query_pending_ = false;
    this->next_query_at_ = now + QUERY_INTERVAL_MS;
    this->process_verify_();
    return;
  }
  if (this->busy_pin_ == nullptr && c.expect != Expect::NONE) this->set_playing_(c.expect == Expect::PLAYING);
  this->complete_();
}

void DFPlayerAck::process_verify_() {
  const Command &c = this->queue_.front();
  const uint32_t now = millis();
  if (this->verify_satisfied_(c)) {
    if (this->busy_pin_ == nullptr) this->set_playing_(c.expect == Expect::PLAYING);
    this->complete_();
    return;
  }
  if (now - this->state_since_ >= this->verify_timeout_) {
    this->attempt_failed_(ERR_NOT_CONFIRMED, false);
    return;
  }
  if (this->verify_mode_ == VERIFY_QUERY) {
    if (this->query_pending_ && now - this->query_sent_at_ >= this->ack_timeout_) {
      ESP_LOGV(TAG, "Status query unanswered");
      this->query_pending_ = false;
      this->next_query_at_ = now;
    }
    if (!this->query_pending_ && (int32_t)(now - this->next_query_at_) >= 0 &&
        now - this->last_tx_at_ >= this->command_interval_) {
      this->write_frame_(CMD_QUERY_STATUS, 0, false);
      this->query_pending_ = true;
      this->query_sent_at_ = now;
    }
  }
}

// True only for a state *change* matching the command (stopped -> playing, playing -> stopped),
// so it can stand in for a lost ACK without being fooled by a track that was already playing.
bool DFPlayerAck::busy_confirms_(const Command &c) {
  if (this->busy_pin_ == nullptr || c.expect == Expect::NONE) return false;
  if (c.expect == Expect::PLAYING) return !this->playing_at_send_ && this->seen_playing_;
  return this->playing_at_send_ && !this->read_busy_playing_();
}

bool DFPlayerAck::verify_satisfied_(const Command &c) {
  switch (this->verify_mode_) {
    case VERIFY_BUSY_PIN:
      return c.expect == Expect::PLAYING ? this->seen_playing_ : !this->read_busy_playing_();
    case VERIFY_QUERY:
      if (c.expect == Expect::PLAYING) return this->seen_playing_;
      return this->last_status_ == 0 || this->last_status_ == 2;
    default:
      return true;
  }
}

void DFPlayerAck::attempt_failed_(uint8_t reason, bool ack_timeout) {
  Command &c = this->queue_.front();

  // Module still booting (power-up or after reset): resend without burning attempts.
  if ((ack_timeout || reason == ERR_BUSY) && this->in_init_window_()) {
    c.attempts--;
    ESP_LOGD(TAG, "%s: module not ready yet, waiting", cmd_to_str(c.cmd));
    this->schedule_retry_(std::max(this->retry_delay_, (uint32_t)250), ack_timeout);
    return;
  }

  // No ACK, but the BUSY line shows the module did what we asked: the ACK got lost, not the command.
  if (ack_timeout && this->busy_confirms_(c)) {
    ESP_LOGW(TAG, "%s: no ACK, but BUSY pin confirms it was executed", cmd_to_str(c.cmd));
    this->complete_();
    return;
  }

  if (ack_timeout && !(c.flags & F_IDEMPOTENT) && !this->retry_non_idempotent_) {
    ESP_LOGE(TAG, "%s: no ACK; not resending (could execute twice)", cmd_to_str(c.cmd));
    this->fail_(reason);
    return;
  }

  if (c.attempts > this->max_retries_) {
    this->fail_(reason);
    return;
  }

  this->retry_count_++;
  publish_(this->retry_count_sensor_, this->retry_count_);
  uint32_t delay = this->retry_delay_;
  if (reason == ERR_BUSY) delay = std::max(delay, BUSY_ERROR_MIN_DELAY_MS);
  ESP_LOGW(TAG, "%s %s: %s, retry %u/%u in %u ms", cmd_to_str(c.cmd), ArgStr(c.cmd, c.arg).s, error_to_str(reason),
           c.attempts, this->max_retries_, (unsigned)delay);
  this->schedule_retry_(delay, ack_timeout);
}

void DFPlayerAck::schedule_retry_(uint32_t delay, bool after_ack_timeout) {
  const uint32_t now = millis();
  this->state_ = State::RETRY_WAIT;
  this->state_since_ = now;
  this->next_tx_at_ = now + delay;
  this->query_pending_ = false;
  this->retry_after_ack_timeout_ = after_ack_timeout;
}

void DFPlayerAck::complete_() {
  const Command &c = this->queue_.front();
  const uint32_t now = millis();
  if (this->ack_latency_ != UINT32_MAX) {
    ESP_LOGD(TAG, "OK %s %s (attempt %u, %u ms total, ACK after %u ms)", cmd_to_str(c.cmd), ArgStr(c.cmd, c.arg).s,
             c.attempts, (unsigned)(now - c.first_tx_at), (unsigned)this->ack_latency_);
  } else {
    ESP_LOGD(TAG, "OK %s %s (attempt %u, %u ms total, no ACK)", cmd_to_str(c.cmd), ArgStr(c.cmd, c.arg).s, c.attempts,
             (unsigned)(now - c.first_tx_at));
  }
  this->queue_.pop_front();
  this->state_ = State::IDLE;
  this->query_pending_ = false;
  this->next_tx_at_ = now + this->command_interval_;
}

void DFPlayerAck::fail_(uint8_t reason) {
  Command c = this->queue_.front();
  this->queue_.pop_front();
  this->state_ = State::IDLE;
  this->query_pending_ = false;
  this->next_tx_at_ = millis() + this->command_interval_;
  ESP_LOGE(TAG, "FAILED %s %s after %u attempt(s): %s", cmd_to_str(c.cmd), ArgStr(c.cmd, c.arg).s, c.attempts,
           error_to_str(reason));
  this->reject_(c.cmd, reason);
}

void DFPlayerAck::set_playing_(bool playing) {
  if (playing == this->playing_ && this->playing_published_) return;
  this->playing_ = playing;
  if (this->playing_binary_sensor_ != nullptr) {
    this->playing_binary_sensor_->publish_state(playing);
    this->playing_published_ = true;
  }
}

void DFPlayerAck::set_last_error_(uint8_t code) {
  this->last_error_ = code;
  publish_(this->last_error_sensor_, code);
}

// ------------------------------------------------------------------ RX

void DFPlayerAck::read_uart_() {
  uint8_t byte;
  while (this->available() > 0 && this->read_byte(&byte)) {
    switch (this->rx_pos_) {
      case 0:
        if (byte != 0x7E) continue;
        break;
      case 1:
      case 2:
        if (byte != (this->rx_pos_ == 1 ? 0xFF : 0x06)) {
          this->rx_pos_ = 0;
          if (byte == 0x7E) this->rx_buf_[this->rx_pos_++] = byte;
          continue;
        }
        break;
      default:
        break;
    }
    this->rx_buf_[this->rx_pos_++] = byte;
    if (this->rx_pos_ < sizeof(this->rx_buf_)) continue;

    this->rx_pos_ = 0;
    if (this->rx_buf_[9] != 0xEF) {
      ESP_LOGW(TAG, "RX bad end byte: %s", HexFrame(this->rx_buf_).s);
      continue;
    }
    if (this->validate_checksum_) {
      uint16_t sum = 0;
      for (uint8_t i = 1; i < 7; i++) sum += this->rx_buf_[i];
      uint16_t checksum = encode_uint16(this->rx_buf_[7], this->rx_buf_[8]);
      if ((uint16_t)(sum + checksum) != 0) {
        ESP_LOGW(TAG, "RX checksum mismatch (frame dropped): %s", HexFrame(this->rx_buf_).s);
        continue;
      }
    }
    ESP_LOGVV(TAG, "RX %s", HexFrame(this->rx_buf_).s);
    this->handle_frame_(this->rx_buf_[3], encode_uint16(this->rx_buf_[5], this->rx_buf_[6]));
  }
}

void DFPlayerAck::handle_frame_(uint8_t cmd, uint16_t arg) {
  // "busy / initialising" is the one answer that means the module is *not* ready yet
  if (!this->ready_ && !(cmd == MSG_ERROR && (arg & 0xFF) == ERR_BUSY)) {
    this->ready_ = true;
    ESP_LOGI(TAG, "Module is responding");
  }

  switch (cmd) {
    case MSG_ACK:
      if (this->state_ == State::WAIT_ACK) {
        this->handle_ack_();
      } else if (this->state_ == State::RETRY_WAIT && this->retry_after_ack_timeout_) {
        // ACK came in after the timeout but before the resend: the command did arrive.
        ESP_LOGW(TAG, "Late ACK for %s, accepting it (consider a longer ack_timeout)",
                 cmd_to_str(this->queue_.front().cmd));
        this->handle_ack_();
      } else {
        ESP_LOGV(TAG, "Unexpected ACK ignored");
      }
      break;
    case MSG_ERROR:
      this->handle_error_frame_(arg & 0xFF);
      break;
    case CMD_QUERY_STATUS:
      this->handle_status_(arg & 0xFF);
      break;
    case MSG_FINISHED_USB:
    case MSG_FINISHED_TF:
    case MSG_FINISHED_FLASH:
      this->handle_finished_(arg);
      break;
    case MSG_ONLINE:
      this->handle_module_online_(arg);
      break;
    case MSG_USB_INSERTED:
      ESP_LOGI(TAG, "%s inserted", arg == 1 ? "USB" : "TF card");
      break;
    case MSG_DEVICE_REMOVED:
      ESP_LOGW(TAG, "%s removed", arg == 1 ? "USB" : "TF card");
      break;
    default:
      ESP_LOGD(TAG, "Unhandled frame cmd 0x%02X arg 0x%04X", cmd, arg);
      break;
  }
}

void DFPlayerAck::handle_error_frame_(uint8_t code) {
  if (code == ERR_OUT_OF_RANGE || code == ERR_NOT_FOUND || code == ERR_SLEEP_ENTERED || code == ERR_SLEEP_ENTERED_ALT) {
    if (this->busy_pin_ == nullptr) this->set_playing_(false);
  }

  switch (this->state_) {
    case State::WAIT_ACK:
      ESP_LOGW(TAG, "Module error for %s: %s", cmd_to_str(this->queue_.front().cmd), error_to_str(code));
      this->set_last_error_(code);
      if (is_retriable_error_(code)) {
        this->attempt_failed_(code, false);
      } else {
        this->fail_(code);
      }
      break;
    case State::VERIFY:
      // A transient error while a status query is outstanding belongs to the query, not to the command.
      if (this->query_pending_ && is_retriable_error_(code)) {
        ESP_LOGV(TAG, "Status query rejected: %s", error_to_str(code));
        this->query_pending_ = false;
        this->next_query_at_ = millis() + QUERY_INTERVAL_MS;
        break;
      }
      ESP_LOGW(TAG, "Module error for %s: %s", cmd_to_str(this->queue_.front().cmd), error_to_str(code));
      this->set_last_error_(code);
      if (is_retriable_error_(code)) {
        this->attempt_failed_(code, false);
      } else {
        this->fail_(code);
      }
      break;
    default:
      ESP_LOGW(TAG, "Module error (no command pending): %s", error_to_str(code));
      this->set_last_error_(code);
      break;
  }
}

void DFPlayerAck::handle_status_(uint8_t status) {
  // low byte: 0 = stopped, 1 = playing, 2 = paused
  ESP_LOGV(TAG, "Status: %u", status);
  this->query_pending_ = false;
  this->next_query_at_ = millis() + QUERY_INTERVAL_MS;
  this->last_status_ = status;
  if (status == 1 && this->state_ == State::VERIFY) this->seen_playing_ = true;
  if (this->busy_pin_ == nullptr) this->set_playing_(status == 1);
}

void DFPlayerAck::handle_finished_(uint16_t track) {
  const uint32_t now = millis();
  // Many modules report "finished" twice for the same track
  if (this->finished_dedupe_valid_ && track == this->last_finished_track_ &&
      now - this->last_finished_at_ < FINISHED_DEDUPE_MS) {
    ESP_LOGV(TAG, "Duplicate finished event for track %u ignored", track);
    return;
  }
  this->finished_dedupe_valid_ = true;
  this->last_finished_track_ = track;
  this->last_finished_at_ = now;
  ESP_LOGD(TAG, "Finished playing track %u", track);

  // A track that already finished certainly started (very short files vs. slow status polling)
  if (this->state_ == State::VERIFY) this->seen_playing_ = true;
  if (this->busy_pin_ == nullptr) this->set_playing_(false);
  this->finished_callback_.call(track);
}

void DFPlayerAck::handle_module_online_(uint16_t devices) {
  this->ready_ = true;
  if (this->reset_expected_) {
    ESP_LOGI(TAG, "Module back online after reset (storage 0x%02X)", devices);
    this->reset_expected_ = false;
  } else if (millis() - this->boot_at_ < this->init_timeout_) {
    ESP_LOGI(TAG, "Module powered up (storage 0x%02X)", devices);
  } else {
    this->reset_count_++;
    publish_(this->reset_count_sensor_, this->reset_count_);
    ESP_LOGW(TAG, "Module reset itself (brown-out?) - storage 0x%02X, resets so far: %u", devices,
             (unsigned)this->reset_count_);
    this->reset_callback_.call();
  }
  if (this->busy_pin_ == nullptr) this->set_playing_(false);

  // Whatever was in flight died with the reset: put it back without counting the attempt.
  if (this->in_flight_() || this->state_ == State::RETRY_WAIT) {
    if (this->queue_.front().attempts > 0) this->queue_.front().attempts--;
    this->state_ = State::IDLE;
    this->query_pending_ = false;
  }

  if (this->restore_on_reset_) {
    // pushed to the front so they go out before anything else (volume first)
    if (this->last_eq_ >= 0 && this->last_eq_ != NORMAL)
      this->queue_.push_front(Command{CMD_SET_EQ, (uint16_t)this->last_eq_, Expect::NONE, F_IDEMPOTENT, 0, 0});
    if (this->last_volume_ >= 0) {
      ESP_LOGI(TAG, "Restoring volume %d", this->last_volume_);
      this->queue_.push_front(Command{CMD_SET_VOLUME, (uint16_t)this->last_volume_, Expect::NONE, F_IDEMPOTENT, 0, 0});
    }
  }
  this->next_tx_at_ = millis() + this->command_interval_;
}

// ------------------------------------------------------------------ helpers

bool DFPlayerAck::is_retriable_error_(uint8_t code) {
  return code == ERR_BUSY || code == ERR_SERIAL || code == ERR_CHECKSUM || code == ERR_SD_READ;
}

const char *DFPlayerAck::error_to_str(uint8_t code) {
  switch (code) {
    case ERR_NONE:
      return "none";
    case ERR_BUSY:
      return "module busy/initialising";
    case ERR_SLEEPING:
      return "module in sleep mode";
    case ERR_SERIAL:
      return "serial receive error";
    case ERR_CHECKSUM:
      return "checksum error";
    case ERR_OUT_OF_RANGE:
      return "track out of range";
    case ERR_NOT_FOUND:
      return "track not found";
    case ERR_INSERTION:
      return "advert needs a track playing";
    case ERR_SD_READ:
      return "SD card read failed";
    case ERR_SLEEP_ENTERED:
    case ERR_SLEEP_ENTERED_ALT:
      return "entered sleep mode";
    case ERR_ACK_TIMEOUT:
      return "no ACK";
    case ERR_NOT_CONFIRMED:
      return "playback state not confirmed";
    case ERR_QUEUE_FULL:
      return "queue full";
    default:
      return "unknown error";
  }
}

const char *DFPlayerAck::cmd_to_str(uint8_t cmd) {
  switch (cmd) {
    case CMD_NEXT:
      return "next";
    case CMD_PREVIOUS:
      return "previous";
    case CMD_PLAY_FILE:
      return "play_file";
    case CMD_VOLUME_UP:
      return "volume_up";
    case CMD_VOLUME_DOWN:
      return "volume_down";
    case CMD_SET_VOLUME:
      return "set_volume";
    case CMD_SET_EQ:
      return "set_eq";
    case CMD_PLAY_FILE_LOOP:
      return "play_file_loop";
    case CMD_SET_DEVICE:
      return "set_device";
    case CMD_SLEEP:
      return "sleep";
    case CMD_RESET:
      return "reset";
    case CMD_START:
      return "start";
    case CMD_PAUSE:
      return "pause";
    case CMD_PLAY_FOLDER:
    case CMD_PLAY_FOLDER_BIG:
      return "play_folder";
    case CMD_PLAY_MP3:
      return "play_mp3";
    case CMD_PLAY_ADVERT:
      return "play_advert";
    case CMD_STOP:
      return "stop";
    case CMD_PLAY_FOLDER_LOOP:
      return "play_folder_loop";
    case CMD_RANDOM:
      return "random";
    case CMD_SET_REPEAT:
      return "set_repeat";
    case CMD_QUERY_STATUS:
      return "query_status";
    default:
      return "cmd";
  }
}

}  // namespace esphome::dfplayer_ack
