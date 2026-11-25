#!/bin/bash
# roda no macOS do CI: compila tudo para iPhone (arm64, iOS 15) e monta o IPA
set -e
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
ENTRY=$(cat ppc_out/entry.txt)
echo "entrada: $ENTRY"
mkdir -p obj logs Payload/NGBC.app/game
curl -sS -L --fail -o game.pkg "https://github.com/souza60029-wq/ngbc-ios-port/releases/download/v1.0/B26872135B74C4F9A63367A857857D22456064C258"
python3 tools/stfs_extract.py game.pkg game_extracted > logs/stfs.txt
cat > compile_one.sh <<'EOF_SH'
#!/bin/bash
f="$1"; b=$(basename "$f" .cpp)
if xcrun --sdk iphoneos clang++ -target arm64-apple-ios15.0 -isysroot "$SDK" -std=c++20 -O1 -w -DNGBC_ENTRY=$ENTRY -I ppc_out -I XenonRecomp/XenonUtils -I XenonRecomp/thirdparty/simde -c "$f" -o "obj/$b.o" > "logs/cc_$b.txt" 2>&1; then echo "OK $b"; else echo "FALHOU $b"; fi
EOF_SH
chmod +x compile_one.sh
export SDK ENTRY
ls ppc_out/ppc_recomp.*.cpp ppc_out/ppc_func_mapping.cpp ppc_out/stubs_gen.cpp runtime/*.cpp | xargs -P 4 -n 1 ./compile_one.sh | tee logs/compilacao.txt
if grep -q FALHOU logs/compilacao.txt; then
  for b in $(grep FALHOU logs/compilacao.txt | head -n 3 | awk '{print $2}'); do echo "=== erro em $b"; head -n 30 "logs/cc_$b.txt"; done
  exit 1
fi
xcrun --sdk iphoneos clang++ -target arm64-apple-ios15.0 -isysroot "$SDK" -std=c++20 -fobjc-arc -O1 -w -c src/app.mm -o obj/app.o
xcrun --sdk iphoneos clang++ -target arm64-apple-ios15.0 -isysroot "$SDK" obj/*.o -framework UIKit -framework Foundation -o Payload/NGBC.app/NGBC
cp dump/img_image.bin Payload/NGBC.app/image.bin
cp -R game_extracted/data game_extracted/media Payload/NGBC.app/game/
cat > Payload/NGBC.app/Info.plist <<'EOF_PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>NGBC</string>
    <key>CFBundleIdentifier</key><string>com.souza.ngbc</string>
    <key>CFBundleName</key><string>NGBC</string>
    <key>CFBundleDisplayName</key><string>NGBC</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>MinimumOSVersion</key><string>15.0</string>
    <key>UIFileSharingEnabled</key><true/>
    <key>LSSupportsOpeningDocumentsInPlace</key><true/>
    <key>UIDeviceFamily</key><array><integer>1</integer></array>
    <key>UIRequiredDeviceCapabilities</key><array><string>arm64</string></array>
    <key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationLandscapeRight</string><string>UIInterfaceOrientationLandscapeLeft</string></array>
</dict>
</plist>
EOF_PLIST
cat > ents.plist <<'EOF_ENT'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>
<key>com.apple.developer.kernel.increased-memory-limit</key><true/>
</dict></plist>
EOF_ENT
brew install ldid > /dev/null 2>&1 || echo "aviso: nao consegui instalar o ldid"
if command -v ldid > /dev/null; then ldid -Sents.plist Payload/NGBC.app/NGBC && echo "entitlements aplicadas"; fi
zip -qr NGBC.ipa Payload
ls -lh NGBC.ipa