#!/usr/bin/env bash
# Test of BC 1D textures on NVIDIA (branch test/bc1d-nvidia). Usage: scripts/test_bc1d.sh <1|2|3> [seconds=7200]
#   1 = control (as released)   2 = BC 1D decoded on the CPU (TLG_BC_TINY=auto)   3 = BC 1D promoted to 2D (TLG_BLOCK1D=1)
#   4/5/6 = occlusion mode 0/1/2 (default 3)   7 = every 1D image as 2D (BP09)
v=${1:-2}
export TLG_BC1D_LOG=1
case $v in
    1) ;;
    2) export TLG_BC_TINY=auto ;;
    3) export TLG_BLOCK1D=1 ;;
    4) export TLG_OCCLUSION_MODE=0 ;;
    5) export TLG_OCCLUSION_MODE=1 ;;
    6) export TLG_OCCLUSION_MODE=2 ;;
    7) export TLG_BP09=1 ;;
    *) echo "variant 1-7" >&2; exit 1 ;;
esac
echo "variant $v"
exec "$(dirname -- "$0")/infamous_run.sh" "${2:-7200}"
