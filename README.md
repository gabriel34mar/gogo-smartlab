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

## Run the API in Docker

Docker Compose keeps the SQLite file in a named volume, so it persists when the
container is recreated. On the Raspberry Pi, from the project directory:

```sh
cp .env.example .env
```

Edit `.env` and replace the sample token with a long random value, then start
the service:

```sh
docker compose up -d --build
docker compose ps
docker compose logs -f smartlab-api
```

The API is available on port 8080 of the Pi. To stop it, run
`docker compose down`. The named `smartlab_data` volume remains in place, so
the database is preserved. Do not use `docker compose down -v` unless you
intend to delete that volume and its database. The `.env` file is excluded from
Git; do not commit its token.

## Dashboard

Open `http://<RASPBERRY_PI_IP>:8080/` from a device on the same local network.
Enter the local API token to view event totals and the latest access records.
The dashboard refreshes every five seconds. This prototype uses the device API
token for dashboard reads; use only on the trusted project network.

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

The current firmware uses FreeRTOS tasks: `RFIDTask` handles card reads,
authorization and feedback; `NetworkTask` handles Wi-Fi, credential/time sync
and event uploads. Access decisions happen on the ESP32 from its persisted
local cache. It signals with the documented LEDs and buzzer. It does not drive
a door lock because the project pin map does not specify a lock driver pin or
circuit.

1. Copy the local configuration template and edit it:

```sh
cp firmware/secrets.example.h firmware/secrets.h
```

Set the Wi-Fi SSID/password, Raspberry Pi API address, and the same device token
used in the Pi's `.env`. Keep `firmware/secrets.h` private; it is ignored by
Git. Example API address for the current Pi network is
`http://192.168.1.115:8080`; use the Pi's current LAN IP if it changes.

2. Provision a test card on the Pi. First read its UID in Serial Monitor, then
run this from the project folder on the Pi:

```sh
sudo docker compose exec smartlab-api python manage_credential.py add 04A1B2C3 'Test user'
```

Replace the sample UID with the card's actual UID. The ESP32 refreshes the
credential snapshot from the Pi at startup and every two minutes. It keeps the
last valid snapshot for authorization while offline and queues access events
in LittleFS for upload when connectivity returns. If its 100-event queue fills,
it denies access rather than granting without being able to persist the event.

3. With PlatformIO Core installed and the ESP32 connected over USB, from the
firmware folder:

```sh
cd firmware
pio run
pio run --target upload
pio device monitor
```

Expected serial output at 115200 baud includes the RFID UID and local decision,
plus `PERF` metrics for RFID read, authorization lookup, event persistence, and
elapsed time from the RFID task's reader call until the indicator GPIO changes.
The 10 ms task polling wait before that call is not included, so this is not a
full RF-field-to-LED measurement. Those values must be measured on the actual
ESP32; no experimental result is claimed here. RFID UID-only cards
such as MIFARE Classic can be cloned, so this is a controlled prototype, not
strong production security.
