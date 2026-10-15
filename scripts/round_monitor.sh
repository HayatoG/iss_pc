#!/usr/bin/env bash
# round_monitor.sh <saida_dir> <segundos> [intervalo=1]
# Amostra GPU (nvidia-smi), CPU (/proc/stat, total e por nucleo) e memoria
# (RAM + RSS/VRAM do processo tlg-probe) e escreve gpu.csv, cpu.csv, mem.csv.
# Termina sozinho; nao falha se o processo do jogo nao existir.
set -u
OUT=${1:?uso: round_monitor.sh <saida_dir> <segundos> [intervalo=1]}
DUR=${2:?uso: round_monitor.sh <saida_dir> <segundos> [intervalo=1]}
IVL=${3:-1}
PROC=${ISS_MONITOR_PROC:-tlg-probe}
mkdir -p "$OUT" || exit 1
TMP=$(mktemp -d "${TMPDIR:-/tmp}/round_mon.XXXXXX")
trap 'exit 0' INT TERM
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMP"' EXIT

HAVE_SMI=0; command -v nvidia-smi >/dev/null && HAVE_SMI=1
GPU_Q="utilization.gpu,utilization.memory,memory.used,memory.total,clocks.sm,clocks.mem,power.draw,temperature.gpu,clocks_throttle_reasons.active"
echo "t,epoch,util_gpu,util_mem,mem_used_mib,mem_total_mib,clk_sm_mhz,clk_mem_mhz,power_w,temp_c,throttle_mask,proc_vram_mib" > "$OUT/gpu.csv"
NCPU=$(grep -c '^cpu[0-9]' /proc/stat)
{ printf 't,epoch,cpu_total'; for ((i=0;i<NCPU;i++)); do printf ',cpu%d' $i; done; printf ',cpu_max_core,cpu_temp_c\n'; } > "$OUT/cpu.csv"
echo "t,epoch,mem_total_mib,mem_used_mib,mem_avail_mib,swap_used_mib,proc_pid,proc_rss_mib,proc_threads,proc_cpu_pct" > "$OUT/mem.csv"

# sensor de temperatura da CPU (k10temp/coretemp), se houver
CPUTEMP=""
for h in /sys/class/hwmon/hwmon*; do
  n=$(cat "$h/name" 2>/dev/null)
  if [[ $n == k10temp || $n == coretemp ]]; then
    for f in "$h"/temp1_input; do [[ -r $f ]] && CPUTEMP=$f; done
    break
  fi
done

grep '^cpu' /proc/stat > "$TMP/prev"
PCLK=$(getconf CLK_TCK); NPROCS=$(nproc)
prev_ptime=""; prev_pid=""; prev_wall=""
START=$(date +%s.%N)
END=$(awk -v s="$START" -v d="$DUR" 'BEGIN{printf "%.3f", s+d}')
n=0
while :; do
  NOW=$(date +%s.%N)
  T=$(awk -v n="$NOW" -v s="$START" 'BEGIN{printf "%.2f", n-s}')
  PID=$(pgrep -x "$PROC" 2>/dev/null | head -1)

  # --- GPU ---
  if ((HAVE_SMI)); then
    G=$(timeout 3 nvidia-smi --query-gpu=$GPU_Q --format=csv,noheader,nounits 2>/dev/null | head -1 | tr -d ' ')
    V=""
    if [[ -n $PID ]]; then
      V=$(timeout 3 nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>/dev/null | tr -d ' ' | awk -F, -v p="$PID" '$1==p{s+=$2;f=1} END{if(f)print s}')
    fi
    echo "$T,$NOW,$G,$V" >> "$OUT/gpu.csv"
  fi

  # --- CPU ---
  grep '^cpu' /proc/stat > "$TMP/cur"
  LINE=$(awk 'NR==FNR{for(i=2;i<=NF;i++)p[$1,i]=$i; next}
    { tot=0; for(i=2;i<=NF;i++) tot+=$i-p[$1,i];
      idle=($5-p[$1,5])+($6-p[$1,6]);
      u=(tot>0)?100*(tot-idle)/tot:0;
      if($1=="cpu") total=u; else { s=s sprintf(",%.1f",u); if(u>m)m=u }
    } END{printf "%.1f%s,%.1f", total, s, m}' "$TMP/prev" "$TMP/cur")
  mv "$TMP/cur" "$TMP/prev"
  CT=""; [[ -n $CPUTEMP ]] && CT=$(awk '{printf "%.1f",$1/1000}' "$CPUTEMP" 2>/dev/null)
  echo "$T,$NOW,$LINE,$CT" >> "$OUT/cpu.csv"

  # --- memoria ---
  MEM=$(awk '/MemTotal/{t=$2}/MemAvailable/{a=$2}/SwapTotal/{st=$2}/SwapFree/{sf=$2}
    END{printf "%d,%d,%d,%d", t/1024,(t-a)/1024,a/1024,(st-sf)/1024}' /proc/meminfo)
  PR=",,,"
  if [[ -n $PID && -r /proc/$PID/stat ]]; then
    RSS=$(awk '/VmRSS/{printf "%d",$2/1024}' /proc/$PID/status 2>/dev/null)
    THR=$(awk '/Threads/{print $2}' /proc/$PID/status 2>/dev/null)
    PT=$(awk '{i=index($0,")"); r=substr($0,i+2); split(r,a," "); print a[12]+a[13]}' /proc/$PID/stat 2>/dev/null)
    CPCT=""
    if [[ $PID == "$prev_pid" && -n $PT ]]; then
      CPCT=$(awk -v a="$PT" -v b="$prev_ptime" -v n="$NOW" -v w="$prev_wall" -v c="$PCLK" 'BEGIN{d=n-w; if(d>0)printf "%.1f",100*(a-b)/c/d}')
    fi
    prev_pid=$PID; prev_ptime=$PT; prev_wall=$NOW
    PR="$PID,$RSS,$THR,$CPCT"
  else
    prev_pid=""
  fi
  echo "$T,$NOW,$MEM,$PR" >> "$OUT/mem.csv"

  n=$((n+1))
  # proximo tick alinhado ao relogio
  NEXT=$(awk -v s="$START" -v n="$n" -v i="$IVL" 'BEGIN{printf "%.3f", s+n*i}')
  if awk -v a="$NEXT" -v b="$END" 'BEGIN{exit !(a>b+0.0005)}'; then break; fi
  SL=$(awk -v a="$NEXT" -v n="$(date +%s.%N)" 'BEGIN{d=a-n; if(d<0)d=0; printf "%.3f", d}')
  sleep "$SL"
done
exit 0
