# Round Radar

A small round desk display with two uses: a **radar scope** of the aircraft overhead, and a
**clock** for the rest of the time. A second gift, for a different person, and a **separate
project** from the Raspberry Pi
[Flight Radar Display](https://github.com/dripster82/flight-radar-display). It shares no code
with the Pi app.

Status (10 November 2026): firmware v0.1.0 is written and compiles (`firmware/`). It has **not
yet run on the board**; the first full test is listed in [TESTING.md](TESTING.md).

## How it differs from the Pi build

| | Pi Flight Radar Display | Round Radar (this project) |
|---|---|---|
| Hardware | Raspberry Pi 4, 7" touchscreen, receiver, GPS | One ESP32-S3 board with a 2.1" round touchscreen |
| Aircraft data | Its own antenna, plus Flightradar24 | A free online feed, over Wi-Fi |
| Flightradar24 website | Yes, signed in | No (it cannot run a web browser) |
| Location | GPS | A postcode, entered once at setup |
| Works without internet | The local radar does | Only the clock does |
| Software | Electron app on Raspberry Pi OS | C++ firmware, no operating system |
| Power cuts | Needs the read-only system and backups | Nothing to corrupt; just restarts |
| Parts cost | £240.41 | £0 (board owned); a case and optional battery to add |

## Hardware

**Waveshare ESP32-S3-Touch-LCD-2.1** (owned).

| Feature | Detail | Use here |
|---|---|---|
| Processor | ESP32-S3R8, dual-core, 240 MHz | One core draws, the other fetches data |
| Memory | 16MB flash, 8MB PSRAM | Map outlines, screen buffers, aircraft lists |
| Screen | 2.1" round IPS, 480×480, ST7701, capacitive touch (CST820) | Scope and clock |
| Wireless | 2.4GHz Wi-Fi, Bluetooth 5 LE | Aircraft data, time, setup from a phone |
| Clock chip | PCF85063, with a battery header | Keeps the time when Wi-Fi is down or before it connects |
| Battery | MX1.25 header for a 3.7V lithium cell, charging built in | Optional: keeps running when unplugged |
| Buzzer | On board | Optional: a ping when a plane passes overhead |
| Motion sensor | QMI8658, 6-axis | Not needed |
| microSD slot | On board | Not needed |
| USB-C | Two ports (one USB, one USB-to-serial) | Power, and programming from the Mac |

Still to choose: a case or stand, and whether to fit a battery. A stand made for this board is
on Printables ("Pouzdro pro kulatý Waveshare 2.1″ displej s ESP32-S3", model 1792969).

## What it will do

Screen mockups and the full flow between screens are in [mockups.html](mockups.html).

### Radar scope (main use)

- Coastline wireframe on black, a rotating sweep that lights the coastline and aircraft as it
  passes and lets them fade, and range rings in km. The same look as the "Radar scope" map style
  in the Pi app.
- Live aircraft around the home position, each with a short trail.
- **Tap** picks the nearest aircraft and shows its callsign, altitude, speed, heading, type and
  registration. Tap again to go back.
- **Swipe sideways** steps the range: 10, 25, 50, 100 km.

### Clock (secondary use)

- Large digits drawn as seven-segment figures in the scope's green, with the date beneath and a
  seconds ring round the rim. No font files are needed for this.
- 12 or 24 hour, chosen in settings.
- UK time with automatic summer time. Set from the internet, then kept by the clock chip, so it
  is right at switch-on even with no Wi-Fi.
- A small line shows how many aircraft are in range, so the clock still hints at the sky.

### Moving between the two

- **Swipe up or down** switches between scope and clock.
- The device stays on the screen picked. It never switches by itself, and it opens on the same
  screen after a restart.

### Aircraft details

- **Tap an aircraft** on the scope for its details: callsign, type in words, altitude, speed,
  heading, climb and distance. Tap to close.
- **Swipe sideways** on the details for a second page: a photo of that aircraft with the
  photographer's name, where the flight is from and to, the airline, type code, registration
  and squawk. While an aircraft's own photo downloads, or when it has
  none, a stored photo of another aircraft of the same type stands in, with a strip marked
  SIMILAR across its corner. Swipe again for a third: a QR code that opens the photo's own page on the site
  it came from (the photo sites ask for that link). Dots at the foot show which page is open. The photo and route
  are only fetched when this page is opened, about 50 KB each time. Every aircraft
  looked up is kept, so opening it again uses no data, even after a restart. With a memory card
  in the slot (FAT32) it goes there, in a `roundradar` folder: room for thousands, and saving
  does not disturb the screen. Without a card it goes to the chip's own storage: room for about
  100 photos, the oldest making way for new ones, and the screen goes dark for about a second
  as the photo page closes, because writing there would otherwise show as a burst of garbage. A photo is looked
  up afresh after a month. The route belongs to the flight, so the same aircraft on another
  flight has its route looked up again; its photo is not downloaded again.
- **Storing Planespotters pictures needs their permission.** Their public terms do not allow
  it. It is switched on here (`PHOTO_STORE` in `Config.h`) with permission given to the author;
  anyone else building this should ask them, or set it to 0.

### Both screens

- **Long press** opens settings: brightness, 12 or 24 hour clock, night dimming (on or off, with
  its own From and Until hours), the buzzer ping on or off, a **Postcode** button, and a
  **Wi-Fi setup** button.
- **The picture is turned 315 degrees**, touches included, so the device sits with its USB
  socket at the bottom (unturned, the socket is at about half past four). It is a build
  setting, `SCREEN_TURN_DEG` in `Config.h`; `rotate <degrees>` over USB changes it on one
  device. Turned, the scope runs at about 10 pictures a second; at 0 it runs at about 15.
- **Helicopters** show as a rotor cross with a tail, planes as an arrowhead. On the photo page,
  **tapping the photo enlarges it**; tap again to go back.
- **Battery.** The clock screen shows a battery with its percentage, and a lightning bolt
  across it while power is coming in. With a memory card fitted, a line a minute goes to
  `roundradar/battery.csv` (voltage, percent, power in, screen turn, backlight); over USB,
  `log` prints it and `clear log` deletes it. Power coming in is worked out from the battery
  voltage and from a computer on the socket, because the board has no pin for it.
- **Wi-Fi setup** lists the networks nearby on the device, strongest first, five to a page.
  Tapping one opens the keypad for its password (lower case, capitals and symbols). The saved
  network is only replaced once the new one has joined. A **QR code** button on the list leads
  to setup from a phone instead; a hidden network needs that route.
- **Postcode** opens a keypad of letters and digits on the device. OK looks the postcode up
  over the Wi-Fi already joined; the saved place is only replaced once the new one is found.
- **After a power cut** it shows the last screen within 12 seconds even if the router is still
  starting, and keeps retrying Wi-Fi by itself. It only shows setup when nothing has been saved.
- **Night hours** (the night page in settings): the screen dims to a chosen level (1% to 30%),
  optionally turns red instead of green for those hours, and aircraft downloads rest. A touch brightens the
  screen for 30 seconds and brings the aircraft back for five; that first touch does nothing
  else, so it cannot open an aircraft or change the range by accident.
- **Factory reset:** hold the BOOT button on the side for 8 seconds, from any screen. A warning
  counts down; letting go cancels. It forgets the Wi-Fi, postcode and every setting, then
  restarts into first setup. The RESET button beside it only restarts.
- **First setup:** with no Wi-Fi saved, the device opens on the network list, then asks for the
  password and the postcode on its own keypad. No phone is needed.
- **Setup from a phone** (the QR code button): the device shows its own setup network and a QR
  code. Joining it opens a short form for the Wi-Fi network, password and postcode. A long
  press goes back to the list.

## What it will not do

- No Flightradar24, and no street or satellite map: only the wireframe scope.
- No receiver of its own and no GPS. The scope needs Wi-Fi; the clock does not.
- No weather, rain radar, forecast or web settings page. These exist in the firmware we borrow
  from and are deliberately left out.
- No comfortable keyboard: 2.1" (about 53 mm across) only takes keys about 4.3 mm wide. That is
  enough for a postcode and a Wi-Fi password, typed once; the phone route is there for anyone
  who finds it fiddly.

## Limits of the screen size

- Readable on a desk or bedside table, not from across a room. The clock digits are the
  exception and will be drawn as large as the circle allows.
- Aircraft close together cannot be tapped individually, which is why a tap picks the nearest.

## Software base

The firmware is our own, built with the Arduino ESP32 core, but it starts from parts of
**MeteoPlaneRadar** by Petr Šrámek (chiptron.cz, MIT licence,
`github.com/petus/MeteoPlaneRadar`, v0.8.5), which runs on this exact board. A whole fork was
rejected: that firmware only redraws when new data arrives and every download blocks its single
loop, which a continuously sweeping scope cannot live with.

| Part | Source | Work needed |
|---|---|---|
| Screen driver, two buffers, synced to refresh (`Display_ST7701`, `Board`, `TCA9554`) | MeteoPlaneRadar | Take as is, with a trimmed `Config.h` |
| Reading the photo JPEGs (`stb_image.h`) | stb_image v2.30 by Sean Barrett, public domain, `github.com/nothings/stb` | Taken as is. The decoder built into the chip cannot read the progressive JPEGs the photo sites send |
| Touch (`Touch_CST820`) | MeteoPlaneRadar | Take as is |
| Drawing surface (`Canvas16`) | MeteoPlaneRadar | Remove its smooth-font hook |
| Map outlines (`EuMapData.h`) and the border drawing in `EuBorder` | MeteoPlaneRadar | Take the border half; drop the city labels |
| Aircraft feed client (`ADSB`, `NetSink`) | MeteoPlaneRadar | Cut its ties to the weather clock, status page and icons; turn on certificate checking |
| QR code (`qrcode.c`) | MeteoPlaneRadar (bundled, MIT) | Take as is |
| Main loop, scope drawing, clock screen, clock chip, phone setup, settings | New | Written here |

Their Wi-Fi setup page is not reused: it is tied to their web settings and language system. Ours
is a single form served by the web server that comes with the ESP32 core.

Borrowed files keep their copyright headers, and this project will carry the MIT notice and a
credit to chiptron.cz. The map data needs credit to Natural Earth and GeoNames.

### How the firmware is arranged

- **Drawing core:** a loop that draws a frame every refresh. The coastline and rings are drawn
  once into a spare buffer whenever range or position changes. Each frame blends that buffer
  with the sweep's fading brightness, then adds aircraft.
- **Network core:** a background task fetches aircraft every 15 seconds (less often at long
  range, backing off on errors) and hands a finished list to the drawing core. The sweep never
  waits on a download.
- **Settings** (Wi-Fi, position, screen picked, range, brightness, clock format, night dimming,
  ping) are stored
  in the board's built-in settings memory.

Build tools on the Mac (all installed): arduino-cli 1.5.1, ESP32 core 3.3.10, GFX Library for
Arduino 1.6.9, ArduinoJson 7.4.3. `firmware/flash.sh` builds, flashes over USB and shows the log. Board settings: ESP32S3 Dev Module, PSRAM OPI
(the screen stays black without it), 16MB flash, USB CDC on boot.

## Data sources

| Need | Source | Status |
|---|---|---|
| Aircraft near a position | adsb.fi open data: `opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nautical miles>`, no account | Tested 10 Nov 2026, returns live aircraft. Free for personal, non-commercial use |
| Postcode to position | `api.postcodes.io/postcodes/<postcode>` (free, UK only) | Tested 10 Nov 2026, returns latitude and longitude |
| Route of a flight | `api.adsbdb.com/v0/callsign/<callsign>`, no account | Tested 11 Nov 2026. From a database of callsigns, so it can be missing or out of date |
| Photo of an aircraft | `api.planespotters.net/pub/photos/hex/<ICAO address>`, no account, but the request must name a contact (the repository URL is sent) | Tested 11 Nov 2026: 19 of 24 aircraft near Bradford, every airliner. Pictures 420 x 280, progressive JPEG; the photographer is credited on screen |
| Photo, second choice | `airport-data.com/api/ac_thumb.json?m=<ICAO address>&n=1`, no account | Tested 11 Nov 2026. Had the five light aircraft and helicopters Planespotters lacked. Small pictures (150 x 100) |
| Coastline | Natural Earth outlines from MeteoPlaneRadar's `EuMapData.h` (covers Britain and Ireland) | In hand. Islands under 15 km across are missing |
| Time | Internet time servers, kept by the clock chip | Standard in the ESP32 core |

The device depends on the aircraft feed staying available and free. The feed address will be a
setting, so it can be pointed elsewhere without rewriting the firmware. Requests carry our own
identifying User-Agent, not MeteoPlaneRadar's.

### Mobile data

The recipient's only internet is a phone hotspot, so downloads are kept few and small:

- One secure connection to the feed is kept open and reused; opening one costs about 6 KB.
- The scope fetches every 15, 20 or 30 seconds (25, 50, 100 km) and moves each aircraft along
  its track in between. The clock fetches every 5 minutes, for its count.
- During the night hours fetching rests five minutes after the last touch, until touched.

Left on the scope all day this is roughly 0.4 GB a month at 25 km, and 0.6 to 2.3 GB at 100 km
depending on traffic (estimates from measured download sizes). Compressed downloads would
cut the long-range figure further and are not done yet. The hotspot must be on 2.4GHz
("Maximise Compatibility" on an iPhone).

## Build order

Each step ends with a check on the real board.

| # | Step | Check |
|---|---|---|
| 1 | **Bring-up on our own build:** new sketch in `firmware/` with the borrowed screen, touch and drawing-surface files. | A test pattern fills the circle with no flicker; touch positions print over USB. |
| 2 | **Scope drawing:** rings, sweep and fade with made-up aircraft, in a continuous loop. | Frame rate printed over USB; the sweep is smooth to the eye (target 20 frames a second or better). |
| 3 | **Coastline:** borders drawn into the static buffer for a fixed test position. | The local coast is recognisable at each range. |
| 4 | **Wi-Fi and live aircraft:** background fetch task, aircraft and trails on the scope. Wi-Fi details hard-coded for now. | Aircraft match adsb.fi's own map for the same place; the sweep does not stutter during downloads. |
| 5 | **Tap for details and range swipe.** | Tapping near an aircraft opens the right one; it stays open while the aircraft moves. |
| 6 | **Clock screen:** digits, date, seconds ring, internet time, clock chip. | Time is right after a restart with Wi-Fi switched off; summer time rule checked with a test date. |
| 7 | **Switching and settings screen:** swipe up or down between scope and clock; long press for settings. | The screen picked and every setting survive a restart; 12 and 24 hour both display correctly. |
| 8 | **Phone setup:** setup network, QR code, form, postcode lookup, settings stored. | A factory-fresh board is set up from a phone alone, by someone who has not seen it before. |
| 9 | **Night dimming** and the optional buzzer ping. | Dims and recovers at the hours set (checked by moving the hours around the current time). |
| 10 | **Case, optional battery,** and a week of leaving it running. | No restarts or frozen screens in the week (restart reason logged). |

Steps 6 and 7 do not depend on steps 3 to 5, so the clock can be built early if the scope stalls.
There is no simulator stage: flashing the board takes seconds, so everything is developed on it.

## Risks

1. **Sweep frame rate.** Blending 230,000 pixels a frame from PSRAM is unmeasured on this board.
   If it is too slow, the fallback is to fade only the wedge behind the sweep. Settled in step 2.
2. **Screen glitches while saving settings.** Writing to flash pauses the memory the screen
   buffers live in. MeteoPlaneRadar blanks the screen during updates for this reason. Settings
   are saved rarely, so a brief blank on save is acceptable.
3. **The aircraft feed changes or closes.** Mitigated by the configurable address; adsb.lol
   offers a similar interface. The clock keeps the gift useful if the feed is lost.
4. **Setup by the recipient, unaided.** The phone flow has to work first time on their Wi-Fi.
   Step 8's check is done by someone else for that reason.
5. **Wrong Wi-Fi password at setup.** New details must only replace saved ones after a
   successful connection, or the device falls back to the setup network.

6. **Sparse map inland.** The borrowed outlines are coasts and country borders only, and the
   gift is going to Bradford, which has none in view until 100 km. The scope therefore also
   draws places from the same data set (GeoNames). Cities of 300,000 people or more are always
   marked (around Bradford: Leeds, Manchester, Sheffield, Hull, Liverpool, Preston). Towns of
   50,000 or more fill the gaps: one is added when it is at least 20 km from every city, or
   when at most one city is in view (the home city counts). Larger towns go first and none is
   placed within 60 px of another marker. Names are full up to 50 km, four letters at 100 km.
   From Filey that gives Scarborough at 25 and 50 km, and Leeds, Hull, York,
   Middlesbrough, Doncaster, Scunthorpe and Scarborough at 100 km.
   Airports are added by hand (the data set has none) as an amber ring with a runway bar:
   LBA, MAN, LPL, BLK, MME and HUY. More are one line each in the `AIRPORTS` table in
   `Scope.cpp`.
   Airport and city names are placed before aircraft callsigns, so callsigns keep off them. A
   name that does not fit beside its marker is tried on the other side, then above and below.

## Open questions

1. **Where will it live?** Bradford. The exact postcode is entered at setup; any Bradford
   postcode will do for testing.
2. **Battery or not?** The header takes a small 3.7V cell. Without one it simply runs from USB-C.
   A small coin cell on the clock chip's own header would keep time through unplugging.
3. **Case or stand:** bought, or 3D-printed from the Printables model.
4. **When is it needed by?** The Pi build's date is 10 December 2026.
5. **Clock style:** seven-segment digits in scope green is the proposal. Say if an analogue face
   or a different colour is preferred.
