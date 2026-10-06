#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Compila y ejecuta el arnes de pruebas de escritorio de sd_logger.
#
# No necesita el target ni STM32CubeIDE: los modulos reales se enlazan contra
# los mocks de este directorio.
#
# Compilador por defecto: LCC-win64, incluido con MATLAB
#   (<MATLABROOT>/sys/lcc64/lcc64). Ajusta LCC_ROOT si tu ruta difiere.
# El codigo es C99 portable; con gcc:
#   gcc -std=c99  -I. -I../../BSP/Inc -I../../App/Inc \
#       mocks.c test_sd_logger.c ../../App/Src/sd_logger.c ../../App/Src/current_log.c ../../BSP/Src/current_calc.c -lm -o test_sd_logger
# ---------------------------------------------------------------------------
set -e
cd "$(dirname "$0")"
HERE="$(pwd)"

LCC_ROOT="${LCC_ROOT:-/c/Program Files/MATLAB/R2025b/sys/lcc64/lcc64}"
LCC64="$LCC_ROOT/bin/lcc64.exe"
LCCLNK64="$LCC_ROOT/bin/lcclnk64.exe"

w() { cygpath -w "$1"; }   # ruta POSIX -> ruta Windows (lcc-win64 la exige)

INCS=(-I"$(w "$HERE")" -I"$(w "$(cd ../../BSP/Inc && pwd)")" -I"$(w "$(cd ../../App/Inc && pwd)")" -I"$(w "$LCC_ROOT/include64")")
SRCS=(mocks.c test_sd_logger.c ../../App/Src/sd_logger.c ../../App/Src/current_log.c ../../BSP/Src/current_calc.c)

OUT_DIR="$HERE/_build"
rm -rf "$OUT_DIR" 2>/dev/null || true
mkdir -p "$OUT_DIR"

echo "--- compilando ---"
OBJS=()
for src in "${SRCS[@]}"; do
    abs="$(cd "$(dirname "$src")" && pwd)/$(basename "$src")"
    echo "  $abs"
    ( cd "$OUT_DIR" && "$LCC64"  "${INCS[@]}" -c "$(w "$abs")" < /dev/null )
    OBJS+=("$(basename "${src%.c}").obj")
done
echo "--- enlazando ---"
( cd "$OUT_DIR" && "$LCCLNK64" -L"$(w "$LCC_ROOT/lib64")" -o test_sd_logger.exe "${OBJS[@]}" )

echo "--- ejecutando ---"
MSYS2_ARG_CONV_EXCL='*' "$OUT_DIR/test_sd_logger.exe"
