#!/bin/bash
# Dry lines in 観測症 (AivisSpeech 1275216064), speedScale 1.05 → dry/l<i>.wav
H=http://100.127.14.64:10101; S=1275216064
i=0
for t in "テストを続けましょう。ケーキはあります。" "計算は終わりました。結果は、残念ながら予想どおりです。" "おめでとうございます。あなたは生き残りました。今のところは。" "ここでは、時間の流れが違います。この星の一時間は、地球の七年に相当します。"; do
  i=$((i+1))
  curl -s -X POST "$H/audio_query?speaker=$S" --get --data-urlencode "text=$t" -o q.json -X POST
  python3 -c "import json;q=json.load(open('q.json'));q['speedScale']=1.05;json.dump(q,open('q.json','w'))"
  curl -s -X POST "$H/synthesis?speaker=$S" -H 'Content-Type: application/json' -d @q.json -o dry/l$i.wav
  soxi dry/l$i.wav 2>/dev/null | grep -E "Rate|Duration" || ffprobe -v error -show_entries stream=sample_rate,duration -of csv dry/l$i.wav
done
