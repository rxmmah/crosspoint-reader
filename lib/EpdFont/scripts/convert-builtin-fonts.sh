#!/bin/bash

set -e

cd "$(dirname "$0")"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-$SCRIPT_DIR/../../../.venv/bin/python}"
if [ ! -x "$PYTHON_BIN" ]; then
  PYTHON_BIN="${PYTHON_BIN:-python3}"
fi

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
NOTOSANS_ARGS=(--2bit --compress --arabic)
ARABIC_FALLBACK_FONT="../builtinFonts/source/NotoNaskhArabic/NotoNaskhArabic-Regular.ttf"

for size in ${BOOKERLY_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="bookerly_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bookerly/Bookerly-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    "$PYTHON_BIN" fontconvert.py $font_name $size $font_path --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    "$PYTHON_BIN" fontconvert.py $font_name $size $font_path "$ARABIC_FALLBACK_FONT" "${NOTOSANS_ARGS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")
UI_FONT_ARGS=(--arabic)

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    "$PYTHON_BIN" fontconvert.py $font_name $size $font_path "$ARABIC_FALLBACK_FONT" "${UI_FONT_ARGS[@]}" > $output_path
    echo "Generated $output_path"
  done
done

"$PYTHON_BIN" fontconvert.py notosans_8_regular 8 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  "$ARABIC_FALLBACK_FONT" --arabic > ../builtinFonts/notosans_8_regular.h

# IPA font — Doulos SIL Regular, 16pt, IPA codepoints only
IPA_SOURCE="../builtinFonts/source/DoulosSIL/DoulosSIL-Regular.ttf"
IPA_STRIPPED="/tmp/doulos_sil_ipa_stripped.ttf"
IPA_UNICODES="U+0250-02AF,U+02B0-02FF,U+1D00-1D7F,U+1D80-1DBF"

pyftsubset "$IPA_SOURCE" \
  --unicodes="$IPA_UNICODES" \
  --output-file="$IPA_STRIPPED" \
  --no-layout-closure \
  --drop-tables+=GSUB,GPOS,GDEF,Silt

python fontconvert.py ipa_16_regular 16 "$IPA_STRIPPED" \
  --2bit --compress \
  --no-default-intervals \
  --additional-intervals 0x0250,0x02AF \
  --additional-intervals 0x02B0,0x02FF \
  --additional-intervals 0x1D00,0x1D7F \
  --additional-intervals 0x1D80,0x1DBF \
  > ../builtinFonts/ipa_16_regular.h

echo "Generated ipa_16_regular.h"
rm -f "$IPA_STRIPPED"

echo ""
echo "Running compression verification..."
"$PYTHON_BIN" verify_compression.py ../builtinFonts/
