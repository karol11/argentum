#!/bin/bash
cmake --build ../build-rel
cp ../build-rel/agc             ./aglan_0.0-1_amd64/usr/bin
cp ../build-rel/libag_runtime.a ./aglan_0.0-1_amd64/usr/lib/aglan
cp ../build-rel/libag_sqlite.a ./aglan_0.0-1_amd64/usr/lib/aglan
cp ../demo/*.ag  ./aglan_0.0-1_amd64/usr/share/aglan/demo-src
cp ../demo/*.jpg ./aglan_0.0-1_amd64/usr/share/aglan/demo-src
cp ../demo/*.png ./aglan_0.0-1_amd64/usr/share/aglan/demo-src
cp ../demo/*.ttf ./aglan_0.0-1_amd64/usr/share/aglan/demo-src
cp ../demo/mydb.sqlite ./aglan_0.0-1_amd64/usr/share/aglan/demo-src

dpkg-deb --build --root-owner-group aglan_0.0-1_amd64

rm ./aglan_0.0-1_amd64/usr/share/aglan/demo-src/*
rm ./aglan_0.0-1_amd64/usr/lib/aglan/*
rm ./aglan_0.0-1_amd64/usr/bin/*

echo "Done"
echo "Test it with: sudo dpkg -i aglan_0.0-1_amd64.deb"
echo "Remove with:  sudo dpkg -r aglan"
