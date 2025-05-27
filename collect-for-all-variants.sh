#!/usr/bin/env bash
set -eux

this_script="$(readlink -f "$0")"
root="$(dirname "$this_script")"

if [ $# -eq 1 ]; then
  current_kernel="$1"
  ran_at_boot=yes
else
  current_kernel=
  ran_at_boot=
fi

if [ -z "${MAKEFLAGS+x}" ]; then
  MAKEFLAGS="-j $(nproc)"
  export MAKEFLAGS
fi

run_as_user() {
  if [ -z "${SUDO_UID+x}" ]; then
    "$@"
  else
    sudo -u "#$SUDO_UID" --preserve-env=MAKEFLAGS -- "$@"
  fi
}

wait_for_internet_connection_if_ran_at_boot() {
  [ -z "$ran_at_boot" ] && \
    return
  while ! ping -c 1 -w 5 git.kernel.org > /dev/null; do
    echo Waiting for Internet connection... >&2
    sleep 5
  done
}

install_generic_deps() {
  apt-get install -y build-essential kexec-tools
  apt-get build-dep -y linux
}

run_this_script_on_next_boot() {
  local environment
  local working_dir
  if [ -z "${SUDO_UID+x}" ]; then
    environment=
  else
    environment="
Environment=SUDO_UID=$SUDO_UID"
  fi
  working_dir="$(dirname "$this_script")"
  cat << EOF > /etc/systemd/system/collect-page-fault-timings.service
[Unit]
Description=Continue collecting page fault timings
After=network-online.target nss-lookup.target
Wants=network-online.target nss-lookup.target

[Service]
Type=simple
ExecStart=$this_script $@
WorkingDirectory=$working_dir$environment
StandardOutput=journal+console
StandardError=inherit

[Install]
WantedBy=multi-user.target
EOF
  systemctl enable collect-page-fault-timings.service
}

kexec_kernel_and_rerun_this_script() {
  local variant="$1"
  local kernelrelease
  cd "linux/$variant"
  make modules_install install
  kernelrelease="$(make kernelrelease)"
  kexec \
    --load "/boot/vmlinuz-$kernelrelease" \
    --initrd "/boot/initrd.img-$kernelrelease" \
    --reuse-cmdline
  run_this_script_on_next_boot "$variant"
  systemctl kexec
  exit 0
}

build_and_kexec_kernel_if_needed() {
  local variant="$1"
  local remote_name="$2"
  local remote_url="$3"
  local ref="$4"
  [ "$current_kernel" = "$variant" ] && \
    return
  wait_for_internet_connection_if_ran_at_boot
  install_generic_deps
  run_as_user ./build-kernel.sh "$variant" "$remote_name" "$remote_url" "$ref"
  kexec_kernel_and_rerun_this_script "$variant"
}

compile_collection_helper() {
  run_as_user gcc collect-page-fault-timings.c \
    -o collect-page-fault-timings \
    -O3
}

run_collection_helper() {
  ./collect-page-fault-timings \
    --length $((4096 * 1024)) \
    --type minor \
    --access "$1"
}

collect_page_fault_timings_generic() {
  local access
  local variant="$1"
  compile_collection_helper
  for access in read write rw; do
    run_collection_helper "$access" | \
      run_as_user sh -c "cat > results/$variant-$access-timeline.csv"
    run_as_user sh -c "./convert-timeline-to-stacked-area-chart.py results/$variant-$access-timeline.csv > results/$variant-$access-stacked.csv"
  done
}

vanilla() {
  build_and_kexec_kernel_if_needed \
    vanilla \
    torvalds \
    "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git" \
    4663747812d1a272312d1b95cbd128f0cdb329f2
  collect_page_fault_timings_generic vanilla
}

extmem() {
  build_and_kexec_kernel_if_needed \
    extmem \
    stable \
    "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git" \
    1c700860e8bc079c5c71d73c55e51865d273943c
  collect_page_fault_timings_generic extmem
}

fbmm() {
  build_and_kexec_kernel_if_needed \
    fbmm \
    stable \
    "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git" \
    46df6964c1a9eb72027710f626cb1c6bfb5d58c9
  # TODO: Actually set up FBMM.
  collect_page_fault_timings_generic fbmm
}

uintr() {
  build_and_kexec_kernel_if_needed \
    uintr \
    stable \
    "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git" \
    ad6c047b2e2f276568ab504deff51d711420b4d3
  collect_page_fault_timings_generic uintr
}

if [ ! -f results/vanilla-read-timeline.csv ]; then
  vanilla
fi

if [ ! -f results/extmem-read-timeline.csv ]; then
  extmem
fi

if [ ! -f results/uintr-read-timeline.csv ]; then
  uintr
fi

# Only remove the service when we're done.
rm -f /etc/systemd/system/collect-page-fault-timings.service \
  /etc/systemd/system/multi-user.target.wants/collect-page-fault-timings.service
