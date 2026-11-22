#pragma once

#include <deque>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome::dfplayer_ack {

enum EqPreset : uint8_t { NORMAL = 0, POP = 1, ROCK = 2, JAZZ = 3, CLASSIC = 4, BASS = 5 };
enum Device : uint8_t { USB = 1, TF_CARD = 2 };
enum VerifyMode : uint8_t { VERIFY_NONE = 0, VERIFY_BUSY_PIN = 1, VERIFY_QUERY = 2 };

/// Error codes passed to on_command_failed (`error`) and published by the last_error sensor.
/// 0x01..0x0A are reported by the module itself in a 0x40 frame, 0x80+ are generated here.
enum ErrorCode : uint8_t {
  ERR_NONE = 0x00,
  ERR_BUSY = 0x01,           // module busy / still initialising       -> retried
  ERR_SLEEPING = 0x02,       // module is in sleep mode                  -> not retried
  ERR_SERIAL = 0x03,         // frame not received completely           -> retried
  ERR_CHECKSUM = 0x04,       // module got a bad checksum                -> retried
  ERR_OUT_OF_RANGE = 0x05,   // track number out of range                -> not retried
  ERR_NOT_FOUND = 0x06,      // track not found                          -> not retried
  ERR_INSERTION = 0x07,      // advert only allowed while playing        -> not retried
  ERR_SD_READ = 0x08,        // SD read failed                           -> retried
  ERR_SLEEP_ENTERED = 0x09,  // entered sleep (0x0A on some datasheets)  -> not retried
  ERR_SLEEP_ENTERED_ALT = 0x0A,
  ERR_ACK_TIMEOUT = 0x80,    // no ACK after all attempts
  ERR_NOT_CONFIRMED = 0x81,  // ACK ok, but BUSY/status never confirmed the expected state
  ERR_QUEUE_FULL = 0x82,     // command dropped, queue full
};

enum class Expect : uint8_t { NONE, PLAYING, STOPPED };

static constexpr uint8_t F_IDEMPOTENT = 1 << 0;  // safe to resend when we don't know if it was executed
static constexpr uint8_t F_NO_ACK = 1 << 1;      // don't wait for 0x41 (reset)
static constexpr uint8_t F_COALESCE = 1 << 2;    // replace an identical command waiting at the tail

struct Command {
  uint8_t cmd;
  uint16_t arg;
  Expect expect;
  uint8_t flags;
  uint8_t attempts;
  uint32_t first_tx_at;
};

class DFPlayerAck : public uart::UARTDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- configuration ----
  void set_max_retries(uint8_t v) { this->max_retries_ = v; }
  void set_ack_timeout(uint32_t v) { this->ack_timeout_ = v; }
  void set_retry_delay(uint32_t v) { this->retry_delay_ = v; }
  void set_command_interval(uint32_t v) { this->command_interval_ = v; }
  void set_use_ack(bool v) { this->use_ack_ = v; }
  void set_retry_non_idempotent(bool v) { this->retry_non_idempotent_ = v; }
  void set_verify_mode(VerifyMode v) { this->verify_mode_ = v; }
  void set_verify_timeout(uint32_t v) { this->verify_timeout_ = v; }
  void set_init_timeout(uint32_t v) { this->init_timeout_ = v; }
  void set_restore_on_reset(bool v) { this->restore_on_reset_ = v; }
  void set_validate_checksum(bool v) { this->validate_checksum_ = v; }
  void set_queue_size(uint8_t v) { this->queue_size_ = v; }
  void set_busy_pin(GPIOPin *pin) { this->busy_pin_ = pin; }
  void set_retry_count_sensor(sensor::Sensor *s) { this->retry_count_sensor_ = s; }
  void set_failed_count_sensor(sensor::Sensor *s) { this->failed_count_sensor_ = s; }
  void set_last_error_sensor(sensor::Sensor *s) { this->last_error_sensor_ = s; }
  void set_reset_count_sensor(sensor::Sensor *s) { this->reset_count_sensor_ = s; }
  void set_playing_binary_sensor(binary_sensor::BinarySensor *s) { this->playing_binary_sensor_ = s; }

  // ---- commands (all of them are queued and confirmed) ----
  void next();
  void previous();
  void play_mp3(uint16_t file);
  void play_file(uint16_t file);
  void play_file_loop(uint16_t file);
  void play_file(uint16_t file, bool loop) { loop ? this->play_file_loop(file) : this->play_file(file); }
  void play_folder(uint16_t folder, uint16_t file);
  void play_folder_loop(uint16_t folder);
  void play_folder(uint16_t folder, uint16_t file, bool loop) {
    loop ? this->play_folder_loop(folder) : this->play_folder(folder, file);
  }
  void play_advert(uint16_t file);
  void volume_up();
  void volume_down();
  void set_volume(uint8_t volume);
  void set_eq(EqPreset preset);
  void set_device(Device device);
  void sleep();
  void reset();
  void start();
  void pause();
  void stop();
  void random();
  void set_current_track_repeat(bool enable);
  void clear_queue();

  // ---- state for lambdas ----
  bool is_playing() const { return this->playing_; }
  size_t queue_length() const { return this->queue_.size(); }
  uint32_t get_retry_count() const { return this->retry_count_; }
  uint32_t get_failed_count() const { return this->failed_count_; }
  uint32_t get_reset_count() const { return this->reset_count_; }
  uint8_t get_last_error() const { return this->last_error_; }
  static const char *error_to_str(uint8_t code);
  static const char *cmd_to_str(uint8_t cmd);

  template <typename F>
  void add_on_finished_playback_callback(F &&cb) {
    this->finished_callback_.add(std::forward<F>(cb));
  }
  template <typename F>
  void add_on_command_failed_callback(F &&cb) {
    this->failed_callback_.add(std::forward<F>(cb));
  }
  template <typename F>
  void add_on_module_reset_callback(F &&cb) {
    this->reset_callback_.add(std::forward<F>(cb));
  }

 protected:
  enum class State : uint8_t { IDLE, WAIT_ACK, VERIFY, RETRY_WAIT };

  void enqueue_(uint8_t cmd, uint16_t arg, Expect expect, uint8_t flags);
  void reject_(uint8_t cmd, uint8_t reason);
  void transmit_();
  void write_frame_(uint8_t cmd, uint16_t arg, bool feedback);
  void read_uart_();
  void handle_frame_(uint8_t cmd, uint16_t arg);
  void handle_ack_(bool from_module = true);
  void handle_error_frame_(uint8_t code);
  void handle_status_(uint8_t status);
  void handle_finished_(uint16_t track);
  void handle_module_online_(uint16_t devices);
  void process_verify_();
  bool verify_satisfied_(const Command &c);
  bool busy_confirms_(const Command &c);
  void attempt_failed_(uint8_t reason, bool ack_timeout);
  void schedule_retry_(uint32_t delay, bool after_ack_timeout);
  void complete_();
  void fail_(uint8_t reason);
  void set_playing_(bool playing);
  bool read_busy_playing_() { return !this->busy_pin_->digital_read(); }  // BUSY is LOW while playing
  void set_last_error_(uint8_t code);
  bool in_init_window_() const { return !this->ready_ && (millis() - this->ready_wait_start_) < this->init_timeout_; }
  bool in_flight_() const { return this->state_ == State::WAIT_ACK || this->state_ == State::VERIFY; }
  static bool is_retriable_error_(uint8_t code);
  static void publish_(sensor::Sensor *s, float v) {
    if (s != nullptr) s->publish_state(v);
  }

  // configuration
  uint8_t max_retries_{3};
  uint32_t ack_timeout_{300};
  uint32_t retry_delay_{100};
  uint32_t command_interval_{50};
  bool use_ack_{true};
  bool retry_non_idempotent_{false};
  VerifyMode verify_mode_{VERIFY_NONE};
  uint32_t verify_timeout_{1500};
  uint32_t init_timeout_{5000};
  bool restore_on_reset_{true};
  bool validate_checksum_{true};
  uint8_t queue_size_{16};
  GPIOPin *busy_pin_{nullptr};
  sensor::Sensor *retry_count_sensor_{nullptr};
  sensor::Sensor *failed_count_sensor_{nullptr};
  sensor::Sensor *last_error_sensor_{nullptr};
  sensor::Sensor *reset_count_sensor_{nullptr};
  binary_sensor::BinarySensor *playing_binary_sensor_{nullptr};

  // command pipeline
  std::deque<Command> queue_;
  State state_{State::IDLE};
  uint32_t state_since_{0};
  uint32_t next_tx_at_{0};
  uint32_t last_tx_at_{0};
  uint32_t ack_latency_{0};
  bool retry_after_ack_timeout_{false};

  // module readiness (power-up / reset)
  bool ready_{false};
  bool reset_expected_{false};
  uint32_t boot_at_{0};
  uint32_t ready_wait_start_{0};

  // verification
  bool playing_at_send_{false};
  bool seen_playing_{false};
  int8_t last_status_{-1};
  bool query_pending_{false};
  uint32_t query_sent_at_{0};
  uint32_t next_query_at_{0};

  // state
  bool playing_{false};
  bool playing_published_{false};
  int16_t last_volume_{-1};
  int16_t last_eq_{-1};
  uint16_t last_finished_track_{0};
  uint32_t last_finished_at_{0};
  bool finished_dedupe_valid_{false};

  // counters
  uint32_t retry_count_{0};
  uint32_t failed_count_{0};
  uint32_t reset_count_{0};
  uint8_t last_error_{ERR_NONE};

  // rx
  uint8_t rx_buf_[10];
  uint8_t rx_pos_{0};

  CallbackManager<void(uint16_t)> finished_callback_;
  CallbackManager<void(uint8_t, uint8_t)> failed_callback_;
  CallbackManager<void()> reset_callback_;
};

// ---------------------------------------------------------------- triggers

class FinishedPlaybackTrigger : public Trigger<uint16_t> {
 public:
  explicit FinishedPlaybackTrigger(DFPlayerAck *parent) {
    parent->add_on_finished_playback_callback([this](uint16_t track) { this->trigger(track); });
  }
};

class CommandFailedTrigger : public Trigger<uint8_t, uint8_t> {
 public:
  explicit CommandFailedTrigger(DFPlayerAck *parent) {
    parent->add_on_command_failed_callback([this](uint8_t cmd, uint8_t error) { this->trigger(cmd, error); });
  }
};

class ModuleResetTrigger : public Trigger<> {
 public:
  explicit ModuleResetTrigger(DFPlayerAck *parent) {
    parent->add_on_module_reset_callback([this]() { this->trigger(); });
  }
};

// ---------------------------------------------------------------- actions

#define DFPLAYER_ACK_SIMPLE_ACTION(ACTION_CLASS, METHOD)                    \
  template <typename... Ts>                                                 \
  class ACTION_CLASS : public Action<Ts...>, public Parented<DFPlayerAck> { \
   public:                                                                  \
    void play(const Ts &...x) override { this->parent_->METHOD(); }         \
  };

DFPLAYER_ACK_SIMPLE_ACTION(NextAction, next)
DFPLAYER_ACK_SIMPLE_ACTION(PreviousAction, previous)
DFPLAYER_ACK_SIMPLE_ACTION(VolumeUpAction, volume_up)
DFPLAYER_ACK_SIMPLE_ACTION(VolumeDownAction, volume_down)
DFPLAYER_ACK_SIMPLE_ACTION(SleepAction, sleep)
DFPLAYER_ACK_SIMPLE_ACTION(ResetAction, reset)
DFPLAYER_ACK_SIMPLE_ACTION(StartAction, start)
DFPLAYER_ACK_SIMPLE_ACTION(PauseAction, pause)
DFPLAYER_ACK_SIMPLE_ACTION(StopAction, stop)
DFPLAYER_ACK_SIMPLE_ACTION(RandomAction, random)
DFPLAYER_ACK_SIMPLE_ACTION(ClearQueueAction, clear_queue)

template <typename... Ts>
class PlayMp3Action : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(uint16_t, file)
  void play(const Ts &...x) override { this->parent_->play_mp3(this->file_.value(x...)); }
};

template <typename... Ts>
class PlayAdvertAction : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(uint16_t, file)
  void play(const Ts &...x) override { this->parent_->play_advert(this->file_.value(x...)); }
};

template <typename... Ts>
class PlayFileAction : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(uint16_t, file)
  TEMPLATABLE_VALUE(bool, loop)
  void play(const Ts &...x) override { this->parent_->play_file(this->file_.value(x...), this->loop_.value(x...)); }
};

template <typename... Ts>
class PlayFolderAction : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(uint16_t, folder)
  TEMPLATABLE_VALUE(uint16_t, file)
  TEMPLATABLE_VALUE(bool, loop)
  void play(const Ts &...x) override {
    this->parent_->play_folder(this->folder_.value(x...), this->file_.value(x...), this->loop_.value(x...));
  }
};

template <typename... Ts>
class SetVolumeAction : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(uint8_t, volume)
  void play(const Ts &...x) override { this->parent_->set_volume(this->volume_.value(x...)); }
};

template <typename... Ts>
class SetEqAction : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(EqPreset, eq)
  void play(const Ts &...x) override { this->parent_->set_eq(this->eq_.value(x...)); }
};

template <typename... Ts>
class SetDeviceAction : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(Device, device)
  void play(const Ts &...x) override { this->parent_->set_device(this->device_.value(x...)); }
};

template <typename... Ts>
class SetRepeatAction : public Action<Ts...>, public Parented<DFPlayerAck> {
 public:
  TEMPLATABLE_VALUE(bool, enable)
  void play(const Ts &...x) override { this->parent_->set_current_track_repeat(this->enable_.value(x...)); }
};

template <typename... Ts>
class IsPlayingCondition : public Condition<Ts...>, public Parented<DFPlayerAck> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_playing(); }
};

}  // namespace esphome::dfplayer_ack
