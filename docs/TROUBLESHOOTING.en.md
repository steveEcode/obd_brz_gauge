# Troubleshooting

English | [简体中文](TROUBLESHOOTING.md)

When the gauge won't connect or shows no data, work through the layers in
order: **BLE link first, then protocol, then data.** With USB attached, watch logs via `idf.py monitor` — key tags
in [log keywords](#log-keywords).

```
Can the gauge see & join the adapter over BLE? ──no──→ Step 1
        │ yes
Protocol handshake OK, RPM readable? ──no──→ Step 2
        │ yes
Every channel you need has data? ──no──→ Step 3
        │ yes
Still misbehaving ──→ Step 4, last resorts
```

## Step 1: the BLE link

1. **Swipe up from the version page** into BLE SCAN and check whether the OBD
   device appears in the list
2. Select it; connection state is shown on the **version page** (status +
   device name)

**Device not found:**

- Make sure the adapter is powered (plugged into the OBD port / ignition ON)
- Confirm it is a **BLE (Bluetooth 4.0+)** device — old Bluetooth-Classic-only
  ELM327s cannot connect
- Power-cycle the adapter and rescan

**Won't connect / drops immediately:**

- First verify the adapter works by connecting with a phone OBD app
  (Car Scanner, Torque, …)
- If a phone app holds the connection the gauge cannot join — disable the
  app's auto-reconnect
- BLE channels support both Write Request and Write Without Response. Only the subscribed data characteristic feeds ELM responses, so notifications from other characteristics cannot corrupt them.

## Step 2: protocol

### Protocol auto-detection

With the protocol set to **0 (auto)** and no `bus.proto` lock in the profile, the gauge probes:

```
ATZ → wait for >
   ↓
6 → 7 → 4 → 5 → 3 → 1 → 2 → 8 → 9 → A → B
   ↓
ATZ / ATD / ATD0 → common setup → ATSP<n> → check_connection (default 0100)
   ├─ positive response → cache session protocol → init → polling
   └─ failure → retry candidate (up to 3 attempts) → next candidate
   ↓
all failed → ATSP0 → init → polling
```

- Command responses, protocol probes and connection warm-up default to at most 5 seconds, including protocols 3/4/5 and `ATSP0`; a prompt advances the engine immediately. Waiting for `>` after a timeout allows a separate 5 seconds. Protocol candidates, setup and retries add up, so first data is not guaranteed within 5 seconds; adapters/vehicles needing longer to establish a connection may be interrupted early.
- After a timeout, the engine interrupts the adapter and waits for `>`; a missing prompt triggers reconnection so another command cannot collide with ongoing initialization.
- Detection uses the profile's `bus.check_connection` (default `0100`) and accepts only a matching positive response. Probe data is not displayed as gauge data.
- The default automatic-detection connection check is `0100`. If no matching positive response arrives and the profile contains an RPM request, the same protocol candidate is checked once with `010C`. This applies to the built-in generic profile, imported profiles and explicit `check_connection: "0100"`. A timed-out `0100` is interrupted and drained to `>` before checking RPM; a missing prompt requires reconnection. Other explicit connection checks retain their meaning, and profiles without RPM do not gain an implicit PID.
- Successful results are reused only in the current session and do not change the NVS Automatic selection into a fixed protocol. After 10 seconds without valid polling data, Auto probes again. Profile or manual protocol locks retain their normal self-heal/reconnect behavior.
- Older firmware may have saved an automatic result as a nonzero setting. After upgrading, select `0 - Automatic` once on the protocol page, then long-press to save and reboot.
- CAN reception supports compact `ATH1 + ATS0` output; protocols 7/9 use the corresponding 29-bit standard OBD request addresses, while protocols 1–5 retain native addressing and `ATH0`.

### Selecting a protocol manually

The firmware only builds in one generic profile, `Generic OBD2`; a
car-specific profile has to be installed from the vehicle catalog
(`vehicles/catalog/*.vehicle.json`) via the SD card's `/OBD/IMPORT` or the
phone app. Once installed, the protocol is already locked for that car and
you don't need to pick it here (most post-2010 cars are 6 or 7; the catalog's
exact lock values are in [VEHICLES.en.md](VEHICLES.en.md)). If you haven't
installed the matching profile yet and want to try protocols by hand, entry
point: **double-tap the logo page** → protocol roller → choose →
**long-press 2 s to save and reboot**.

Standard ELM327 protocol numbers (`ATSP`):

| # | Protocol | Typical cars |
|---|----------|--------------|
| 0 | Auto-detect (recommended starting point) | all |
| 1 | SAE J1850 PWM | older US cars (Ford) |
| 2 | SAE J1850 VPW | older US cars (GM) |
| 3 | ISO 9141-2 | older European cars |
| 4 | ISO 14230 KWP2000 (5 baud) | European cars |
| 5 | ISO 14230 KWP2000 (fast init) | European cars |
| 6 | ISO 15765-4 CAN 11-bit 500k | **most modern cars** (BRZ ZN/C6, BMW F/G, Porsche, …) |
| 7 | ISO 15765-4 CAN 29-bit 500k | BMW G OBD, FCA / 11th-gen Honda |
| 8 | ISO 15765-4 CAN 11-bit 250k | a few |
| 9 | ISO 15765-4 CAN 29-bit 250k | a few |
| 10 | SAE J1939 CAN | commercial vehicles |
| 11 | User-defined CAN protocol 1 | special adapters |
| 12 | User-defined CAN protocol 2 | special adapters |

Manual trial procedure: switch protocol → reconnect the adapter → wait 3–5 s
and see whether RPM shows a value (idle typically 600–1200). If RPM works the
protocol is right; individual missing channels are a Step-3 problem.

## Step 3: polling and parsing

### PIDs on the standard poll loop

| PID | Meaning | When missing |
|-----|---------|--------------|
| `01 0C` | RPM | 0 |
| `01 0D` | Speed | 0 |
| `01 05` | Coolant temp | 0 |
| `01 0F` | Intake air temp | 0 |
| `01 04` | Engine load | −1 |
| `01 11` | Throttle | −1 |
| `01 42` | Battery voltage | 0 |
| `01 5C` or manufacturer-specific (mode 21/22) | Oil temp | −100 (strategy is per-vehicle, see [VEHICLES.en.md](VEHICLES.en.md)) |

**Everything zero / error values**: polling never started or init failed →
back to Step 2. **Only some channels missing**: the PID isn't supported on
your car or needs a manufacturer-specific path — verify the same PID with a
phone app first; for genuinely private PIDs, dig them out with the
[PID-hunting workflow in DEVELOPMENT.en.md](DEVELOPMENT.en.md#tool-scripts)
and add a vehicle profile.

### Reading raw logs (USB required)

```
elm327_ble: Send 01 0C
elm327_ble: RAW[...]: 41 0C 12 34     ← healthy: got a response
elm327_ble: Parsed RPM: 4660
```

```
elm327_ble: Send 01 0C
elm327_ble: RAW[...]: ?               ← broken: wrong protocol / no bus reply
elm327_ble: PARSE_FAIL / TIMEOUT_AGAIN
```

## Step 4: last resorts

1. Power-cycle the whole chain: gauge → OBD adapter → ignition, then reconnect
2. Re-check settings: right vehicle profile? (protocol lock and oil-temp
   path follow the profile) Is the BLE device name the intended adapter?
3. Repeated init failures or NVS errors in the log: reflash over USB (a full
   first flash erases NVS anyway); for WiFi OOM crashes in master/slave mode,
   set Settings → MULTI-GAUGE back to standalone (takes effect after reboot)

## Log keywords

| Tag / keyword | Meaning |
|---------------|---------|
| `elm327_ble` | BLE connection, init, polling, protocol detection (`[DETECT]` prefix) |
| `vp_manager` | vehicle switching, verification period, safe mode, auto rollback |
| `sd_import` | processing results for SD card `/OBD/IMPORT` |
| `storage` | background storage service (mirroring, resume, periodic scans) |
| `theme_lib` / `theme_mount` | theme-library write progress / resuming a theme write at boot or card insert |
| `theme_engine` | theme partition loading / fallback |
| `coredump` | core dump storage (paired with `GET /ota/coredump`) |
| `ota_wifi` | OTA WiFi hotspot mode and firmware / theme / boot-animation pushes |
| `ERROR` / `WARN` | any module |

To debug-log: `idf.py menuconfig → Component config → Log output → Default
log verbosity → Debug`, then rebuild. Filtered viewing:

```bash
idf.py monitor | grep -E "elm327|DETECT"
```

## FAQ

**Q: every value on screen is 0?**
Polling never started or init failed. Go through Steps 1–2 and check whether
the init commands get answers in the log.

**Q: connects but no fresh data?**
Wrong protocol (most modern cars are 6/7); or a phone app is holding the
adapter. Try another protocol and reconnect.

**Q: RPM/speed fine but oil temp / load missing?**
The oil-temp path is per-vehicle (standard 5C is only supported on some cars;
Toyota/Mazda/BMW use private modes) — confirm you have the right catalog
profile installed and activated (the firmware only builds in `Generic OBD2`;
a car-specific profile has to be installed from the catalog, see the next
entry). If it still fails, that car needs a new oil formula
([VEHICLES.en.md](VEHICLES.en.md)).

**Q: how do I tell a dead adapter apart?**
Install Car Scanner / Torque Pro on a phone and connect to the same adapter.
Phone can't read either → adapter or car problem; phone fine but gauge not →
open an issue with the log attached.

**Q: which protocol does the BRZ ZD8 (Gen2) use? How do I install the profile?**
Protocol 6. The firmware only builds in the generic `Generic OBD2`; the
`subaru.zd8.obd` and `subaru.zd8.pid` profiles both live in the vehicle
catalog (`vehicles/catalog/`) and have to be installed first: drop the
matching `*.vehicle.json` into the SD card's `/OBD/IMPORT` (or push it via
the phone app), then select it on the vehicle page and tap USE — or drop an
`ACTIVATE.TXT` with the profile id into `/OBD/IMPORT` at the same time to do
it in one step. Both profiles already lock protocol 6 and read oil temp via
standard PID `01 5C`.

**Q: do multiple gauges interfere over protocols?**
No. Each gauge stores its protocol in its own NVS.

**Q: the SD card or an import does not work?**

Firmware supports FAT16 / FAT32 / exFAT; format an unformatted card on a computer. The device never formats it automatically. Check the card slot, wiring and mount logs. The background service scans `/OBD/IMPORT` on boot or card insertion; inspect `/OBD/IMPORT.LOG` or the import log on the OTA page for results. Select and activate an installed profile separately; installation does not switch profiles automatically.

**Q: a theme or boot-animation upload reports busy?**

Another storage job may be running. Wait for it to finish and retry. If it keeps failing, check the upload response and device logs for the actual error.

**Q: the VEHICLE button on the vehicle page shows `SAFE`, or the gauge keeps running Generic and won't switch vehicles?**
That's **safe mode**: after 3 abnormal resets in a row (crash / watchdog),
the next boot is forced to run only `Generic OBD2`, no pending vehicle
switch is promoted, and the SD card only processes `RESET.TXT`. Running
normally for 60 seconds clears the crash counter and recovers
automatically. To get out immediately: tap **GENERIC** on the vehicle page,
or drop a `RESET.TXT` file into `/OBD/IMPORT` on the SD card. If the vehicle
page instead shows `FAIL`, that's a different thing — a vehicle failed
verification on 3 boots in a row and was auto rolled back; it is not safe
mode, and the reason is in the vehicle page's error area.

**Q: how do I retrieve a core dump?**
Enter OTA mode, join the `OBD-Gauge-OTA-XXXX` hotspot with a phone, open
`http://192.168.4.1/` in a browser, and click Download in the "Core dump"
panel — or call `GET /ota/coredump` directly (needs the `X-OTA-Token` from
`GET /ota/discover` first). With no core dump stored it returns
**204 No Content** (the web page shows "No core dump stored."), not 404.
Clear it afterwards with Erase or `POST /ota/coredump/erase`.

---

Further help: [GitHub Issues](https://github.com/steveEcode/obd_brz_gauge/issues) —
attach an `idf.py monitor` log plus your car and adapter model.
