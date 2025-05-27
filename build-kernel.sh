#!/usr/bin/env bash
set -eux

variant="$1"
remote_name="$2"
remote_url="$3"
ref="$4"
root="$(dirname "$(readlink -f "$0")")"

create_kernel_repo_if_not_exists() {
  [ -d linux ] || \
    git init --bare linux
}

git_remote_add_if_missing() {
  local name="$1"
  local url="$2"

  git remote get-url "$name" > /dev/null 2>&1 || \
    git remote add "$name" "$url"
}

create_kernel_repo_if_not_exists

cd linux

if [ -d "$variant" ]; then
  cd "$variant"
else
  git_remote_add_if_missing "$remote_name" "$remote_url"
  git fetch "$remote_name" "$ref"

  git worktree add "$variant" --detach "$ref"

  cd "$variant"

  GIT_COMMITTER_NAME=build-kernel \
    GIT_COMMITTER_EMAIL='build-kernel@internship.invalid' \
    git am "$root/variants/$variant"/patches/*.patch

  yes '' | make localmodconfig

  scripts/config --set-str CONFIG_LOCALVERSION "-$variant"
  scripts/config --enable CONFIG_TRACE_PF
  scripts/config --enable CONFIG_FAST_TRACEPOINTS

  make olddefconfig
fi

make
make modules
make headers_install
