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
change the place, or "Wi-Fi setup" to redo both from a phone.

## 1. Screen and touch

| Check | Expected |
|---|---|
| Start-up | "ROUND RADAR" splash, then the setup QR screen. No flicker, picture centred. |
| Log | `Board: 2.1` without "(fallback)". No "touch did not start" warning. |

## 2. Phone setup

| Check | Expected |
|---|---|
| Scan the QR code with a phone | Phone offers to join "Round Radar"; the form opens by itself, or at `192.168.4.1`. |
| Form | Nearby networks are suggested in the Wi-Fi field. |
| Wrong password | "COULDN'T JOIN", a 10 second countdown, then the QR screen again. |
| Wrong postcode (e.g. `ZZ99 9ZZ`) | "PLACE NOT FOUND", then the QR screen again. |
| Correct details | "CONNECTING", then the scope. Log: `SETUP: saved, position ...`. |

## 3. Scope

| Check | Expected |
|---|---|
| Sweep | Turns smoothly, one turn every 4 seconds. Log prints `SCOPE: N frames/s` every 10 seconds: **20 or more is the target.** |
| During downloads | No stutter every 5 seconds. |
| Aircraft | Count and positions match `globe.adsb.fi` for the same place. Arrowheads point along their track; trails grow behind them. |
| Map | Town squares and names in the right directions (see below). Coast appears only at 100 km. |
| Swipe left / right | Range steps wider / closer, stops at 10 and 100 km, and the picture redraws. |
| Tap an aircraft | Detail page for the right aircraft; values change while open; tap closes it. |
| Readability | Callsigns and the top line readable at arm's length. |

## 4. Clock

| Check | Expected |
|---|---|
| Swipe up or down on the scope | Clock, with the right UK time, date and a filling seconds ring. |
| Log | `TIME: set from the internet` within a minute of joining Wi-Fi. |
| Foot line | Aircraft count, refreshed about every 30 seconds. |
| Unplug and replug | Opens on the clock again (the screen picked is remembered). |

## 5. Settings

| Check | Expected |
|---|---|
| Long press on scope or clock | Settings opens after about a second. |
| Brightness − / + | Screen dims and brightens in steps of 10%. |
| 12h / 24h | Clock changes form; 12 hour shows AM or PM and no leading zero. |
| Night dim row | Opens the night page. Set From to the current hour: after Back and Done the screen dims to 10% within a second. Set it back: it recovers. |
| Ping On | **Unverified hardware.** A short buzz when an aircraft first comes within 5 km. If the screen blanks instead of buzzing, turn Ping off and tell me: the buzzer pin is an assumption. |
| Postcode | QWERTY keypad with a digit row, showing the saved postcode. All keys and Back / Del / OK clear the rim. A real postcode and OK: "Looking up", then back to settings, and the scope is centred on the new place. `ZZ99 9ZZ`: "Not found", saved place unchanged. Log: `PLACE: saved ...`. |
| Wi-Fi setup | Setup QR screen with "Hold to cancel"; a long press returns to the saved network. |
| Restart | Every setting survives unplugging. |

## 6. Faults

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
- **Buzzer pin unverified** (see Ping above). Ping is off by default.
- **Clock chip address assumed** to be the standard one for this part. The log says
  `RTC: clock chip not found` if not.
- **Wi-Fi password is stored unencrypted** on the board, as is normal for this class of device.
- The form accepts UK postcodes only.
