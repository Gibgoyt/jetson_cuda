#!/usr/bin/env bash
#
# Scripts/init_Gibgoyt_git.sh
#
# Bootstrap the Gibgoyt/jetson_cuda consolidated fork.
#
# In every .git (superproject + every submodule, including the nested ssd):
#   - renames remote 'origin' (NVIDIA official) -> 'nvidia_upstream'
#   - adds remote 'my_origin' pointing at git@github.com:Gibgoyt/jetson_cuda
#   - for every branch on nvidia_upstream, creates a matching LOCAL branch and
#     pushes it to my_origin
#
# Branch namespace on my_origin (single repo, many orphan branches):
#   - superproject:        flat (master, dev, jp5, L4T-R36.3.0, ...)
#   - submodules:          submodules/<path>/<branch>
#                          e.g. submodules/utils/v1
#                               submodules/c/plugins/pose/master
#                               submodules/python/training/detection/ssd/onnx
#
# my_origin in each submodule uses a RESTRICTIVE fetch refspec that maps
#   refs/heads/submodules/<path>/*   <-->   refs/remotes/my_origin/*
# so inside a submodule, plain `git fetch my_origin` only pulls down THAT
# submodule's branches (renamed to drop the prefix locally). Plain `git push`
# from a local branch then goes to the right place automatically.
#
# Also rewrites the top-level .gitmodules so that fresh clones pull submodules
# from the Gibgoyt fork at the right branch.
#
# Idempotent: safe to re-run. Plain pushes (no force) so a divergence will
# surface as a clear error rather than silently overwriting.

set -euo pipefail

# ----------------------------------------------------------------------------
# config
# ----------------------------------------------------------------------------
GIBGOYT_URL="git@github.com:Gibgoyt/jetson_cuda"
MY_REMOTE="my_origin"
NV_REMOTE="nvidia_upstream"

# "<.gitmodules section name>|<path>|<my_origin branch prefix>"
SUBMODULES=(
  "utils|utils|submodules/utils"
  "ros|ros|submodules/ros"
  "tools/camera-capture|tools/camera-capture|submodules/tools/camera-capture"
  "docker/containers|docker/containers|submodules/docker/containers"
  "plugins/pose|c/plugins/pose|submodules/c/plugins/pose"
  "python/training/classification|python/training/classification|submodules/python/training/classification"
  "python/training/detection|python/training/detection|submodules/python/training/detection"
  "python/training/segmentation|python/training/segmentation|submodules/python/training/segmentation"
)

# Submodules nested inside other submodules. Not in the top-level .gitmodules.
# "<path>|<my_origin branch prefix>"
NESTED_SUBMODULES=(
  "python/training/detection/ssd|submodules/python/training/detection/ssd"
)

# ----------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------
log()  { printf '\n\033[1;36m=== %s ===\033[0m\n' "$*"; }
note() { printf '  \033[0;90m%s\033[0m\n' "$*"; }
warn() { printf '  \033[1;33mWARN: %s\033[0m\n' "$*"; }
ok()   { printf '  \033[1;32mOK:   %s\033[0m\n' "$*"; }

# Rename 'origin' -> nvidia_upstream if needed.
rename_origin_to_nvidia_upstream() {
  local dir="$1"
  if git -C "$dir" remote get-url "$NV_REMOTE" >/dev/null 2>&1; then
    note "remote '$NV_REMOTE' already present"
    return 0
  fi
  if git -C "$dir" remote get-url origin >/dev/null 2>&1; then
    git -C "$dir" remote rename origin "$NV_REMOTE"
    ok "renamed origin -> $NV_REMOTE"
  else
    warn "$dir has neither 'origin' nor '$NV_REMOTE'"
  fi
}

# Add/refresh my_origin remote with the given fetch refspec(s).
# Replaces all existing fetch refspecs for my_origin with exactly the given ones.
set_my_origin() {
  local dir="$1"
  shift
  local refspecs=("$@")

  if ! git -C "$dir" remote get-url "$MY_REMOTE" >/dev/null 2>&1; then
    git -C "$dir" remote add "$MY_REMOTE" "$GIBGOYT_URL"
    ok "added remote $MY_REMOTE = $GIBGOYT_URL"
  else
    git -C "$dir" remote set-url "$MY_REMOTE" "$GIBGOYT_URL"
  fi

  git -C "$dir" config --unset-all "remote.${MY_REMOTE}.fetch" 2>/dev/null || true
  local r
  for r in "${refspecs[@]}"; do
    git -C "$dir" config --add "remote.${MY_REMOTE}.fetch" "$r"
  done
}

# For every nvidia_upstream/<b>:
#   - create or fast-forward local branch <b> to that commit
#   - queue a push  refs/heads/<b> -> refs/heads/<my_prefix>/<b> on my_origin
#     (if my_prefix is empty, push as refs/heads/<b>)
# Then push all queued refspecs in a single `git push` (one SSH session) and
# set upstream tracking for every local branch to my_origin/<b>.
mirror_branches() {
  local dir="$1" my_prefix="$2"

  note "fetching $NV_REMOTE ..."
  git -C "$dir" fetch --prune "$NV_REMOTE"

  local branches
  branches=$(git -C "$dir" for-each-ref --format='%(refname:lstrip=3)' \
             "refs/remotes/${NV_REMOTE}/" | grep -v '^HEAD$' || true)

  if [ -z "$branches" ]; then
    warn "no branches found on $NV_REMOTE in $dir"
    return 0
  fi

  local refspecs=()
  local b upstream_sha local_sha my_branch
  for b in $branches; do
    upstream_sha=$(git -C "$dir" rev-parse "${NV_REMOTE}/${b}")

    if git -C "$dir" show-ref --verify --quiet "refs/heads/${b}"; then
      local_sha=$(git -C "$dir" rev-parse "refs/heads/${b}")
      if [ "$local_sha" != "$upstream_sha" ]; then
        if git -C "$dir" merge-base --is-ancestor "$local_sha" "$upstream_sha"; then
          git -C "$dir" branch -f "$b" "$upstream_sha"
        else
          warn "local '$b' has diverged from $NV_REMOTE/$b; leaving local alone and skipping push"
          continue
        fi
      fi
    else
      git -C "$dir" branch "$b" "$upstream_sha"
    fi

    if [ -n "$my_prefix" ]; then
      my_branch="${my_prefix}/${b}"
    else
      my_branch="${b}"
    fi
    refspecs+=("refs/heads/${b}:refs/heads/${my_branch}")
  done

  note "queued ${#refspecs[@]} branches"
  if [ "${#refspecs[@]}" -gt 0 ]; then
    note "pushing -> $MY_REMOTE ..."
    if ! git -C "$dir" push "$MY_REMOTE" "${refspecs[@]}"; then
      warn "push to $MY_REMOTE had errors (see above); continuing"
    fi
  fi

  note "fetching $MY_REMOTE to populate remote-tracking refs ..."
  git -C "$dir" fetch --prune "$MY_REMOTE" 2>/dev/null || true

  # Set upstream tracking so a future plain `git push` from each local branch
  # writes back to my_origin (which physically routes to the prefixed branch
  # on Gibgoyt via the my_origin fetch refspec).
  for b in $branches; do
    if git -C "$dir" show-ref --verify --quiet "refs/remotes/${MY_REMOTE}/${b}"; then
      git -C "$dir" branch --set-upstream-to="${MY_REMOTE}/${b}" "$b" >/dev/null 2>&1 || true
    fi
  done
}

# ----------------------------------------------------------------------------
# main repo
# ----------------------------------------------------------------------------
log "main repo (./.git)"
rename_origin_to_nvidia_upstream .
# Pull all top-level branches but skip submodules/* so my_origin/<...> refs in
# the superproject stay clean.
set_my_origin . \
  "+refs/heads/*:refs/remotes/${MY_REMOTE}/*" \
  "^refs/heads/submodules/*"
mirror_branches . ""

# ----------------------------------------------------------------------------
# top-level submodules
# ----------------------------------------------------------------------------
for entry in "${SUBMODULES[@]}"; do
  IFS='|' read -r name path prefix <<<"$entry"
  log "submodule [$name] -> $path -> branches at $prefix/*"
  rename_origin_to_nvidia_upstream "$path"
  set_my_origin "$path" "+refs/heads/${prefix}/*:refs/remotes/${MY_REMOTE}/*"
  mirror_branches "$path" "$prefix"
done

# ----------------------------------------------------------------------------
# nested submodules
# ----------------------------------------------------------------------------
for entry in "${NESTED_SUBMODULES[@]}"; do
  IFS='|' read -r path prefix <<<"$entry"
  log "nested submodule $path -> branches at $prefix/*"
  rename_origin_to_nvidia_upstream "$path"
  set_my_origin "$path" "+refs/heads/${prefix}/*:refs/remotes/${MY_REMOTE}/*"
  mirror_branches "$path" "$prefix"
done

# ----------------------------------------------------------------------------
# rewrite .gitmodules so fresh clones pull from Gibgoyt
# ----------------------------------------------------------------------------
log "rewriting top-level .gitmodules"
for entry in "${SUBMODULES[@]}"; do
  IFS='|' read -r name path prefix <<<"$entry"
  git config -f .gitmodules "submodule.${name}.url"    "$GIBGOYT_URL"
  git config -f .gitmodules "submodule.${name}.branch" "${prefix}/master"
  printf '  %-40s url    = %s\n' "$name" "$GIBGOYT_URL"
  printf '  %-40s branch = %s\n' "$name" "${prefix}/master"
done

log "DONE"
cat <<EOF
Summary:
  * In every repo:
      - remote 'origin' renamed to '$NV_REMOTE' (the NVIDIA official source)
      - remote '$MY_REMOTE' = $GIBGOYT_URL (your consolidated fork)
  * Submodule remotes use a restrictive fetch refspec so inside a submodule:
      git fetch $MY_REMOTE          # only that submodule's branches
      git fetch $NV_REMOTE          # all of NVIDIA upstream's branches
      git push                      # pushes to the right submodules/<...>/<b>
  * .gitmodules now points each submodule at the Gibgoyt fork at the right
    branch. Review the diff before committing:
      git diff .gitmodules

Useful follow-ups:
  * commit the changed .gitmodules and the new Scripts/ on master:
      git add Scripts/ .gitmodules
      git commit -m "fork init: dual-remote setup + consolidated submodule branches"
      git push $MY_REMOTE master
  * to merge a future NVIDIA upstream change into your fork (per repo):
      git fetch $NV_REMOTE
      git checkout master
      git merge $NV_REMOTE/master
      git push                      # goes to $MY_REMOTE/master (or the
                                    # submodules/<...>/master equivalent)
EOF
