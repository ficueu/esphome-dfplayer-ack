"""DFPlayer Mini with command queue, ACK wait, retries and playback verification.

Drop-in alternative to the built-in `dfplayer` component (same action names under
`dfplayer_ack.`), for installations where a lost command is not acceptable.
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import binary_sensor, sensor, uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_DEVICE,
    CONF_FILE,
    CONF_ID,
    CONF_TRIGGER_ID,
    CONF_VOLUME,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor"]
CODEOWNERS = ["@ficueu"]
MULTI_CONF = True

CONF_LOOP = "loop"
CONF_FOLDER = "folder"
CONF_ENABLE = "enable"
CONF_EQ_PRESET = "eq_preset"
CONF_MAX_RETRIES = "max_retries"
CONF_ACK_TIMEOUT = "ack_timeout"
CONF_RETRY_DELAY = "retry_delay"
CONF_COMMAND_INTERVAL = "command_interval"
CONF_USE_ACK = "use_ack"
CONF_RETRY_NON_IDEMPOTENT = "retry_non_idempotent"
CONF_BUSY_PIN = "busy_pin"
CONF_VERIFY_PLAYBACK = "verify_playback"
CONF_VERIFY_TIMEOUT = "verify_timeout"
CONF_INIT_TIMEOUT = "init_timeout"
CONF_RESTORE_ON_RESET = "restore_on_reset"
CONF_VALIDATE_CHECKSUM = "validate_checksum"
CONF_QUEUE_SIZE = "queue_size"
CONF_RETRY_COUNT = "retry_count"
CONF_FAILED_COUNT = "failed_count"
CONF_LAST_ERROR = "last_error"
CONF_RESET_COUNT = "reset_count"
CONF_PLAYING = "playing"
CONF_ON_FINISHED_PLAYBACK = "on_finished_playback"
CONF_ON_COMMAND_FAILED = "on_command_failed"
CONF_ON_MODULE_RESET = "on_module_reset"

dfplayer_ack_ns = cg.esphome_ns.namespace("dfplayer_ack")
DFPlayerAck = dfplayer_ack_ns.class_("DFPlayerAck", cg.Component, uart.UARTDevice)

EqPreset = dfplayer_ack_ns.enum("EqPreset")
EQ_PRESET = {
    "NORMAL": EqPreset.NORMAL,
    "POP": EqPreset.POP,
    "ROCK": EqPreset.ROCK,
    "JAZZ": EqPreset.JAZZ,
    "CLASSIC": EqPreset.CLASSIC,
    "BASS": EqPreset.BASS,
}
Device = dfplayer_ack_ns.enum("Device")
DEVICE = {"USB": Device.USB, "TF_CARD": Device.TF_CARD}
VerifyMode = dfplayer_ack_ns.enum("VerifyMode")
VERIFY_MODES = {
    "none": VerifyMode.VERIFY_NONE,
    "busy_pin": VerifyMode.VERIFY_BUSY_PIN,
    "query": VerifyMode.VERIFY_QUERY,
}

FinishedPlaybackTrigger = dfplayer_ack_ns.class_(
    "FinishedPlaybackTrigger", automation.Trigger.template(cg.uint16)
)
CommandFailedTrigger = dfplayer_ack_ns.class_(
    "CommandFailedTrigger", automation.Trigger.template(cg.uint8, cg.uint8)
)
ModuleResetTrigger = dfplayer_ack_ns.class_(
    "ModuleResetTrigger", automation.Trigger.template()
)


def _counter_schema(icon):
    return sensor.sensor_schema(
        accuracy_decimals=0,
        icon=icon,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


def _validate(config):
    mode = config[CONF_VERIFY_PLAYBACK]
    if mode == "auto":
        mode = "busy_pin" if CONF_BUSY_PIN in config else "none"
    if mode == "busy_pin" and CONF_BUSY_PIN not in config:
        raise cv.Invalid("verify_playback: busy_pin requires busy_pin to be set")
    if not config[CONF_USE_ACK] and mode == "none":
        raise cv.Invalid(
            "use_ack: false without busy_pin/query verification would never confirm anything"
        )
    config[CONF_VERIFY_PLAYBACK] = mode
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DFPlayerAck),
            cv.Optional(CONF_MAX_RETRIES, default=3): cv.int_range(min=0, max=10),
            cv.Optional(
                CONF_ACK_TIMEOUT, default="300ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_RETRY_DELAY, default="100ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_COMMAND_INTERVAL, default="50ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_USE_ACK, default=True): cv.boolean,
            cv.Optional(CONF_RETRY_NON_IDEMPOTENT, default=False): cv.boolean,
            cv.Optional(CONF_BUSY_PIN): pins.gpio_input_pin_schema,
            cv.Optional(CONF_VERIFY_PLAYBACK, default="auto"): cv.one_of(
                "auto", "none", "busy_pin", "query", lower=True
            ),
            cv.Optional(
                CONF_VERIFY_TIMEOUT, default="1500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_INIT_TIMEOUT, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RESTORE_ON_RESET, default=True): cv.boolean,
            cv.Optional(CONF_VALIDATE_CHECKSUM, default=True): cv.boolean,
            cv.Optional(CONF_QUEUE_SIZE, default=16): cv.int_range(min=1, max=64),
            cv.Optional(CONF_RETRY_COUNT): _counter_schema("mdi:reload"),
            cv.Optional(CONF_FAILED_COUNT): _counter_schema("mdi:alert-circle-outline"),
            cv.Optional(CONF_RESET_COUNT): _counter_schema("mdi:restart-alert"),
            cv.Optional(CONF_LAST_ERROR): sensor.sensor_schema(
                accuracy_decimals=0,
                icon="mdi:alert",
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PLAYING): binary_sensor.binary_sensor_schema(
                device_class="sound",
            ),
            cv.Optional(CONF_ON_FINISHED_PLAYBACK): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FinishedPlaybackTrigger)}
            ),
            cv.Optional(CONF_ON_COMMAND_FAILED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CommandFailedTrigger)}
            ),
            cv.Optional(CONF_ON_MODULE_RESET): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ModuleResetTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _validate,
)


def _final_validate(config):
    need_rx = config[CONF_USE_ACK] or config[CONF_VERIFY_PLAYBACK] == "query"
    return uart.final_validate_device_schema(
        "dfplayer_ack",
        baud_rate=9600,
        require_tx=True,
        require_rx=need_rx,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
    )(config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_max_retries(config[CONF_MAX_RETRIES]))
    cg.add(var.set_ack_timeout(config[CONF_ACK_TIMEOUT]))
    cg.add(var.set_retry_delay(config[CONF_RETRY_DELAY]))
    cg.add(var.set_command_interval(config[CONF_COMMAND_INTERVAL]))
    cg.add(var.set_use_ack(config[CONF_USE_ACK]))
    cg.add(var.set_retry_non_idempotent(config[CONF_RETRY_NON_IDEMPOTENT]))
    cg.add(var.set_verify_mode(VERIFY_MODES[config[CONF_VERIFY_PLAYBACK]]))
    cg.add(var.set_verify_timeout(config[CONF_VERIFY_TIMEOUT]))
    cg.add(var.set_init_timeout(config[CONF_INIT_TIMEOUT]))
    cg.add(var.set_restore_on_reset(config[CONF_RESTORE_ON_RESET]))
    cg.add(var.set_validate_checksum(config[CONF_VALIDATE_CHECKSUM]))
    cg.add(var.set_queue_size(config[CONF_QUEUE_SIZE]))

    if CONF_BUSY_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_BUSY_PIN])
        cg.add(var.set_busy_pin(pin))

    for key, setter in (
        (CONF_RETRY_COUNT, "set_retry_count_sensor"),
        (CONF_FAILED_COUNT, "set_failed_count_sensor"),
        (CONF_LAST_ERROR, "set_last_error_sensor"),
        (CONF_RESET_COUNT, "set_reset_count_sensor"),
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    if CONF_PLAYING in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_PLAYING])
        cg.add(var.set_playing_binary_sensor(bs))

    for conf in config.get(CONF_ON_FINISHED_PLAYBACK, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.uint16, "track")], conf)
    for conf in config.get(CONF_ON_COMMAND_FAILED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.uint8, "cmd"), (cg.uint8, "error")], conf
        )
    for conf in config.get(CONF_ON_MODULE_RESET, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


# ------------------------------------------------------------------ actions

ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(DFPlayerAck)})


def _register_simple(name, class_name):
    action_class = dfplayer_ack_ns.class_(class_name, automation.Action)

    @automation.register_action(name, action_class, ACTION_SCHEMA, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        return var


for _name, _cls in (
    ("dfplayer_ack.play_next", "NextAction"),
    ("dfplayer_ack.play_previous", "PreviousAction"),
    ("dfplayer_ack.volume_up", "VolumeUpAction"),
    ("dfplayer_ack.volume_down", "VolumeDownAction"),
    ("dfplayer_ack.sleep", "SleepAction"),
    ("dfplayer_ack.reset", "ResetAction"),
    ("dfplayer_ack.start", "StartAction"),
    ("dfplayer_ack.pause", "PauseAction"),
    ("dfplayer_ack.stop", "StopAction"),
    ("dfplayer_ack.random", "RandomAction"),
    ("dfplayer_ack.clear_queue", "ClearQueueAction"),
):
    _register_simple(_name, _cls)


def _register_templated(name, class_name, schema, fields):
    """fields: list of (config key, setter name, C++ type)"""
    action_class = dfplayer_ack_ns.class_(class_name, automation.Action)

    @automation.register_action(name, action_class, schema, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        for key, setter, cpp_type in fields:
            template_ = await cg.templatable(config[key], args, cpp_type)
            cg.add(getattr(var, setter)(template_))
        return var


_register_templated(
    "dfplayer_ack.play_mp3",
    "PlayMp3Action",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Required(CONF_FILE): cv.templatable(cv.uint16_t),
        },
        key=CONF_FILE,
    ),
    [(CONF_FILE, "set_file", cg.uint16)],
)

_register_templated(
    "dfplayer_ack.play_advert",
    "PlayAdvertAction",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Required(CONF_FILE): cv.templatable(cv.uint16_t),
        },
        key=CONF_FILE,
    ),
    [(CONF_FILE, "set_file", cg.uint16)],
)

_register_templated(
    "dfplayer_ack.play",
    "PlayFileAction",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Required(CONF_FILE): cv.templatable(cv.uint16_t),
            cv.Optional(CONF_LOOP, default=False): cv.templatable(cv.boolean),
        },
        key=CONF_FILE,
    ),
    [(CONF_FILE, "set_file", cg.uint16), (CONF_LOOP, "set_loop", cg.bool_)],
)

_register_templated(
    "dfplayer_ack.play_folder",
    "PlayFolderAction",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Required(CONF_FOLDER): cv.templatable(cv.uint16_t),
            cv.Optional(CONF_FILE, default=0): cv.templatable(cv.uint16_t),
            cv.Optional(CONF_LOOP, default=False): cv.templatable(cv.boolean),
        }
    ),
    [
        (CONF_FOLDER, "set_folder", cg.uint16),
        (CONF_FILE, "set_file", cg.uint16),
        (CONF_LOOP, "set_loop", cg.bool_),
    ],
)

_register_templated(
    "dfplayer_ack.set_volume",
    "SetVolumeAction",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Required(CONF_VOLUME): cv.templatable(cv.int_range(min=0, max=30)),
        },
        key=CONF_VOLUME,
    ),
    [(CONF_VOLUME, "set_volume", cg.uint8)],
)

_register_templated(
    "dfplayer_ack.set_eq",
    "SetEqAction",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Required(CONF_EQ_PRESET): cv.templatable(cv.enum(EQ_PRESET, upper=True)),
        },
        key=CONF_EQ_PRESET,
    ),
    [(CONF_EQ_PRESET, "set_eq", EqPreset)],
)

_register_templated(
    "dfplayer_ack.set_device",
    "SetDeviceAction",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Required(CONF_DEVICE): cv.enum(DEVICE, upper=True),
        },
        key=CONF_DEVICE,
    ),
    [(CONF_DEVICE, "set_device", Device)],
)

_register_templated(
    "dfplayer_ack.set_current_track_repeat",
    "SetRepeatAction",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(DFPlayerAck),
            cv.Optional(CONF_ENABLE, default=True): cv.templatable(cv.boolean),
        }
    ),
    [(CONF_ENABLE, "set_enable", cg.bool_)],
)

# ------------------------------------------------------------------ condition

IsPlayingCondition = dfplayer_ack_ns.class_("IsPlayingCondition", automation.Condition)


@automation.register_condition("dfplayer_ack.is_playing", IsPlayingCondition, ACTION_SCHEMA)
async def is_playing_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
