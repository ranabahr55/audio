# audio_zenoh_cpp — Real-time Opus audio over Zenoh

A low-latency microphone streaming pair:

```
  terminal 1                                   terminal 2
  ┌─────────────────────────────┐  Zenoh   ┌──────────────────────────────┐
  │ audio_pub  (C++)            │  key:    │ audio_sub  (Python)          │
  │ mic → PortAudio → Opus enc  │ ───────▶ │ Opus dec → FLAC file         │
  │ → Zenoh put (express, RT)   │ audio/.. │ (+ packet-loss concealment)  │
  └─────────────────────────────┘          └──────────────────────────────┘
```

* **Publisher** (`audio_pub/`) is C++: PortAudio capture on a real-time
  callback, Opus encoding on a worker thread, Zenoh publishing with
  `DROP` congestion control + `REAL_TIME` priority + express (no batching) so
  latency stays minimal.
* **Subscriber** (`audio_sub/`) is Python: decodes Opus by calling the system
  `libopus` through `ctypes` (no extra pip installs), conceals lost packets
  using the per-packet sequence number, and streams the result to **FLAC**
  via `soundfile`.

Both ends read the **same** `audio_pub/config.cfg`, so the Zenoh key, sample
rate, channels and frame size can never drift out of sync.

---

## Build the publisher (you run these — they are not run for you)

```bash
cd audio_pub
mkdir build && cd build
cmake ..
make
```

CMake auto-discovers Zenoh (`/usr/local`), PortAudio (pkg-config) and Opus
(searched under `$CONDA_PREFIX` / Anaconda / system paths). If Opus is
elsewhere: `cmake .. -DOPUS_ROOT=/your/prefix`.

## Run — two terminals, simultaneously

**Terminal 1 — publisher** (captures the mic, streams in real time):

```bash
cd audio_pub/build
./audio_pub               # reads ../config.cfg automatically
# or: ./audio_pub /path/to/config.cfg
```

**Terminal 2 — subscriber** (receives, decodes, saves):

```bash
cd audio_sub
python3 audio_sub.py      # reads ../audio_pub/config.cfg automatically
# or: python3 audio_sub.py /path/to/config.cfg
```

Press **Ctrl-C** in terminal 2 first to finalize the FLAC file
(`audio_sub/capture.flac` by default), then Ctrl-C the publisher.

> Order tip: start the subscriber first so it is ready when the first packets
> arrive — though either order works, peers discover each other automatically.

---

## Configuration (`audio_pub/config.cfg`)

Everything tunable lives there with inline documentation: Zenoh `mode` /
`connect` / `listen` / `key`, capture `sample_rate` / `channels` / `frame_ms`,
and the Opus `bitrate` / `complexity` / `vbr` / `fec` / `dtx` / `signal` /
`low_delay`, plus the subscriber's `output` path.

### Same machine
Defaults work out of the box: `mode = peer`, empty `connect` → peers find each
other by multicast scouting on localhost.

### Two machines
On the publisher host set, e.g. `listen = tcp/0.0.0.0:7447`; on the subscriber
host set `connect = tcp/<publisher-ip>:7447`. (Or point both `connect` at a
running `zenohd` router and use `mode = client`.)

---

## Subscriber dependencies

Python 3 with `zenoh`, `numpy`, `soundfile` (all already present in this
environment). Opus decoding uses the system `libopus.so` via `ctypes` — nothing
to install. FLAC encoding uses `libsndfile` through `soundfile`.

## Wire format and Trouble Shooting

**1. Digital Microphone usually requires 5 primary connections to the 40 pin header:**

VDD (VOltage): Pin 1 (3.3V)

GND (Ground): Pin 6

SCK (Serial Clock): Pin 12

WS (Word Select): Pin 35

SD/DIN (Data Input): Pin 38

**2. Configure the 40 Pin Header**

1. Open the Jetson Terminal and execute:

```bash
sudo /opt/nvidia/jetson-io/jetson-io.py
```

2. Select **Configure 40-pin expansion header**
3. Enable **i2s2**
4. Save and select **Reboot** to apply changes

**3. ALSA Audio Mixer Routing**
The Jetson audio subsystem requires explicit routing via the digital crossbar mixer (XBAR) to bridge the hardware `I2S2` port to an active audio DMA interface (`ADMAIF1`).

Run the following commands to initialize the paths and set the sample rate:
```bash
amixer -c APE cset name='I2S2 Mux' 'ADMAIF1'
amixer -c APE cset name='ADMAIF1 Mux' 'I2S2'
amixer -c APE cset name='I2S2 Sample Rate' 48000
```
Identify your hardware capture cards using:
```bash
arecord -l
```
On the Jetson Orin Nano, the APE subsystem maps to **card 1**, and `XBAR-ADMAIF1-0` handles **device 0**. 