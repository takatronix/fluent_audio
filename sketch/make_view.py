#!/usr/bin/env python3
"""Build the audition view (/api/view JSON, type html) with the rendered mp3s inlined as data: URIs.
  python3 sketch/make_view.py <media_dir>     -> <media_dir>/faudio.view.json"""
import base64, json, sys
from pathlib import Path

d = Path(sys.argv[1])
ITEMS = [
    ("dry", "そのまま", "今の Kepler の声（効果なし）。比べる基準"),
    ("kepler", "kepler", "低域の濁りを切り、ゆるく整え、歯擦音を抑え、高域に少し空気。部屋の響きはほんのわずか"),
    ("fluent", "fluent", "やわらかいコーラスで少し広げ、1 オクターブ上のきらめく残響（シマー）を薄く"),
    ("kitt", "kitt", "車内の無線のように帯域を絞り、中域を少し前へ、軽い歪みとかすかな電子の粒"),
    ("gargantua", "gargantua", "低域に厚み、遠くまで伸びる暗い宇宙の残響と 1 オクターブ下の響きをごく薄く"),
]
cards = []
for key, name, desc in ITEMS:
    b64 = base64.b64encode((d / f"faudio-{key}.mp3").read_bytes()).decode()
    cards.append(f"""<div class="card"><div class="h"><b>{name}</b><span>{desc}</span></div>
<audio controls preload="auto" src="data:audio/mpeg;base64,{b64}"></audio></div>""")
html = f"""<!doctype html><meta charset="utf-8"><style>
body{{margin:0;padding:24px;font:15px/1.6 system-ui,sans-serif;background:#05070d;color:#dfe8ff}}
h1{{font-size:20px;margin:0 0 4px;letter-spacing:.08em}} p.s{{color:#8fa3c8;margin:0 0 18px}}
.card{{background:linear-gradient(135deg,#0d1424,#0a0f1c);border:1px solid #1f2d4d;border-radius:14px;
padding:14px 16px;margin:0 0 12px}} .h b{{display:inline-block;min-width:110px;color:#7fd4ff;font-size:17px}}
.h span{{color:#aab8d8}} audio{{width:100%;margin-top:8px}}
</style><h1>FLUENT AUDIO — 試聴（試作）</h1>
<p class="s">Kepler の同じ声に、見た目ごとの効果をかけた案。音量はそろえてあります。遅れは足さない設計（ここではファイルで試作）。</p>
{''.join(cards)}
<p class="s">Python で作った試作の音です。本番は C++ のコア（ブラウザでは AudioWorklet）で同じ処理をします。</p>"""
view = {"type": "html", "title": "fluent audio 試聴", "html": html}
out = d / "faudio.view.json"
out.write_text(json.dumps(view, ensure_ascii=False))
print(out, out.stat().st_size, "bytes")
