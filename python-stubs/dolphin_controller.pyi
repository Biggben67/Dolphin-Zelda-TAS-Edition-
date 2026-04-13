"""
Module for programmatic inputs.

Supports GameCube inputs, Wii Remote buttons/IR/accelerometer/gyroscope,
and Nunchuck buttons/stick/accelerometer.
"""
from typing import TypedDict, type_check_only


@type_check_only
class GCInputs(TypedDict):
    Left: bool
    Right: bool
    Down: bool
    Up: bool
    Z: bool
    R: bool
    L: bool
    A: bool
    B: bool
    X: bool
    Y: bool
    Start: bool
    StickX: int  # 0-255, 128 is neutral
    StickY: int  # 0-255, 128 is neutral
    CStickX: int  # 0-255, 128 is neutral
    CStickY: int  # 0-255, 128 is neutral
    TriggerLeft: int  # 0-255
    TriggerRight: int  # 0-255
    AnalogA: int  # 0-255
    AnalogB: int  # 0-255
    Connected: bool


@type_check_only
class WiiInputs(TypedDict):
    Left: bool
    Right: bool
    Down: bool
    Up: bool
    Plus: bool
    Minus: bool
    One: bool
    Two: bool
    A: bool
    B: bool
    Home: bool


class WiiAccelerometerInputs(TypedDict):
    X: int  # 0-1023
    Y: int  # 0-1023
    Z: int  # 0-1023


class WiiGyroscopeInputs(TypedDict):
    X: int  # 0-16383, centered at 8192
    Y: int  # 0-16383, centered at 8192
    Z: int  # 0-16383, centered at 8192
    SlowX: bool
    SlowY: bool
    SlowZ: bool


class NunchuckInputs(TypedDict):
    C: bool
    Z: bool
    StickX: int  # 0-255, 128 is neutral
    StickY: int  # 0-255, 128 is neutral


class NunchuckAccelerometerInputs(TypedDict):
    X: int  # 0-1023
    Y: int  # 0-1023
    Z: int  # 0-1023


def get_gc_buttons(controller_id: int, /) -> GCInputs:
    """
    Retrieves the current input map for the given GameCube controller.

    :param controller_id: 0-based index of the controller
    :return: dictionary describing the current input map
    """


def set_gc_buttons(controller_id: int, inputs: GCInputs, /) -> None:
    """
    Sets the current input map for the given GameCube controller.
    The override will hold for the current frame.

    :param controller_id: 0-based index of the controller
    :param inputs: dictionary describing the input map
    """


def get_wii_buttons(controller_id: int, /) -> WiiInputs:
    """
    Retrieves the current input map for the given Wii controller.

    :param controller_id: 0-based index of the controller
    :return: dictionary describing the current input map
    """


def set_wii_buttons(controller_id: int, inputs: WiiInputs, /) -> None:
    """
    Sets the current input map for the given Wii controller.
    The override will hold for the current frame.

    :param controller_id: 0-based index of the controller
    :param inputs: dictionary describing the input map
    """


def set_wii_ircamera_transform(
    controller_id: int, x: float, y: float,
    z: float = -2, pitch: float = 0, yaw: float = 0, roll: float = 0, /,
) -> None:
    """
    Places the simulated IR camera at the specified location
    with the specified rotation relative to the sensor bar.
    For example, to move 2 meters away from the sensor,
    15 centimeters to the right and 5 centimeters down, use:
    `set_wii_ircamera_transform(controller_id, 0.15, -0.05, -2)`.

    :param controller_id: 0-based index of the controller
    :param x: x-position of the simulated IR camera in meters
    :param y: y-position of the simulated IR camera in meters
    :param z: z-position of the simulated IR camera in meters.
              Default is -2, meaning 2 meters from the simulated sensor bar.
    :param pitch: pitch of the simulated IR camera in radians.
    :param yaw: yaw of the simulated IR camera in radians.
    :param roll: roll of the simulated IR camera in radians.
    """


def get_wii_accelerometer(controller_id: int, /) -> WiiAccelerometerInputs:
    """
    Retrieves the current Wii Remote accelerometer state for the given controller.

    :param controller_id: 0-based index of the controller
    :return: dictionary describing raw accelerometer values
    """


def set_wii_accelerometer(controller_id: int, inputs: WiiAccelerometerInputs, /) -> None:
    """
    Sets the Wii Remote accelerometer state for the given controller.
    The override will hold for the current frame.

    :param controller_id: 0-based index of the controller
    :param inputs: dictionary describing raw accelerometer values
    """


def get_wii_gyroscope(controller_id: int, /) -> WiiGyroscopeInputs | None:
    """
    Retrieves the current MotionPlus gyroscope state for the given controller.
    Returns None when MotionPlus data is not currently present.

    :param controller_id: 0-based index of the controller
    :return: dictionary describing raw gyroscope values and slow/fast flags
    """


def set_wii_gyroscope(controller_id: int, inputs: WiiGyroscopeInputs, /) -> None:
    """
    Sets the MotionPlus gyroscope state for the given controller.
    The override will hold for the current frame.

    :param controller_id: 0-based index of the controller
    :param inputs: dictionary describing raw gyroscope values and slow/fast flags
    """


def get_nunchuck_buttons(controller_id: int, /) -> NunchuckInputs:
    """
    Retrieves the current input map for the given Nunchuck extension.

    :param controller_id: 0-based index of the controller
    :return: dictionary describing the current input map
    """


def set_nunchuck_buttons(controller_id: int, inputs: NunchuckInputs, /) -> None:
    """
    Sets the current input map for the given Nunchuck extension.
    The override will hold for the current frame.

    :param controller_id: 0-based index of the controller
    :param inputs: dictionary describing the input map
    """


def get_nunchuck_accelerometer(controller_id: int, /) -> NunchuckAccelerometerInputs:
    """
    Retrieves the current Nunchuck accelerometer state for the given controller.

    :param controller_id: 0-based index of the controller
    :return: dictionary describing raw accelerometer values
    """


def set_nunchuck_accelerometer(
    controller_id: int, inputs: NunchuckAccelerometerInputs, /
) -> None:
    """
    Sets the Nunchuck accelerometer state for the given controller.
    The override will hold for the current frame.

    :param controller_id: 0-based index of the controller
    :param inputs: dictionary describing raw accelerometer values
    """
