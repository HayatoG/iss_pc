#!/usr/bin/env bash
# Test variants for the GPU study (BC 1D on NVIDIA, occlusion, async wait). Usage: scripts/test_bc1d.sh <1-8> [seconds=7200]
#   1 = control (as released)   2 = BC 1D decoded on the CPU (TLG_BC_TINY=auto)   3 = BC 1D promoted to 2D (TLG_BLOCK1D=1)
#   4/5/6 = occlusion mode 0/1/2 (default 3)   7 = every 1D image as 2D (BP09)
#   8 = bounded wait for compiling shaders/pipelines (8 ms per wait, 16 ms per frame) + region trace
v=${1:-2}
export ISS_LANGUAGE=${ISS_LANGUAGE:-17}   # pt-BR, like the launcher on this PC
export TLG_BC1D_LOG=1
case $v in
    1) ;;
    2) export TLG_BC_TINY=auto ;;
    3) export TLG_BLOCK1D=1 ;;
    4) export TLG_OCCLUSION_MODE=0 ;;
    5) export TLG_OCCLUSION_MODE=1 ;;
    6) export TLG_OCCLUSION_MODE=2 ;;
    7) export TLG_BP09=1 ;;
    8) export TLG_ASYNC_WAIT_MS=8 TLG_ASYNC_WAIT_FRAME_MS=16 TLG_REGION_TRACE=1 ;;
    *) echo "variant 1-8" >&2; exit 1 ;;
esac
[[ -n ${SHOTS:-} ]] && export ISS_FRAME_RECORD_TRIGGER=$SHOTS/rec.trigger
echo "variant $v"
exec "$(dirname -- "$0")/infamous_run.sh" "${2:-7200}" "${SHOTS:-}"
