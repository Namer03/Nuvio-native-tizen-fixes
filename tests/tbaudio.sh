#!/bin/bash
# Original audio with TorBox burn-in (src/dts/tbaudio_tpk.c) for real on the
# host: system FFmpeg (libav* dev + the ffmpeg/ffprobe CLI with libx264,
# libopus and the experimental DTS encoder), a fake "TorBox" HLS made from the
# original by ffmpeg (MPEG-TS and fMP4, last #EXTINF overreported like TorBox),
# served by tests/tbaudio_origem.py.
set -eu
cd "$(dirname "$0")/.."
if ! pkg-config --exists libavformat libavcodec libswresample libavutil || ! command -v ffmpeg >/dev/null || ! command -v ffprobe >/dev/null; then
  echo "tbaudio: skipped (needs the FFmpeg dev libraries and the ffmpeg/ffprobe CLI)"
  exit 0
fi
work=$(mktemp -d /tmp/nuvio-tbaudio.XXXXXX)
pids=()
trap 'for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; rm -rf "$work"' EXIT
mkdir -p "$work/src/tb" "$work/src/tb4" "$work/out"
ffmpeg -v error -f lavfi -i testsrc2=s=320x180:r=24 -f lavfi -i "sine=f=440:r=48000" -f lavfi -i "sine=f=660:r=48000" -t 32 \
  -filter_complex "[1:a]asplit=4[a0][a1][a2][a3];[a1]pan=5.1|FL=c0|FR=c0|FC=c0|LFE=c0|BL=c0|BR=c0[a51];[a3]pan=5.1|FL=c0|FR=c0|FC=c0|LFE=c0|BL=c0|BR=c0[d51];[2:a]aformat=channel_layouts=stereo[f]" \
  -map 0:v -map "[a0]" -map "[a51]" -map "[f]" -map "[a2]" -map "[d51]" \
  -c:v libx264 -preset ultrafast -g 48 -keyint_min 48 -sc_threshold 0 -pix_fmt yuv420p \
  -c:a:0 aac -ac:a:0 2 -c:a:1 ac3 -c:a:2 flac -c:a:3 libopus -ac:a:3 2 -c:a:4 dca -strict -2 -b:a:4 768k \
  -metadata:s:a:0 language=eng -metadata:s:a:1 language=jpn -metadata:s:a:2 language=eng -metadata:s:a:3 language=fre \
  "$work/src/orig.mkv"
for t in ts fmp4; do
  d=tb; [ $t = fmp4 ] && d=tb4
  ffmpeg -v error -i "$work/src/orig.mkv" -map 0:v -map 0:a:0 -c:v libx264 -preset ultrafast \
    -force_key_frames "expr:gte(t,n_forced*5)" -c:a aac -ac 2 -f hls -hls_time 5 -hls_playlist_type vod \
    -hls_segment_type $([ $t = ts ] && echo mpegts || echo fmp4) -hls_fmp4_init_filename init.mp4 -hls_segment_filename "$work/src/$d/s%d.$([ $t = ts ] && echo ts || echo m4s)" \
    "$work/src/$d/real.m3u8"
  # TorBox lists every segment as 5 s, the last one too.
  sed 's/^#EXTINF:[0-9.]*,/#EXTINF:5.000000,/' "$work/src/$d/real.m3u8" > "$work/src/$d/playlist.m3u8"
done
printf '#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=320x180\nplaylist.m3u8\n' > "$work/src/tb4/master.m3u8"
python3 tests/tbaudio_origem.py "$work/src" "$work/port" "$work/origem.log" & pids+=($!)
for _ in $(seq 50); do [ -s "$work/port" ] && break; sleep 0.1; done
port=$(cat "$work/port")
${CC:-gcc} -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-misleading-indentation -Wno-format-truncation \
  -DNV_TBAUDIO_TEST -DNV_DTS_FFMPEG -Isrc tests/tbaudio.c src/dts/tbaudio_tpk.c src/dts/dts_engine.c src/rede.c src/redeurl.c \
  $(pkg-config --cflags --libs libavformat libavcodec libswresample libavutil) -lz -lm -lpthread -ldl -o "$work/t"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
inicio() { ffprobe -v error -select_streams "$2" -show_entries stream=start_time -of csv=p=0 "$1" | head -1; }
perto() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN{d=a-b; if (d<0) d=-d; exit !(d<=t)}'; }
codec() { ffprobe -v error -select_streams a -show_entries stream=codec_name,channels -of csv=p=0 "$1" | head -1; }

"$work/t" puros "$work/src/tb/s0.ts" "$work/src/tb4/init.mp4" "$work/src/tb4/s0.m4s" "$work/out/strip.ts" || fail=1
check "TS strip: ffprobe sees the video only" "[ \"\$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 '$work/out/strip.ts' | grep -v '^\$' | sort -u | tr '\n' ' ')\" = 'video ' ]"

sessao() {   # <playlist path> <modo> <ordinal> <flac>
  rm -f "$work/url"
  "$work/t" sessao "http://127.0.0.1:$port/$1?token=secret" "http://127.0.0.1:$port/orig.mkv" "$2" "$3" "$4" "$work/url" > "$work/s.log" 2>&1 &
  sp=$!; pids+=($sp)
  for _ in $(seq 50); do [ -s "$work/url" ] && break; sleep 0.1; done
  U=$(cat "$work/url"); B=${U%/*}
}
fim() { kill "$sp" 2>/dev/null || true; wait "$sp" 2>/dev/null || true; }

# Modes: 1 Remux, 2 Parallel (codecs hint), 3 Parallel (strip TorBox audio).
# 1. Parallel over MPEG-TS: every codec, on TorBox's timeline, 2 s segments.
sessao tb/playlist.m3u8 2 0 0
curl -s "$U" > "$work/out/master.m3u8"
check "master: one rendition per audio track (5)" "[ \$(grep -c '^#EXT-X-MEDIA:TYPE=AUDIO' '$work/out/master.m3u8') = 5 ]"
check "master: the chosen track is DEFAULT" "grep -q 'DEFAULT=YES.*URI=\"a0.m3u8\"' '$work/out/master.m3u8'"
curl -s "$B/v.m3u8" > "$work/out/v.m3u8"
check "video playlist: TorBox's segments, absolute URLs" "grep -q '^http://127.0.0.1:$port/tb/s0.ts' '$work/out/v.m3u8'"
check "S3: the last #EXTINF matches the file (2.0 s, TorBox said 5)" "tail -3 '$work/out/v.m3u8' | grep -qE '^#EXTINF:2\\.0[0-9]*,'"
check "no URL of the source in the log" "! grep -q 'token=secret\\|orig.mkv' '$work/s.log'"
check "codecs hint: CODECS names the video and the renditions' audio" "grep -qE 'CODECS=\"avc1\\.[0-9A-F]{6},mp4a\\.40\\.2,ac-3,fLaC,Opus\"' '$work/out/master.m3u8'"
curl -s "$B/a0.m3u8" > "$work/out/a0.m3u8"
check "audio playlist: 2 s segments (16 for 32 s)" "grep -q '^#EXT-X-TARGETDURATION:2' '$work/out/a0.m3u8' && [ \$(grep -c '^a0_' '$work/out/a0.m3u8') = 16 ]"
check "seek index read at preparation" "grep -q 'seek index read' '$work/s.log'"
for k in 0 1 3 4; do curl -s -o "$work/out/a${k}_2.ts" "$B/a${k}_5.ts"; done
curl -s -o "$work/out/a2i.mp4" "$B/a2_init.mp4"; curl -s -o "$work/out/a2_2.m4s" "$B/a2_5.m4s"
cat "$work/out/a2i.mp4" "$work/out/a2_2.m4s" > "$work/out/flac.mp4"
v2=$(inicio "$work/src/tb/s2.ts" v)
check "AAC copied, starts with TorBox's video segment ($(inicio "$work/out/a0_2.ts" a) vs $v2)" "[ '$(codec "$work/out/a0_2.ts")' = 'aac,2' ] && perto $(inicio "$work/out/a0_2.ts" a) $v2 0.05"
check "AC3 5.1 copied ($(codec "$work/out/a1_2.ts"))" "[ '$(codec "$work/out/a1_2.ts")' = 'ac3,6' ] && perto $(inicio "$work/out/a1_2.ts" a) $v2 0.05"
check "Opus copied ($(codec "$work/out/a3_2.ts"))" "[ '$(codec "$work/out/a3_2.ts")' = 'opus,2' ] && perto $(inicio "$work/out/a3_2.ts" a) $v2 0.05"
check "DTS converted to AC3 5.1 ($(codec "$work/out/a4_2.ts"))" "[ '$(codec "$work/out/a4_2.ts")' = 'ac3,6' ] && perto $(inicio "$work/out/a4_2.ts" a) $v2 0.05"
check "FLAC as is in fMP4 (init + segment: $(codec "$work/out/flac.mp4"))" "[ '$(codec "$work/out/flac.mp4")' = 'flac,2' ] && perto $(inicio "$work/out/flac.mp4" a) $v2 0.1"
timeout 120 ffmpeg -v error -i "$U" -map 0:v:0 -map 0:a:0 -t 12 -c copy "$work/out/play.ts" || true
check "a player reads the master: video + audio, starts together" \
  "[ -s '$work/out/play.ts' ] && perto $(inicio "$work/out/play.ts" v 2>/dev/null || echo 9) $(inicio "$work/out/play.ts" a 2>/dev/null || echo 0) 0.05"
check "the session got ready" "grep -q 'ready in' '$work/s.log'"
# A request the player gives up on is dropped, and the next one is not kept waiting.
curl -s -m 0.3 -o /dev/null "$B/a4_14.ts" || true
t0=$(date +%s%N); curl -s -o "$work/out/after.ts" "$B/a4_3.ts"; t1=$(date +%s%N)
check "after a hang-up the next segment comes ($(( (t1 - t0) / 1000000 )) ms)" "[ -s '$work/out/after.ts' ] && [ \$(( (t1 - t0) / 1000000 )) -lt 5000 ]"
fim

# 1b. Parallel, strip TorBox's audio: the video through Nuvio without audio.
sessao tb/playlist.m3u8 3 0 0
curl -s "$U" > "$work/out/ms.m3u8"
check "strip: the master points at Nuvio's video playlist" "grep -q '^vs.m3u8' '$work/out/ms.m3u8' && ! grep -q CODECS '$work/out/ms.m3u8'"
curl -s "$B/vs.m3u8" | grep -q '^v_0.ts' && curl -s -o "$work/out/v2.ts" "$B/v_2.ts"
check "strip: video segment without audio, same start as TorBox's" \
  "[ \"\$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 '$work/out/v2.ts' | grep -v '^\$' | sort -u | tr '\n' ' ')\" = 'video ' ] && perto $(inicio "$work/out/v2.ts" v 2>/dev/null || echo 0) $(inicio "$work/src/tb/s2.ts" v) 0.001"
timeout 120 ffmpeg -v error -i "$U" -map 0:v:0 -map 0:a:0 -t 12 -c copy "$work/out/plays.ts" || true
check "strip: a player reads it, video + Nuvio's audio together" \
  "[ -s '$work/out/plays.ts' ] && perto $(inicio "$work/out/plays.ts" v 2>/dev/null || echo 9) $(inicio "$work/out/plays.ts" a 2>/dev/null || echo 0) 0.05"
fim

# 2. FLAC converted (test row), Parallel.
sessao tb/playlist.m3u8 2 2 1
curl -s -o "$work/out/f.ts" "$B/a2_3.ts"
check "FLAC converted: AAC 2.0 in MPEG-TS ($(codec "$work/out/f.ts"))" "[ '$(codec "$work/out/f.ts")' = 'aac,2' ]"
fim

# 3. Remux: TorBox's video + the original audio in one MPEG-TS.
sessao tb/playlist.m3u8 1 4 0
for i in 0 1 6; do curl -s -o "$work/out/r$i.ts" "$B/r_$i.ts"; done
check "remux: TorBox's segment downloads alongside the audio" "grep -q '(alongside)' '$work/s.log'"
check "remux: video + AC3 5.1 from DTS" "ffprobe -v error -show_entries stream=codec_name -of csv=p=0 '$work/out/r1.ts' | tr '\n' ' ' | grep -q 'h264 ac3'"
check "remux: audio starts with the video ($(inicio "$work/out/r1.ts" a) vs $(inicio "$work/out/r1.ts" v))" \
  "perto $(inicio "$work/out/r1.ts" a) $(inicio "$work/out/r1.ts" v) 0.05"
check "remux: the last segment too" "[ -s '$work/out/r6.ts' ] && ffprobe -v error -show_entries stream=codec_name -of csv=p=0 '$work/out/r6.ts' | grep -q ac3"
fim

# 4. fMP4 TorBox behind a master playlist, both modes.
sessao tb4/master.m3u8 2 1 0
curl -s -o "$work/out/m1.ts" "$B/a1_2.ts"
check "fMP4 source: first PTS from tfdt, audio placed" "grep -q \"TorBox's first video PTS: 0.000 s\" '$work/s.log' && [ '$(codec "$work/out/m1.ts")' = 'ac3,6' ]"
fim
sessao tb4/master.m3u8 1 0 0
curl -s -o "$work/out/m4r.ts" "$B/r_2.ts"
check "fMP4 source, Remux: video + AAC in MPEG-TS" "ffprobe -v error -show_entries stream=codec_name -of csv=p=0 '$work/out/m4r.ts' | tr '\n' ' ' | grep -q 'h264 aac'"
check "fMP4 source, Remux: nothing passed through" "! grep -q 'passing TorBox' '$work/s.log'"
fim

[ $fail = 0 ] && echo "tbaudio: ok" || { echo "tbaudio: FAIL"; exit 1; }
