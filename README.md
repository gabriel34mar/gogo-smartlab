# GOGO SmartLab

Initial coding slice for the offline-first prototype. The ESP32 is the access
decision maker, as requested. The Raspberry Pi service distributes the active
credential snapshot and accepts queued access events from the ESP32.

## Current state

This repository currently contains a small Python standard-library API,
credential command-line tool, and an ESP32 RFID-reading proof of concept. The
firmware only prints card UIDs; it does not grant access yet. There is no web
dashboard. The API is a development starting point, not a production security
boundary.

## Start the local API

On the Raspberry Pi (or a development computer with Python 3.10+):

```sh
cd gogo-smartlab/backend
export SMARTLAB_DEVICE_TOKEN='replace-with-a-long-random-local-token'
python3 server.py
```

The server listens on port 8080 by default and creates `smartlab.db` beside the
Python files. Keep the token private; the prototype uses plain HTTP and should
only run on a trusted isolated development network.

Add a test card after reading its UID from the ESP32 serial monitor:

```sh
python3 manage_credential.py add 04A1B2C3 'Test user'
```

Disable a card:

```sh
python3 manage_credential.py disable 04A1B2C3
```

## Device API contract

Send `Authorization: Bearer $SMARTLAB_DEVICE_TOKEN` on device endpoints.

- `GET /health` returns API liveness.
- `GET /v1/credentials` returns `{ "version": "…", "credentials": ["04A1B2C3"] }`.
  The ESP32 should persist the last valid snapshot locally and use it to make
  access decisions while disconnected. It should replace its snapshot only
  after receiving and validating a complete response.
- `POST /v1/events/batch` accepts up to 100 events, for example:

```json
{
  "events": [{
    "event_id": "a-unique-uuid",
    "device_id": "door-01",
    "card_uid": "04A1B2C3",
    "authorized": true,
    "occurred_at": "2026-11-16T18:00:00Z"
  }]
}
```

The event ID is an idempotency key, so retrying an upload does not create a
duplicate row. The ESP32 should queue events in persistent storage until the
server acknowledges them. The API records the decision reported by the node;
it does not independently authorize cards.

## Pin map from the project document

MFRC522 uses SPI: SDA/SS GPIO 5, SCK 18, MOSI 23, MISO 19, RST 22. Green LED
GPIO 32, red LED GPIO 33, active buzzer GPIO 25. Verify the exact ESP32 board
and module pin labels before wiring. Do not connect a door lock directly to a
GPIO; use a properly rated driver and separate actuator supply, or use an LED
as the first actuator simulation.

## Build the RFID proof of concept

With PlatformIO Core installed and the board connected over USB:

```sh
cd gogo-smartlab/firmware
pio run
pio run --target upload
pio device monitor
```

Expected serial output at 115200 baud: a reader-ready message, then one
`RFID UID: ...` line for each card presentation. This is a design expectation,
not a measured result; the firmware has not been compiled or flashed here.

Next implement firmware in small steps: (1) local credential snapshot and
decision, (2) LED/buzzer outputs based on that local decision, and (3) Wi-Fi
sync plus a persistent event queue. The PDF specifies the Arduino framework and
RC522 pin map, but confirm the exact board and installed components before
wiring or flashing firmware.
