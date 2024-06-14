#!/bin/bash
cmake --build ../build-rel
cp ../output/bin/agc   ./aglan_0.0-1_amd64/usr/bin
cp ../output/libs/*    ./aglan_0.0-1_amd64/usr/lib/aglan
cp ../output/ag/*      ./aglan_0.0-1_amd64/usr/share/aglan/demo-src
cp ../output/workdir/* ./aglan_0.0-1_amd64/usr/share/aglan/demo-src

dpkg-deb --build --root-owner-group aglan_0.0-1_amd64

rm ./aglan_0.0-1_amd64/usr/share/aglan/demo-src/*
rm ./aglan_0.0-1_amd64/usr/lib/aglan/*
rm ./aglan_0.0-1_amd64/usr/bin/*

echo "Done"
echo "Test it with: sudo dpkg -i aglan_0.0-1_amd64.deb"
echo "Remove with:  sudo dpkg -r aglan"
