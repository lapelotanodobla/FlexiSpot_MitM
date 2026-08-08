import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_ID

DEPENDENCIES = ["uart"]

desk_mitm_ns = cg.esphome_ns.namespace("desk_mitm")
DeskMitm = desk_mitm_ns.class_("DeskMitm", cg.Component)

CONF_DESK_UART = "desk_uart"
CONF_KEYPAD_UART = "keypad_uart"
CONF_PIN20_SENSE = "pin20_sense_pin"
CONF_PIN20_DRIVE = "pin20_drive_pin"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DeskMitm),
        cv.Required(CONF_DESK_UART): cv.use_id(uart.UARTComponent),
        cv.Required(CONF_KEYPAD_UART): cv.use_id(uart.UARTComponent),
        cv.Required(CONF_PIN20_SENSE): pins.gpio_input_pin_schema,
        cv.Required(CONF_PIN20_DRIVE): pins.gpio_output_pin_schema,
        cv.Optional("min_height", default=60.0): cv.float_,
        cv.Optional("max_height", default=121.0): cv.float_,
        cv.Optional("coast_margin", default=0.7): cv.float_,
        cv.Optional("deadband", default=0.3): cv.float_,
        cv.Optional("settle_ms", default=1200): cv.positive_int,
        cv.Optional("move_timeout_ms", default=30000): cv.positive_int,
        cv.Optional("stall_ms", default=2000): cv.positive_int,
        cv.Optional("max_taps", default=5): cv.positive_int,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    desk = await cg.get_variable(config[CONF_DESK_UART])
    keypad = await cg.get_variable(config[CONF_KEYPAD_UART])
    cg.add(var.set_uarts(desk, keypad))
    sense = await cg.gpio_pin_expression(config[CONF_PIN20_SENSE])
    drive = await cg.gpio_pin_expression(config[CONF_PIN20_DRIVE])
    cg.add(var.set_pin20(sense, drive))
    cg.add(
        var.set_move_config(
            config["min_height"], config["max_height"], config["coast_margin"],
            config["deadband"], config["settle_ms"], config["move_timeout_ms"],
            config["stall_ms"], config["max_taps"],
        )
    )
