# Vehicle Profiles

Suzuki Swift KWP initialization compatibility: profiles with explicit protocol 4/5 may use `ATSH` headers `8111F1`, `8101F1` and `8001F1` in `bus.init`, with `2100` for connection checks and data-block reads. This is engine support, not a new built-in/catalog vehicle or verification of a particular year. Standard OBD2, CAN vehicles and existing profiles retain their behavior; see [PID_GUIDE.en.md](PID_GUIDE.en.md).

English | [简体中文](VEHICLES.md)

The firmware only has **`OBD2 Generic`** (`obd2.generic`, standard SAE J1979)
built in. Every other vehicle is a **JSON vehicle profile**
(schema `skygauge.vehicle/1`) that gets installed/activated through the web UI
or companion app (via the `/api/v1/vehicles` API) — it is never compiled into
the firmware, so adding a vehicle needs no
firmware code changes.

```
Default behavior = firmware-builtin obd2.generic (standard OBD2, nothing to install)
Other vehicles are installed and activated through the web UI or App; changes take effect after reboot
```

The web editor at `/vehicles/editor` starts with standard OBD2, local JSON or a community profile, lists collection data and PIDs, and supports editing or adding items.
New standard profiles include 10 built-in standard items at editable one-second intervals; imported profiles retain their original frequencies.
Adding standard data preserves custom or disabled items and complete manufacturer requests and fallback fields for BMW, Jeep and other vehicles. See [PID_GUIDE.md](PID_GUIDE.en.md) for fields and formulas.

## Vehicle profile catalog

The catalog repo currently holds **18 profiles**
([vehicles/catalog/](../vehicles/catalog/)); the firmware only has
`obd2.generic` built in, the other 17 are JSON files waiting to be installed:

| id | Name | Protocol | Addressing | Gear source | Boost | Oil pressure (OBD) | Notes |
|---|---|---|---|---|---|---|---|
| `obd2.generic` | `OBD2 Generic` | Auto | Functional 7DF (`18DB33F1` on protocols 7/9) | Ratio estimate | ✓ | — | **Firmware-builtin**, generic SAE J1979 |
| `subaru.zc6.can` | `ZN/C6 CAN` | 6 locked | Physical 7E0 | Ratio estimate | — | — | The only profile using CAN broadcast monitoring (`ATMA`, `monitor` block); RPM still comes from OBD |
| `subaru.zc6.pid` | `ZN/C6 PID` | 6 locked | Physical 7E0 | Ratio estimate | — | — | Pure-OBD fallback for adapters that don't support ATMA |
| `subaru.zd8.obd` | `ZD8 OBD` | 6 locked | Physical 7E0 | Ratio estimate | — | — | Same polling as `subaru.zd8.pid`; both profiles are kept so the legacy NVS index maps 1:1 |
| `subaru.zd8.pid` | `ZD8` | 6 locked | Physical 7E0 | Ratio estimate | — | — | Same as above |
| `mazda.mx5nd` | `MX-5 ND` | Auto | Physical 7E0 | Ratio estimate | — | — | 40 ms short timeout (Mazda CAN answers fast) |
| `bmw.fg` | `BMW F/G` | 6 locked | Functional 7DF | Direct DID + ratio fallback | ✓ | — | G20/G21/G22; falls back to ratio estimation when the gear DID answer misbehaves (`fail_after 1`) |
| `toyota.supra_a90` | `Supra A90` | 6 locked | Functional 7DF | Ratio estimate | ✓ | ✓ | B58/B48; the only other profile with an OBD oil-pressure signal |
| `bmw.g.obd` | `BMW G OBD` | **7 locked** | Functional 7DF | Ratio estimate | ✓ | — | G-series auto-detect is unstable, locked to protocol 7 |
| `bmw.e` | `BMW E` | 6 locked | Functional 7DF | Direct DID | — | ✓ | E9x/E46/E39 |
| `mini.jcw_f56` | `JCW F56` | Auto | Physical 7E0 | Ratio estimate | ✓ | — | B48, FWD |
| `mini.r55` | `MINI R55` | 6 locked | Functional 7DF | Ratio estimate | ✓ | — | N14/N18/N16; UDS `22 58 22` always goes over physical 7E0 (see behavior changes below) |
| `porsche.997_2` | `POS 997.2` | 6 locked | Physical 7E0 | Ratio estimate (7-speed) | — | — | 987.2/997.2 DFI |
| `porsche.997_1` | `POS 997.1` | 6 locked | Physical 7E0 | Ratio estimate | — | — | 987.1/997.1 M96/M97 |
| `porsche.987_1` | `POS 987.1` | 3 locked | ISO 9141-2 native addressing | Ratio estimate | — | — | ELM protocol 3 verified on a 987.1; initial ratios copied from 997.1 need gearbox calibration |
| `alfa.giulia_20t` | `GIULIA 2.0T` | 7 locked | 29-bit functional `18DB33F1` | Ratio estimate | ✓ | — | Giorgio platform; 50 ms poll gap (slow UDS responses) |
| `jeep.generic` | `jeep` | 7 locked | 29-bit functional `18DB33F1` | Ratio estimate | — | — | Generic placeholder; ratios/tire still need to be refined for the real car |
| `honda.integra` | `Honda Integra` | 7 locked | 29-bit functional `18DB33F1` | Disabled (CVT) | ✓ | — | 11th-gen Civic platform L15C7; non-Type-R trims may lack a physical oil-temp sensor |

The oil-temperature source in `subaru.zc6.pid` remains the Mode 21 request
`2101`. `multiframe: true` means the response may span multiple frames. While
the firmware uses `ATCAF0` to handle that response manually, it wraps the
request as the ISO-TP single frame `02 21 01` (shown as `022101` in the ELM
command); do not add the length byte to the JSON.
To let ELM handle ISO-TP automatically, add `"multiframe_mode": "elm"` to
that source. The request remains `2101`, the full multiframe response is still
parsed, and the engine does not switch to `CAF0/CFC0`.

Numbers come from [`vehicles/catalog/*.vehicle.json`](../vehicles/catalog/);
field definitions are in [`contracts/vehicle-rpn.md`](../contracts/vehicle-rpn.md)
(the `skygauge.vehicle/1` schema, RPN formula opcodes, service/addressing
whitelist, static validation rules).

## Editable import pack

[vehicles/import/](../vehicles/import/README.en.md) provides **19 complete importable
templates**: independent `custom.*` copies of all 18 catalog profiles, plus
`custom.bmw.g.gear`. The original catalog has 18 profiles. Templates expose
request headers, response prefixes and polling intervals for customization. Copy
a selected JSON to the SD card's `OBD/IMPORT/`, then select the profile and reboot.

`custom.bmw.g.gear` reuses `bmw.fg` protocol 6 and the raw EGS request
`18 03 22 DA 2E` (`hdr: 6F1`, `rx: 618`, `expect: 62DA2E`) to read a gear
value of 0..8, with ratio fallback on failure. The protocol 7
`custom.bmw.g.obd` is still supplied separately. Choose for the actual vehicle;
generation adds no vehicle testing.

Requested signals use `rate: "round"` and `period_ms`: RPM 50 ms; speed 100 ms;
gear/load/throttle/boost 200 ms; AFR/oil pressure 500 ms;
temperatures/battery voltage 1000 ms. These are minimum intervals; polling still
runs serially. CAN monitoring and ratio derivation retain their original settings.
See the pack README and [PID_GUIDE.md](PID_GUIDE.en.md) for request formats,
bit decoding, adjustments and the complete file list.

## Inherit standard OBD2 by default

Only vehicle-specific signals need declarations. The root `defaults` field defaults to
`"obd2"`; omitted standard signals inherit the built-in Generic OBD2 requests, formulas,
units, filters and polling policies. The smallest importable profile can omit both
`bus` and `signals`:

```json
{
  "schema": "skygauge.vehicle/1",
  "id": "custom.mycar",
  "rev": 1,
  "name": "My Car"
}
```

These 10 standard signals are added:

| 信号 / Signal | PID |
|---|---|
| `obd.rpm` | `010C` |
| `obd.intake_temp` | `010F` |
| `obd.speed` | `010D` |
| `obd.coolant_temp` | `0105` |
| `obd.load` | `0104` |
| `obd.throttle` | `0111` |
| `obd.oil_temp` | `015C` |
| `obd.battery_voltage` | `0142` |
| `obd.afr` | `0144` |
| `obd.boost` | `0170`, fallback `010B` |

An explicit signal **replaces the entire default definition** for its id. Fields are
not merged, and a standard PID is not added as an automatic failure fallback. For
example, a custom `obd.oil_temp` polls only the oil-temperature paths written in its
`src` array. An id alone or only a `period_ms` change is insufficient: a complete
`src` / `derive` definition is still required. Omission uses the default; to explicitly
disable a signal, add this entry to `signals`:

```json
{"id": "obd.oil_temp", "disabled": true}
```

Disabled entries allow only `id`, `disabled` and `notes`, and are excluded from polling.
For fully custom requests or pure CAN monitoring, set root `"defaults": "none"`;
`signals` must then be a nonempty array. When `bus.proto` is 0 or omitted, a manually selected device protocol takes precedence. With `0 - Automatic`, detection results are cached only for the current session and do not overwrite the NVS selection. Sustained decode failures trigger another probe; an explicit `bus.proto` remains locked. Existing nonzero settings saved by older firmware cannot be distinguished from manual choices: select `0 - Automatic` once after upgrading. Without
`bus`, the other defaults are header `7E0`, `ATST=0x19` and a 30 ms request gap. Inherited standard requests use the
vehicle's `bus.hdr`; special headers remain configurable through each `src.hdr`.
On J1850 / ISO 9141-2 / ISO KWP2000 (protocols 1–5), ordinary OBD requests retain the ELM327's native addressing. CAN-style `bus.hdr` / `src.hdr` values such as `7E0` do not cause `ATSH` commands; `ATH0` hides response headers. Protocols 7/9 map standard OBD request headers `7DF` and `7E0`–`7E7` to their 29-bit functional/physical equivalents; existing 29-bit headers are retained. CAN responses support compact `ATH1 + ATS0` output and 29-bit headers printed as separate bytes.
An imported `ATSP0` does not overwrite an already selected concrete protocol. After adapter auto mode (`ATSP0` / `ATSPAh`), `ATDPN` confirms the actual protocol before polling. A rejected protocol-selection command cannot make a reply on the previous protocol count as a successful candidate. On protocols 7/9, standard receive filters `7E8`–`7EF` are also mapped to 29-bit addresses; native 29-bit physical requests support Mode 01 functional fallback. Ordinary request/setup timeouts also interrupt and wait for `>`; BLE write failures reconnect before another command can extend a partial line.
Drivetrain ratios and ratio-derived `obd.gear` are not inherited: declare a direct gear
source or supply the vehicle's own `drivetrain`. Automatic completion does not imply
ECU support for a PID; unavailable values still follow existing failure rules and
show `--`. Resource limits, polling rates and signal dependencies are validated
after completion.

[Sparse examples](../vehicles/examples/README.en.md) include a minimal standard OBD2
profile and a BMW G profile that declares only direct gear. Installation works the
same way and requires firmware with this inheritance feature.

## Install and activate

There are three equivalent ways to get a profile onto the device:

### 1. SD card drop folder (`/OBD/IMPORT`)

Drop these files into the SD card's `OBD/IMPORT/` directory; a scan runs on
boot or when the card is inserted:

| File | Effect |
|---|---|
| `<id>.vehicle.json` | Installs a vehicle profile (validated + compiled, not activated) |
| `ACTIVATE.TXT` | First line holds a profile id; activates it (takes effect on next boot) |
| `RESET.TXT` | Rolls back to `obd2.generic` |

Processing order (`sd_import.c`): `*.vehicle.json` files are installed first,
then `ACTIVATE.TXT` is processed; **`RESET.TXT` is always processed last and
overrides any `ACTIVATE.TXT` in the same batch.** Results are appended to
`OBD/IMPORT/IMPORT.LOG` (`<time> OK|ERR <file>: <detail>`); successful files
are renamed to `.ok`, failed ones to `.err` plus a matching `.err.txt` listing
the specific validator issues.

### 2. Web UI and companion App

Install, activate, roll back and remove vehicle profiles through the web UI or
companion App. The gauge has no vehicle-management page. The web UI and App use
the device API below; activation still requires a reboot to take effect.

### 3. `/api/v1/vehicles` API

This HTTP API is served on the OTA hotspot (requires `X-OTA-Token`) for the
web UI and companion App to manage profiles:

| Method | Path | Effect |
|---|---|---|
| `GET` | `/api/v1/vehicles` | Lists installed profiles + active state/safe mode/last error |
| `POST` | `/api/v1/vehicles` | Uploads and installs a profile (validate + compile + store, **not activated**) |
| `POST` | `/api/v1/vehicles/activate` | `{"id":"..."}`, activates it (returns `reboot_required:true`) |
| `POST` | `/api/v1/vehicles/reset` | Rolls back to `obd2.generic` |
| `POST` | `/api/v1/vehicles/rollback` | Rolls back to the previous profile |
| `POST` | `/api/v1/vehicles/remove` | `{"id":"..."}`, removes an installed profile |
| `GET` | `/api/v1/vehicles/<id>` | Reads a profile's raw source JSON |
| `DELETE` | `/api/v1/vehicles/<id>` | Removes a profile |

**Whichever method is used, activation only takes effect on the next
reboot** — the current boot keeps running the old profile.

Storage: installed profiles are kept at `/config/vehicles/<id>.json` on the
config partition, mirrored to the SD card at `/OBD/CFG/vehicles`; the active
profile is additionally copied into `nvs_app`'s `veh/blob` (≤8KB) so it
survives a config-partition wipe.

## Lifecycle and safety

State machine (`main/vehicle/vp_manager.c`):

```
install        -> INSTALLED
activate(id)   -> PENDING (takes effect on next boot)
next boot verification passes -> ACTIVE
3 boots without passing verification -> roll back to the previous profile,
                                         then to obd2.generic
```

**Verification rule**: wait 20 seconds after boot, require battery voltage
≥13.0V, and require `min(3, number of signals in the profile)` distinct
signal slots to answer (RPM counts toward this if the profile has it).

**Safe mode**: 3 consecutive crashes (panic / interrupt watchdog / task
watchdog reset) trigger safe mode, which forces `obd2.generic` and stops
processing SD-import `*.vehicle.json` / `ACTIVATE.TXT` files (`RESET.TXT` is
still processed, for manual recovery). The crash counter clears after 60
seconds of crash-free uptime.

## Legacy index migration

Legacy firmware stored a vehicle index in NVS (`vehicle_profile_idx`). After
an upgrade, the first boot maps it to a new profile id using the table below
and auto-installs + activates it
(`main/app_obd_dsp/first_boot_migration.c`'s `vp_manager_legacy_id()`):

| Legacy index | New id |
|---|---|
| 0 | `obd2.generic` |
| 1 | `subaru.zc6.can` |
| 2 | `subaru.zc6.pid` |
| 3 | `subaru.zd8.obd` |
| 4 | `subaru.zd8.pid` |
| 5 | `mazda.mx5nd` |
| 6 | `bmw.fg` |
| 7 | `toyota.supra_a90` |
| 8 | `bmw.g.obd` |
| 9 | `bmw.e` |
| 10 | `mini.jcw_f56` |
| 11 | `mini.r55` |
| 12 | `porsche.997_2` |
| 13 | `porsche.997_1` |
| 14 | `alfa.giulia_20t` |
| 15 | `jeep.generic` |
| 16 | `honda.integra` |

Migration first looks for `OBD/CATALOG/<id>.vehicle.json` (or `<id>.json`) on
the SD card to install + activate; if not found, it records the target id in
NVS and retries later once an SD card is inserted or in the background.
**If the user manually resets or activates a different vehicle in the
meantime, migration no longer overrides that choice**
(`first_boot_migration_drop_vehicle()`, triggered internally by `RESET.TXT`,
the web UI or App's `/api/v1/vehicles/reset`, or any manual `activate` call).

## Behavior changes vs legacy firmware

These differences come from the old-vs-new comparison tests in
`test/host/vehicle/fixtures/golden/*.json`, pinned by the host unit tests:

- **`obd.boost` (7 turbo profiles)**: legacy firmware truncated
  `(MAP-100)/10` to 0.1 bar resolution; the new profiles store 0.01 bar
  resolution, so the display is finer-grained
- **`mini.r55`'s `22 58 22` (oil-temp query)**: legacy sent it on the
  functional header 7DF; the new engine refuses service `22` on a functional
  header and always sends it on the physical header 7E0 instead
- **`bmw.fg`'s `obd.gear`**: legacy fell back to the ratio-estimated gear
  after a single out-of-range DID answer; the new engine uses
  `fail_after 1` + `fallback: "ratio"` — more explicit, same net effect
- **`subaru.zc6.can`'s CAN coolant/oil temp**: when no frames arrive within
  `monitor.stale_ms` (15000 ms), legacy firmware wrote coolant as -40 and
  marked oil invalid; the new engine marks both invalid (no more fake -40°C)
- **`mazda.mx5nd` / `mini.jcw_f56` / `obd2.generic`'s `ATSP0`**: when
  `bus.proto` is 0, the engine retains the user's manual/automatic selection. Auto results are session hints and are discarded after sustained decode failures, rather than saved as a forced NVS protocol

## ADS1115 oil-pressure sensor (V1 hardware) fallback

The V1 hardware board (`CONFIG_OBD_HW_VERSION_V1_WAVESHARE`) has an ADS1115
analog oil-pressure sensor wired in. The firmware only enables ADS1115
sampling when **the active profile has no `obd.oil_pressure` signal**
(`main/app_main.c`):

```c
#if CONFIG_OBD_HW_VERSION_V1_WAVESHARE
    if (!vp_manager_has_signal(SIG_OBD_OIL_PRESSURE)) {
        signal_store_set_local(SIG_OBD_OIL_PRESSURE, true);
        oil_pressure_start();
    }
#endif
```

Currently only `toyota.supra_a90` (DID `4436`) and `bmw.e` (DID `586F`) carry
an OBD oil-pressure signal; activating either disables ADS1115. Every other
profile (including `obd2.generic`) falls back to ADS1115.

## Adding or editing a vehicle

No firmware code changes needed — just write a JSON file that matches the
`skygauge.vehicle/1` schema:

1. Follow [`contracts/vehicle-rpn.md`](../contracts/vehicle-rpn.md) (signals,
   sources, RPN formulas, CAN rules, the service/AT whitelist — with 3 full
   worked examples); for a tutorial-style walkthrough you can copy field by
   field, see [PID_GUIDE.en.md](PID_GUIDE.en.md)
2. Use any existing file in [`vehicles/catalog/`](../vehicles/catalog/) as a
   starting template
3. Install + activate it through one of the three methods above, and check
   `IMPORT.LOG` or `GET /api/v1/vehicles`'s `last_error` for validation
   failures (error codes such as `E_SIZE`/`E_SCHEMA`/`E_LIMIT`/`E_SIGNAL`/
   `E_SERVICE`/`E_AT`/`E_RPN_*` are documented in the schema doc)
4. Limits: profile text ≤16384 bytes, ≤48 signals total (of which ≤32 may be
   custom `veh.*` signals)
5. To discover private PIDs for an unknown car, use the
   [fake-ELM327 hunting workflow in the tools](DEVELOPMENT.en.md#tool-scripts) —
   no car needed

When a profile JSON is added to or replaced in the catalog, update the
catalog table at the top of this file to match.

## Clear installed vehicle profiles

The web USB flasher provides “Clear installed vehicle profiles”. After explicit checkbox confirmation, it deletes config `/vehicles/*.json` and SD `/OBD/CFG/vehicles/*.json`, including the current profile, and schedules builtin `obd2.generic` for reboot. Mounted SD is required and deletion errors stop the operation. The current compiled profile remains alive until reboot so polling cannot access freed memory. CATALOG / IMPORT / EXPORT templates, themes and boot animations are retained. Firmware supporting `vehicle.clear` is required.

## Boost and high temperature readings

Builtin Generic and profiles omitting `obd.boost` request standard PID `0170` first. From its 10-byte multi-frame response, the profile decodes Boost Pressure Sensor A bytes D/E as `(D*256+E)/32` kPa absolute, then subtracts a fixed `100 kPa` to produce relative pressure. Invalid or unsupported `0170` falls back to `010B` and estimates `MAP - 100 kPa`. Both paths preserve negative pressure in `kPa_rel`; the builtin display rounds to 0.1 bar, with needle and chart ranges starting at -1.0 bar. The vehicle table lists explicit boost definitions. The vehicle must support one of these PIDs; missing or stale data still displays `--`. The fixed 100 kPa reference is affected by altitude and weather; accurate gauge pressure should subtract the vehicle's measured barometric pressure.

Coolant and oil signal limits and builtin gauge scales now extend to 255°C; oil hysteresis and ESP-NOW v5 receive limits are extended too. Large temperature jumps still require consecutive confirmation. Standard single-byte `0105` / `015C` with `A-40` can only represent up to 215°C; 255°C requires a capable source formula and cannot be created by raising limits. Catalog profiles are updated to rev 3 with matching import templates. Firmware does not overwrite explicit old `valid` bounds or negative-pressure clamps in installed profiles; reimport the updated profile and check custom formula limits.

## RPM, speed and driver-input acquisition scheduling

Firmware schedules demanded RPM, speed, boost and throttle requests ahead of background parameters. Without `period_ms`, RPM/speed have minimum request intervals of 50 / 100 ms, while boost/throttle are serviced in each batch. Explicit `period_ms` remains authoritative, and explicit `rate: "slow"` keeps boost/throttle on the slow path. New import templates use RPM 50 ms, speed 100 ms and boost/throttle 200 ms; installed profiles are not rewritten automatically.

Deadlines use actual send times. Signals sharing a PID are deduplicated within a batch. After at least two requests have completed in a longer batch with at least two requests remaining, at most one overdue RPM refresh is inserted before finishing the remaining parameters. Background polling retains service, and before/after commands, multiframe transactions and prompt waits remain uninterrupted. Consecutive failures back off for 1 / 2 / 4 / 8 / 16 seconds; success and reconnect reset the backoff. Only demanded page/gear signals are requested; RPM remains enabled.

The speed cache retains actual samples. Built-in numeric, speed and needle pages show the latest RPM, speed, boost, throttle and load samples without an additional display ramp; slower inputs such as temperature retain smoothing. Periods are minimum intervals, not guaranteed 20 / 10 Hz rates: adapter, protocol, timeouts, request count and profile determine throughput. Additional simultaneous priority inputs share the serial bandwidth; see the simulated scheduling comparison in [PERFORMANCE.en.md](PERFORMANCE.en.md#driver-input-priority-scheduling-validation).

## Scirocco 2.0T profile library check

The user-provided Car Scanner `vehicle-profiles.json` (1304 profiles) contains no dedicated profile named `Scirocco`, `Sirocco` or `尚酷`. Related profiles `872` (`PQ25, PQ35, PQ46`) and `873` (`PQ25, PQ35, PQ46 + UDS AT/DSG DQ500`) contain VW parameters, but their ECM pressure request is `VWTP:01:2173`, a transport the current engine does not support. Dashboard `222211` is a percentage and cannot be imported directly as bar. Profile `742` for EA888 Gen.3 / MQB cannot be considered compatible based only on 2.0T displacement. Year, engine and ECU information require verification. No unverified dedicated Scirocco profile is added; the generic standard PID path remains independently usable.

Oil-temperature hysteresis now confirms jumps of 16–19°C and rounds negative temperatures correctly.
