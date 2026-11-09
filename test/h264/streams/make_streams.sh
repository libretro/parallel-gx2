#!/bin/sh
# Makes the test's streams with ffmpeg (libx264) and, beside each, what
# ffmpeg decodes it to: its access units' sizes in decode order, then each
# picture in display order as the access unit (by index) that coded it and
# the MD5 of its I420 planes, cropped. Kept in the tree, so running the test
# needs no ffmpeg; rerun only to change the streams.
set -e
cd "$(dirname "$0")"

# High profile, two B pictures between references, 200x120 in a 208x128
# coded picture (a crop rectangle right and bottom)
ffmpeg -v error -y -f lavfi -i testsrc2=size=200x120:rate=30 -frames:v 24 -c:v libx264 -profile:v high -pix_fmt yuv420p \
	-x264-params "bframes=2:b-pyramid=none:keyint=12:min-keyint=12:scenecut=0" bframes.h264
# Baseline, no reordering: what a game decoding one picture per input sends
ffmpeg -v error -y -f lavfi -i testsrc2=size=96x72:rate=30 -frames:v 12 -c:v libx264 -profile:v baseline -pix_fmt yuv420p \
	-x264-params "keyint=6:min-keyint=6:scenecut=0" baseline.h264
# 1920x1080 coded 1920x1088, as Breath of the Wild's movies are
ffmpeg -v error -y -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 2 -c:v libx264 -profile:v main -bf 0 -crf 51 -pix_fmt yuv420p fullhd.h264

for s in bframes baseline fullhd; do
	{
		ffprobe -v error -show_entries packet=pos,size -of csv=p=0:nk=0 "$s.h264" | sed 's/^size=\([0-9]*\),pos=\([0-9]*\)$/packet \1 \2/'
		ffprobe -v error -show_entries frame=pkt_pos -of csv=p=0:nk=0 "$s.h264" | sed 's/^pkt_pos=\([0-9]*\).*$/frame \1/' > frames.tmp
		ffmpeg -v error -i "$s.h264" -f framemd5 - | grep -v '^#' | awk -F, '{gsub(/ /, "", $6); print $6}' > md5.tmp
		paste -d' ' frames.tmp md5.tmp
	} > "$s.txt"
	rm -f frames.tmp md5.tmp
done
