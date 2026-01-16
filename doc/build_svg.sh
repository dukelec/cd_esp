#!/bin/bash
cd "$(dirname "$0")"

convert_svg()
{
    echo "convert_svg: ${1}*.svg"
    cd "$1"
    for i in *.svg; do
        echo "convert $i"
        inkscape --export-area-page "$i" --export-filename="../svg_out/_$i" --export-text-to-path
    done
    cd - > /dev/null
    echo ""
}

convert_svg svg_src/

