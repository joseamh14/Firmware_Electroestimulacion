#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Compila y ejecuta el arnes de pruebas de escritorio de dac_waveform.
#
# No necesita el target ni STM32CubeIDE: el TU de test incluye entero
# ../../BSP/Src/dac_waveform.c (para alcanzar la funcion estatica
# ComputeTimerParams) y lo enlaza contra stubs no-op de la HAL.
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
I_BSP_INC="$(w "$(cd ../../BSP/Inc && pwd)")"
I_BSP_SRC="$(w "$(cd ../../BSP/Src && pwd)")"
I_LCC="$(w "$LCC_ROOT/include64")"
L_LCC="$(w "$LCC_ROOT/lib64")"

MOCKS_SRC="$(w "$HERE/mocks.c")"
TEST_SRC="$(w "$HERE/test_dac_waveform.c")"

OUT_DIR="$HERE/_build"
rm -rf "$OUT_DIR" 2>/dev/null || true
mkdir -p "$OUT_DIR"

# -I orden: primero el dir de test (su stm32l4xx_hal.h shim gana), luego BSP.
INCS=(-I"$I_TEST" -I"$I_BSP_INC" -I"$I_BSP_SRC" -I"$I_LCC")

echo "--- compilando ---"
( cd "$OUT_DIR"
  for src in "$MOCKS_SRC" "$TEST_SRC"; do
      echo "  $src"
      "$LCC64" "${INCS[@]}" -c "$src" < /dev/null
  done
  echo "--- enlazando ---"
  "$LCCLNK64" -L"$L_LCC" -o test_dac_waveform.exe \
      mocks.obj test_dac_waveform.obj
)

echo "--- ejecutando ---"
MSYS2_ARG_CONV_EXCL='*' "$OUT_DIR/test_dac_waveform.exe"
