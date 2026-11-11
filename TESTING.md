# Round Radar: first full test

Firmware v0.1.0 compiles but has never run on the board. Work through this in order; the early
steps prove the things later steps depend on. Note what fails and the log lines around it.

## Flash it

Plug the board into the Mac by the port marked **USB**, then either:

- run `firmware/flash.sh` (builds, flashes, then shows the log), or
- flash `firmware/build/RoundRadar-0.1.0.merged.bin` with the browser flasher used before
  (choose ESP32-S3).

The log should start with `=== Round Radar v0.1.0 ===` and end with `Setup done`.

Round Radar keeps its own settings, separate from MeteoPlaneRadar's, so the first start should
go to setup. Reflashing later keeps the saved Wi-Fi and postcode; use "Postcode" in settings to
change the place, or "Wi-Fi setup" to change the network.

## 1. Screen and touch

| Check | Expected |
|---|---|
| Start-up | "ROUND RADAR" splash, then the Wi-Fi list (a new board) or the last screen. No flicker, picture centred. |
| Log | `Board: 2.1` without "(fallback)". No "touch did not start" warning. |

## 2. Setup on the device

| Check | Expected |
|---|---|
| Wi-Fi list | Nearby networks, strongest first, with signal bars and a padlock on those needing a password. Five to a page; < and > appear when there are more. Everything clear of the rim. |
| Rescan | "Looking for Wi-Fi" for a few seconds, then the list again. |
| Tap a network | Password keypad with the network's name under the typed text. "Aa" switches capitals, "#?" switches to symbols and a space bar. |
| Wrong password, Join | "CONNECTING" for up to 20 seconds, then the keypad again with "Couldn't join" and the password kept. |
| Right password, Join | New board: the postcode keypad, then the scope. From settings: back to settings. Log: `WIFI: joined and saved`. |
| Back from the list (from settings) | Settings again, still on the saved network; aircraft keep updating. |

## 3. Setup from a phone

Reached with the "QR code" button on the Wi-Fi list.

| Check | Expected |
|---|---|
| Scan the QR code with a phone | Phone offers to join "Round Radar"; the form opens by itself, or at `192.168.4.1`. |
| Form | Nearby networks are suggested in the Wi-Fi field. |
| Wrong password | "COULDN'T JOIN", a 10 second countdown, then the QR screen again. |
| Wrong postcode (e.g. `ZZ99 9ZZ`) | "PLACE NOT FOUND", then the QR screen again. |
| Correct details | "CONNECTING", then the scope. Log: `SETUP: saved, position ...`. |

## 4. Scope

| Check | Expected |
|---|---|
| Sweep | Turns smoothly, one turn every 4 seconds. Log prints `SCOPE: N frames/s` every 10 seconds: **20 or more is the target.** |
| During downloads | No stutter when a download happens (every 15, 20 or 30 seconds at 25, 50 or 100 km). |
| Between downloads | Aircraft glide along their track; they do not sit still and then jump. A small correction at each download is normal. |
| Data use | The `NET:` log line counts fetches, connections and KB. Connections should stay far below fetches (one connection is reused). |
| Night rest | In the night hours, five minutes after the last touch: "Resting: touch to wake", aircraft go grey, `NET:` says "resting" and fetches stop rising. Any touch brings them back within seconds. |
| Aircraft | Count and positions match `globe.adsb.fi` for the same place. Arrowheads point along their track; trails grow behind them. |
| Map | Town squares and names in the right directions (see below). Coast appears only at 100 km. |
| Swipe left / right | Range steps wider / closer, stops at 10 and 100 km, and the picture redraws. |
| Tap an aircraft | Detail page for the right aircraft, with its type in words under the callsign; values change while open; tap closes it. |
| Swipe sideways on the details | Second page: "Getting photo" then a photo with the photographer's name and the site it came from under it, the route (e.g. `EDI > MLA`), airline, type code, registration and squawk. Light aircraft often show "Route not known" or "No photo found". Log: `ROUTE: ...` and `PHOTO: <address> from planespotters.net ...`. Swipe again goes back; tap closes. |
| Swipe once more from the photo | Third page: a QR code with the photographer and the site under it. A phone camera opens the photo's page on that site. In night red the code is black on red; check it still scans. With no photo: "No photo page for this aircraft". The code also shows for a stored aircraft with Wi-Fi off. |
| Open the same aircraft's photo page again | The photo and route appear at once, with no "Getting photo". Log: `PHOTO: <address> from memory`. |
| Restart, then open that aircraft again | The photo appears within a second or so without a download. Log at start: `STORE: memory card, ... MB free` (or `STORE: no memory card, using built-in storage`), then `PHOTO: <address> from storage`. With a card there should be no flicker or dark moment when a photo is saved; without one the screen goes dark for about a second as the photo page closes. |
| Over USB, type `list card` | The log lists what is on the card. `wipe card` removes everything on it (formatting it first if it cannot be read). `clear photos` forgets every stored photo and route, on the card, the built-in storage and in memory, and touches nothing else. |
| Readability | Callsigns and the top line readable at arm's length. |

## 5. Clock

| Check | Expected |
|---|---|
| Swipe up or down on the scope | Clock, with the right UK time, date and a filling seconds ring. |
| Log | `TIME: set from the internet` within a minute of joining Wi-Fi. |
| Foot line | Aircraft count, refreshed about every 5 minutes (kept rare to spare mobile data). |
| Unplug and replug | Opens on the clock again (the screen picked is remembered). |

## 6. Settings

| Check | Expected |
|---|---|
| Long press on scope or clock | Settings opens after about a second. |
| Brightness − / + | Screen dims and brightens in steps of 10%. |
| 12h / 24h | Clock changes form; 12 hour shows AM or PM and no leading zero. |
| Night dim row | Opens the night page. Set From to the current hour: after Back and Done the screen stays bright for 30 seconds after the last touch, then dims to the "Dim to" level. Set it back: it recovers. |
| Dim to − / + | Steps 1, 2, 5, 10, 15, 20, 30%. Check 1% is still readable in a dark room and 5% in a dim one. |
| Colour Red | Red for the whole of the night hours on every screen, including the 30 seconds after a touch; green outside them. The night page shows red at once as a preview. |
| Touch when dimmed | The first touch only brightens the screen (no aircraft detail, no range change). A second touch within the 30 seconds acts as normal. |
| Ping On | Switching Ping from Off to On sounds a test beep (confirmed working on 11 Nov 2026). After that, a beep when an aircraft first comes within 5 km. |
| Postcode | QWERTY keypad with a digit row, showing the saved postcode. All keys and Back / Del / OK clear the rim. A real postcode and OK: "Looking up", then back to settings, and the scope is centred on the new place. `ZZ99 9ZZ`: "Not found", saved place unchanged. Log: `PLACE: saved ...`. |
| Wi-Fi setup | The Wi-Fi list (section 2). "QR code" shows the setup QR screen with "Hold to go back"; a long press returns to the list. |
| Restart | Every setting survives unplugging. |
| Factory reset | Hold the side BOOT button: after a second "FACTORY RESET" with a countdown from 7. Let go: the screen before returns, nothing lost. Hold to the end: "ERASED", a restart, then the Wi-Fi list as on a new board. Log: `BOOT button down`, `FACTORY RESET: erasing saved settings`. |

## 7. Faults

| Check | Expected |
|---|---|
| Switch the router's Wi-Fi off | Scope: sweep stops, "No Wi-Fi" box. Clock: keeps time, foot line "No Wi-Fi". Both recover by themselves when Wi-Fi returns. |
| Power on with Wi-Fi off | Splash for 12 seconds, then the last screen. No setup screen. |
| Clock chip | With Wi-Fi off, unplug for a minute and replug. If the time is right, the chip kept it. Without a backup cell it will not, and the clock shows "SETTING TIME" until Wi-Fi returns. |
| Leave it running overnight | Log shows no restarts; free memory in the `SCOPE:` line is not falling. |

## Known before testing

- **Never run on hardware.** Expect first-run faults; the log over USB is the way to find them.
- **Map for Bradford.** No coast or border is in view until 100 km, so the scope shows places
  instead: small squares with names. Cities always show; towns only where no city is
  within 20 km, or when one city or none is in view. Expect Leeds alone at 25 km; Manchester,
  Leeds, York, Burnley and Harrogate among those at 50 km. Check they sit in the right
  directions and that Manchester's name shows at 50 km.
  Airports are drawn in amber, a ring with a bar along the runway: LBA to the north-east,
  and at 100 km also MAN, LPL, BLK, MME and HUY. For LBA: It is about 10.2 km away, so it shows from 25 km outward, not at 10 km.
  Departures and arrivals should start and end on it.
  Test with a Bradford postcode, e.g. the City Hall's, `BD1 1HY`.
- **Clock chip address assumed** to be the standard one for this part. The log says
  `RTC: clock chip not found` if not.
- **Wi-Fi password is stored unencrypted** on the board, as is normal for this class of device.
- The form accepts UK postcodes only.
