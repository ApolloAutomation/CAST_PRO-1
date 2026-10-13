"""Sendspin source@v1 role for a microphone / line-in, added to the stock sendspin hub.

Temporary external component: delete it once ESPHome wires the source role into the hub.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import audio, esp32, microphone
from esphome.components.const import CONF_ENABLED
from esphome.components.esp32.const import KEY_COMPONENTS, KEY_REF
from esphome.components.sendspin import (
    CODEC_OPUS,
    CODEC_PCM,
    CODECS,
    CONF_SENDSPIN_ID,
    SendspinHub,
)
from esphome.components.audio import CONF_MAX_SAMPLE_RATE, CONF_MIN_SAMPLE_RATE
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_CHANNELS,
    CONF_HYSTERESIS,
    CONF_ID,
    CONF_MICROPHONE,
    CONF_THRESHOLD,
    CONF_TRIGGER_ID,
    KEY_ESP32,
)
from esphome.core import CORE, EsphomeError, Version
from esphome.coroutine import CoroPriority, coroutine_with_priority
import esphome.final_validate as fv
from esphome.types import ConfigType

AUTO_LOAD = ["audio"]
DEPENDENCIES = ["sendspin", "microphone"]

CONF_CODEC = "codec"
CONF_CHUNK_DURATION = "chunk_duration"
CONF_CAPTURE_BUFFER = "capture_buffer"
CONF_LINE_SENSE = "line_sense"
CONF_ATTACK = "attack"
CONF_RELEASE = "release"
CONF_ON_STREAMING_STARTED = "on_streaming_started"
CONF_ON_STREAMING_STOPPED = "on_streaming_stopped"
CONF_ON_SIGNAL_PRESENT = "on_signal_present"
CONF_ON_SIGNAL_ABSENT = "on_signal_absent"

# sendspin-cpp registry entry written by the hub's to_code; the source role arrived in 0.9.0.
SENDSPIN_CPP_COMPONENT = "sendspin/sendspin-cpp"
MIN_SENDSPIN_CPP = Version(0, 9, 0)

# SourceRoleConfig::CHUNK_MIN_MS / CHUNK_MAX_MS and the Opus frame durations the library accepts.
CHUNK_MIN_MS = 5
CHUNK_MAX_MS = 150
OPUS_CHUNK_MS = (5, 10, 20, 40, 60)
OPUS_SAMPLE_RATES = (8000, 12000, 16000, 24000, 48000)
BITS_PER_SAMPLE = (16, 24)
# One chunk must fit a transport message: MAX_TRANSPORT_PLAINTEXT (65519) - SOURCE_CHUNK_HEADER_SIZE (9).
MAX_CHUNK_BYTES = 65519 - 9
# The largest write the capture ring accepts is half its storage (1.25 x the audio), so ~26 ms is the floor
# that still takes the i2s microphone's 16 ms reads; 50 ms leaves two reads of margin.
CAPTURE_BUFFER_MIN_MS = 50

sendspin_source_ns = cg.esphome_ns.namespace("sendspin_source")
SendspinSource = sendspin_source_ns.class_(
    "SendspinSource", cg.Component, cg.Parented.template(SendspinHub)
)
StreamingStartedTrigger = sendspin_source_ns.class_(
    "StreamingStartedTrigger", automation.Trigger.template()
)
StreamingStoppedTrigger = sendspin_source_ns.class_(
    "StreamingStoppedTrigger", automation.Trigger.template()
)
SignalPresentTrigger = sendspin_source_ns.class_(
    "SignalPresentTrigger", automation.Trigger.template()
)
SignalAbsentTrigger = sendspin_source_ns.class_(
    "SignalAbsentTrigger", automation.Trigger.template()
)

TRIGGERS = (
    (CONF_ON_STREAMING_STARTED, StreamingStartedTrigger),
    (CONF_ON_STREAMING_STOPPED, StreamingStoppedTrigger),
    (CONF_ON_SIGNAL_PRESENT, SignalPresentTrigger),
    (CONF_ON_SIGNAL_ABSENT, SignalAbsentTrigger),
)


class SendspinCppTooOld(cv.Invalid, EsphomeError):
    """Raised from codegen: a cv.Invalid that the CLI still reports without a traceback."""


def _validate_bits_per_sample(config: ConfigType) -> ConfigType:
    # MicrophoneSource packs any 17..23-bit request into 3 bytes, but sendspin-cpp rejects the
    # bit depth and leaves the role inert, so only the two formats it streams are accepted.
    if config[CONF_BITS_PER_SAMPLE] not in BITS_PER_SAMPLE:
        raise cv.Invalid(
            f"{CONF_BITS_PER_SAMPLE} must be one of {list(BITS_PER_SAMPLE)}",
            path=[CONF_BITS_PER_SAMPLE],
        )
    return config


MICROPHONE_SCHEMA = cv.All(
    microphone.microphone_source_schema(
        min_bits_per_sample=16,
        max_bits_per_sample=24,
        min_channels=1,
        max_channels=2,
    ),
    _validate_bits_per_sample,
)

LINE_SENSE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_THRESHOLD, default="-60dB"): cv.All(
            cv.decibel, cv.Range(max=0.0)
        ),
        cv.Optional(CONF_HYSTERESIS, default="6dB"): cv.All(
            cv.decibel, cv.Range(min=0.0)
        ),
        cv.Optional(CONF_ATTACK, default="100ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RELEASE, default="2s"): cv.positive_time_period_milliseconds,
    }
)


def _validate_line_sense(value):
    if isinstance(value, bool):
        return LINE_SENSE_SCHEMA({CONF_ENABLED: value})
    return LINE_SENSE_SCHEMA(value)


def _validate_codec_rules(config: ConfigType) -> ConfigType:
    if config[CONF_CODEC] != CODEC_OPUS:
        return config
    if config[CONF_MICROPHONE][CONF_BITS_PER_SAMPLE] != 16:
        raise cv.Invalid(
            f"{CONF_CODEC} opus requires {CONF_BITS_PER_SAMPLE}: 16",
            path=[CONF_MICROPHONE, CONF_BITS_PER_SAMPLE],
        )
    if config[CONF_CHUNK_DURATION].total_milliseconds not in OPUS_CHUNK_MS:
        raise cv.Invalid(
            f"{CONF_CODEC} opus requires {CONF_CHUNK_DURATION} of "
            f"{', '.join(f'{ms}ms' for ms in OPUS_CHUNK_MS)}",
            path=[CONF_CHUNK_DURATION],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SendspinSource),
            cv.GenerateID(CONF_SENDSPIN_ID): cv.use_id(SendspinHub),
            cv.Required(CONF_MICROPHONE): MICROPHONE_SCHEMA,
            cv.Optional(CONF_CODEC, default=CODEC_PCM): cv.one_of(
                CODEC_PCM, CODEC_OPUS, lower=True
            ),
            cv.Optional(CONF_CHUNK_DURATION, default="20ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=CHUNK_MIN_MS),
                    max=cv.TimePeriod(milliseconds=CHUNK_MAX_MS),
                ),
            ),
            cv.Optional(CONF_CAPTURE_BUFFER, default="500ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=CAPTURE_BUFFER_MIN_MS)),
            ),
            cv.Optional(CONF_LINE_SENSE, default=False): _validate_line_sense,
            cv.Optional(CONF_ON_STREAMING_STARTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StreamingStartedTrigger)}
            ),
            cv.Optional(CONF_ON_STREAMING_STOPPED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StreamingStoppedTrigger)}
            ),
            cv.Optional(CONF_ON_SIGNAL_PRESENT): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SignalPresentTrigger)}
            ),
            cv.Optional(CONF_ON_SIGNAL_ABSENT): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SignalAbsentTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
    cv.require_esphome_version(2026, 10, 0),
    _validate_codec_rules,
)


def _validate_opus_sample_rate(mic_config: ConfigType) -> ConfigType:
    rate = mic_config.get(CONF_MIN_SAMPLE_RATE)
    if rate != mic_config.get(CONF_MAX_SAMPLE_RATE) or rate not in OPUS_SAMPLE_RATES:
        raise cv.Invalid(
            f"{CONF_CODEC} opus requires the microphone sample rate to be one of "
            f"{list(OPUS_SAMPLE_RATES)}"
        )
    return mic_config


def _validate_chunk_fits_transport(config: ConfigType):
    chunk_ms = config[CONF_CHUNK_DURATION].total_milliseconds
    bytes_per_frame = len(config[CONF_MICROPHONE][CONF_CHANNELS]) * (
        config[CONF_MICROPHONE][CONF_BITS_PER_SAMPLE] // 8
    )

    def validator(mic_config: ConfigType) -> ConfigType:
        # The i2s microphone's sample_rate is only bounded below, so the role could reject the
        # chunk at runtime (source_chunk_bytes() == 0) and never advertise itself.
        rate = mic_config.get(CONF_MAX_SAMPLE_RATE)
        if rate is not None and chunk_ms * rate // 1000 * bytes_per_frame > MAX_CHUNK_BYTES:
            raise cv.Invalid(
                f"a {chunk_ms}ms {CONF_CHUNK_DURATION} at {rate} Hz does not fit one "
                f"transport message ({MAX_CHUNK_BYTES} bytes); lower it"
            )
        return mic_config

    return validator


def _validate_opus_pseudostack() -> None:
    # The hub keeps CONFIG_SENDSPIN_ENABLE_OPUS on for the player, so enabling the source role
    # always compiles the library's Opus encoder, which #errors on the non-thread-safe pseudostack.
    audio_config = fv.full_config.get().get(audio.DOMAIN) or {}
    opus_config = (audio_config.get(audio.CONF_CODECS) or {}).get(audio.CONF_OPUS) or {}
    pseudostack = opus_config.get(audio.CONF_PSEUDOSTACK) or {}
    if pseudostack.get(audio.CONF_THREADSAFE) is False:
        raise cv.Invalid(
            "sendspin_source needs micro-opus's thread-safe pseudostack; remove "
            f"{audio.DOMAIN}.{audio.CONF_CODECS}.{audio.CONF_OPUS}.{audio.CONF_PSEUDOSTACK}."
            f"{audio.CONF_THREADSAFE}: false"
        )


def _final_validate(config: ConfigType) -> ConfigType:
    mic_source = config[CONF_MICROPHONE]
    microphone.final_validate_microphone_source_schema("sendspin_source")(mic_source)
    mic_validators = [_validate_chunk_fits_transport(config)]
    if config[CONF_CODEC] == CODEC_OPUS:
        mic_validators.append(_validate_opus_sample_rate)
    cv.Schema(
        {
            cv.Required(CONF_MICROPHONE): fv.id_declaration_match_schema(
                cv.All(*mic_validators)
            )
        },
        extra=cv.ALLOW_EXTRA,
    )(mic_source)
    _validate_opus_pseudostack()
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.FINAL)
async def _enable_source_role(codec: str) -> None:
    # Runs after the hub's to_code, which pins sendspin-cpp and forces the source role off.
    entry = CORE.data.get(KEY_ESP32, {}).get(KEY_COMPONENTS, {}).get(
        SENDSPIN_CPP_COMPONENT
    )
    ref = entry.get(KEY_REF) if entry else None
    try:
        pinned = Version.parse(ref) if ref else None
    except ValueError:
        pinned = None
    if pinned is None or pinned < MIN_SENDSPIN_CPP:
        raise SendspinCppTooOld(
            f"sendspin_source needs sendspin-cpp >= {MIN_SENDSPIN_CPP} but this ESPHome's "
            f"sendspin hub pins {ref!r}; use ESPHome 2026.10.0b2 or newer"
        )
    esp32.add_idf_sdkconfig_option("CONFIG_SENDSPIN_ENABLE_SOURCE", True)
    if codec == CODEC_OPUS:
        esp32.add_idf_sdkconfig_option("CONFIG_SENDSPIN_ENABLE_OPUS", True)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_SENDSPIN_ID])

    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=False
    )
    cg.add(var.set_microphone_source(mic_source))
    cg.add(var.set_codec(CODECS[config[CONF_CODEC]]))
    cg.add(var.set_chunk_duration_ms(config[CONF_CHUNK_DURATION]))
    cg.add(var.set_capture_buffer_ms(config[CONF_CAPTURE_BUFFER]))

    line_sense = config[CONF_LINE_SENSE]
    cg.add(
        var.set_line_sense(
            line_sense[CONF_ENABLED],
            line_sense[CONF_THRESHOLD],
            line_sense[CONF_HYSTERESIS],
            line_sense[CONF_ATTACK],
            line_sense[CONF_RELEASE],
        )
    )

    for key, _ in TRIGGERS:
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)

    CORE.add_job(_enable_source_role, config[CONF_CODEC])
