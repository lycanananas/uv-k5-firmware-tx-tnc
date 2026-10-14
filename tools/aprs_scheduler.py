#!/usr/bin/env python3
"""Schedule APRS object reports through the APRS TX firmware."""

import argparse
import logging
import random
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
from pathlib import Path
from typing import Any

import serial
import yaml


MAX_COMMENT_LENGTH = 90
VHF_RANGE = (Decimal("144.000"), Decimal("146.000"))
UHF_RANGE = (Decimal("430.000"), Decimal("440.000"))


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class APRSObject:
    sender: str
    name: str
    latitude: Decimal
    longitude: Decimal
    symbol_table: str
    symbol: str
    comment: str


@dataclass(frozen=True)
class Schedule:
    path: str
    interval_seconds: float


@dataclass(frozen=True)
class Config:
    port: str
    baudrate: int
    frequencies: tuple[str, ...]
    destination: str
    objects: tuple[APRSObject, ...]
    schedules: tuple[Schedule, ...]
    jitter_percent: float
    startup_delay_seconds: float
    qrg_settle_seconds: float
    comment_max_length: int


def require_mapping(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigError(f"{field} must be a mapping")
    return value


def require_list(value: Any, field: str) -> list[Any]:
    if not isinstance(value, list) or not value:
        raise ConfigError(f"{field} must be a non-empty list")
    return value


def require_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ConfigError(f"{field} must be a non-empty string")
    return value


def validate_callsign(value: Any, field: str) -> str:
    callsign = require_string(value, field).upper()
    base, separator, ssid_text = callsign.partition("-")
    if not 1 <= len(base) <= 6 or not base.isalnum() or not base.isascii():
        raise ConfigError(f"{field} must contain 1 to 6 ASCII letters or digits")
    if separator:
        if not ssid_text.isdecimal() or not 0 <= int(ssid_text) <= 15:
            raise ConfigError(f"{field} SSID must be between 0 and 15")
    return callsign


def parse_frequency(value: Any, field: str) -> str:
    try:
        frequency = Decimal(str(value))
    except InvalidOperation as error:
        raise ConfigError(f"{field} must be a frequency in MHz") from error
    if frequency.as_tuple().exponent < -3:
        raise ConfigError(f"{field} has more than three decimal places")
    if not (VHF_RANGE[0] <= frequency <= VHF_RANGE[1] or UHF_RANGE[0] <= frequency <= UHF_RANGE[1]):
        raise ConfigError(f"{field} must be within 144.000-146.000 or 430.000-440.000 MHz")
    return f"{frequency:.3f}"


def parse_coordinate(value: Any, field: str, limit: Decimal) -> Decimal:
    try:
        coordinate = Decimal(str(value))
    except InvalidOperation as error:
        raise ConfigError(f"{field} must be a decimal coordinate") from error
    if not -limit <= coordinate <= limit:
        raise ConfigError(f"{field} must be between {-limit} and {limit}")
    return coordinate


def format_coordinate(value: Decimal, width: int, positive: str, negative: str) -> str:
    magnitude = abs(value)
    degrees = int(magnitude)
    hundredths_of_minutes = int(((magnitude - degrees) * Decimal(6000)).to_integral_value(rounding=ROUND_HALF_UP))
    if hundredths_of_minutes == 6000:
        degrees += 1
        hundredths_of_minutes = 0
    hemisphere = negative if value < 0 else positive
    return f"{degrees:0{width}d}{hundredths_of_minutes // 100:02d}.{hundredths_of_minutes % 100:02d}{hemisphere}"


def parse_object(value: Any, index: int, comment_max_length: int) -> APRSObject:
    raw = require_mapping(value, f"objects[{index}]")
    name = require_string(raw.get("name"), f"objects[{index}].name").upper()
    if not name.isascii() or not name.isprintable() or len(name) > 9:
        raise ConfigError(f"objects[{index}].name must contain 1 to 9 printable ASCII characters")
    symbol = require_string(raw.get("symbol"), f"objects[{index}].symbol")
    if len(symbol) != 1 or not symbol.isascii() or not symbol.isprintable():
        raise ConfigError(f"objects[{index}].symbol must be one printable ASCII character")
    symbol_table = raw.get("symbol_table", "/")
    if symbol_table not in ("/", "\\"):
        raise ConfigError(f"objects[{index}].symbol_table must be / or \\")
    comment = raw.get("comment", "")
    if not isinstance(comment, str) or not comment.isascii() or not comment.isprintable() or len(comment) > comment_max_length:
        raise ConfigError(f"objects[{index}].comment must contain at most {comment_max_length} printable ASCII characters")
    return APRSObject(
        sender=validate_callsign(raw.get("sender"), f"objects[{index}].sender"),
        name=name,
        latitude=parse_coordinate(raw.get("latitude"), f"objects[{index}].latitude", Decimal(90)),
        longitude=parse_coordinate(raw.get("longitude"), f"objects[{index}].longitude", Decimal(180)),
        symbol_table=symbol_table,
        symbol=symbol,
        comment=comment,
    )


def parse_config(path: Path) -> Config:
    try:
        raw = yaml.safe_load(path.read_text(encoding="ascii"))
    except OSError as error:
        raise ConfigError(f"cannot read {path}: {error}") from error
    except yaml.YAMLError as error:
        raise ConfigError(f"invalid YAML: {error}") from error

    root = require_mapping(raw, "config")
    serial_config = require_mapping(root.get("serial"), "serial")
    comment_max_length = root.get("comment_max_length", MAX_COMMENT_LENGTH)
    if not isinstance(comment_max_length, int) or not 0 <= comment_max_length <= MAX_COMMENT_LENGTH:
        raise ConfigError(f"comment_max_length must be an integer between 0 and {MAX_COMMENT_LENGTH}")

    frequencies = tuple(parse_frequency(value, f"qrg[{index}]") for index, value in enumerate(require_list(root.get("qrg"), "qrg")))
    objects = tuple(parse_object(value, index, comment_max_length) for index, value in enumerate(require_list(root.get("objects"), "objects")))
    schedules = []
    for index, value in enumerate(require_list(root.get("schedules"), "schedules")):
        raw_schedule = require_mapping(value, f"schedules[{index}]")
        path_value = raw_schedule.get("path", "")
        path = "" if path_value in (None, "") else validate_callsign(path_value, f"schedules[{index}].path")
        try:
            interval_minutes = float(raw_schedule.get("interval_minutes"))
        except (TypeError, ValueError) as error:
            raise ConfigError(f"schedules[{index}].interval_minutes must be a number") from error
        if interval_minutes <= 0:
            raise ConfigError(f"schedules[{index}].interval_minutes must be greater than zero")
        schedules.append(Schedule(path=path, interval_seconds=interval_minutes * 60.0))

    try:
        baudrate = int(serial_config.get("baudrate", 38400))
        jitter_percent = float(root.get("jitter_percent", 5))
        startup_delay_seconds = float(root.get("startup_delay_seconds", 10))
        qrg_settle_seconds = float(root.get("qrg_settle_seconds", 0.25))
    except (TypeError, ValueError) as error:
        raise ConfigError("baudrate, jitter_percent, startup_delay_seconds, and qrg_settle_seconds must be numeric") from error
    if baudrate <= 0 or not 0 <= jitter_percent <= 50 or not 0 <= startup_delay_seconds <= 300 or not 0 <= qrg_settle_seconds <= 5:
        raise ConfigError("baudrate must be positive, jitter_percent must be 0-50, startup_delay_seconds must be 0-300, qrg_settle_seconds must be 0-5")

    return Config(
        port=require_string(serial_config.get("port"), "serial.port"),
        baudrate=baudrate,
        frequencies=frequencies,
        destination=validate_callsign(root.get("destination"), "destination"),
        objects=objects,
        schedules=tuple(schedules),
        jitter_percent=jitter_percent,
        startup_delay_seconds=startup_delay_seconds,
        qrg_settle_seconds=qrg_settle_seconds,
        comment_max_length=comment_max_length,
    )


def build_frame(aprs_object: APRSObject, destination: str, path: str) -> str:
    timestamp = datetime.now(timezone.utc).strftime("%d%H%Mz")
    latitude = format_coordinate(aprs_object.latitude, 2, "N", "S")
    longitude = format_coordinate(aprs_object.longitude, 3, "E", "W")
    route = f",{path}" if path else ""
    return f"{aprs_object.sender}>{destination}{route}:;{aprs_object.name:<9}*{timestamp}{latitude}{aprs_object.symbol_table}{longitude}{aprs_object.symbol}{aprs_object.comment}"


def write_line(connection: serial.Serial, line: str) -> None:
    connection.write((line + "\n").encode("ascii"))
    connection.flush()


def transmit_cycle(connection: serial.Serial, config: Config, schedule: Schedule, frequency_index: int) -> int:
    for aprs_object in config.objects:
        frequency = config.frequencies[frequency_index]
        frequency_index = (frequency_index + 1) % len(config.frequencies)
        write_line(connection, f"!QRG {frequency}")
        time.sleep(config.qrg_settle_seconds)
        frame = build_frame(aprs_object, config.destination, schedule.path)
        write_line(connection, frame)
        logging.info("sent qrg=%s path=%s object=%s", frequency, schedule.path or "DIRECT", aprs_object.name.rstrip())
    return frequency_index


def jittered_delay(interval_seconds: float, jitter_percent: float) -> float:
    return interval_seconds * random.uniform(1.0 - jitter_percent / 100.0, 1.0 + jitter_percent / 100.0)


def run(config: Config) -> None:
    next_due = [time.monotonic() + config.startup_delay_seconds for _ in config.schedules]
    frequency_index = 0

    with serial.Serial(config.port, config.baudrate, timeout=1) as connection:
        logging.info("opened %s at %d baud", config.port, config.baudrate)
        while True:
            schedule_index = min(range(len(next_due)), key=next_due.__getitem__)
            delay = next_due[schedule_index] - time.monotonic()
            if delay > 0:
                time.sleep(delay)
                continue
            schedule = config.schedules[schedule_index]
            frequency_index = transmit_cycle(connection, config, schedule, frequency_index)
            next_due[schedule_index] = time.monotonic() + jittered_delay(schedule.interval_seconds, config.jitter_percent)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path, help="YAML configuration file")
    parser.add_argument("--validate", action="store_true", help="validate configuration and exit")
    args = parser.parse_args()

    try:
        config = parse_config(args.config)
    except ConfigError as error:
        parser.error(str(error))
    if args.validate:
        print("configuration valid")
        return 0

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        run(config)
    except serial.SerialException as error:
        logging.error("serial error: %s", error)
        return 1
    except KeyboardInterrupt:
        logging.info("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())