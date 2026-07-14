# GenICam

Native GigE Vision/GenICam control and GVSP video capture for the MindVision
MV-GEC501M-T camera with a Sony IMX269 sensor. The default build does **not** use
MindVision's SDK or `libmvsdk`.

## Verified camera

- Address: `192.168.9.104` (persistent), host: `192.168.9.100/24`
- Model: `GEC501M`, serial `058130110308`, firmware `V1.0.90-2.0.11`
- Video: `2640 x 1978`, Mono8 or Mono12 Packed
- Sensor mode: IMX269 2 x 2, effective 6.6 um pixels
- Exposure: 11 us to 5.7672 s in the factory firmware
- Transport: GigE Vision GVCP/GVSP, 1 Gbit/s, packet size 1400 bytes

The camera does not answer Aravis broadcast discovery reliably. Connect to its
address directly. Its device-provided GenICam description is saved as
[`config/GEC501M-genicam.xml`](config/GEC501M-genicam.xml).

## Dependencies

```sh
brew install aravis sdl2
```

## Live focus view

Build the live viewer once:

```sh
cmake -S . -B build-standard
cmake --build build-standard --target genicam-live-app -j4
```

Then open the macOS app bundle:

```sh
open "build-standard/GenICam Live.app"
```

The app opens a camera picker; choose the GenICam device from the dropdown and
click `Connect`. For debugging, optional command-line arguments can still set
the camera address and initial exposure in microseconds. Optional third and
fourth arguments set a centered hardware ROI:

```sh
cmake --build build-standard --target genicam-live -j4
./build-standard/genicam-live 192.168.9.104 10000 640 480
```

The live window uses Dear ImGui controls for exposure, gain and digital zoom,
plus a center crosshair, 2x focus ROI, histogram and focus score. Drag a box on
the image to select a hardware ROI; use `Reset ROI` to return to the full
sensor. Press `q` or `Esc` to stop.

## Build manually

```sh
cmake -S . -B build-standard
cmake --build build-standard --target genicam-live-app genicam-live -j4
```

## Astronomy RAW12 capture

`genicam-capture` uses the camera's GenICam nodes to select Mono12 Packed,
disable auto exposure, configure exposure/gain, arm software trigger, reject
incomplete GVSP frames, unpack 12-bit pixels and write unsigned 16-bit FITS.

```sh
cmake --build build-standard --target genicam-capture -j4
./build-standard/genicam-capture 0.002 captures/test.fits 1.0
```

Arguments are exposure seconds, output FITS path, optional gain and optional
camera IP. FITS metadata includes `EXPTIME`, `GAIN`, `CCD-TEMP`, `SENSOR`,
`INSTRUME` and `FRAMEID`.

The factory firmware limits a single exposure to about 5.7672 seconds. Longer
astronomical integrations will be implemented as calibrated RAW12 sequences;
changing that single-frame limit requires separate firmware work.

## Tests

```sh
cmake --build build-standard -j4
ctest --test-dir build-standard --output-on-failure
```

## Protocol evidence

Direct GenICam inspection without a vendor SDK:

```sh
arv-tool-0.8 -a 192.168.9.104 values
arv-tool-0.8 -a 192.168.9.104 genicam
```

The XML maps `AcquisitionStart/Stop` to `0x10000014`, `ExposureTime` to
`0x10000160`, `PixelFormat` to `0x10000008`, and `GevSCPSPacketSize` to
`0x0D04`.

No source file or build target links against MindVision's SDK.
