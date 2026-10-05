# mp3rom

A launcher application that runs the S1C33E07 boot ROM's MP3 decoder over
test files and saves what it produces.  What the runs showed is in
[../romdump/BOOTROM.md](../romdump/BOOTROM.md).

It calls the ROM's frame decoder directly, never `open` or `play`, so P04-P07
stay as they are, and nothing is played.  Each run writes under the first
number no file in `mp3test/out/` uses yet, creating every file new.

## Test files

Pink noise fills the band, so where the output stops says what the decoder
drops; one file a sample rate, plus a mono one and an MPEG-2.5 one:

```sh
for rate in 48000 32000 22050 11025; do
  ffmpeg -f lavfi -i anoisesrc=d=2:c=pink:r=$rate:a=0.3 \
         -f lavfi -i anoisesrc=d=2:c=pink:r=$rate:a=0.3:s=7 \
         -filter_complex '[0][1]amerge=inputs=2[a]' -map '[a]' -ac 2 src_$rate.wav
done
lame -m s -b 128 src_48000.wav n48s.mp3
lame -m s -b 128 src_32000.wav n32s.mp3
lame -m s -b 64 src_22050.wav n22s.mp3   # MPEG-2
lame -m s -b 32 src_11025.wav n11s.mp3   # MPEG-2.5
ffmpeg -f lavfi -i anoisesrc=d=2:c=pink:r=44100:a=0.3 -ac 1 src_mono.wav
lame -m m -b 128 src_mono.wav n44m.mp3
```

The tones of the first run, which measure precision rather than bandwidth:

```sh
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 \
       -f lavfi -i sine=frequency=1000:sample_rate=44100:duration=2 \
       -filter_complex '[0][1]amerge=inputs=2,volume=0.5[a]' -map '[a]' -ac 2 src44.wav
ffmpeg -f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 \
       -f lavfi -i anoisesrc=d=2:c=pink:r=44100:a=0.2 \
       -filter_complex '[0][1]amerge=inputs=2,pan=stereo|c0=0.6*c0+0.4*c1|c1=0.4*c0+0.6*c1[a]' \
       -map '[a]' srcj.wav
ffmpeg -i src44.wav -ar 22050 src22.wav
lame -m s -b 128 src44.wav t44s.mp3
lame -m j -b 128 srcj.wav t44j.mp3
lame -m s -b 64 src22.wav t22s.mp3
```

`mpg123 -s --no-gapless` gives the reference to compare against.

## Install

```sh
make -C mp3rom
cp -X mp3rom/mp3rom.app mp3rom/mp3rom.ico /Volumes/WRBOOT/
mkdir -p /Volumes/WRBOOT/mp3test
cp -X *.mp3 /Volumes/WRBOOT/mp3test/
printf 'mp3rom.ico : mp3rom.app\n' >> /Volumes/WRBOOT/init.ini
```

Tap the speaker icon.  While the decoder holds IVRAM, where the LCD's frame
lives, the screen shows whatever the decoder leaves there.  The report
follows, then "press any button" returns to the launcher.

## What it writes

| File | Contents |
| --- | --- |
| `mp3test/out/rNNN.txt` | per pass: frames, status, reads, samples a frame, time a frame, workspace and IVRAM use, PCM CRC-32 |
| `mp3test/out/rNNNxM.pcm` | the 0x1200-byte output half after each frame; x is the file (the letter the pass list gives it), M the mode, g the 0xa5-fill pass |
| `mp3test/out/rNNNiv.bin` | IVRAM after the 0xa5-fill pass |

With `mp3test/dry` present it does everything but call the ROM, for emulators
whose boot ROM lacks the unreadable part.
