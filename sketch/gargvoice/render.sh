#!/bin/bash
# Render dry/l1..l4.wav through presets/gargantua_<name>.yaml as the server would (one line at a time, then
# loudnorm I=-20:TP=-2:LRA=11 like mode.sh), with a tail of $TAIL s, joined → out/<name>.wav + media mp3.
# usage: render.sh <name>... ("dry" = untouched)
cd "$(dirname "$0")"; FA=../../build/fa_render; TAIL=${TAIL:-3.5}; mkdir -p out
for n in "$@"; do
  list=out/$n.list; : > $list
  for i in 1 2 3 4; do
    if [ "$n" = dry ]; then
      ffmpeg -v error -y -i dry/l$i.wav -af "apad=pad_dur=1.2" -ac 2 out/${n}_l$i.raw.wav
    else
      $FA ../../presets/gargantua_$n.yaml - dry/l$i.wav out/${n}_l$i.raw.wav --tail $TAIL >/dev/null || exit 1
    fi
    ffmpeg -v error -y -i out/${n}_l$i.raw.wav -af "loudnorm=I=-20:TP=-2:LRA=11,aresample=48000" -ac 2 out/${n}_l$i.wav
    echo "file '${n}_l$i.wav'" >> $list
  done
  ffmpeg -v error -y -f concat -safe 0 -i $list -c copy out/$n.wav
  ffmpeg -v error -y -i out/$n.wav -c:a libmp3lame -b:a 192k ~/.local/share/kepler/media/gargvoice-$n.mp3
  echo "$n: $(ffprobe -v error -show_entries format=duration -of csv=p=0 out/$n.wav) s"
done
