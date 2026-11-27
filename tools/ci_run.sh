#!/bin/bash
# uso: bash tools/ci_run.sh <numero_do_teste>      (ONLY_GEN=1 para so gerar o C++ uma vez)
# Gera o C++ do jogo, compila, roda no Linux e, se o jogo chamar uma funcao que o recompilador nao achou,
# acrescenta o endereco a lista e repete (ate 10 vezes). A lista final fica em $L/extra_found.txt.
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
cp dump/img_manifest.txt "$L/manifest.txt"
XR=$(find xr_build -type f -name XenonRecomp -perm -u+x | head -n1)
CTX=$(find XenonRecomp -name ppc_context.h | head -n1)
cat > xr_base.toml <<EOF_TOML
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

# passada 1: so para saber quais funcoes o recompilador acha sozinho
cp xr_base.toml xr.toml
mkdir -p ppc_out
timeout 1500 "$XR" xr.toml "$CTX" > "$L/05_recomp.txt" 2>&1
tr '\r' '\n' < "$L/05_recomp.txt" | grep -v '^[[:space:]]*$' | tail -n 3
mkdir -p pass1
cp ppc_out/ppc_func_mapping.cpp pass1/
touch "$L/extra_found.txt"

# gera ppc_out com a lista atual de funcoes extras
gen_cpp() {
  { cat tools/extra_funcs.txt; echo; cat "$L/extra_found.txt"; } > "$L/extra_all.txt"
  python tools/find_funcs.py dump/img_image.bin "$L/manifest.txt" pass1 "$L/extra_all.txt" > "$L/funcs.toml" 2> "$L/funcs_stats.txt" || true
  cat "$L/funcs_stats.txt"
  cp xr_base.toml xr.toml
  if [ -s "$L/funcs.toml" ]; then cat "$L/funcs.toml" >> xr.toml; fi
  rm -rf ppc_out
  mkdir -p ppc_out
  timeout 1500 "$XR" xr.toml "$CTX" > "$L/05b_recomp.txt" 2>&1
  tr '\r' '\n' < "$L/05b_recomp.txt" | grep -v '^[[:space:]]*$' | tail -n 2
  if grep -q 'PPC_EXTERN_FUNC(_xstart)' ppc_out/ppc_recomp_shared.h; then ENTRY=_xstart; else ENTRY=sub_822C64A0; fi
  echo "$ENTRY" > ppc_out/entry.txt
  export ENTRY
  grep -ohE '__imp__[A-Za-z0-9_]+' ppc_out/ppc_recomp_shared.h ppc_out/ppc_func_mapping.cpp | grep -v '__imp__sub_' | sort -u > "$L/imports.txt"
  python tools/gen_stubs.py "$L/imports.txt" ppc_out/stubs_gen.cpp
}

# compila tudo (Linux) e liga; funcoes chamadas mas nao recompiladas viram stubs que avisam
cat > compile_one.sh <<'EOF_SH'
#!/bin/bash
f="$1"; b=$(basename "$f" .cpp)
if clang++ -std=c++20 -O1 -w -march=native -DNGBC_HOST -DNGBC_ENTRY=$ENTRY -I ppc_out -I XenonRecomp/XenonUtils -I XenonRecomp/thirdparty/simde -c "$f" -o "obj/$b.o" > "$L/cc_$b.txt" 2>&1; then echo "OK $b"; else echo "FALHOU $b"; fi
EOF_SH
chmod +x compile_one.sh
export L
build_host() {
  rm -rf obj; mkdir -p obj
  ls ppc_out/ppc_recomp.*.cpp ppc_out/ppc_func_mapping.cpp ppc_out/stubs_gen.cpp runtime/*.cpp | xargs -P 4 -n 1 ./compile_one.sh > "$L/compilacao.txt"
  echo "compilados: $(grep -c '^OK' "$L/compilacao.txt")  falhas: $(grep -c FALHOU "$L/compilacao.txt" || true)"
  for b in $(grep FALHOU "$L/compilacao.txt" | head -n 3 | awk '{print $2}'); do echo "=== erro em $b"; head -n 30 "$L/cc_$b.txt"; done
  if grep -q FALHOU "$L/compilacao.txt"; then exit 1; fi
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
}

for it in 1 2 3 4 5 6 7 8 9 10; do
  echo "=================== iteracao $it"
  gen_cpp
  build_host
  echo "=== rodando o jogo (30 s)"
  set +e
  rm -rf "$L/gpu"; mkdir -p "$L/gpu"
  NGBC_DUMP_DIR="$L/gpu" timeout 90 ./ngbc_host dump/img_image.bin game_extracted 30 > "$L/run_stdout.txt" 2> "$L/run.txt"
  GRC=$?
  set -e
  cp "$L/run.txt" "$L/run_$it.txt"
  echo "saida do jogo: $GRC"
  if [ -n "$ONLY_GEN" ]; then echo "so gerar: pronto"; exit 0; fi
  if [ "$GRC" -ne 3 ]; then break; fi
  A=$(grep -oE 'nao resolvida: ctr=[0-9A-F]{8}' "$L/run.txt" | head -n1 | grep -oE '[0-9A-F]{8}$' || true)
  if [ -z "$A" ]; then break; fi
  if grep -qx "$A" "$L/extra_found.txt"; then echo "o mesmo endereco $A voltou a falhar; parando"; break; fi
  echo "funcao nao descoberta: $A (adicionada)"
  echo "$A" >> "$L/extra_found.txt"
done
ls -l "$L/gpu" | head -80 > "$L/gpu_lista.txt" || true
echo "=== funcoes extras descobertas durante o teste:"
cat "$L/extra_found.txt"
for fn in _xstart; do
  F=$(grep -l "PPC_FUNC_IMPL(__imp__$fn)" ppc_out/ppc_recomp.*.cpp | head -n1 || true)
  [ -n "$F" ] && awk -v f="PPC_FUNC_IMPL(__imp__$fn)" 'index($0,f){p=1} p{print} p&&/^}/{exit}' "$F" | head -n 220 > "$L/src_$fn.txt" || true
done
echo "=== fim da ultima execucao:"
head -n 100 "$L/run.txt"