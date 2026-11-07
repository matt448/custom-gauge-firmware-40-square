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
RECEIVE_POLL_INTERVAL = 0.01


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


def receive_until(bus: can.BusABC, deadline: float) -> tuple[int, int, int]:
    """Drain gs_usb transmit echoes and bus traffic until the next update."""
    echoes = 0
    received = 0
    errors = 0
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return echoes, received, errors
        # gs_usb maps explicit zero to 1 ms; tiny positive timeouts can round
        # to USB timeout zero (infinite), including retries inside BusABC.recv.
        message = bus.recv(timeout=0.0)
        if message is None:
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(remaining, RECEIVE_POLL_INTERVAL))
            continue
        if message.is_error_frame:
            errors += 1
        elif message.is_rx:
            received += 1
        else:
            echoes += 1


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
        print("Generic sensor 1 sweeps from 60 to 70 (oil pressure in PSI).")
        print("Draining CANable TX echoes; submitted counts are not CAN acknowledgments.")
        print("Press Ctrl+C to stop.")

        started = time.monotonic()
        next_update = started
        next_status = started
        submitted = 0
        echoes = 0
        received = 0
        errors = 0

        while True:
            now = time.monotonic()
            elapsed = now - started

            # One complete sweep approximately every 12.6 seconds.
            sweep = (1.0 + math.sin(elapsed * 0.5)) / 2.0

            rpm = round(800 + 5200 * sweep)
            oil_pressure_psi = 60.0 + 10.0 * sweep

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
            submitted += 1

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
            submitted += 1

            if now >= next_status:
                print(
                    f"TX: RPM={rpm:4d}  "
                    f"Oil={oil_pressure_psi:5.1f} PSI  "
                    f"CLT=180.0 F  Battery=14.2 V  "
                    f"submitted={submitted} echoes={echoes} "
                    f"rx={received} error_frames={errors}"
                )
                if elapsed >= 2.0 and echoes == 0:
                    print(
                        "WARNING: No TX echoes observed. Check CANable status, "
                        "wiring, bitrate, and an active receiver for CAN ACKs."
                    )
                next_status = now + 1.0

            next_update += UPDATE_INTERVAL
            if next_update <= time.monotonic():
                # Avoid sending a burst if the computer falls behind.
                next_update = time.monotonic() + UPDATE_INTERVAL

            new_echoes, new_received, new_errors = receive_until(bus, next_update)
            echoes += new_echoes
            received += new_received
            errors += new_errors


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nSimulator stopped.")
    except (can.CanError, RuntimeError) as error:
        print(f"\nSimulator error: {error}")
        raise SystemExit(1)