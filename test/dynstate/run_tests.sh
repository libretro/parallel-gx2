#!/bin/sh
# The state the pipelines leave to the draw, drawn baked and dynamic. RUNNER prefixes the test
# (wine for a MinGW build).
set -e
cd "$(dirname "$0")"
make -s
# the driver's threads' allocations (lsan.supp)
export LSAN_OPTIONS="suppressions=$(pwd)/lsan.supp${LSAN_OPTIONS:+:$LSAN_OPTIONS}"
RUN=${RUNNER:-}
command -v timeout >/dev/null 2>&1 && RUN="timeout 300 $RUN"
if [ -z "$VN_VALIDATE" ]; then
	$RUN ./dynstate_test${EXE:-}
	exit 0
fi
# VN_VALIDATE: under the Khronos validation layer, every pipeline and draw
# checked against the spec; any error fails
LAYER=VK_LAYER_KHRONOS_validation
found=
for d in /usr/share/vulkan/explicit_layer.d /usr/local/share/vulkan/explicit_layer.d /etc/vulkan/explicit_layer.d; do
	[ -f "$d/VkLayer_khronos_validation.json" ] && found=1
done
if [ -z "$found" ]; then
	echo "FAIL: VN_VALIDATE is set but the validation layer is not installed"
	exit 1
fi
out=$(VK_INSTANCE_LAYERS=$LAYER $RUN ./dynstate_test${EXE:-} 2>&1) || { echo "$out"; exit 1; }
echo "$out" | grep -v "^VUID\|Validation" || true
errors=$(echo "$out" | grep -c "Validation Error" || true)
echo "validation errors: $errors"
[ "$errors" = 0 ]
