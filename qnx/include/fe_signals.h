/*
 * Four Eyes: the vehicle signals, in COVESA VSS terms, and their CAN encoding on the virtual bus.
 *
 * Paths follow the VSS master branch (https://github.com/COVESA/vehicle_signal_specification). The CAN identifiers and
 * byte layouts are this project's own; no production DBC is reproduced.
 */
#ifndef FE_SIGNALS_H
#define FE_SIGNALS_H

#include <stdint.h>

/* Signal identifiers carried in a proposal. Values are stable: the record and the attack set refer to them. */
typedef enum {
    FE_SIG_NONE = 0,
    /* Sensors: the agent may read them, never write them. */
    FE_SIG_SPEED = 1,          /* Vehicle.Speed, km/h, 0.1 km/h units on the bus */
    FE_SIG_SELECTED_GEAR = 2,  /* Vehicle.Powertrain.Transmission.SelectedGear, int8, 126 = Park, 127 = Drive */
    FE_SIG_PARKING_BRAKE = 3,  /* Vehicle.Chassis.ParkingBrake.IsEngaged, boolean */
    /* Actuators on the agent's allowlist. */
    FE_SIG_LOCK_ROW1_DRIVER = 16,     /* Vehicle.Cabin.Door.Row1.DriverSide.IsLocked */
    FE_SIG_LOCK_ROW1_PASSENGER = 17,  /* Vehicle.Cabin.Door.Row1.PassengerSide.IsLocked */
    FE_SIG_LOCK_ROW2_DRIVER = 18,     /* Vehicle.Cabin.Door.Row2.DriverSide.IsLocked */
    FE_SIG_LOCK_ROW2_PASSENGER = 19,  /* Vehicle.Cabin.Door.Row2.PassengerSide.IsLocked */
    FE_SIG_WINDOW_ROW1_DRIVER = 32,     /* Vehicle.Cabin.Door.Row1.DriverSide.Window.Position, percent */
    FE_SIG_WINDOW_ROW1_PASSENGER = 33,  /* Vehicle.Cabin.Door.Row1.PassengerSide.Window.Position */
    FE_SIG_WINDOW_ROW2_DRIVER = 34,     /* Vehicle.Cabin.Door.Row2.DriverSide.Window.Position */
    FE_SIG_WINDOW_ROW2_PASSENGER = 35,  /* Vehicle.Cabin.Door.Row2.PassengerSide.Window.Position */
    FE_SIG_TRUNK_OPEN = 48,             /* Vehicle.Body.Trunk.Rear.IsOpen */
    FE_SIG_HVAC_TEMP_DRIVER = 64,       /* Vehicle.Cabin.HVAC.Station.Row1.Driver.Temperature, degrees Celsius */
} fe_signal_t;

#define FE_GEAR_NEUTRAL 0
#define FE_GEAR_REVERSE (-1)
#define FE_GEAR_PARK 126
#define FE_GEAR_DRIVE 127

/* CAN identifiers on /dev/vcan0 (11-bit). */
#define FE_CAN_ID_COMMAND 0x210  /* gate -> body: one actuator command */
#define FE_CAN_ID_STATE 0x110    /* body -> all, every 20 ms: speed, gear, brake, locks, trunk */
#define FE_CAN_ID_WINDOWS 0x111  /* body -> all, every 20 ms: four window positions, HVAC set point */

/*
 * COMMAND frame, 8 bytes:
 *   0     signal id (fe_signal_t)
 *   1..2  value, int16 little-endian
 *   3..4  sequence, uint16 little-endian (the record's sequence number, low 16 bits)
 *   5..6  reserved, 0
 *   7     checksum: XOR of bytes 0..6, then XOR 0xA5
 * STATE frame, 8 bytes:
 *   0..1  speed in 0.1 km/h, uint16 little-endian
 *   2     selected gear, int8
 *   3     parking brake engaged, 0 or 1
 *   4     lock bitmask: bit 0 row1 driver, 1 row1 passenger, 2 row2 driver, 3 row2 passenger (1 = locked)
 *   5     trunk open, 0 or 1
 *   6     alive counter
 *   7     checksum as above
 * WINDOWS frame, 8 bytes:
 *   0..3  window positions in percent: row1 driver, row1 passenger, row2 driver, row2 passenger
 *   4     HVAC driver set point, int8 degrees Celsius
 *   5     reserved
 *   6     alive counter
 *   7     checksum as above
 */
static inline uint8_t fe_can_checksum(const uint8_t d[8]) {
    uint8_t c = 0;
    for (int i = 0; i < 7; i++) c ^= d[i];
    return (uint8_t)(c ^ 0xA5u);
}

#endif
