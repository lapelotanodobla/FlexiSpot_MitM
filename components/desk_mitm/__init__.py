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
