#!/usr/bin/env bash
set -eu

worktree_name="$1"
remote_name="$2"
remote_url="$3"
ref="$4"
root="$(pwd)"

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

git_worktree_add_detach_or_checkout() {
  local name="$1"
  local ref="$2"

  if [ -d "$name" ]; then
    pushd "$name"
    git checkout "$ref"
    popd
  else
    git worktree add "$name" --detach "$ref"
  fi
}

create_kernel_repo_if_not_exists

cd linux

git_remote_add_if_missing "$remote_name" "$remote_url"
git fetch "$remote_name"
git_worktree_add_detach_or_checkout "$worktree_name" "$ref"

cd "$worktree_name"

git am "$root/patches/$worktree_name"/*.patch

cp "$root/patches/$worktree_name/config" .config
make olddefconfig

make
make modules
