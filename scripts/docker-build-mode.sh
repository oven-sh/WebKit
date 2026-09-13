# Sourced by the *-release.sh scripts, which all end in `docker buildx build ... "${BASE[@]}" "${OUTPUT[@]}" .`
#
# Every Dockerfile's `base` stage is the toolchain and nothing else, the same for every lane of a platform and
# architecture. CI builds it once per change and keeps it in a registry (the `image` job in .github/workflows/ci.yml):
#
#   BASE_IMAGE=<ref>        use that image in place of building the `base` stage
#   PUSH_BASE_IMAGE=<ref>   build only the `base` stage and push it there, instead of building bun-webkit
#
# With neither set, which is what running a release script by hand does, everything is built here:
# $temp/bun-webkit from the `artifact` stage, toolchain included.

BASE=()
if [ -n "${BASE_IMAGE:-}" ]; then
    BASE=(--build-context "base=docker-image://$BASE_IMAGE")
fi

if [ -n "${PUSH_BASE_IMAGE:-}" ]; then
    # --provenance=false: a plain image manifest, not an index with an attestation hanging off it.
    OUTPUT=(--target=base --tag "$PUSH_BASE_IMAGE" --provenance=false --push)
else
    OUTPUT=(--tag "$CONTAINER_NAME" --target=artifact --output "type=local,dest=$temp/bun-webkit")
fi
