import math
import struct
import time

import can
import libusb_package
import usb.backend.libusb1


CONNECTION = {
    "interface": "gs_usb",
    "channel": 0,
    "bitrate": 500_000,
}

UPDATE_INTERVAL = 0.05  # Approximately 20 updates per second.


def send_frame(bus, frame_id, payload):
    bus.send(
        can.Message(
            arbitration_id=frame_id,
            is_extended_id=False,
            is_fd=False,
            data=payload,
            check=True,
        ),
        timeout=0.1,
    )


def main():
    # Load the libusb DLL supplied by libusb-package.
    backend = usb.backend.libusb1.get_backend(
        find_library=libusb_package.find_library
    )

    if backend is None:
        raise RuntimeError(
            "Could not load libusb. Install it with: "
            "py -m pip install libusb-package"
        )

    with can.Bus(**CONNECTION) as bus:
        print("CANable opened successfully at 500 kbps.")
        print("Sending MS3 simplified dash broadcasts at about 20 Hz.")
        print("RPM sweeps from 800 to 6000.")
        print("Generic sensor 1 sweeps from 20 to 80 (oil pressure in PSI).")
        print("Press Ctrl+C to stop.")

        started = time.monotonic()
        next_update = started
        next_status = started

        while True:
            now = time.monotonic()
            elapsed = now - started

            # One complete sweep approximately every 12.6 seconds.
            sweep = (1.0 + math.sin(elapsed * 0.5)) / 2.0

            rpm = round(800 + 5200 * sweep)
            oil_pressure_psi = 20.0 + 60.0 * sweep

            # ID 0x5E8: MS3 simplified dash broadcast.
            #
            # Bytes 0-1: MAP, signed, kPa * 10
            # Bytes 2-3: RPM, unsigned
            # Bytes 4-5: Coolant, signed, degrees F * 10
            # Bytes 6-7: TPS, signed, percent * 10
            #
            # All multibyte values are big-endian.
            send_frame(
                bus,
                0x5E8,
                struct.pack(
                    ">hHhh",
                    1000,   # MAP: 100.0 kPa absolute
                    rpm,
                    1800,   # Coolant: 180.0 degrees F
                    250,    # TPS: 25.0%
                ),
            )

            # ID 0x5EB: MS3 simplified dash broadcast.
            #
            # Bytes 0-1: Battery voltage, signed, volts * 10
            # Bytes 2-3: Generic sensor 1, signed, value * 10
            # Bytes 4-5: Generic sensor 2, signed, value * 10
            # Byte 6:    Knock retard, unsigned, degrees * 10
            # Byte 7:    Reserved
            #
            # Test mapping: generic sensor 1 = oil pressure in PSI.
            send_frame(
                bus,
                0x5EB,
                struct.pack(
                    ">hhhBB",
                    142,                            # Battery: 14.2 V
                    round(oil_pressure_psi * 10),
                    0,                              # Generic sensor 2
                    0,                              # Knock retard
                    0,                              # Reserved
                ),
            )

            if now >= next_status:
                print(
                    f"TX: RPM={rpm:4d}  "
                    f"Oil={oil_pressure_psi:5.1f} PSI  "
                    f"CLT=180.0 F  Battery=14.2 V"
                )
                next_status = now + 1.0

            next_update += UPDATE_INTERVAL
            delay = next_update - time.monotonic()

            if delay > 0:
                time.sleep(delay)
            else:
                # Avoid sending a burst if the computer falls behind.
                next_update = time.monotonic()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nSimulator stopped.")
    except (can.CanError, RuntimeError) as error:
        print(f"\nSimulator error: {error}")
        raise SystemExit(1)