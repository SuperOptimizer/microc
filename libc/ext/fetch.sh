#!/bin/sh
# fetch.sh: download the sources that mkulibs.sh compiles into build/ext/dl, check each against the sha256
# recorded in libc/ext/SOURCES.txt and unpack it under build/ext. This is the one step that uses the
# system's tools (curl, shasum, tar): mcc has no downloader. Nothing from the archives is run.
set -e
cd "$(dirname "$0")/../.."
L=libc/ext/SOURCES.txt; E=build/ext
mkdir -p $E/dl
grep -v '^#' $L | while IFS='|' read -r file url sum how; do
    file=$(echo $file); url=$(echo $url); sum=$(echo $sum)
    [ -f $E/dl/$file ] || curl -sSL --max-time 300 -A curl -o $E/dl/$file "$url"
    got=$(shasum -a 256 $E/dl/$file | cut -d' ' -f1)
    [ "$got" = "$sum" ] || { echo "fetch: $file: sha256 is $got, expected $sum" >&2; exit 1; }
    dir=${file%.tar.gz}
    [ -d $E/$dir ] || tar xzf $E/dl/$file -C $E
    [ -d $E/$dir ] || { echo "fetch: $file did not unpack into $E/$dir" >&2; exit 1; }
    echo "ok $dir"
done
