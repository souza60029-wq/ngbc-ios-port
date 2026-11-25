#!/bin/bash
# uso: bash tools/ci_run.sh <numero_do_teste>      (ONLY_GEN=1 para so gerar o C++)
set -e
N="$1"; L="logs/teste_$N"; mkdir -p "$L"
cp tools/ci_run.sh "$L/00_comando.txt"
sudo apt-get update -qq
sudo apt-get install -y -qq clang cmake ninja-build > /dev/null
curl -sS -L --fail -o game.pkg "https://github.com/souza60029-wq/ngbc-ios-port/releases/download/v1.0/B26872135B74C4F9A63367A857857D22456064C258"
python tools/stfs_extract.py game.pkg game_extracted > "$L/02_stfs.txt"
git clone --recurse-submodules --shallow-submodules --depth 1 https://github.com/hedge-dev/XenonRecomp.git > /dev/null 2>&1
python tools/patch_xenonrecomp.py XenonRecomp | tee "$L/06_patch.txt"
python tools/patch_extras.py XenonRecomp | tee "$L/06b_patch_extras.txt"
cmake -S XenonRecomp -B xr_build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$L/03_cmake.txt" 2>&1 || { tail -n 60 "$L/03_cmake.txt"; exit 1; }
cmake --build xr_build -j4 > "$L/04_build.txt" 2>&1 || { tail -n 60 "$L/04_build.txt"; exit 1; }
XA=$(find xr_build -type f -name XenonAnalyse -perm -u+x | head -n1)
"$XA" game_extracted/default.xex "$L/switch_tables.toml"
mkdir -p dump
DUMP=$(find xr_build -type f -name XenonDump -perm -u+x | head -n1)
"$DUMP" game_extracted/default.xex dump/img
XR=$(find xr_build -type f -name XenonRecomp -perm -u+x | head -n1)
CTX=$(find XenonRecomp -name ppc_context.h | head -n1)
cat > xr.toml <<EOF_TOML
[main]
file_path = "game_extracted/default.xex"
out_directory_path = "ppc_out"
switch_table_file_path = "$L/switch_tables.toml"
savegprlr_14_address = 0x822AB790
restgprlr_14_address = 0x822AB7E0
savefpr_14_address = 0x822ABF00
restfpr_14_address = 0x822ABF4C
savevmx_14_address = 0x822ADA40
restvmx_14_address = 0x822ADCD8
savevmx_64_address = 0x822ADAD4
restvmx_64_address = 0x822ADD6C
EOF_TOML
mkdir -p ppc_out
timeout 1500 "$XR" xr.toml "$CTX" > "$L/05_recomp.txt" 2>&1
tr '\r' '\n' < "$L/05_recomp.txt" | grep -v '^[[:space:]]*$' | tail -n 4
cp dump/img_manifest.txt "$L/manifest.txt"
python tools/find_funcs.py dump/img_image.bin "$L/manifest.txt" ppc_out tools/extra_funcs.txt > "$L/funcs.toml" 2> "$L/funcs_stats.txt" || true
cat "$L/funcs_stats.txt"
if [ -s "$L/funcs.toml" ]; then
  cat "$L/funcs.toml" >> xr.toml
  rm -rf ppc_out
  mkdir -p ppc_out
  timeout 1500 "$XR" xr.toml "$CTX" > "$L/05b_recomp.txt" 2>&1
  tr '\r' '\n' < "$L/05b_recomp.txt" | grep -v '^[[:space:]]*$' | tail -n 3
fi
# a funcao de entrada se chama _xstart quando o recompilador reconhece o ponto de entrada
if grep -q 'PPC_EXTERN_FUNC(_xstart)' ppc_out/ppc_recomp_shared.h; then ENTRY=_xstart; else ENTRY=sub_822C64A0; fi
echo "$ENTRY" > ppc_out/entry.txt
export ENTRY
echo "entrada: $ENTRY"
grep -ohE '__imp__[A-Za-z0-9_]+' ppc_out/ppc_recomp_shared.h ppc_out/ppc_func_mapping.cpp | grep -v '__imp__sub_' | sort -u > "$L/imports.txt"
python tools/gen_stubs.py "$L/imports.txt" ppc_out/stubs_gen.cpp
for fn in _xstart sub_822CB918 sub_822CB840; do
  F=$(grep -l "PPC_FUNC_IMPL(__imp__$fn)" ppc_out/ppc_recomp.*.cpp | head -n1 || true)
  [ -n "$F" ] && awk -v f="PPC_FUNC_IMPL(__imp__$fn)" 'index($0,f){p=1} p{print} p&&/^}/{exit}' "$F" | head -n 220 > "$L/src_$fn.txt" || true
done
if [ -n "$ONLY_GEN" ]; then echo "so gerar: pronto"; exit 0; fi
mkdir -p obj
cat > compile_one.sh <<'EOF_SH'
#!/bin/bash
f="$1"; b=$(basename "$f" .cpp)
if clang++ -std=c++20 -O1 -w -march=native -DNGBC_HOST -DNGBC_ENTRY=$ENTRY -I ppc_out -I XenonRecomp/XenonUtils -I XenonRecomp/thirdparty/simde -c "$f" -o "obj/$b.o" > "$L/cc_$b.txt" 2>&1; then echo "OK $b"; else echo "FALHOU $b"; fi
EOF_SH
chmod +x compile_one.sh
export L
ls ppc_out/ppc_recomp.*.cpp ppc_out/ppc_func_mapping.cpp ppc_out/stubs_gen.cpp runtime/*.cpp | xargs -P 4 -n 1 ./compile_one.sh > "$L/compilacao.txt"
echo "compilados: $(grep -c '^OK' "$L/compilacao.txt")  falhas: $(grep -c FALHOU "$L/compilacao.txt" || true)"
for b in $(grep FALHOU "$L/compilacao.txt" | head -n 3 | awk '{print $2}'); do echo "=== erro em $b"; head -n 30 "$L/cc_$b.txt"; done
set +e
clang++ obj/*.o -o ngbc_host -rdynamic -lpthread 2> "$L/link1.txt"
RC=$?
set -e
if [ $RC -ne 0 ]; then
  grep -oE 'undefined reference to .sub_[0-9A-F]{8}' "$L/link1.txt" | grep -oE 'sub_[0-9A-F]{8}' | sort -u > "$L/faltando.txt" || true
  echo "funcoes chamadas mas nao recompiladas: $(wc -l < "$L/faltando.txt")"
  head -n 40 "$L/faltando.txt"
  {
    echo '#include "ppc_recomp_shared.h"'
    echo '#include <cstdio>'
    echo '#include <unistd.h>'
    for f in $(cat "$L/faltando.txt"); do
      echo "PPC_FUNC($f) { fprintf(stderr, \"[!] funcao nao recompilada: $f lr=%08X\\n\", (uint32_t)ctx.lr); _exit(4); }"
    done
  } > ppc_out/missing_stubs.cpp
  ./compile_one.sh ppc_out/missing_stubs.cpp
  clang++ obj/*.o -o ngbc_host -rdynamic -lpthread 2> "$L/link2.txt" || { echo "=== erro de link"; head -n 40 "$L/link2.txt"; exit 1; }
fi
ls -l ngbc_host
echo "=== rodando o jogo (30 s)"
set +e
timeout 90 ./ngbc_host dump/img_image.bin game_extracted 30 > "$L/run_stdout.txt" 2> "$L/run.txt"
echo "saida do jogo: $?"
set -e
head -n 150 "$L/run.txt"