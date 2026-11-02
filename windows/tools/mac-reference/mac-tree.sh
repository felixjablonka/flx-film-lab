# The Mac app's tree, the release the port follows, for the scripts here to
# source with the port's root in $root: mac/, the submodule, in the port's own
# repository; the repository's root, beside windows/, in the public one.
if [[ -f "$root/mac/build.sh" ]]; then
  mac="$root/mac"
else
  mac="$(cd "$root/.." && pwd -P)"
fi
[[ -f "$mac/build.sh" ]] || { echo "error: no Mac app tree at $root/mac or $root/.." >&2; exit 1; }
