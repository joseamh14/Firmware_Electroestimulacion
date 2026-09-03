#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Compila y ejecuta el arnes de pruebas de escritorio de session_fsm.
#
# No necesita el target ni STM32CubeIDE: compila la FSM real
# (../../App/Src/session_fsm.c) contra los mocks de este directorio con un
# compilador de escritorio y ejecuta el binario.
#
# Compilador por defecto: LCC-win64, incluido con MATLAB
#   (<MATLABROOT>/sys/lcc64/lcc64). Ajusta LCC_ROOT si tu ruta difiere.
# El codigo es C99 portable: gcc/clang tambien valen si prefieres.
# ---------------------------------------------------------------------------
set -e
cd "$(dirname "$0")"
HERE="$(pwd)"

LCC_ROOT="${LCC_ROOT:-/c/Program Files/MATLAB/R2025b/sys/lcc64/lcc64}"
LCC64="$LCC_ROOT/bin/lcc64.exe"
LCCLNK64="$LCC_ROOT/bin/lcclnk64.exe"

w() { cygpath -w "$1"; }   # ruta POSIX -> ruta Windows (lcc-win64 la exige)

I_TEST="$(w "$HERE")"
I_APP="$(w "$(cd ../../App/Inc && pwd)")"
I_BSP="$(w "$(cd ../../BSP/Inc && pwd)")"
I_LCC="$(w "$LCC_ROOT/include64")"
L_LCC="$(w "$LCC_ROOT/lib64")"

FSM_SRC="$(w "$(cd ../../App/Src && pwd)/session_fsm.c")"
MOCKS_SRC="$(w "$HERE/mocks.c")"
TEST_SRC="$(w "$HERE/test_session_fsm.c")"

OUT_DIR="$HERE/_build"
rm -rf "$OUT_DIR" 2>/dev/null || true
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.obj "$OUT_DIR"/*.exe 2>/dev/null || true

INCS=(-I"$I_TEST" -I"$I_APP" -I"$I_BSP" -I"$I_LCC")

echo "--- compilando ---"
( cd "$OUT_DIR"
  for src in "$FSM_SRC" "$MOCKS_SRC" "$TEST_SRC"; do
      echo "  $src"
      "$LCC64" "${INCS[@]}" -c "$src" < /dev/null
  done
  echo "--- enlazando ---"
  "$LCCLNK64" -L"$L_LCC" -o test_session_fsm.exe \
      session_fsm.obj mocks.obj test_session_fsm.obj
)

echo "--- ejecutando ---"
MSYS2_ARG_CONV_EXCL='*' "$OUT_DIR/test_session_fsm.exe"
